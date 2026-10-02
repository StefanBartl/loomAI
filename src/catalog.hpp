#pragma once

#include <string>

#include "nlohmann/json.hpp"

// The model listing behind GET /models and GET /models/info: local models from
// Ollama, cloud models from the providers' own list endpoints (only when a key is
// available, otherwise clearly marked suggestions), and the user's custom models.
//
// Remote lists are fetched in parallel with short timeouts and cached (cloud
// ~10 min, Ollama ~15 s, failures ~30 s). One provider failing leaves the rest of
// the list intact; its message ends up in "errors", never as a failed request.
namespace loomai::catalog {

// {"default": "<default model>", "models": [entry...], "errors": {"<provider>": "msg"}}.
// `refresh` bypasses the cache.
nlohmann::json list(bool refresh);

struct InfoResult {
    int http = 200;
    nlohmann::json body;  // {"id","category","provider","details"} or {"error":{"message"}}
};

// Details for the hover popup. Local models are asked at Ollama (/api/show) for
// context length and capabilities; for everything else the details already in
// the list are returned. 404 for an unknown id, 502 if Ollama cannot be reached.
InfoResult info(const std::string& id);

}  // namespace loomai::catalog
