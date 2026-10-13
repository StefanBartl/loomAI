#pragma once

#include <functional>
#include <string>

#include "model_client.hpp"

// Minimal client for Ollama's local REST API (POST /api/generate), used to
// back loomAI's /ask and /ask/stream endpoints. See wkdbook-native/loomai/Guides/
// ki-agenten-framework-architektur.md (private repo), Phase 1, "Ollama
// REST-Client in C++".
//
// Returns the shared loomai::AskResult (see model_client.hpp) rather than a
// private struct: main.cpp's model-name-prefix router dispatches to this or
// one of the cloud clients (openai/anthropic/gemini) and needs one uniform
// result shape to build its HTTP response from, not four near-identical
// ones.
namespace loomai::ollama {

// Base URL of the local Ollama daemon. Reads LOOMAI_OLLAMA_HOST, falls back
// to http://127.0.0.1:11434 (Ollama's own default). Deliberately not named
// OLLAMA_HOST -- that variable already means "address the Ollama *server*
// binds to" (e.g. commonly "0.0.0.0:11434") on machines running Ollama,
// which is a different thing from "address a client should connect to".
std::string host();

// Model used when the caller doesn't specify one. Reads
// LOOMAI_DEFAULT_MODEL, falls back to a small local model.
std::string default_model();

// Blocking, non-streaming call to /api/generate.
loomai::AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms);

// Streaming call to /api/generate. Invokes on_delta once per text chunk as
// it arrives. Returns an empty string on success, or an error message.
std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta);

}  // namespace loomai::ollama
