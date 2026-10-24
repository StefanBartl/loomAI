# loomAI

Lokaler Agenten-Framework-Server (C++17, `cpp-httplib`). Dient als Backend für
ein Live-Dashboard (SSE) und als Ask/Stream-Endpoint für Editor-Integrationen
(siehe [`ai.nvim`](../ai.nvim)s `loomai`-Provider,
dort `lua/ai/providers/loomai.lua`).

*[English version](README.md)*

## loomAI herunterladen

[**Neuestes Release herunterladen**](https://github.com/StefanBartl/loomAI/releases/latest)
-- `loomai-setup-<version>-x64.exe`, ein Windows-Installer, ohne Compiler oder
MSYS2. Er installiert für den aktuellen Benutzer (keine Administratorrechte),
legt einen Startmenü-Eintrag an und kann optional den Befehl `loomai` in den
`PATH` aufnehmen. Start über das Startmenü oder mit `loomai`; das Dashboard
läuft dann auf `http://127.0.0.1:8080/`. Deinstallation über *Einstellungen ->
Apps*.

Der Installer ist nicht codesigniert, Windows SmartScreen kann deshalb warnen
("Weitere Informationen", dann "Trotzdem ausführen"). Jedes Release enthält
außerdem eine `SHA256SUMS`-Datei (v0.1.0: eine `.sha256`-Datei), um den
Download mit `Get-FileHash` zu prüfen.
Gebaut von [`.github/workflows/release.yml`](.github/workflows/release.yml)
aus einem gepushten Tag `vX.Y.Z` und dort vor der Freigabe durch Installieren,
Starten und Deinstallieren geprüft. Unter Linux/macOS oder zum Selbstbauen die
Skripte unten verwenden.

**Oder per Skript installieren** -- für den aktuellen Benutzer, ohne
Administratorrechte, und ohne dass SmartScreen etwas aufhält: Eine Datei, die
das Skript selbst lädt, trägt keine „aus dem Internet"-Markierung. Das Skript
prüft sie stattdessen gegen `SHA256SUMS` und führt nichts aus, was nicht passt
(das belegt, dass die Datei vollständig und die des Releases ist, nicht, wer
das Release gemacht hat). In der eigenen PowerShell, nicht erhöht:

```powershell
# der neueste Stand von main (die Vorabversion "latest")
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1 | iex"

# ein Release: dasselbe mit dem Tag statt latest, z. B. .../releases/download/v0.1.1/install.ps1
```

Das Skript eines Releases installiert genau dieses Release, das von `latest`
den Rolling-Build ([`rolling.yml`](.github/workflows/rolling.yml), nach jedem
grünen CI-Lauf auf `main` neu gebaut und durch Installieren, Starten und
Deinstallieren geprüft, zum Ausprobieren des Neuesten). Als Datei gespeichert
nimmt das Skript mehr an: `-InstallArgs '/TASKS=addtopath'` nimmt den Befehl
`loomai` in den `PATH` auf, `-File <setup.exe>` installiert ein vorhandenes
Setup. Die Skripte sind die des
[Release-Kits](https://github.com/StefanBartl/appkit/tree/main/release-kit) von
appkit, beim Anhängen für diese App und den Kanal gestempelt. (v0.1.0 ist
älter und hat kein Skript.)

## Aus dem Quellcode installieren

Die Setup-Skripte erledigen alles Folgende in einem Rutsch: Voraussetzungen
prüfen, die beiden Header-only-Abhängigkeiten holen (feste Versionen,
SHA-256-geprüft), bauen, installieren und die installierte Kopie per
`GET /health` testen.

Windows (MSYS2/MinGW-w64; CMake >= 3.20 muss schon installiert sein):

```powershell
.\scripts\setup.ps1 -InstallDeps -AddToPath
```

`-InstallDeps` installiert gcc, make und OpenSSL über MSYS2s `pacman`,
`-AddToPath` trägt das Installationsverzeichnis in den User-`PATH` ein. Weitere
Schalter: `-Prefix` (Standard `%LOCALAPPDATA%\loomai`), `-BuildType`, `-Clean`,
`-CheckOnly`, `-SkipInstall`, `-SkipSmokeTest`, `-Uninstall`. Details:
`Get-Help .\scripts\setup.ps1 -Detailed`.

Linux/macOS (CMake >= 3.20, C++17-Compiler, curl, OpenSSL-Dev-Dateien):

```bash
scripts/setup.sh
```

Optionen: `--prefix`, `--bin-dir`, `--build-type`, `--clean`, `--check`,
`--skip-install`, `--skip-smoke-test`, `--uninstall` (siehe `--help`).

Der Server liefert `./dashboard` relativ zu seinem Arbeitsverzeichnis aus,
deshalb ist das installierte Layout `<prefix>/app` (Binary, Dashboard, unter
Windows die Runtime-DLLs) plus ein Launcher -- `<prefix>\loomai.cmd` unter
Windows, sonst `~/.local/bin/loomai` --, der ihn aus dem richtigen Verzeichnis
startet. loomAI also über den Launcher starten, nicht `app/loomai` direkt. Die
Skripte schreiben nie API-Keys; die kommen in die Umgebung (siehe "Starten").

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

Zusätzlich braucht der ModelRouter OpenSSL (Header + Lib), für die HTTPS-Calls
gegen OpenAI/Anthropic/Gemini (`ollama_client` allein bräuchte es nicht, aber
alle vier Backends werden in einem Binary gelinkt). Unter MSYS2/mingw64:
`pacman -S mingw-w64-x86_64-openssl` (bringt Header + `libssl`/`libcrypto`
mit); unter Debian/Ubuntu `apt install libssl-dev`.

Danach mit CMake bauen:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

Unter Windows mit MinGW/g++ explizit den Generator angeben, falls CMake nicht
automatisch das Richtige wählt -- und `OPENSSL_ROOT_DIR` setzen, falls CMake
das MSYS2-OpenSSL nicht von selbst findet (ein separat installiertes CMake
sucht standardmäßig nicht im mingw64-Baum):

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug \
    -DOPENSSL_ROOT_DIR=C:/msys64/mingw64
cmake --build build -j4
```

## Starten

```bash
./build/loomai      # bzw. build/loomai.exe unter Windows
```

Der Server bindet auf `127.0.0.1:8080` (bewusst nicht `0.0.0.0` — kein
Editor-Companion-Prozess soll versehentlich im LAN erreichbar sein).

`/ask`/`/ask/stream` routen per Modellname-Präfix an eines von vier Backends
(siehe "ModelRouter" unten) -- ohne passenden Präfix (oder ganz ohne `model`)
geht es an das lokale Ollama:

Für den Ollama-Fallback muss ein lokaler [Ollama](https://ollama.com)
laufen (Standard: `http://127.0.0.1:11434`). Abweichender Host via
`LOOMAI_OLLAMA_HOST` (**nicht** `OLLAMA_HOST` — das ist bereits durch Ollama
selbst als Bind-Adresse des Ollama-*Servers* belegt, z. B. `0.0.0.0:11434`,
und damit kein gültiges Client-Ziel). Standardmodell via
`LOOMAI_DEFAULT_MODEL`, sonst `llama3.1:8b-q5_K_M`.

Für die drei Cloud-Backends je ein API-Key als Env-Var, sonst meldet `/ask*`
einen klaren Konfigurationsfehler statt einen Request zu versuchen:

| Backend | Modellname-Präfix | Env-Var |
|---|---|---|
| OpenAI | `gpt-`, `o1-`, `o3-` | `OPENAI_API_KEY` |
| Anthropic | `claude-` | `ANTHROPIC_API_KEY` |
| Gemini | `gemini-` | `GEMINI_API_KEY` |
| Ollama (lokal) | alles andere / kein `model` | -- (s. o.) |

Die Präfix-Liste jedes Backends lässt sich komplett überschreiben
(kommagetrennt) via `LOOMAI_PREFIXES_OPENAI`/`LOOMAI_PREFIXES_CLAUDE`/
`LOOMAI_PREFIXES_GEMINI` — z. B. `LOOMAI_PREFIXES_OPENAI=gpt-,o1-,o3-,o4-`,
um eine neue OpenAI-Modellfamilie ohne Neubau aufzunehmen. Wird einmal beim
Start gelesen.

`GET /health` zeigt, welche Backends aktuell konfiguriert sind (nur
Presence-Check, nie der Key-Wert selbst).

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
{
  "status": "ready",
  "model_router_ready": true,
  "backends": { "ollama": true, "openai": true, "anthropic": false, "gemini": false }
}
```

`/ask` — Request:

```json
{ "prompt": "...", "system": "optional", "model": "optional", "timeout_ms": 60000 }
```

`timeout_ms` ist standardmäßig 60000 für die drei Cloud-Backends und
120000 für Ollama (ein lokales Modell kann bei einem kalten ersten Request
mehrere Sekunden zum Laden brauchen) — nur wenn der Request es weglässt.

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

Die Motivation für diesen Vertrag (was `ai.nvim` als Provider-Kontrakt
voraussetzt) steht in `ai.nvim`s `lua/ai/providers/loomai.lua`
(Modul-Doku-Kommentar am Dateikopf).

## ModelRouter

`/ask`/`/ask/stream` dispatchen per Modellname-Präfix an eines von vier
Backend-Clients (`src/{ollama,openai,anthropic,gemini}_client.{hpp,cpp}`),
siehe Tabelle oben. Bewusst Präfix-Routing statt eines expliziten
`provider`-Felds im Request: der HTTP-Vertrag bleibt dadurch unverändert
(kein Anpassungsbedarf in `ai.nvim`s `loomai.lua`). Jeder Cloud-Client
behandelt einen Fehler mitten im Stream defensiv (siehe `model_client.hpp`s
`error_text`/`recover_error_body`) -- eine Auth-Fehlerantwort kommt bei
OpenAI/Anthropic/Gemini teils als rohe, nicht-SSE-förmige JSON ohne
abschließenden Zeilenumbruch zurück statt als SSE-Event; live gegen alle
drei echten APIs verifiziert (inkl. eines bewusst ungültigen Test-Keys, um
den Fehlerpfad auszulösen), nicht nur angenommen.

Der Gemini-Client erkennt zusätzlich einen von Google zurückgegebenen
Safety-/Policy-Block (`promptFeedback.blockReason` ohne `candidates`, eine
normale `200`-Antwort ohne `error`-Feld) und meldet ihn als Fehler
(`gemini: prompt blocked (SAFETY)` o. ä.) statt einer leeren
Erfolgsantwort. `model` wird beim Gemini-Client außerdem auf ein sicheres
Zeichen-Set (`[A-Za-z0-9._-]`) beschränkt, bevor es in der Request-URL
landet -- anders als OpenAI/Anthropic, die `model` sicher im JSON-Body
verschicken, baut Gemini den Pfad direkt aus dem Modellnamen
(`/v1beta/models/<model>:generateContent`).

## Stand / was fehlt

- Kein Auth/API-Key für den Server selbst nötig. Das reine Binden auf
  `127.0.0.1` verhindert aber nicht, dass eine im selben Browser offene
  Webseite Requests dagegen schickt — deshalb wird zusätzlich jeder
  Request gegen seinen `Origin`-Header geprüft: ein Request mit fremdem
  `Origin` wird mit `403` abgelehnt, bevor er einen Route-Handler erreicht
  (diesen Header kann eine Webseite bei einem echten Cross-Origin-Request
  weder fälschen noch weglassen). curl-basierte Clients (`ai.nvim`s
  `loomai.lua`, reines `curl`) schicken gar keinen `Origin`-Header und sind
  davon nicht betroffen; die drei Cloud-*Backends* brauchen natürlich
  weiterhin ihre eigenen Keys, s. o.
- `ollama_client.cpp` öffnet pro Request eine neue TCP-Verbindung zu Ollama
  statt sie wiederzuverwenden -- bekannt, bewusst zurückgestellt (bräuchte
  ein durchdachtes Pooling-Design), siehe `ai.nvim`-Handover.
- Modellname-Präfix-Liste (`gpt-`/`o1-`/`o3-`/`claude-`/`gemini-`) ist
  hartkodiert in `main.cpp`s Router, keine Konfigurationsmöglichkeit für
  eigene/zukünftige Präfixe -- bislang kein Bedarf, da alle drei Anbieter
  ihre Modellnamen stabil so präfixieren.
