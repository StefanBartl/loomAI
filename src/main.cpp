#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <string>
#include <ctime>
#include <chrono>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "anthropic_client.hpp"
#include "gemini_client.hpp"
#include "ollama_client.hpp"
#include "openai_client.hpp"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// ModelRouter: dispatch by model-name prefix ("claude-*"/"gpt-*"/"o1-*"/
// "o3-*"/"gemini-*" go to the matching cloud client; anything else,
// including no model at all, falls back to the local Ollama client -- the
// only backend that existed before this router). A prefix router keeps the
// existing /ask and /ask/stream HTTP contract unchanged (no new request
// field ai.nvim's loomai.lua would need to set) -- see the ai.nvim
// handover's loomAI ModelRouter scoping, Option (a), for the reasoning.
// ---------------------------------------------------------------------------
namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        size_t len = (comma == std::string::npos) ? std::string::npos : comma - start;
        std::string part = value.substr(start, len);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return parts;
}

// Model-name prefixes routed to each cloud backend, previously hardcoded
// with no config option at all. `env_var`, if set, REPLACES `fallback`
// wholesale (comma-separated) -- e.g. LOOMAI_PREFIXES_OPENAI=gpt-,o1-,o3-,o4-
// to pick up a new model family without a rebuild. Read once per backend
// (function-local static), same one-time-read contract every env lookup in
// this codebase already has.
std::vector<std::string> prefixes_for(const char* env_var,
                                       std::initializer_list<const char*> fallback) {
    const char* override_value = std::getenv(env_var);
    if (override_value && *override_value) {
        return split_csv(override_value);
    }
    return std::vector<std::string>(fallback.begin(), fallback.end());
}

const std::vector<std::string>& claude_prefixes() {
    static const std::vector<std::string> prefixes = prefixes_for("LOOMAI_PREFIXES_CLAUDE", {"claude-"});
    return prefixes;
}

const std::vector<std::string>& openai_prefixes() {
    static const std::vector<std::string> prefixes =
        prefixes_for("LOOMAI_PREFIXES_OPENAI", {"gpt-", "o1-", "o3-"});
    return prefixes;
}

const std::vector<std::string>& gemini_prefixes() {
    static const std::vector<std::string> prefixes = prefixes_for("LOOMAI_PREFIXES_GEMINI", {"gemini-"});
    return prefixes;
}

bool matches_any(const std::string& model, const std::vector<std::string>& prefixes) {
    for (const auto& prefix : prefixes) {
        if (starts_with(model, prefix)) {
            return true;
        }
    }
    return false;
}

// Per-backend default timeout, used only when the caller's request body
// omits "timeout_ms" -- ai.nvim's own loomai.lua always sends one, so this
// only matters for other callers (a raw curl call, a future client).
// Ollama's model can take several seconds just to load on a cold first
// request (measured live against this machine's Ollama: ~4.5s cold vs
// ~300ms warm for a 4.7GB model) -- a local backend earns a longer default
// than the cloud ones, which fail on their own gateway timeouts well under
// a minute anyway.
constexpr int kDefaultTimeoutOllamaMs = 120000;
constexpr int kDefaultTimeoutCloudMs = 60000;

int default_timeout_for(const std::string& model) {
    if (matches_any(model, claude_prefixes()) || matches_any(model, openai_prefixes()) ||
        matches_any(model, gemini_prefixes())) {
        return kDefaultTimeoutCloudMs;
    }
    return kDefaultTimeoutOllamaMs;
}

loomai::AskResult route_ask(const std::string& prompt,
                             const std::string& system,
                             const std::string& model,
                             int timeout_ms) {
    if (matches_any(model, claude_prefixes())) {
        return loomai::anthropic::ask(prompt, system, model, timeout_ms);
    }
    if (matches_any(model, openai_prefixes())) {
        return loomai::openai::ask(prompt, system, model, timeout_ms);
    }
    if (matches_any(model, gemini_prefixes())) {
        return loomai::gemini::ask(prompt, system, model, timeout_ms);
    }
    return loomai::ollama::ask(prompt, system, model, timeout_ms);
}

std::string route_ask_stream(const std::string& prompt,
                              const std::string& system,
                              const std::string& model,
                              int timeout_ms,
                              const std::function<void(const std::string&)>& on_delta) {
    if (matches_any(model, claude_prefixes())) {
        return loomai::anthropic::ask_stream(prompt, system, model, timeout_ms, on_delta);
    }
    if (matches_any(model, openai_prefixes())) {
        return loomai::openai::ask_stream(prompt, system, model, timeout_ms, on_delta);
    }
    if (matches_any(model, gemini_prefixes())) {
        return loomai::gemini::ask_stream(prompt, system, model, timeout_ms, on_delta);
    }
    return loomai::ollama::ask_stream(prompt, system, model, timeout_ms, on_delta);
}

}  // namespace

// ---------------------------------------------------------------------------
// Event-Bus (vereinfacht für ersten Schritt)
// ---------------------------------------------------------------------------
static std::vector<std::string> event_log;
static std::mutex               event_mutex;
static std::atomic<bool>        running{true};

void push_event(const std::string& type,
                const std::string& agent,
                const std::string& message) {
    json ev;
    ev["type"]    = type;
    ev["agent"]   = agent;
    ev["message"] = message;
    ev["ts"]      = static_cast<long>(std::time(nullptr));

    std::lock_guard<std::mutex> lk(event_mutex);
    event_log.push_back(ev.dump());
    std::cout << "[" << agent << "] " << message << "\n" << std::flush;
}

// ---------------------------------------------------------------------------
// ai.nvim-Provider-Kontrakt (/health, /ask, /ask/stream)
// siehe nvim/docs/ROADMAP/reports/loomai-ai-nvim-integration.md
// ---------------------------------------------------------------------------
void send_json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    json body;
    body["error"]["message"] = message;
    res.set_content(body.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// Cross-origin protection: the server binds 127.0.0.1 only, but that alone
// does not stop an arbitrary web page open in the same browser from firing
// fetch()/XHR requests at it -- a browser happily lets JS on any origin talk
// to localhost, and (for POST bodies whose Content-Type doesn't trigger a
// CORS preflight, e.g. "text/plain" carrying JSON-shaped text, which every
// handler here parses regardless of the declared Content-Type) that request
// still executes server-side even without a wildcard Access-Control-Allow-
// Origin response header -- that header only controls whether the page can
// *read* the response, not whether the request happens at all. A browser
// cannot forge or suppress its own Origin header on a cross-origin request
// though, so rejecting any request whose Origin doesn't match this server's
// own origin closes that gap without requiring a shared secret on ai.nvim's
// side: curl-based clients (ai.nvim's loomai.lua, plain `curl`) send no
// Origin header at all and are unaffected, and the dashboard's own JS is
// always same-origin.
// ---------------------------------------------------------------------------
bool is_allowed_origin(const std::string& origin) {
    return origin == "http://127.0.0.1:8080" || origin == "http://localhost:8080";
}

// ---------------------------------------------------------------------------
// Simulations-Agent (wird später durch echten LLM-Agent ersetzt)
// ---------------------------------------------------------------------------
void run_simulation_agent() {
    using namespace std::chrono_literals;

    std::this_thread::sleep_for(1s);
    push_event("agent_status", "Planer",
               "loomAI gestartet — analysiere Projektstruktur...");

    std::this_thread::sleep_for(3s);
    push_event("agent_status", "Planer",
               "Projektstruktur erkannt: src/, agents/, containers/");

    std::this_thread::sleep_for(2s);
    push_event("agent_status", "Executor",
               "Prüfe verfügbare Container-Runtime (Podman)...");

    std::this_thread::sleep_for(2s);
    push_event("agent_status", "Executor",
               "Podman rootless verfügbar — bereit für Sandbox");

    std::this_thread::sleep_for(2s);
    push_event("decision_point", "Planer",
               "Soll ich jetzt den ersten Agent-Container starten?");

    std::this_thread::sleep_for(6s);
    push_event("agent_status", "Executor",
               "Warte auf Human-Entscheidung...");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== loomAI Framework ===\n";
    std::cout << "Dashboard: http://127.0.0.1:8080\n";
    std::cout << "SSE:       http://127.0.0.1:8080/events\n";
    std::cout << "Health:    http://127.0.0.1:8080/health\n";
    std::cout << "Ask:       POST http://127.0.0.1:8080/ask\n";
    std::cout << "Stream:    POST http://127.0.0.1:8080/ask/stream\n";
    std::cout << "Ollama:    " << loomai::ollama::host()
               << " (model default: " << loomai::ollama::default_model() << ")\n";
    std::cout << "OpenAI:    "
               << (loomai::openai::configured() ? "configured" : "not configured (OPENAI_API_KEY unset)")
               << "\n";
    std::cout << "Anthropic: "
               << (loomai::anthropic::configured() ? "configured"
                                                    : "not configured (ANTHROPIC_API_KEY unset)")
               << "\n";
    std::cout << "Gemini:    "
               << (loomai::gemini::configured() ? "configured" : "not configured (GEMINI_API_KEY unset)")
               << "\n";
    std::cout << "Herkunft:  Anfragen mit fremdem Origin-Header werden abgelehnt "
                  "(schuetzt vor einer bösartigen Webseite im selben Browser)\n";
    std::cout << "Stoppen:   Ctrl+C\n\n";

    httplib::Server svr;

    // Statische Dashboard-Dateien aus ./dashboard/ ausliefern
    if (!svr.set_mount_point("/", "./dashboard")) {
        std::cerr << "[FEHLER] ./dashboard Verzeichnis nicht gefunden!\n";
        return 1;
    }

    // Reject any request whose Origin header names a foreign page before it
    // reaches a single route handler -- see is_allowed_origin()'s comment.
    // A top-level navigation (loading the dashboard itself) and every
    // non-browser client send no Origin header at all, so those still pass.
    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !is_allowed_origin(origin)) {
            send_json_error(res, 403, "cross-origin request blocked");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // SSE-Endpoint: Browser verbindet sich hier und empfängt live Events
    svr.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink& sink) {
                std::string data;
                {
                    std::lock_guard<std::mutex> lk(event_mutex);
                    if (!event_log.empty()) {
                        data = "data: " + event_log.front() + "\n\n";
                        event_log.erase(event_log.begin());
                    }
                }

                if (!data.empty()) {
                    sink.write(data.c_str(), data.size());
                } else {
                    // Heartbeat damit die Verbindung offen bleibt
                    std::string hb = ": heartbeat\n\n";
                    sink.write(hb.c_str(), hb.size());
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                return running.load();
            }
        );
    });

    // Decision-Endpoint: Browser schickt hier die Human-Antwort rein
    svr.Post("/decision", [](const httplib::Request& req,
                              httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid json\"}", "application/json");
            return;
        }
        std::string id  = body.value("id",     "unknown");
        std::string ans = body.value("answer", "unknown");
        push_event("decision_answer", "human",
                   "Decision [" + id + "] -> " + ans);
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Health-Endpoint: billiger "läuft und kann annehmen"-Check für
    // ai.nvim/andere Clients (Aufgabe A)
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json body;
        body["status"] = "ready";
        body["model_router_ready"] = true;
        // Presence-only, like everywhere else in this project -- never the
        // key values themselves. Lets a manual `curl /health` (see the
        // ai.nvim live-testing-plan) see which cloud backends are even
        // worth trying without guessing from an /ask error message.
        body["backends"] = {
            {"ollama", true},
            {"openai", loomai::openai::configured()},
            {"anthropic", loomai::anthropic::configured()},
            {"gemini", loomai::gemini::configured()},
        };
        res.set_content(body.dump(), "application/json");
    });

    // Non-Streaming Ask-Endpoint für ai.nvim's M.ask() (Aufgabe B).
    // route_ask() dispatcht per Modellname-Präfix an Ollama/OpenAI/
    // Anthropic/Gemini -- der HTTP-Kontrakt selbst ändert sich dadurch
    // nicht (kein neues Request-Feld nötig).
    svr.Post("/ask", [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("prompt") ||
            !body["prompt"].is_string()) {
            send_json_error(res, 400, "invalid request: 'prompt' (string) is required");
            return;
        }

        std::string prompt = body.value("prompt", "");
        std::string system = body.value("system", "");
        std::string model  = body.value("model", "");
        int timeout_ms     = body.value("timeout_ms", default_timeout_for(model));

        auto result = route_ask(prompt, system, model, timeout_ms);
        if (!result.ok) {
            send_json_error(res, 502, result.error_message);
            return;
        }

        json out;
        out["text"]     = result.text;
        out["provider"] = "loomai";
        if (!result.stop_reason.empty()) {
            out["stop_reason"] = result.stop_reason;
        }
        out["usage"] = {
            {"prompt_tokens", result.prompt_tokens},
            {"completion_tokens", result.completion_tokens},
        };
        res.set_content(out.dump(), "application/json");
    });

    // Streaming Ask-Endpoint für ai.nvim's M.stream() (Aufgabe C). SSE mit
    // einem JSON-Objekt pro "data:"-Zeile; Fehler mitten im Stream werden
    // als reguläres SSE-Event gesendet statt die Verbindung roh abzureißen.
    svr.Post("/ask/stream", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("prompt") ||
            !body["prompt"].is_string()) {
            send_json_error(res, 400, "invalid request: 'prompt' (string) is required");
            return;
        }

        std::string prompt = body.value("prompt", "");
        std::string system = body.value("system", "");
        std::string model  = body.value("model", "");
        int timeout_ms     = body.value("timeout_ms", default_timeout_for(model));

        res.set_chunked_content_provider(
            "text/event-stream",
            [prompt, system, model, timeout_ms](size_t, httplib::DataSink& sink) {
                auto write_event = [&sink](const json& payload) {
                    std::string data = "data: " + payload.dump() + "\n\n";
                    sink.write(data.c_str(), data.size());
                };

                // This callback runs outside cpp-httplib's own
                // request-routing try/catch (it executes while the response
                // body is being written, not while the handler above is
                // being routed) -- an exception escaping it is not caught
                // anywhere and crashes the whole process, not just this
                // request. Everything below is defensive because of that,
                // not because an exception is expected in the normal case.
                try {
                    std::string err = route_ask_stream(
                        prompt, system, model, timeout_ms,
                        [&](const std::string& delta) {
                            write_event(json{{"delta", delta}});
                        });

                    if (!err.empty()) {
                        write_event(json{{"error", {{"message", err}}}});
                    } else {
                        static const std::string done_event = "data: [DONE]\n\n";
                        sink.write(done_event.c_str(), done_event.size());
                    }
                } catch (const std::exception& e) {
                    write_event(json{{"error", {{"message", std::string("loomai: ") + e.what()}}}});
                } catch (...) {
                    write_event(json{{"error", {{"message", "loomai: unknown internal error"}}}});
                }

                sink.done();
                return true;
            }
        );
    });

    // Simulations-Agent in eigenem Thread starten
    std::thread sim(run_simulation_agent);

    // HTTP-Server blockiert hier (Ctrl+C beendet). listen() returns false on
    // a bind failure (e.g. port already in use) instead of throwing -- an
    // unchecked return here made the process exit silently with no
    // indication why, which is exactly what happened during ModelRouter
    // testing when a stray leftover instance still held the port.
    bool listen_ok = svr.listen("127.0.0.1", 8080);

    running = false;
    sim.join();

    if (!listen_ok) {
        std::cerr << "loomai: failed to bind 127.0.0.1:8080 (port already in use?)\n";
        return 1;
    }
    return 0;
}
