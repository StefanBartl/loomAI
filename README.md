# loomAI

Local agent-framework server (C++17, `cpp-httplib`). Serves as the backend
for a live dashboard (SSE) and as an ask/stream endpoint for editor
integrations (see [`ai.nvim`](../ai.nvim)'s `loomai` provider, there
`lua/ai/providers/loomai.lua`).

*[Deutsche Version](README.de.md)*

## Get loomAI

[**Download the latest release**](https://github.com/StefanBartl/loomAI/releases/latest)
-- `loomai-setup-<version>-x64.exe`, a Windows installer, no compiler or MSYS2
needed. It installs for the current user (no administrator rights), adds a
Start-menu entry, and can optionally put the `loomai` command on your `PATH`.
Start it from the Start menu or with `loomai`; it serves the dashboard on
`http://127.0.0.1:8080/`. Uninstall via *Settings -> Apps*.

The installer is not code-signed, so Windows SmartScreen may warn ("More info",
then "Run anyway"). Each release also carries a `SHA256SUMS` file (v0.1.0: a
`.sha256` file) to check the download against `Get-FileHash`. Built by
[`.github/workflows/release.yml`](.github/workflows/release.yml) from a pushed
`vX.Y.Z` tag, and verified there by installing, starting and uninstalling it
before it is offered. On Linux/macOS, or to build it yourself, use the scripts
below.

**Or install it with a script** -- for the current user, no administrator
rights, and nothing for SmartScreen to stop: a file the script downloads
carries no "from the internet" mark, so the script checks it against
`SHA256SUMS` instead and runs nothing that does not match (that proves the
file is whole and the release's own, not who made the release). In your own
PowerShell, not elevated:

```powershell
# the newest state of main (the pre-release "latest")
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1 | iex"

# a release: the same with the tag in place of latest, e.g. .../releases/download/v0.1.1/install.ps1
```

The script of a release installs exactly that release, the script of `latest`
the rolling build ([`rolling.yml`](.github/workflows/rolling.yml), rebuilt
after every green CI run on `main` and proven by installing, starting,
updating and uninstalling it, for trying what is new). The line above installs
without the "add to PATH" step, so the `loomai` command is not in new
terminals; start loomAI from the Start menu. For the step, or any other
argument of the script, use the form that takes arguments:

```powershell
& ([scriptblock]::Create((irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1))) -InstallArgs '/TASKS=addtopath'
```

Saved as a file, the script is run as `powershell -ExecutionPolicy Bypass -File
.\install.ps1 -InstallArgs '/TASKS=addtopath'` (one value per `-File` command
line: a list would arrive as one string; in a PowerShell session
`-InstallArgs 'a','b'` is fine). `-File <setup.exe>` installs a setup you
already have; its checksum then comes from `-Sums`, `-Sha256` or a
`SHA256SUMS` beside it. Run the script again to repair or update (stop loomAI
first if it is running). The scripts are the ones of appkit's
[release-kit](https://github.com/StefanBartl/appkit/tree/main/release-kit),
stamped for this app and the channel when they are attached. (v0.1.0 predates
them and has no script.)

## Installing from source

The setup scripts do everything below in one go: check the prerequisites,
fetch the two header-only dependencies (pinned versions, SHA-256 verified),
build, install, and smoke-test the installed copy against `GET /health`.

Windows (MSYS2/MinGW-w64; CMake >= 3.20 must be installed already):

```powershell
.\scripts\setup.ps1 -InstallDeps -AddToPath
```

`-InstallDeps` pulls gcc, make and OpenSSL via MSYS2's `pacman`, `-AddToPath`
adds the install directory to your user `PATH`. Further switches: `-Prefix`
(default `%LOCALAPPDATA%\loomai`), `-BuildType`, `-Clean`, `-CheckOnly`,
`-SkipInstall`, `-SkipSmokeTest`, `-Uninstall`. Run
`Get-Help .\scripts\setup.ps1 -Detailed` for details.

Linux/macOS (CMake >= 3.20, a C++17 compiler, curl, OpenSSL dev files):

```bash
scripts/setup.sh
```

Options: `--prefix`, `--bin-dir`, `--build-type`, `--clean`, `--check`,
`--skip-install`, `--skip-smoke-test`, `--uninstall` (see `--help`).

The server serves `./dashboard` relative to its working directory, so the
installed layout is `<prefix>/app` (binary, dashboard, on Windows the runtime
DLLs) plus a launcher -- `<prefix>\loomai.cmd` on Windows, `~/.local/bin/loomai`
elsewhere -- that starts it from the right directory. Start loomAI with the
launcher, not with `app/loomai` directly. The scripts never write API keys;
set them in your environment (see "Running").

## Building

Dependencies are header-only and vendored locally into `third_party/` (not
checked into git, see `.gitignore`):

```bash
mkdir -p third_party/nlohmann
curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
    -o third_party/nlohmann/json.hpp
curl -sL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h \
    -o third_party/httplib.h
```

The ModelRouter additionally needs OpenSSL (headers + lib) for the HTTPS
calls to OpenAI/Anthropic/Gemini (`ollama_client` alone wouldn't need it,
but all four backends link into one binary). On MSYS2/mingw64:
`pacman -S mingw-w64-x86_64-openssl` (brings headers plus
`libssl`/`libcrypto`); on Debian/Ubuntu `apt install libssl-dev`.

Then build with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

On Windows with MinGW/g++, pass the generator explicitly if CMake doesn't
pick the right one automatically -- and set `OPENSSL_ROOT_DIR` if CMake
doesn't find the MSYS2 OpenSSL on its own (a separately installed CMake
doesn't search the mingw64 tree by default):

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug \
    -DOPENSSL_ROOT_DIR=C:/msys64/mingw64
cmake --build build -j4
```

## Running

```bash
./build/loomai      # build/loomai.exe on Windows
```

The server binds `127.0.0.1:8080` (deliberately not `0.0.0.0` -- an editor
companion process shouldn't be accidentally reachable on the LAN).

`/ask`/`/ask/stream` route by model-name prefix to one of four backends
(see "ModelRouter" below) -- with no matching prefix (or no `model` at
all), the request goes to local Ollama:

For the Ollama fallback, a local [Ollama](https://ollama.com) instance
must be running (default: `http://127.0.0.1:11434`). A different host can
be set via `LOOMAI_OLLAMA_HOST` (**not** `OLLAMA_HOST` -- that's already
taken by Ollama itself as the Ollama *server's* own bind address, e.g.
`0.0.0.0:11434`, and thus not a valid client target). Default model via
`LOOMAI_DEFAULT_MODEL`, otherwise `llama3.1:8b-q5_K_M`.

Each of the three cloud backends needs its own API key as an env var,
otherwise `/ask*` reports a clear configuration error instead of
attempting a request:

| Backend | Model-name prefix | Env var |
|---|---|---|
| OpenAI | `gpt-`, `o1-`, `o3-` | `OPENAI_API_KEY` |
| Anthropic | `claude-` | `ANTHROPIC_API_KEY` |
| Gemini | `gemini-` | `GEMINI_API_KEY` |
| Ollama (local) | anything else / no `model` | -- (see above) |

Each backend's prefix list can be overridden wholesale (comma-separated)
via `LOOMAI_PREFIXES_OPENAI`/`LOOMAI_PREFIXES_CLAUDE`/`LOOMAI_PREFIXES_GEMINI`
-- e.g. `LOOMAI_PREFIXES_OPENAI=gpt-,o1-,o3-,o4-` to pick up a new OpenAI
model family without a rebuild. Read once at startup.

`GET /health` shows which backends are currently configured (presence
check only, never the key value itself).

## HTTP API

| Endpoint | Method | Purpose |
|---|---|---|
| `/events` | GET (SSE) | Dashboard events (agent status, decision points) |
| `/decision` | POST | Human-in-the-loop answer for the dashboard |
| `/health` | GET | Cheap readiness check |
| `/ask` | POST | A single prompt → full answer (non-streaming) |
| `/ask/stream` | POST | Same as `/ask`, but as an SSE token stream |

`/health`:

```json
{
  "status": "ready",
  "model_router_ready": true,
  "backends": { "ollama": true, "openai": true, "anthropic": false, "gemini": false }
}
```

`/ask` -- request:

```json
{ "prompt": "...", "system": "optional", "model": "optional", "timeout_ms": 60000 }
```

`timeout_ms` defaults to 60000 for the three cloud backends and 120000 for
Ollama (a local model can take several seconds just to load on a cold
first request) -- only when the request omits it.

`/ask` -- response (`200`):

```json
{ "text": "...", "provider": "loomai", "stop_reason": "stop", "usage": { "prompt_tokens": 37, "completion_tokens": 3 } }
```

`/ask` -- error (`4xx`/`5xx`):

```json
{ "error": { "message": "ollama: connection refused" } }
```

`/ask/stream` -- SSE, one JSON object per `data:` line, terminated by
`data: [DONE]`. An error mid-stream is sent as a regular SSE event
(`data: {"error":{"message":"..."}}`), not as an aborted connection:

```
data: {"delta": "Hello"}

data: {"delta": ", world"}

data: [DONE]
```

The motivation for this contract (what `ai.nvim` requires from a provider)
lives in `ai.nvim`'s `lua/ai/providers/loomai.lua` (the module doc comment
at the top of the file).

## ModelRouter

`/ask`/`/ask/stream` dispatch by model-name prefix to one of four backend
clients (`src/{ollama,openai,anthropic,gemini}_client.{hpp,cpp}`), see the
table above. Deliberately prefix-based routing instead of an explicit
`provider` field in the request: the HTTP contract stays unchanged (no
adjustment needed in `ai.nvim`'s `loomai.lua`). Every cloud client
defensively handles an error mid-stream (see `model_client.hpp`'s
`error_text`/`recover_error_body`) -- an auth error response from
OpenAI/Anthropic/Gemini sometimes comes back as a raw, non-SSE-shaped JSON
body with no trailing newline instead of an SSE event; verified live
against all three real APIs (including a deliberately invalid test key to
trigger the error path), not just assumed.

The Gemini client additionally recognizes a safety/policy block returned
by Google (`promptFeedback.blockReason` with no `candidates`, a normal
`200` response with no `error` field) and reports it as an error
(`gemini: prompt blocked (SAFETY)` or similar) instead of an empty
success. `model` is also restricted to a safe character set
(`[A-Za-z0-9._-]`) for the Gemini client before it lands in the request
URL -- unlike OpenAI/Anthropic, which send `model` safely inside the JSON
body, Gemini builds the path directly from the model name
(`/v1beta/models/<model>:generateContent`).

## Status / what's missing

- No auth/API key needed for the server itself. `127.0.0.1`-only binding
  alone doesn't stop a web page open in the same browser from firing
  requests at it though, so every request is additionally checked against
  its `Origin` header: a request whose `Origin` names a foreign page is
  rejected with `403` before it reaches any route handler (a browser
  cannot forge or omit that header for a genuinely cross-origin request).
  curl-based clients (`ai.nvim`'s `loomai.lua`, plain `curl`) send no
  `Origin` header at all and are unaffected; the three cloud *backends*
  obviously still need their own keys, see above.
- `ollama_client.cpp` opens a new TCP connection to Ollama per request
  instead of reusing one -- known, deliberately deferred (would need a
  proper pooling design), see the `ai.nvim` handover.
- The model-name-prefix list (`gpt-`/`o1-`/`o3-`/`claude-`/`gemini-`) is
  hardcoded in `main.cpp`'s router, no configuration option for
  custom/future prefixes -- no need for that so far, since all three
  providers prefix their model names stably this way.
