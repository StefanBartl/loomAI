#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <string>
#include <ctime>
#include <chrono>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"

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
    std::cout << "Dashboard: http://localhost:8080\n";
    std::cout << "SSE:       http://localhost:8080/events\n";
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

    // Simulations-Agent in eigenem Thread starten
    std::thread sim(run_simulation_agent);

    // HTTP-Server blockiert hier (Ctrl+C beendet)
    svr.listen("0.0.0.0", 8080);

    running = false;
    sim.join();
    return 0;
}
