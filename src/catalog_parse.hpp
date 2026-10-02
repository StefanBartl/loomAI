#pragma once

#include <string>

#include "nlohmann/json.hpp"

// Turns the model-list responses of the backends into loomAI's own entry shape.
// Pure functions on already-parsed JSON (no HTTP), so they are unit-tested with
// fixtures and a provider changing its format degrades one list, not the UI.
//
// Entry: {"id", "name", "category": "local"|"cloud", "provider", "available",
//         "details": {...}}. Only what the provider actually reports goes into
// "details"; nothing is guessed, and there are deliberately no prices (they go
// stale in code).
namespace loomai::catalog {

// GET <ollama>/api/tags  ->  entries (category "local").
nlohmann::json parse_ollama_tags(const nlohmann::json& response);

// GET https://api.openai.com/v1/models  ->  chat models matching the routing
// prefixes, newest first. Embedding/audio/image/moderation models are dropped:
// they would not work through /ask.
nlohmann::json parse_openai_models(const nlohmann::json& response);

// GET https://api.anthropic.com/v1/models
nlohmann::json parse_anthropic_models(const nlohmann::json& response);

// GET https://generativelanguage.googleapis.com/v1beta/models  ->  models that
// support generateContent and match the routing prefixes.
nlohmann::json parse_gemini_models(const nlohmann::json& response);

// POST <ollama>/api/show  ->  the details worth showing in the hover popup.
nlohmann::json parse_ollama_show(const nlohmann::json& response);

// Fallback suggestions for a cloud provider without a key: available=false and
// details.preset=true. "openai" | "anthropic" | "gemini".
nlohmann::json presets(const std::string& provider);

// "YYYY-MM-DD" for a Unix timestamp (UTC), "" for non-positive input.
std::string iso_date(long long epoch_seconds);

}  // namespace loomai::catalog
