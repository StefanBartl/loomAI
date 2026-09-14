# loomAI

Lokaler Agenten-Framework-Server (C++23, `cpp-httplib`). Dient als Backend für
ein Live-Dashboard (SSE) und als Ask/Stream-Endpoint für Editor-Integrationen
(siehe [`ai.nvim`](../ai.nvim)-Integrationsplan unter
`nvim/docs/ROADMAP/reports/loomai-ai-nvim-integration.md`).

## Bauen

Abhängigkeiten sind Header-only und werden lokal in `third_party/` abgelegt
(nicht in Git, siehe `.gitignore`):

```bash
mkdir -p third_party/nlohmann
curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
    -o third_party/nlohmann/json.hpp
curl -sL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h \
    -o third_party/httplib.h
```

Danach mit CMake bauen:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

Unter Windows mit MinGW/g++ explizit den Generator angeben, falls CMake nicht
automatisch das Richtige wählt:

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

## Starten

```bash
./build/loomai      # bzw. build/loomai.exe unter Windows
```

Der Server bindet auf `127.0.0.1:8080` (bewusst nicht `0.0.0.0` — kein
Editor-Companion-Prozess soll versehentlich im LAN erreichbar sein).

Für `/ask` und `/ask/stream` muss ein lokaler [Ollama](https://ollama.com)
laufen (Standard: `http://127.0.0.1:11434`). Abweichender Host via
`LOOMAI_OLLAMA_HOST` (**nicht** `OLLAMA_HOST` — das ist bereits durch Ollama
selbst als Bind-Adresse des Ollama-*Servers* belegt, z. B. `0.0.0.0:11434`,
und damit kein gültiges Client-Ziel). Standardmodell via
`LOOMAI_DEFAULT_MODEL`, sonst `llama3.1:8b-q5_K_M`.

## HTTP-API

| Endpoint | Methode | Zweck |
|---|---|---|
| `/events` | GET (SSE) | Dashboard-Events (Agent-Status, Decision-Points) |
| `/decision` | POST | Human-in-the-Loop-Antwort fürs Dashboard |
| `/health` | GET | Billiger Readiness-Check |
| `/ask` | POST | Einzelner Prompt → vollständige Antwort (nicht-streamend) |
| `/ask/stream` | POST | Wie `/ask`, aber als SSE-Token-Stream |

`/health`:

```json
{ "status": "ready", "model_router_ready": true }
```

`/ask` — Request:

```json
{ "prompt": "...", "system": "optional", "model": "optional", "timeout_ms": 60000 }
```

`/ask` — Response (`200`):

```json
{ "text": "...", "provider": "loomai", "stop_reason": "stop", "usage": { "prompt_tokens": 37, "completion_tokens": 3 } }
```

`/ask` — Fehler (`4xx`/`5xx`):

```json
{ "error": { "message": "ollama: connection refused" } }
```

`/ask/stream` — SSE, ein JSON-Objekt pro `data:`-Zeile, abgeschlossen mit
`data: [DONE]`. Ein Fehler mitten im Stream wird als reguläres SSE-Event
gesendet (`data: {"error":{"message":"..."}}`), nicht als abgebrochene
Verbindung:

```
data: {"delta": "Hello"}

data: {"delta": ", world"}

data: [DONE]
```

Details und die Motivation für diesen Vertrag (was `ai.nvim` als
Provider-Kontrakt voraussetzt) stehen in
`nvim/docs/ROADMAP/reports/loomai-ai-nvim-integration.md`.

## Stand / was fehlt

- Kein `ModelRouter` — `/ask*` ruft aktuell direkt Ollama auf, `model` wird
  ungeprüft durchgereicht (siehe `docs/Guides/
  ki-agenten-framework-architektur.md`, Abschnitt 11).
- Kein Anthropic-Client (Cloud-Fallback).
- Kein Auth/API-Key nötig, solange der Server nur auf `127.0.0.1` hört.
