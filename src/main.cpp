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
#include "ollama_client.hpp"

using json = nlohmann::json;

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
    std::cout << "Stoppen:   Ctrl+C\n\n";

    httplib::Server svr;

    // Statische Dashboard-Dateien aus ./dashboard/ ausliefern
    if (!svr.set_mount_point("/", "./dashboard")) {
        std::cerr << "[FEHLER] ./dashboard Verzeichnis nicht gefunden!\n";
        return 1;
    }

    // SSE-Endpoint: Browser verbindet sich hier und empfängt live Events
    svr.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
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
        res.set_header("Access-Control-Allow-Origin", "*");
        json body;
        body["status"] = "ready";
        body["model_router_ready"] = true;
        res.set_content(body.dump(), "application/json");
    });

    // Non-Streaming Ask-Endpoint für ai.nvim's M.ask() (Aufgabe B). Ruft
    // aktuell direkt Ollama; ein echter ModelRouter (lokal/cloud) kann hier
    // später ansetzen, ohne dass sich der HTTP-Kontrakt ändert.
    svr.Post("/ask", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("prompt") ||
            !body["prompt"].is_string()) {
            send_json_error(res, 400, "invalid request: 'prompt' (string) is required");
            return;
        }

        std::string prompt = body.value("prompt", "");
        std::string system = body.value("system", "");
        std::string model  = body.value("model", "");
        int timeout_ms     = body.value("timeout_ms", 60000);

        auto result = loomai::ollama::ask(prompt, system, model, timeout_ms);
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
            {"prompt_tokens", result.prompt_eval_count},
            {"completion_tokens", result.eval_count},
        };
        res.set_content(out.dump(), "application/json");
    });

    // Streaming Ask-Endpoint für ai.nvim's M.stream() (Aufgabe C). SSE mit
    // einem JSON-Objekt pro "data:"-Zeile; Fehler mitten im Stream werden
    // als reguläres SSE-Event gesendet statt die Verbindung roh abzureißen.
    svr.Post("/ask/stream", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
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
        int timeout_ms     = body.value("timeout_ms", 60000);

        res.set_chunked_content_provider(
            "text/event-stream",
            [prompt, system, model, timeout_ms](size_t, httplib::DataSink& sink) {
                auto write_event = [&sink](const json& payload) {
                    std::string data = "data: " + payload.dump() + "\n\n";
                    sink.write(data.c_str(), data.size());
                };

                std::string err = loomai::ollama::ask_stream(
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

                sink.done();
                return true;
            }
        );
    });

    // Simulations-Agent in eigenem Thread starten
    std::thread sim(run_simulation_agent);

    // HTTP-Server blockiert hier (Ctrl+C beendet)
    svr.listen("127.0.0.1", 8080);

    running = false;
    sim.join();
    return 0;
}
