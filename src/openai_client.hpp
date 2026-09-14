#pragma once

#include <functional>
#include <string>

#include "model_client.hpp"

// Client for the OpenAI Chat Completions API -- one of loomAI's ModelRouter
// backends (see main.cpp's model-name-prefix dispatch). Mirrors
// ollama_client.hpp's shape so main.cpp can treat every backend uniformly
// through loomai::AskResult.
namespace loomai::openai {

// True if OPENAI_API_KEY is set. The router only dispatches a "gpt-"/"o1-"/
// "o3-" model here when this is true; an unset key is a config error the
// caller should surface before even attempting a request.
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

}  // namespace loomai::openai
