#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

// Shared result shape and SSE helpers for every backend client (ollama,
// openai, anthropic, gemini) that main.cpp's /ask and /ask/stream handlers
// dispatch to by model-name prefix -- see wkdbook-loomai/Guides/
// ki-agenten-framework-architektur.md (private repo) and the ai.nvim
// handover's loomAI ModelRouter scoping for the reasoning behind the
// prefix-based router.
namespace loomai {

struct AskResult {
    bool ok = false;
    std::string text;
    std::string stop_reason;
    std::string error_message;
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

// Stringify a JSON "error" field defensively. It is not always a plain
// string (OpenAI/Anthropic/Gemini all nest it as {"message": "..."} inside
// an object; some backends might send something else entirely), and
// nlohmann::json::value<std::string>()/.get<std::string>() throws a
// type_error on a mismatch. On the streaming path that throw is fatal: it
// happens inside a response content-provider callback, which runs outside
// cpp-httplib's own request-routing try/catch, so an uncaught exception
// there crashes the whole process, not just one request (see main.cpp's
// 4e2777c fix history for the ollama path this same class of bug hit
// first). Never inspect an "error" field directly; always go through this.
inline std::string error_text(const nlohmann::json& error_value) {
    if (error_value.is_string()) {
        return error_value.get<std::string>();
    }
    if (error_value.is_object() && error_value.contains("message") &&
        error_value["message"].is_string()) {
        return error_value["message"].get<std::string>();
    }
    return error_value.dump();
}

// Extract the payload from an SSE "data: ..." line, if it is one. Mirrors
// ai.nvim's lua/ai/providers/sse.lua (M.data_payload) -- same transport
// quirk, ported to C++ for the cloud clients that speak SSE.
inline std::optional<std::string> sse_data_payload(const std::string& line) {
    static constexpr const char* prefix = "data:";
    static constexpr size_t prefix_len = 5;
    if (line.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    size_t pos = prefix_len;
    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }
    return line.substr(pos);
}

// Try to recover a fatal error response that arrived as a plain,
// pretty-printed JSON body instead of an SSE event -- the same quirk
// ai.nvim's claude.lua/openai.lua hit and ai.providers.sse's
// recover_error_body exists for: a streaming auth/validation failure does
// not always come back as an SSE event, and the connection still closes
// cleanly. Callers collect every non-"data:" line into non_data_lines and
// call this once the stream ends with no actual content having arrived.
inline std::optional<nlohmann::json> recover_error_body(
    const std::vector<std::string>& non_data_lines) {
    if (non_data_lines.empty()) {
        return std::nullopt;
    }
    std::string joined;
    for (size_t i = 0; i < non_data_lines.size(); ++i) {
        if (i > 0) {
            joined += "\n";
        }
        joined += non_data_lines[i];
    }
    auto parsed = nlohmann::json::parse(joined, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace loomai
