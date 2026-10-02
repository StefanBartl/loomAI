#pragma once

#include <string>

#include "config.hpp"
#include "nlohmann/json.hpp"

// API keys for the cloud backends, kept out of config.json and out of every HTTP
// response. Lookup order is environment variable first (the established way,
// and what CI uses), then the secret store the settings dialog writes to.
//
// Store backends:
//   * Windows: the Credential Manager (generic credentials, target
//     "loomai/<name>"), protected by the user's login.
//   * elsewhere: <config dir>/secrets.json with owner-only permissions (0600).
//     This is weaker than a keyring and the README says so.
//   * LOOMAI_SECRET_BACKEND=file forces the file backend on Windows too (headless
//     setups, and the tests, which must not touch the real Credential Manager).
//
// A secret value is never returned by any function here except get(), which only
// the backend clients call, and is never logged.
namespace loomai::secrets {

// OPENAI_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY, or "custom:<model id>" for the
// key of a user-defined model. Anything else is not a valid secret name.
bool valid_name(const std::string& name);

// The secret's value, or "" if there is none. Environment first, then the store.
std::string get(const std::string& name);

// Where get() would take the value from: "env", "store" or "none".
std::string source(const std::string& name);

// Stores `value` (trimmed; no inner whitespace or control characters, at most
// 2048 bytes). 400 for an invalid name or value, 409 if an environment variable
// of that name already provides the key, 500 if the store refuses the write.
config::Status set(const std::string& name, const std::string& value);

// Deletes the stored value. Idempotent; works even when an environment variable
// shadows it (only the stored copy is removed).
config::Status remove(const std::string& name);

// {"keys": {"<NAME>": {"set": bool, "source": "env|store|none", "locked": bool}},
//  "store": "wincred|file"} for the three built-in provider keys. Presence only.
nlohmann::json keys_json();

// "wincred" or "file".
std::string backend_name();

// Forgets cached lookups (tests, and after LOOMAI_CONFIG_DIR / backend changed).
void reload();

}  // namespace loomai::secrets
