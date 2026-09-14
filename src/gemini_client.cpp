#include "gemini_client.hpp"

#include <cstdlib>

#include "httplib.h"

namespace loomai::gemini {

using json = nlohmann::json;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

std::string api_key() {
    return env_or("GEMINI_API_KEY", "");
}

httplib::Client make_client(int timeout_ms) {
    httplib::Client cli("https://generativelanguage.googleapis.com");
    cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
    return cli;
}

json build_body(const std::string& prompt, const std::string& system) {
    json body;
    body["contents"] =
        json::array({{{"role", "user"}, {"parts", json::array({{{"text", prompt}}})}}});
    if (!system.empty()) {
        body["systemInstruction"] = {{"parts", json::array({{{"text", system}}})}};
    }
    return body;
}

// Concatenate every text part of the first candidate, and its finish
// reason, same shape as ai.nvim's gemini.lua candidate_text().
std::pair<std::string, std::string> candidate_text(const json& parsed) {
    auto candidates = parsed.value("candidates", json::array());
    if (candidates.empty()) {
        return {"", ""};
    }
    const auto& candidate = candidates[0];
    std::string text;
    auto parts = candidate.value("content", json::object()).value("parts", json::array());
    for (const auto& part : parts) {
        if (part.contains("text") && part["text"].is_string()) {
            text += part["text"].get<std::string>();
        }
    }
    return {text, candidate.value("finishReason", "")};
}

}  // namespace

bool configured() {
    return !api_key().empty();
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms) {
    AskResult result;
    std::string key = api_key();
    if (key.empty()) {
        result.error_message = "gemini: GEMINI_API_KEY not set";
        return result;
    }

    auto cli = make_client(timeout_ms);
    httplib::Headers headers = {{"x-goog-api-key", key}};
    auto body = build_body(prompt, system);
    std::string path = "/v1beta/models/" + model + ":generateContent";

    auto res = cli.Post(path, headers, body.dump(), "application/json");
    if (!res) {
        result.error_message = "gemini: " + httplib::to_string(res.error());
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message =
            "gemini: invalid JSON response (HTTP " + std::to_string(res->status) + ")";
        return result;
    }
    if (parsed.contains("error")) {
        result.error_message = "gemini: " + error_text(parsed["error"]);
        return result;
    }
    if (res->status != 200) {
        result.error_message = "gemini: HTTP " + std::to_string(res->status) + ": " + res->body;
        return result;
    }

    auto [text, finish_reason] = candidate_text(parsed);
    result.text = text;
    result.stop_reason = finish_reason;
    auto usage = parsed.value("usageMetadata", json::object());
    result.prompt_tokens = usage.value("promptTokenCount", 0L);
    result.completion_tokens = usage.value("candidatesTokenCount", 0L);
    result.ok = true;
    return result;
}

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta) {
    std::string key = api_key();
    if (key.empty()) {
        return "gemini: GEMINI_API_KEY not set";
    }

    auto cli = make_client(timeout_ms);
    auto body = build_body(prompt, system);
    std::string path = "/v1beta/models/" + model + ":streamGenerateContent?alt=sse";

    std::string buffer;
    std::string stream_error;
    std::vector<std::string> non_data_lines;
    bool got_content = false;

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.set_header("Content-Type", "application/json");
    req.set_header("x-goog-api-key", key);
    req.body = body.dump();
    // Unlike OpenAI/Anthropic, Gemini's stream has no "[DONE]" sentinel --
    // it just closes the connection after the last chunk.
    req.content_receiver = [&](const char* data, size_t len, uint64_t,
                                uint64_t) -> bool {
        buffer.append(data, len);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            buffer.erase(0, pos + 1);

            auto payload = sse_data_payload(line);
            if (!payload) {
                if (!line.empty()) {
                    non_data_lines.push_back(line);
                }
                continue;
            }
            if (payload->empty()) {
                continue;
            }

            auto obj = json::parse(*payload, nullptr, false);
            if (obj.is_discarded()) continue;

            if (obj.contains("error")) {
                stream_error = "gemini: " + error_text(obj["error"]);
                return false;
            }
            auto [chunk, _] = candidate_text(obj);
            if (!chunk.empty()) {
                got_content = true;
                on_delta(chunk);
            }
        }
        return true;
    };

    auto res = cli.send(req);

    if (!stream_error.empty()) {
        return stream_error;
    }
    if (!res) {
        return "gemini: " + httplib::to_string(res.error());
    }
    // A fatal error body has no guaranteed trailing newline (unlike a
    // well-formed SSE stream, which always ends each line with one), so the
    // last line can still be sitting unflushed in `buffer` once the
    // connection closes -- verified live against a real OpenAI 401 hitting
    // the identical code shape; recover_error_body() silently saw an empty
    // non_data_lines without this.
    if (!buffer.empty()) {
        non_data_lines.push_back(buffer);
        buffer.clear();
    }
    if (!got_content) {
        auto recovered = recover_error_body(non_data_lines);
        if (recovered && recovered->contains("error")) {
            return "gemini: " + error_text((*recovered)["error"]);
        }
        if (res->status != 200) {
            return "gemini: HTTP " + std::to_string(res->status) + ": " + res->body;
        }
    }
    return "";
}

}  // namespace loomai::gemini
