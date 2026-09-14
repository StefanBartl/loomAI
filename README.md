# loomAI

Local agent-framework server (C++17, `cpp-httplib`). Serves as the backend
for a live dashboard (SSE) and as an ask/stream endpoint for editor
integrations (see [`ai.nvim`](../ai.nvim)'s `loomai` provider, there
`lua/ai/providers/loomai.lua`).

*[Deutsche Version](README.de.md)*

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

- No auth/API key needed for the server itself, as long as it only
  listens on `127.0.0.1` (the three cloud *backends* obviously need their
  own keys, see above).
- `ollama_client.cpp` opens a new TCP connection to Ollama per request
  instead of reusing one -- known, deliberately deferred (would need a
  proper pooling design), see the `ai.nvim` handover.
- The model-name-prefix list (`gpt-`/`o1-`/`o3-`/`claude-`/`gemini-`) is
  hardcoded in `main.cpp`'s router, no configuration option for
  custom/future prefixes -- no need for that so far, since all three
  providers prefix their model names stably this way.
