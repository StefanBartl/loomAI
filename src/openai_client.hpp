#pragma once

#include <functional>
#include <string>

#include "model_client.hpp"

// Client for the OpenAI Chat Completions API -- one of loomAI's ModelRouter
// backends (see main.cpp's model-name-prefix dispatch). Mirrors
// ollama_client.hpp's shape so main.cpp can treat every backend uniformly
// through loomai::AskResult.
//
// The same wire protocol is spoken by many other servers (LM Studio, vLLM, Groq,
// OpenRouter, llama.cpp, ...), so the calls also take an Endpoint: user-defined
// "openai-compatible" models point at one of those instead of api.openai.com.
namespace loomai::openai {

// Where and how to talk. The default is OpenAI itself.
struct Endpoint {
    std::string base_url = "https://api.openai.com";  // scheme://host[:port], no path
    std::string path_prefix = "/v1";                  // chat route is <path_prefix>/chat/completions
    std::string api_key;                              // empty: send no Authorization header
    std::string label = "openai";                     // prefix of error messages
    bool key_required = true;                         // false for local servers without auth
    std::string missing_key_message = "OPENAI_API_KEY not set";
};

// OpenAI proper, with the key resolved from the environment / secret store.
Endpoint default_endpoint();

// True if OPENAI_API_KEY is set (environment or secret store). The router only
// dispatches a "gpt-"/"o1-"/"o3-" model here when this is true; an unset key is
// a config error the caller should surface before even attempting a request.
bool configured();

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms);

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms,
              const Endpoint& endpoint);

// Streaming call. Invokes on_delta once per text chunk as it arrives.
// Returns an empty string on success, or an error message.
std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta);

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const Endpoint& endpoint,
                        const std::function<void(const std::string&)>& on_delta);

}  // namespace loomai::openai
