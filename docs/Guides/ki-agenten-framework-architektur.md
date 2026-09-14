# KI-Agenten-Framework — Architekturplan

> **Ziel:** Ein selbst-konfigurierendes, multi-agentes KI-System mit lokalen & Cloud-Modellen,
> containerisierter Sandbox, Live-Dashboard und menschlicher Entscheidungskontrolle.

---

## Inhaltsverzeichnis

1. [Systembild & Philosophie](#1-systembild--philosophie)
2. [Tech-Stack (alle Stages)](#2-tech-stack-alle-stages)
3. [Architektur-Schichten](#3-architektur-schichten)
4. [Komponenten-Detail](#4-komponenten-detail)
5. [Hardware-Setup (RTX 4070 Super)](#5-hardware-setup-rtx-4070-super)
6. [Agent-Sandbox (Podman)](#6-agent-sandbox-podman)
7. [Live-Dashboard (HTML)](#7-live-dashboard-html)
8. [Human-in-the-Loop (Decision Control)](#8-human-in-the-loop-decision-control)
9. [State-Management & Time-Travel](#9-state-management--time-travel)
10. [Erster Test-Task: Self-Setup](#10-erster-test-task-self-setup)
11. [Implementierungs-Roadmap](#11-implementierungs-roadmap)
12. [Dateistruktur](#12-dateistruktur)

---

## 1. Systembild & Philosophie

```
User (Browser) ──── Dashboard (HTML/SSE) ──── Orchestrator (C++ / Zig)
                                                      │
                         ┌────────────────────────────┼──────────────────────┐
                         │                            │                      │
                    Agent Pool                  Model Router            Sandbox Mgr
                  (Lua scripts /             (lokal: ollama /          (Podman API)
                   C++ workers)              cloud: Anthropic)
```

**Kernprinzipien:**
- **Null Globals** — jeder Agent hat isolierten State, kein shared mutable state im Hauptprozess
- **Fail-Safe-First** — jede Sandbox-Operation ist rückrollbar (Git-Snapshots)
- **Observable by Default** — jede Aktion emittiert ein Event an den Dashboard-Bus
- **Human Checkpoint** — kritische Entscheidungen blockieren oder loggen für späteren Rollback

---

## 2. Tech-Stack (alle Stages)

### Core Orchestrator
| Komponente | Technologie | Begründung |
|---|---|---|
| **Orchestrator** | **C++23** (oder Zig 0.13) | Performance, direkte CUDA-Bindung, kein GC |
| **Build System** | CMake 3.28+ / zig build | Cross-platform |
| **Async Runtime** | `libuv` (C++) oder Zig `std.event` | Non-blocking I/O, identisch zu Neovim |
| **HTTP Server** | `cpp-httplib` oder `Zig std.http` | SSE für Dashboard, REST für Agenten-API |
| **IPC / Message Bus** | `nanomsg-ng` (nng) | Schnell, zero-copy zwischen Prozessen |
| **Serialisierung** | `nlohmann/json` (C++) | State-Austausch, Dashboard-Events |

### Modell-Integration
| Layer | Tech | Details |
|---|---|---|
| **Lokal (GPU)** | `ollama` + `llama.cpp` | RTX 4070 Super, CUDA 12.x |
| **Lokal (C++ direkt)** | `llama.cpp` als Library | Einbinden in Orchestrator für maximale Kontrolle |
| **Cloud** | Anthropic API (HTTP/SSE) | claude-sonnet-4-6, Fallback wenn lokal überlastet |
| **Model Router** | Eigene C++-Klasse | Routing nach Task-Typ, VRAM-Budget, Latenz |

### Agent-Sandbox
| Layer | Tech | Details |
|---|---|---|
| **Container Runtime** | **Podman** (rootless) | Kein Daemon, OCI-kompatibel, sicherer als Docker |
| **Base Image** | `debian:bookworm-slim` | Minimaler Footprint |
| **Snapshot/Rollback** | `podman commit` + `git` | Jeder Decision Point = Commit |
| **Filesystem** | `overlayfs` Layers | Copy-on-Write für schnelle Snapshots |
| **Netzwerk-Isolation** | Podman Network (bridge) | Agenten nur über definierten Proxy ins Netz |

### Dashboard
| Layer | Tech | Details |
|---|---|---|
| **Transport** | Server-Sent Events (SSE) | Einseitig, kein WS-Overhead, funktioniert überall |
| **Frontend** | Vanilla HTML + CSS + JS | Zero-Dependencies, schnell, kein Build-Step |
| **Styling** | CSS Custom Properties | Dark-Mode, kein Framework |
| **Decision UI** | Native `<dialog>` Element | Modal für Hard-Stop, inline für weiche Checkpoints |

### Persistenz
| Zweck | Tech |
|---|---|
| Agent-State / Snapshots | SQLite (eingebettet, `sqlite3.h`) |
| Projektdateien in Sandbox | Git-Repository (über `libgit2`) |
| Logs / Events | Strukturierte JSONL-Dateien |
| VRAM-Cache / Modell-State | `llama.cpp` KV-Cache |

### Testing & Tooling
| Tool | Zweck |
|---|---|
| `Catch2` | C++ Unit-Tests |
| `valgrind` / `sanitizers` | Memory-Safety |
| `perf` + `nvtop` | GPU/CPU-Profiling |
| `podman-compose` | Multi-Container-Setup |

---

## 3. Architektur-Schichten

```
┌─────────────────────────────────────────────────────────────┐
│  LAYER 5: Dashboard / Human Interface                        │
│  HTML + SSE + decision dialogs                               │
├─────────────────────────────────────────────────────────────┤
│  LAYER 4: Orchestrator (C++23)                               │
│  Agent-Lifecycle, Model-Routing, Event-Bus, SSE-Server       │
├──────────────────────────┬──────────────────────────────────┤
│  LAYER 3a: Agent Workers │  LAYER 3b: Model Router           │
│  C++ Worker-Threads +    │  llama.cpp (lokal GPU)            │
│  Lua Scripting (sol2)    │  Anthropic API (Cloud)            │
├──────────────────────────┴──────────────────────────────────┤
│  LAYER 2: Sandbox Manager                                    │
│  Podman API, Snapshot/Restore, Git-Integration               │
├─────────────────────────────────────────────────────────────┤
│  LAYER 1: Hardware                                           │
│  RTX 4070 Super (CUDA 12), CPU, NVMe                        │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Komponenten-Detail

### 4.1 Orchestrator (Kern)

```cpp
// Hauptverantwortlichkeiten
class Orchestrator {
  AgentPool     agents;         // Verwaltung aller laufenden Agenten
  ModelRouter   router;         // lokal vs. cloud Entscheidung
  SandboxMgr    sandbox;        // Podman-Interface
  EventBus      bus;            // nng push/pull socket
  SSEServer     dashboard;      // HTTP-Server für Dashboard
  StateStore    store;          // SQLite für Snapshots
  DecisionQueue decisions;      // Pending Human-Checkpoints
};
```

**Ablauf eines Tasks:**
1. `Orchestrator::submit_task(TaskSpec)` — Task einreihen
2. `AgentPool::spawn(task)` — Worker-Thread + Sandbox-Container starten
3. Agent ruft `ModelRouter::infer(prompt)` — lokal oder cloud
4. Agent emittiert Events → `EventBus` → `SSEServer` → Browser
5. Bei Decision Point: `DecisionQueue::push(dp)` — Hard Stop oder Snapshot
6. Agent-Ergebnis → `SandboxMgr::commit_snapshot()` — Git-Commit in Container

### 4.2 Model Router

```cpp
class ModelRouter {
  struct RoutingPolicy {
    size_t local_vram_budget_mb = 6000;  // RTX 4070 Super: ~12GB VRAM
    TaskType cloud_threshold;            // z.B. komplexe Code-Analyse → cloud
    bool prefer_local = true;
  };

  InferResult route(const Prompt& p, const RoutingPolicy& policy);
  // → llama.cpp wenn VRAM frei & Task passt
  // → Anthropic API sonst
};
```

**Modell-Empfehlungen für lokalen Betrieb (RTX 4070 Super, 12 GB VRAM):**

| Modell | VRAM | Stärke | Use Case |
|---|---|---|---|
| `qwen2.5-coder:14b-q4` | ~9 GB | Code-Generation | Primärer Code-Agent |
| `llama3.1:8b-q5` | ~6 GB | Reasoning | Planer-Agent |
| `deepseek-r1:8b` | ~6 GB | Reasoning/Analysis | Analyse-Agent |
| `nomic-embed-text` | ~1 GB | Embeddings | Kontext-Suche |

→ Cloud (Anthropic claude-sonnet-4-6): Architektur-Entscheidungen, komplexe Reviews

### 4.3 Agent-Worker

Jeder Agent ist ein C++-Worker-Thread mit:
- Eigenem `Prompt-History-Buffer` (keine shared history)
- `Lua`-Scripting-Layer via `sol2` für Agent-Verhalten (analog zu Neovim-Architektur!)
- Klar definiertem `ToolSet` (Funktionen die der Agent aufrufen darf)

```lua
-- Agent-Verhalten als Lua-Script (analog zu Neovim-Config!)
---@module 'agents.code_agent'
local M = {}

M.tools = {
  write_file = true,
  run_command = true,
  read_file = true,
  git_commit = true,
}

function M.on_decision(context)
  -- Entscheidungslogik in Lua definierbar
  if context.risk_level > 0.8 then
    return "HUMAN_REQUIRED"
  end
  return "PROCEED"
end

return M
```

---

## 5. Hardware-Setup (RTX 4070 Super)

### CUDA / Ollama Setup

```bash
# CUDA 12.x Toolkit
# Ollama mit CUDA-Unterstützung
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen2.5-coder:14b-q4_K_M
ollama pull llama3.1:8b-q5_K_M

# llama.cpp direkt (für C++-Integration)
git clone https://github.com/ggerganov/llama.cpp
cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89  # Ada Lovelace = arch 89
cmake --build build --config Release -j$(nproc)
```

### VRAM-Budget-Strategie

```
RTX 4070 Super: 12 GB VRAM
├── Aktives Modell: max 9 GB  (qwen2.5-coder:14b-q4)
├── KV-Cache:       ~1.5 GB
└── Reserve:        1.5 GB   (für GPU-Tasks der Agenten)
```

- **VRAM-Guard** im Model Router: vor jedem Laden VRAM prüfen (`nvmlDeviceGetMemoryInfo`)
- Bei Engpass: kleineres Quantisierungs-Level oder Cloud-Fallback

---

## 6. Agent-Sandbox (Podman)

### Warum Podman (nicht Docker)?

- **Rootless**: Agenten laufen ohne Root-Rechte → kein Privilege-Escalation-Risiko
- **Kein Daemon**: Podman ist ein CLI-Tool, kein Background-Service
- **OCI-kompatibel**: Alle Docker-Images funktionieren
- **Systemd-Integration**: Einfaches Service-Management

### Container-Setup

```dockerfile
# base-agent.Containerfile
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    git curl python3 nodejs npm \
    build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

# Git-Konfiguration für Snapshots
RUN git config --global user.email "agent@local" \
 && git config --global user.name "AI Agent"

WORKDIR /workspace
RUN git init && git commit --allow-empty -m "initial"
```

```cpp
// Sandbox Manager
class SandboxMgr {
  // Container starten
  ContainerID spawn(const AgentSpec& spec);

  // Datei in Container schreiben
  bool write_file(ContainerID, const Path&, const Content&);

  // Befehl in Container ausführen (mit Timeout!)
  ExecResult exec(ContainerID, const Command&, Duration timeout);

  // Git-Snapshot bei Decision Point
  SnapshotID commit_snapshot(ContainerID, const string& message);

  // Rollback zu einem früheren Snapshot
  bool restore_snapshot(ContainerID, SnapshotID);

  // Container sauber beenden
  void teardown(ContainerID);
};
```

### Sicherheits-Regeln für Agenten

| Erlaubt | Verboten |
|---|---|
| Dateien in `/workspace` schreiben | Host-Filesystem lesen |
| Pakete installieren (via apt/npm) | Netzwerkzugriff außer Proxy |
| Git-Commits innerhalb Container | Port < 1024 öffnen |
| definierte Tool-Calls | `sudo`, `su`, Container-Escape |

---

## 7. Live-Dashboard (HTML)

### Transport: Server-Sent Events (SSE)

```cpp
// SSE-Event-Format (JSONL über text/event-stream)
struct DashboardEvent {
  string event_type;  // "agent_status" | "decision_point" | "log" | "snapshot"
  string agent_id;
  string payload;     // JSON
  uint64_t timestamp;
};

// C++ SSE-Server (cpp-httplib)
svr.Get("/events", [](const Request&, Response& res) {
  res.set_chunked_content_provider("text/event-stream",
    [&](size_t, DataSink& sink) {
      auto event = bus.next_event();
      sink.write("data: " + event.to_json() + "\n\n");
      return true;
    });
});
```

### Dashboard-Layout

```
┌─────────────────────────────────────────────────┐
│  KI-Framework Dashboard              [⏸ Pause]  │
├──────────────┬──────────────┬───────────────────┤
│ Agent: Planer│ Agent: Coder │ Agent: Tester     │
│ ● Aktiv      │ ● Aktiv      │ ○ Wartend         │
│ "Analysiere  │ "Schreibe    │                   │
│  Anforderung"│  Dockerfile" │                   │
│ [████░░] 60% │ [██░░░░] 30% │                   │
├──────────────┴──────────────┴───────────────────┤
│ Decision Points                                  │
│ ● [12:34] Coder: "PostgreSQL oder SQLite?" → KI │
│   wählte PostgreSQL  [↩ Rollback]               │
│                                                  │
│ 🔴 WARTEN: "Docker-Befehl ausführen?"           │
│    > docker run --rm postgres:16                 │
│    [✓ Erlauben]  [✗ Ablehnen]  [Bearbeiten]     │
├─────────────────────────────────────────────────┤
│ Live-Log                                         │
│ 12:34:01 [Planer]  Task aufgeteilt in 3 Subtasks│
│ 12:34:05 [Coder]   Dockerfile generiert         │
│ 12:34:08 [Sandbox] git commit "Decision Point 1"│
└─────────────────────────────────────────────────┘
```

---

## 8. Human-in-the-Loop (Decision Control)

### Modus A: Hard Stop (synchron, blockierend)

Für hochriskante Operationen (Netzwerkzugriff, System-Befehle, irreversible Aktionen):

```cpp
// Agent signalisiert Decision Point
DecisionResult Orchestrator::request_decision(
  const DecisionPoint& dp,
  DecisionMode mode = HARD_STOP
) {
  if (mode == HARD_STOP) {
    agent.pause();                    // Agent-Thread schlafen legen
    dashboard.emit_blocking(dp);      // Modal im Browser
    auto result = decision_queue.wait(dp.id, timeout);  // Blockiert
    agent.resume(result);
    return result;
  }
  // ...
}
```

**Browser-Seite:** Natives `<dialog>` Element, verhindert alle anderen Interaktionen.

### Modus B: Async Checkpoint (mit Rollback-Option)

Für weniger kritische Entscheidungen — KI entscheidet, du kannst später revidieren:

```cpp
// Snapshot BEVOR die Entscheidung ausgeführt wird
SnapshotID snap = sandbox.commit_snapshot(container,
  "Decision Point: " + dp.description);

// KI-Entscheidung ausführen
agent.execute(dp.ai_choice);

// Event ans Dashboard (mit Rollback-Option)
dashboard.emit({
  .type = "soft_decision",
  .snapshot_id = snap,
  .description = dp.description,
  .ai_choice = dp.ai_choice,
  .rollback_available = true
});
```

**Browser-Seite:** Chronologische Liste aller Entscheidungen mit [Rollback]-Button.

---

## 9. State-Management & Time-Travel

### Snapshot-Strategie

Jeder Decision Point erzeugt einen vollständigen Snapshot:

```
Timeline:
  T0: [Init]          git: "initial commit"
  T1: [DP-1] Planer   git: "Decision: Use PostgreSQL"
  T2: [DP-2] Coder    git: "Decision: Use Docker Compose"
  T3: [DP-3] Coder    git: "Decision: Port 5432 extern"
         ↑
         Rollback hier → T2 löschen, neu starten mit "Port intern"
```

### Rollback-Mechanismus

```cpp
bool Orchestrator::rollback_to(SnapshotID target) {
  // 1. Alle Agenten nach diesem Snapshot pausieren
  agent_pool.pause_after(target.timestamp);

  // 2. Container-Filesystem zurücksetzen
  sandbox.restore_snapshot(container, target);

  // 3. Agent-History trimmen (Nachrichten nach Snapshot löschen)
  for (auto& agent : agents) {
    agent.trim_history(target.timestamp);
  }

  // 4. Agenten mit neuer Direktive neu starten
  agent_pool.resume_from(target, new_directive);

  return true;
}
```

---

## 10. Erster Test-Task: Self-Setup

**Ziel:** Die Agenten bauen sich selbst ihre Arbeitsumgebung.

### Task-Spezifikation

```json
{
  "task": "setup_dev_environment",
  "agents": ["planner", "executor"],
  "objective": "Erstelle eine vollständige Entwicklungsumgebung für ein Node.js-Projekt",
  "constraints": {
    "use_containers": true,
    "decision_checkpoints": ["package_manager", "database", "port_assignment"]
  }
}
```

### Ablauf (mit Dashboard-Events)

```
[Planer-Agent]  → Analysiert Task
                → Event: "Teile Task in 3 Phasen"
                → Decision Point (soft): "npm oder pnpm?"
                  → KI wählt pnpm → Snapshot erstellt

[Executor-Agent] → Schreibt Containerfile
                 → Event: "Containerfile generiert"
                 → HARD STOP: "Darf ich 'podman build' ausführen?"
                   → [Erlauben] geklickt
                 → Event: "Build läuft..."
                 → Event: "Build erfolgreich"
                 → Snapshot: "DP-2: Container gebaut"
```

---

## 11. Implementierungs-Roadmap

### Phase 0: Foundation (Woche 1-2)

- [x] C++-Projekt-Skeleton (CMake; Catch2/clang-format noch offen) — `CMakeLists.txt`
- [ ] SQLite-Integration für State-Storage
- [x] Einfacher SSE-Server (cpp-httplib) → `GET /events` gibt Test-Events aus
- [x] Basis-Dashboard HTML-Seite empfängt und zeigt Events

### Phase 1: Model Integration (Woche 3-4)

- [x] Ollama REST-Client in C++ (HTTP GET/POST via cpp-httplib) — `src/ollama_client.hpp/.cpp`
- [x] Anthropic API Client (streaming SSE-Antworten) — `src/anthropic_client.hpp/.cpp`
      (2026-09-14). Bonus, über den ursprünglichen Punkt hinaus: auch
      `src/openai_client.hpp/.cpp` und `src/gemini_client.hpp/.cpp`, aus
      demselben `ai.nvim`-Integrationsanlass (siehe README "ModelRouter").
- [ ] Model Router mit VRAM-Heuristik/Task-Typ-Routing (4.2 oben) — **nicht**
      das, was jetzt existiert. Umgesetzt (2026-09-14) ist ein simpler
      Modellname-Präfix-Router in `main.cpp` (`gpt-`/`o1-`/`o3-`→OpenAI,
      `claude-`→Anthropic, `gemini-`→Gemini, sonst→Ollama) — der Aufrufer
      wählt das Modell explizit, keine automatische VRAM-/Task-Entscheidung.
      Reicht für `ai.nvim`s Anwendungsfall (Nutzer wählt `provider`/`model`
      selbst), deckt aber nicht die hier ursprünglich skizzierte
      RoutingPolicy ab. Die dafür nötige VRAM-Erkennung/Task-Klassifikation
      bleibt offen.
- [x] Erster "Hello World" Agent: sendet Prompt, gibt Antwort zurück — `POST /ask`
      (siehe [README.md](../../README.md) und `nvim/docs/ROADMAP/reports/
      loomai-ai-nvim-integration.md`, Aufgabe B/C für den HTTP-Vertrag,
      2026-09-14)

### Phase 2: Sandbox (Woche 5-6)

- [ ] Podman CLI-Wrapper in C++ (fork/exec, stdout capture)
- [ ] Container spawnen, Dateien schreiben, Befehle ausführen
- [ ] Git-Snapshot-Mechanismus (libgit2 oder CLI-Wrapper)
- [ ] Sicherheits-Regeln durchsetzen (Timeout, Netzwerk-Block)

### Phase 3: Orchestration (Woche 7-8)

- [ ] Agent Worker-Threads mit eigenem State
- [ ] Lua-Scripting Layer (sol2) für Agent-Verhalten
- [ ] EventBus (nng) zwischen Orchestrator und Agenten
- [ ] Decision Queue: Hard Stop + Soft Checkpoint

### Phase 4: Dashboard (Woche 9-10)

- [ ] Dashboard HTML: Agent-Status-Cards, Live-Log
- [ ] Decision-Point-UI: Modal für Hard Stop, Chronologie für Soft
- [ ] Rollback-UI: Liste der Snapshots mit Rollback-Button
- [ ] VRAM/CPU/Status Monitoring-Panel

### Phase 5: Self-Setup Task (Woche 11-12)

- [ ] Erster echter Multi-Agenten-Task
- [ ] Agenten konfigurieren ihre eigene Sandbox
- [ ] Vollständige Dashboard-Integration und End-to-End-Test

---

## 12. Dateistruktur

```
ki-framework/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp                    # Entry Point
│   ├── orchestrator/
│   │   ├── orchestrator.hpp/.cpp   # Hauptkoordinator
│   │   ├── agent_pool.hpp/.cpp     # Agent Worker-Threads
│   │   ├── decision_queue.hpp/.cpp # Hard Stop / Soft Checkpoint
│   │   └── event_bus.hpp/.cpp      # nng Message Bus
│   ├── models/
│   │   ├── model_router.hpp/.cpp   # lokal vs. cloud Routing
│   │   ├── ollama_client.hpp/.cpp  # Ollama REST-Client
│   │   └── anthropic_client.hpp/.cpp # Anthropic API-Client
│   ├── sandbox/
│   │   ├── sandbox_mgr.hpp/.cpp    # Podman-Interface
│   │   ├── snapshot.hpp/.cpp       # Git-Snapshot-Verwaltung
│   │   └── security.hpp/.cpp       # Sandbox-Sicherheitsregeln
│   ├── dashboard/
│   │   ├── sse_server.hpp/.cpp     # HTTP + SSE-Server
│   │   └── event_serializer.hpp    # JSON-Event-Format
│   └── state/
│       ├── state_store.hpp/.cpp    # SQLite-Persistenz
│       └── types.hpp               # Gemeinsame Datentypen
├── agents/                         # Lua-Agent-Scripts
│   ├── planner.lua
│   ├── coder.lua
│   └── tester.lua
├── dashboard/                      # Frontend
│   ├── index.html
│   ├── style.css
│   └── dashboard.js
├── containers/                     # Podman/Container-Definitionen
│   ├── base-agent.Containerfile
│   └── dev-env.Containerfile
├── tests/
│   ├── test_orchestrator.cpp
│   ├── test_model_router.cpp
│   └── test_sandbox.cpp
└── scripts/
    ├── setup.sh                    # Ersteinrichtung (CUDA, Ollama, Podman)
    └── dev.sh                      # Entwicklungsumgebung starten
```

---

## Literatur und Referenzen

- **llama.cpp** — https://github.com/ggerganov/llama.cpp (lokale LLM-Inferenz, C++)
- **Ollama** — https://ollama.ai (LLM-Server für lokale Modelle)
- **sol2** — https://github.com/ThePhD/sol2 (Lua-C++-Bindung)
- **cpp-httplib** — https://github.com/yhirose/cpp-httplib (HTTP-Server/-Client)
- **nanomsg-ng (nng)** — https://nng.nanomsg.org (Message Bus)
- **libgit2** — https://libgit2.org (Git-Integration)
- **Podman** — https://podman.io (Rootless Container)
- **Catch2** — https://github.com/catchorg/Catch2 (C++ Testing)
- **Anthropic API Docs** — https://docs.anthropic.com
- **NVIDIA Management Library (NVML)** — für VRAM-Monitoring
- **CUDA 12 Programming Guide** — https://docs.nvidia.com/cuda/
- Henney, K. (2010): *97 Things Every Programmer Should Know*
- Meyers, S. (2014): *Effective Modern C++*
