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

#ifdef _WIN32
#include <windows.h>
#endif

#include "anthropic_client.hpp"
#include "gemini_client.hpp"
#include "models.hpp"
#include "ollama_client.hpp"
#include "openai_client.hpp"
#include "secrets.hpp"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// ModelRouter: models::resolve() decides the backend -- an exact custom model
// id first, then the model-name prefixes ("claude-*"/"gpt-*"/"o1-*"/"o3-*"/
// "gemini-*", overridable with LOOMAI_PREFIXES_*), and anything else,
// including no model at all, goes to the local Ollama client (the only backend
// that existed before this router). Prefix routing keeps the /ask and
// /ask/stream HTTP contract unchanged (no new request field ai.nvim's
// loomai.lua would need to set) -- see the ai.nvim handover's loomAI
// ModelRouter scoping, Option (a), for the reasoning. A custom model is just a
// `model` value that happens to be registered in config.json.
// ---------------------------------------------------------------------------
namespace {

// Per-backend default timeout, used only when the caller's request body
// omits "timeout_ms" -- ai.nvim's own loomai.lua always sends one, so this
// only matters for other callers (a raw curl call, a future client).
// Ollama's model can take several seconds just to load on a cold first
// request (measured live against this machine's Ollama: ~4.5s cold vs
// ~300ms warm for a 4.7GB model) -- a local backend (Ollama or a loopback
// OpenAI-compatible server) earns a longer default than the cloud ones, which
// fail on their own gateway timeouts well under a minute anyway.
constexpr int kDefaultTimeoutLocalMs = 120000;
constexpr int kDefaultTimeoutCloudMs = 60000;

int default_timeout_for(const std::string& model) {
    return loomai::models::resolve(model).local ? kDefaultTimeoutLocalMs : kDefaultTimeoutCloudMs;
}

// Endpoint for a user-defined OpenAI-compatible model. A key is optional (local
// servers usually have none) but is sent when one is stored.
loomai::openai::Endpoint endpoint_for(const loomai::models::Route& route) {
    loomai::openai::Endpoint endpoint;
    endpoint.base_url = route.base_url;
    endpoint.path_prefix = route.path_prefix;
    endpoint.api_key = loomai::secrets::get(route.key_name);
    endpoint.label = route.label;
    endpoint.key_required = false;
    return endpoint;
}

loomai::AskResult route_ask(const std::string& prompt,
                             const std::string& system,
                             const std::string& model,
                             int timeout_ms) {
    const auto route = loomai::models::resolve(model);
    if (route.provider == "anthropic") {
        return loomai::anthropic::ask(prompt, system, route.upstream_model, timeout_ms);
    }
    if (route.provider == "openai") {
        return loomai::openai::ask(prompt, system, route.upstream_model, timeout_ms);
    }
    if (route.provider == "gemini") {
        return loomai::gemini::ask(prompt, system, route.upstream_model, timeout_ms);
    }
    if (route.provider == "openai-compatible") {
        return loomai::openai::ask(prompt, system, route.upstream_model, timeout_ms, endpoint_for(route));
    }
    return loomai::ollama::ask(prompt, system, route.upstream_model, timeout_ms);
}

std::string route_ask_stream(const std::string& prompt,
                              const std::string& system,
                              const std::string& model,
                              int timeout_ms,
                              const std::function<void(const std::string&)>& on_delta) {
    const auto route = loomai::models::resolve(model);
    if (route.provider == "anthropic") {
        return loomai::anthropic::ask_stream(prompt, system, route.upstream_model, timeout_ms, on_delta);
    }
    if (route.provider == "openai") {
        return loomai::openai::ask_stream(prompt, system, route.upstream_model, timeout_ms, on_delta);
    }
    if (route.provider == "gemini") {
        return loomai::gemini::ask_stream(prompt, system, route.upstream_model, timeout_ms, on_delta);
    }
    if (route.provider == "openai-compatible") {
        return loomai::openai::ask_stream(prompt, system, route.upstream_model, timeout_ms, endpoint_for(route),
                                          on_delta);
    }
    return loomai::ollama::ask_stream(prompt, system, route.upstream_model, timeout_ms, on_delta);
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
// see wkdbook-loomai/Backlog/FEATURES/loomai-ai-nvim-integration.md (author notes, not in this repo)
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
// The port is configurable (LOOMAI_PORT / --port, needed by the desktop app's
// sidecar), so the allowed origins follow the port the server really listens
// on instead of a hardcoded 8080.
constexpr int kDefaultPort = 8080;
int g_port = kDefaultPort;

bool is_allowed_origin(const std::string& origin) {
    const std::string port = std::to_string(g_port);
    return origin == "http://127.0.0.1:" + port || origin == "http://localhost:" + port;
}

// Parses a TCP port; returns 0 for anything that is not a plain 1..65535.
int parse_port(const std::string& text) {
    if (text.empty() || text.size() > 5) {
        return 0;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    int value = std::stoi(text);
    return (value >= 1 && value <= 65535) ? value : 0;
}

// The Windows console defaults to a legacy code page, so the UTF-8 this server
// writes (umlauts, dashes in the status lines) shows up as mojibake. Switching
// the output code page fixes that for the process; it never touches the
// user's console settings afterwards.
void use_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
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
int main(int argc, char** argv) {
    use_utf8_console();

    // Port: --port N wins over LOOMAI_PORT, default 8080 (what ai.nvim expects).
    // An empty LOOMAI_PORT counts as unset (a common way to "clear" a variable),
    // but an explicit empty --port is a mistake and must not silently fall back
    // to the default.
    std::string port_text;
    bool have_port = false;
    if (const char* env_port = std::getenv("LOOMAI_PORT")) {
        if (*env_port) {
            port_text = env_port;
            have_port = true;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port_text = argv[++i];
            have_port = true;
        } else if (arg.rfind("--port=", 0) == 0) {
            port_text = arg.substr(7);
            have_port = true;
        } else {
            std::cerr << "loomai: unknown argument '" << arg << "' (supported: --port N)\n";
            return 2;
        }
    }
    if (have_port) {
        g_port = parse_port(port_text);
        if (g_port == 0) {
            std::cerr << "loomai: invalid port '" << port_text << "' (expected 1-65535)\n";
            return 2;
        }
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(g_port);

    // The dashboard is served from this directory; the desktop app's sidecar
    // does not start inside it, so it can be pointed elsewhere.
    std::string dashboard_dir = "./dashboard";
    if (const char* env_dir = std::getenv("LOOMAI_DASHBOARD_DIR")) {
        if (*env_dir) {
            dashboard_dir = env_dir;
        }
    }

    std::cout << "=== loomAI Framework ===\n";
    std::cout << "Dashboard: " << base << "\n";
    std::cout << "SSE:       " << base << "/events\n";
    std::cout << "Health:    " << base << "/health\n";
    std::cout << "Ask:       POST " << base << "/ask\n";
    std::cout << "Stream:    POST " << base << "/ask/stream\n";
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

    // Request bodies here are prompts and small JSON documents; a megabyte is
    // far more than any legitimate one, and bounds what a local client can make
    // the server buffer.
    svr.set_payload_max_length(1024 * 1024);

    // Statische Dashboard-Dateien ausliefern
    if (!svr.set_mount_point("/", dashboard_dir)) {
        std::cerr << "[FEHLER] Dashboard-Verzeichnis '" << dashboard_dir
                  << "' nicht gefunden!\n";
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
    bool listen_ok = svr.listen("127.0.0.1", g_port);

    running = false;
    sim.join();

    if (!listen_ok) {
        std::cerr << "loomai: failed to bind 127.0.0.1:" << g_port << " (port already in use?)\n";
        return 1;
    }
    return 0;
}
