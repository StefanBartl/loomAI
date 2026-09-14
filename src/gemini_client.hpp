#pragma once

#include <functional>
#include <string>

#include "model_client.hpp"

// Client for Google's Gemini API (Generative Language API) -- one of
// loomAI's ModelRouter backends (see main.cpp's model-name-prefix dispatch).
// Mirrors ollama_client.hpp's shape so main.cpp can treat every backend
// uniformly through loomai::AskResult.
//
// Auth deliberately uses the x-goog-api-key header, not the "?key=..." query
// parameter most Gemini quickstarts show -- the query-param form would put
// the key in the request URL, and this project's own README explains why
// loomAI binds to 127.0.0.1 rather than 0.0.0.0 in the first place (keeping
// secrets out of anything that could leak them). Same choice ai.nvim's own
// gemini.lua makes for the same reason.
namespace loomai::gemini {

// True if GEMINI_API_KEY is set. The router only dispatches a "gemini-"
// model here when this is true; an unset key is a config error the caller
// should surface before even attempting a request.
bool configured();

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms);

// Streaming call. Invokes on_delta once per text chunk as it arrives.
// Returns an empty string on success, or an error message.
std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta);

}  // namespace loomai::gemini
