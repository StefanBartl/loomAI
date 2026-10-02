#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "nlohmann/json.hpp"

// Model registry: decides which backend a model name goes to, and manages the
// user-defined ("custom") models stored in config.json. Pure logic, no HTTP, so
// it lives in loomai_core and is unit-tested directly.
//
// Resolution order for a request's `model`:
//   1. an exact match with a custom model id,
//   2. the model-name prefixes (claude-, gpt-/o1-/o3-, gemini-; overridable with
//      LOOMAI_PREFIXES_* exactly as before),
//   3. local Ollama (also for an empty model).
// The /ask and /ask/stream contract is unchanged: `model` may simply also be the
// id of a custom model.
namespace loomai::models {

struct Route {
    std::string provider = "ollama";  // ollama | openai | anthropic | gemini | openai-compatible
    std::string upstream_model;       // name sent to the backend ("" = the backend's default)
    std::string base_url;             // openai-compatible: scheme://host[:port]
    std::string path_prefix;          // openai-compatible: e.g. "/v1" (may be empty)
    std::string key_name;             // secret holding the API key, "" if none
    std::string label;                // prefix for error messages
    bool custom = false;
    bool local = false;               // Ollama or a loopback endpoint: gets the longer default timeout
};

Route resolve(const std::string& model);

// Model-name prefixes of "openai", "anthropic" or "gemini" (environment override
// read once, like every other environment lookup at startup).
const std::vector<std::string>& prefixes(std::string_view provider);

// Public view of the custom models (never contains a key):
// [{"id","name","provider","upstream_model","base_url"?,"has_key"}].
nlohmann::json custom_list();

// Adds a custom model from {"id","provider","name"?,"upstream_model"?,"base_url"?,
// "api_key"?}. 400 for invalid input, 409 for a duplicate id, 500 on a storage
// failure. `created` receives the public view of the new entry.
config::Status custom_add(const nlohmann::json& body, nlohmann::json& created);

// Removes a custom model and its stored key. 404 if the id is unknown.
config::Status custom_remove(const std::string& id);

// Splits "http(s)://host[:port][/path]" into the origin and the path prefix
// (trailing '/' dropped, path limited to [A-Za-z0-9._~/-] without '..').
// Returns false for anything else (query, fragment, credentials, bad port, ...).
bool split_base_url(const std::string& url, std::string& origin, std::string& path_prefix);

}  // namespace loomai::models
