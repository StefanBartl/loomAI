# KI-Framework — Schritt-für-Schritt Setup

> **Ziel dieser Phase:** Eine lauffähige Umgebung, in der du einen Agenten starten kannst,
> der per Anthropic API denkt, in einem Podman-Container arbeitet und dir über ein
> HTML-Dashboard live Rückmeldung gibt — inklusive einem Hard-Stop-Button.

---

## Voraussetzungen prüfen

```bash
# Diese Befehle alle einmal ausführen — prüfen, was schon da ist
nvidia-smi                  # GPU + Treiber sichtbar?
nvcc --version              # CUDA Toolkit installiert?
podman --version            # Podman vorhanden?
git --version
cmake --version             # >= 3.28 empfohlen
clang++ --version           # oder g++ --version (>= 13)
curl --version
python3 --version           # für kleine Hilfsskripte
```

**Minimalanforderungen:**
- NVIDIA-Treiber >= 535 (für CUDA 12)
- Podman >= 4.x (rootless)
- CMake >= 3.20
- C++23-fähiger Compiler (clang 17+ oder gcc 13+)

---

## Phase 0 — System-Abhängigkeiten (Ubuntu/Debian)

### 0.1 CUDA Toolkit (falls noch nicht installiert)

```bash
# CUDA 12.x auf Ubuntu 22.04 / 24.04
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-4

# Umgebungsvariablen setzen (in ~/.bashrc oder ~/.zshrc eintragen)
export PATH=/usr/local/cuda/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH
source ~/.bashrc
nvcc --version   # sollte 12.x zeigen
```

### 0.2 Podman (rootless)

```bash
sudo apt-get install -y podman

# Rootless-Setup (einmalig)
podman system migrate

# Test
podman run --rm hello-world
podman info | grep -A2 "rootless"
```

### 0.3 Build-Tools und C++-Bibliotheken

```bash
sudo apt-get install -y \
    build-essential cmake ninja-build \
    clang-17 clang-format-17 clang-tidy-17 \
    libssl-dev libcurl4-openssl-dev \
    libsqlite3-dev \
    git pkg-config

# Lua für Agent-Scripting
sudo apt-get install -y liblua5.4-dev lua5.4
```

### 0.4 Ollama (lokale Modelle auf GPU)

```bash
curl -fsSL https://ollama.com/install.sh | sh

# Service starten
systemctl --user enable ollama
systemctl --user start ollama

# Erstes Modell ziehen (passt auf RTX 4070 Super)
ollama pull llama3.1:8b-q5_K_M
ollama pull qwen2.5-coder:7b-q5_K_M   # kompakter Code-Agent

# Test
ollama run llama3.1:8b-q5_K_M "Antworte mit genau einem Satz: Was ist 2+2?"
```

---

## Phase 1 — Projektstruktur anlegen

```bash
mkdir -p ~/ki-framework
cd ~/ki-framework

# Verzeichnisstruktur
mkdir -p \
    src/orchestrator \
    src/models \
    src/sandbox \
    src/dashboard \
    src/state \
    agents \
    dashboard \
    containers \
    tests \
    scripts \
    third_party

# Git initialisieren
git init
echo "build/" >> .gitignore
echo ".env" >> .gitignore
echo "*.sqlite" >> .gitignore
git add .gitignore
git commit -m "initial: project skeleton"
```

---

## Phase 2 — Abhängigkeiten (Header-only / vendored)

Alles lokal in `third_party/` — keine system-weiten Installationen nötig.

### 2.1 cpp-httplib (HTTP-Server + SSE)

```bash
cd ~/ki-framework/third_party
git clone --depth=1 https://github.com/yhirose/cpp-httplib.git
# Nur ein Header: third_party/cpp-httplib/httplib.h
```

### 2.2 nlohmann/json (JSON-Serialisierung)

```bash
cd ~/ki-framework/third_party
mkdir nlohmann
curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
    -o nlohmann/json.hpp
```

### 2.3 sol2 (Lua-C++-Bindung für Agent-Scripts)

```bash
cd ~/ki-framework/third_party
git clone --depth=1 --branch v3.3.0 https://github.com/ThePhD/sol2.git
```

### 2.4 SQLiteCpp (SQLite-Wrapper)

```bash
cd ~/ki-framework/third_party
git clone --depth=1 https://github.com/SRombauts/SQLiteCpp.git
```

### 2.5 Catch2 (Testing)

```bash
cd ~/ki-framework/third_party
git clone --depth=1 --branch v3.6.0 https://github.com/catchorg/Catch2.git
```

---

## Phase 3 — CMakeLists.txt

Datei anlegen: `~/ki-framework/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(ki_framework CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler-Flags
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Third-party
add_subdirectory(third_party/SQLiteCpp)
add_subdirectory(third_party/Catch2)

# sol2 (header-only)
add_library(sol2 INTERFACE)
target_include_directories(sol2 INTERFACE third_party/sol2/include)

# nlohmann/json (header-only)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE third_party)

# Lua
find_package(Lua 5.4 REQUIRED)

# Haupt-Executable
file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(ki_framework ${SOURCES})

target_include_directories(ki_framework PRIVATE
    src/
    third_party/cpp-httplib
    ${LUA_INCLUDE_DIR}
)

target_link_libraries(ki_framework PRIVATE
    SQLiteCpp sqlite3
    sol2
    nlohmann_json
    ${LUA_LIBRARIES}
    pthread
    curl
    ssl
    crypto
)

# Tests
file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
add_executable(ki_tests ${TEST_SOURCES})
target_link_libraries(ki_tests PRIVATE
    Catch2::Catch2WithMain
    SQLiteCpp sqlite3
    sol2
    nlohmann_json
    ${LUA_LIBRARIES}
    pthread curl ssl crypto
)
target_include_directories(ki_tests PRIVATE src/ third_party/cpp-httplib ${LUA_INCLUDE_DIR})

include(CTest)
include(Catch)
catch_discover_tests(ki_tests)
```

---

## Phase 4 — Erster lauffähiger Stub

### 4.1 src/main.cpp

```cpp
#include <iostream>
#include <thread>
#include <atomic>
#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// Globaler Event-Buffer (vereinfacht für ersten Schritt)
std::vector<std::string> event_log;
std::mutex               event_mutex;
std::atomic<bool>        running{true};

void push_event(const std::string& type,
                const std::string& agent,
                const std::string& message) {
    json ev;
    ev["type"]    = type;
    ev["agent"]   = agent;
    ev["message"] = message;
    ev["ts"]      = std::time(nullptr);

    std::lock_guard lk(event_mutex);
    event_log.push_back(ev.dump());
    std::cout << "[EVENT] " << agent << ": " << message << "\n";
}

int main() {
    std::cout << "KI-Framework starting...\n";

    httplib::Server svr;

    // Statische Dashboard-Dateien
    svr.set_mount_point("/", "./dashboard");

    // SSE-Endpoint
    svr.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t /*offset*/, httplib::DataSink& sink) {
                std::unique_lock lk(event_mutex);
                if (!event_log.empty()) {
                    std::string data = "data: " + event_log.back() + "\n\n";
                    event_log.clear();
                    lk.unlock();
                    sink.write(data.c_str(), data.size());
                } else {
                    lk.unlock();
                    // Heartbeat
                    std::string hb = ": heartbeat\n\n";
                    sink.write(hb.c_str(), hb.size());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                return running.load();
            }
        );
    });

    // Decision-Antwort-Endpoint
    svr.Post("/decision", [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        std::string id  = body.value("id",     "?");
        std::string ans = body.value("answer", "?");
        push_event("decision_answer", "human", "Decision " + id + " -> " + ans);
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Simulations-Thread (später durch echten Agenten ersetzen)
    std::thread sim([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        push_event("agent_status", "Planer",   "Task erhalten: Projektstruktur aufbauen");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        push_event("agent_status", "Planer",   "Analysiere Anforderungen...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        push_event("decision_point", "Planer", "Welche Sprache? Python oder Node.js?");
        std::this_thread::sleep_for(std::chrono::seconds(4));
        push_event("agent_status", "Executor", "Starte Container-Setup...");
    });

    std::cout << "Dashboard: http://localhost:8080\n";
    std::cout << "SSE:       http://localhost:8080/events\n";
    std::cout << "Ctrl+C zum Beenden\n";

    svr.listen("0.0.0.0", 8080);
    running = false;
    sim.join();
    return 0;
}
```

### 4.2 dashboard/index.html

```html
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>KI-Framework Dashboard</title>
  <style>
    :root {
      --bg: #0f1117; --card: #1a1d27; --border: #2a2d3e;
      --text: #e2e8f0; --muted: #64748b;
      --teal: #14b8a6; --amber: #f59e0b;
      --red: #ef4444; --green: #22c55e; --purple: #a78bfa;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: var(--bg); color: var(--text);
           font-family: 'Cascadia Code', 'JetBrains Mono', monospace;
           font-size: 14px; padding: 1.5rem; }
    h1 { color: var(--teal); font-size: 1.1rem; letter-spacing: .08em;
         text-transform: uppercase; margin-bottom: 1.5rem; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
    .card { background: var(--card); border: 1px solid var(--border);
            border-radius: 8px; padding: 1rem; }
    .card h2 { font-size: .75rem; color: var(--muted); text-transform: uppercase;
               letter-spacing: .1em; margin-bottom: .75rem; }
    .agent { display: flex; align-items: flex-start; gap: .6rem;
             padding: .5rem 0; border-bottom: 1px solid var(--border); }
    .agent:last-child { border-bottom: none; }
    .dot { width: 8px; height: 8px; border-radius: 50%;
           background: var(--muted); margin-top: 4px; flex-shrink: 0; }
    .dot.active { background: var(--teal);
                  box-shadow: 0 0 6px var(--teal); }
    .agent-name { color: var(--purple); font-weight: 600; font-size: .8rem; }
    .agent-msg  { color: var(--muted); font-size: .8rem; margin-top: .15rem; }
    #log { height: 220px; overflow-y: auto; font-size: .78rem; }
    .log-line { padding: .2rem 0; border-bottom: 1px solid #1e2130;
                display: flex; gap: .5rem; }
    .log-ts   { color: var(--muted); flex-shrink: 0; }
    .log-agent { color: var(--purple); flex-shrink: 0; min-width: 70px; }
    .log-msg  { color: var(--text); }
    .decision-banner {
      margin-top: 1rem; background: #2d1515; border: 1px solid var(--red);
      border-radius: 8px; padding: 1rem; display: none;
    }
    .decision-banner.visible { display: block; }
    .decision-banner h3 { color: var(--red); font-size: .8rem;
                          text-transform: uppercase; letter-spacing: .1em; }
    .decision-banner p  { color: var(--text); margin: .5rem 0 .75rem; }
    .btn { border: none; border-radius: 5px; padding: .4rem .9rem;
           font-family: inherit; font-size: .8rem; cursor: pointer;
           transition: opacity .15s; }
    .btn:hover { opacity: .8; }
    .btn-green { background: var(--green); color: #000; }
    .btn-red   { background: var(--red);   color: #fff; }
    .status-row { display: flex; gap: .5rem; align-items: center;
                  padding: .25rem 0; font-size: .78rem; }
    .badge { padding: .15rem .5rem; border-radius: 4px; font-size: .7rem;
             text-transform: uppercase; letter-spacing: .05em; }
    .badge-ok  { background: #14402a; color: var(--green); }
    .badge-wait { background: #2d2a10; color: var(--amber); }
  </style>
</head>
<body>
<h1>▸ KI-Framework Dashboard</h1>

<div class="grid">
  <div class="card">
    <h2>Agenten</h2>
    <div id="agents">
      <div class="agent" id="agent-Planer">
        <div class="dot" id="dot-Planer"></div>
        <div>
          <div class="agent-name">Planer</div>
          <div class="agent-msg" id="msg-Planer">—</div>
        </div>
      </div>
      <div class="agent" id="agent-Executor">
        <div class="dot" id="dot-Executor"></div>
        <div>
          <div class="agent-name">Executor</div>
          <div class="agent-msg" id="msg-Executor">—</div>
        </div>
      </div>
      <div class="agent" id="agent-human">
        <div class="dot" id="dot-human"></div>
        <div>
          <div class="agent-name">Human</div>
          <div class="agent-msg" id="msg-human">Bereit</div>
        </div>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>System-Status</h2>
    <div class="status-row">
      <span class="badge badge-ok">SSE</span>
      <span id="sse-status" style="color:var(--muted)">Verbinde...</span>
    </div>
    <div class="status-row">
      <span class="badge badge-ok">Sandbox</span>
      <span style="color:var(--muted)">Podman rootless</span>
    </div>
    <div class="status-row">
      <span class="badge badge-wait">Model</span>
      <span style="color:var(--muted)">llama3.1:8b lokal</span>
    </div>
  </div>
</div>

<div class="decision-banner" id="decision-banner">
  <h3>⚠ Entscheidung erforderlich</h3>
  <p id="decision-text"></p>
  <button class="btn btn-green" onclick="sendDecision('allow')">✓ Erlauben</button>
  <button class="btn btn-red"   onclick="sendDecision('deny')" style="margin-left:.5rem">✗ Ablehnen</button>
</div>

<div class="card" style="margin-top:1rem">
  <h2>Live-Log</h2>
  <div id="log"></div>
</div>

<script>
  let currentDecisionId = null;

  function ts() {
    return new Date().toLocaleTimeString('de-AT', {hour12:false});
  }

  function appendLog(agent, message, color) {
    const log = document.getElementById('log');
    const line = document.createElement('div');
    line.className = 'log-line';
    line.innerHTML = `<span class="log-ts">${ts()}</span>
                      <span class="log-agent" style="color:${color||'var(--purple)'}">${agent}</span>
                      <span class="log-msg">${message}</span>`;
    log.appendChild(line);
    log.scrollTop = log.scrollHeight;
  }

  function handleEvent(ev) {
    const dot = document.getElementById('dot-' + ev.agent);
    const msg = document.getElementById('msg-' + ev.agent);

    if (ev.type === 'agent_status') {
      if (dot) { dot.className = 'dot active'; }
      if (msg) { msg.textContent = ev.message; }
      appendLog(ev.agent, ev.message, 'var(--purple)');
    }

    if (ev.type === 'decision_point') {
      currentDecisionId = ev.ts;
      document.getElementById('decision-text').textContent = ev.message;
      document.getElementById('decision-banner').classList.add('visible');
      appendLog('SYSTEM', '⚠ Decision Point: ' + ev.message, 'var(--red)');
    }

    if (ev.type === 'decision_answer') {
      document.getElementById('decision-banner').classList.remove('visible');
      appendLog('human', ev.message, 'var(--green)');
    }
  }

  function sendDecision(answer) {
    fetch('/decision', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({id: currentDecisionId, answer})
    });
    document.getElementById('decision-banner').classList.remove('visible');
  }

  // SSE Verbindung
  const sse = new EventSource('/events');
  sse.onopen = () => {
    document.getElementById('sse-status').textContent = 'Verbunden';
    document.getElementById('sse-status').style.color = 'var(--green)';
    appendLog('SYSTEM', 'Dashboard verbunden', 'var(--teal)');
  };
  sse.onmessage = (e) => {
    try { handleEvent(JSON.parse(e.data)); } catch(_) {}
  };
  sse.onerror = () => {
    document.getElementById('sse-status').textContent = 'Getrennt – reconnecting...';
    document.getElementById('sse-status').style.color = 'var(--red)';
  };
</script>
</body>
</html>
```

---

## Phase 5 — Bauen und starten

```bash
cd ~/ki-framework

# Build-Verzeichnis anlegen
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++-17 \
      -G Ninja

# Kompilieren
cmake --build build -j$(nproc)

# Starten
cd ~/ki-framework
./build/ki_framework
# → Dashboard: http://localhost:8080
```

**Erwartetes Ergebnis:** Browser öffnen, `http://localhost:8080` aufrufen.
Nach ~2 Sekunden erscheinen live die ersten Agenten-Events, nach ~7 Sekunden
poppt das Decision-Banner auf.

---

## Phase 6 — Erster echter Anthropic-Agent

Wenn der Stub funktioniert, den Simulations-Thread durch einen echten API-Call ersetzen.

### .env Datei (nicht in Git!)

```bash
cat > ~/ki-framework/.env << 'EOF'
ANTHROPIC_API_KEY=sk-ant-...dein-key...
OLLAMA_HOST=http://localhost:11434
EOF
chmod 600 ~/ki-framework/.env
```

### Minimaler Anthropic-Client (src/models/anthropic_client.hpp)

```cpp
#pragma once
#include <string>
#include <functional>
#include <curl/curl.h>
#include "nlohmann/json.hpp"

class AnthropicClient {
public:
    explicit AnthropicClient(std::string api_key)
        : api_key_(std::move(api_key)) {}

    // Einfacher (nicht-streaming) Aufruf
    std::string complete(const std::string& system_prompt,
                         const std::string& user_message,
                         const std::string& model = "claude-sonnet-4-6") {
        using json = nlohmann::json;

        json body;
        body["model"]      = model;
        body["max_tokens"] = 1024;
        body["system"]     = system_prompt;
        body["messages"]   = json::array({{{"role","user"},{"content",user_message}}});

        std::string response_body;
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("x-api-key: " + api_key_).c_str());
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        headers = curl_slist_append(headers, "content-type: application/json");

        std::string payload = body.dump();
        curl_easy_setopt(curl, CURLOPT_URL,
                         "https://api.anthropic.com/v1/messages");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t, size_t nmemb, void* data) {
                static_cast<std::string*>(data)->append(ptr, nmemb);
                return nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        try {
            auto resp = json::parse(response_body);
            return resp["content"][0]["text"].get<std::string>();
        } catch (...) { return response_body; }
    }

private:
    std::string api_key_;
};
```

### Agent-Aufruf im main.cpp ersetzen

```cpp
// .env laden
auto api_key = std::getenv("ANTHROPIC_API_KEY");
AnthropicClient llm(api_key ? api_key : "");

// Simulations-Thread → echter Agent
std::thread agent_thread([&]() {
    push_event("agent_status", "Planer", "Analysiere Projektanforderungen...");

    std::string antwort = llm.complete(
        "Du bist ein Software-Architekt. Antworte präzise auf Deutsch.",
        "Schlage 3 konkrete nächste Schritte vor, um ein "
        "KI-Agenten-Framework in C++ zu starten. Jeweils max. 1 Satz."
    );

    push_event("agent_status", "Planer", antwort);
    push_event("decision_point", "Planer",
               "Soll ich mit Schritt 1 beginnen?");
});
```

---

## Zusammenfassung: Reihenfolge

| Schritt | Aktion | Prüfen mit |
|---|---|---|
| 1 | CUDA + Treiber prüfen | `nvidia-smi` |
| 2 | Podman installieren | `podman run hello-world` |
| 3 | Ollama + Modell laden | `ollama run llama3.1:8b "test"` |
| 4 | Projektstruktur + Git | `ls ~/ki-framework` |
| 5 | Deps in `third_party/` | alle 5 Ordner vorhanden |
| 6 | CMakeLists.txt anlegen | `cmake -B build` ohne Fehler |
| 7 | main.cpp + dashboard/index.html | `./build/ki_framework` startet |
| 8 | Browser öffnen | Events erscheinen live |
| 9 | .env + API-Key | echter Agent antwortet |
| 10 | Decision-Banner testen | [Erlauben] / [Ablehnen] klicken |

**Nächste Ausbaustufe danach:**
- Podman-Wrapper für Container-Spawn
- Git-Snapshot-Mechanismus im Container
- Lua-Agent-Scripts laden (sol2)
- Rollback-UI im Dashboard
