#pragma once

#include <functional>
#include <string>

#include "nlohmann/json.hpp"

// Persistent settings (config.json) for loomAI, shared by the web dashboard and
// the desktop app: the server is the single source of truth, the UI only reads
// and writes through the HTTP API.
//
// Value precedence for every setting is environment variable > config.json >
// built-in default. An environment variable therefore *locks* its setting: the
// API reports it as locked and refuses to change it, instead of accepting a
// write that would have no effect.
//
// The file is written atomically (temp file + rename), a corrupt file is moved
// aside rather than overwritten, and unknown keys are preserved so a newer
// version's file survives a round trip through an older one.
namespace loomai::config {

// Outcome of a mutating call. `http == 0` means success; otherwise `http` is the
// status the API should answer with and `message` names the problem.
struct Status {
    int http = 0;
    std::string message;
    bool ok() const { return http == 0; }
};

// Directory that holds config.json (and the file-based secret store).
// LOOMAI_CONFIG_DIR, else %APPDATA%\loomai (Windows), $XDG_CONFIG_HOME/loomai or
// ~/.config/loomai (Linux), ~/Library/Application Support/loomai (macOS).
std::string directory();

// Effective value of a setting: env > config.json > default. Unknown key -> "".
std::string effective(const std::string& key);

// Every setting as {"<key>": {"value", "source": "env|config|default",
// "locked": bool, "default": "..."}}.
nlohmann::json settings_json();

// Partial update, {"<key>": "<value>" | null}. null resets a key to its default.
// All-or-nothing: either every key validates and the file is written, or nothing
// changes. 400 for an unknown key or an invalid value, 409 for a key that an
// environment variable locks, 500 if the file cannot be written.
Status update_settings(const nlohmann::json& patch);

// Copy of the whole document (settings plus whatever other modules store).
nlohmann::json snapshot();

// Lets another module (the custom model list) edit the document under the same
// lock and the same atomic write. `edit` works on a copy; it is persisted only
// if it returns an ok status.
Status mutate(const std::function<Status(nlohmann::json&)>& edit);

// Drops the in-memory copy so the next call re-reads the file. For tests, and
// after LOOMAI_CONFIG_DIR changed.
void reload();

// Validation helpers, exposed for unit tests.
// Normalises `url` in place (trims, drops a trailing '/') and returns true if it
// is a plain http(s) origin: scheme, host, optional port, nothing else.
bool normalize_http_origin(std::string& url);

}  // namespace loomai::config
