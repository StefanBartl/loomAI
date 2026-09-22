#include "anthropic_client.hpp"

#include <cstdlib>

#include "httplib.h"

namespace loomai::anthropic {

using json = nlohmann::json;

namespace {

constexpr const char* kApiVersion = "2023-06-01";
constexpr int kMaxTokens = 4096;

// A legitimate SSE chunk is at most a few KB -- this only guards against a
// malformed/adversarial response that never sends a newline, which would
// otherwise grow `buffer` in ask_stream()'s content_receiver without bound
// for the life of the request. Mirrors gemini_client.cpp's kMaxLineBufferBytes.
constexpr size_t kMaxLineBufferBytes = 1 << 20;  // 1 MiB

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

std::string api_key() {
    return env_or("ANTHROPIC_API_KEY", "");
}

// One persistent, keep-alive client (TCP + TLS session) per worker thread
// instead of a fresh connection and handshake on every /ask and
// /ask/stream request. See ollama_client.cpp's client_for_thread() for the
// full reasoning (thread_local avoids any locking, since main.cpp's
// httplib Server hands each request to one worker thread that handles
// requests sequentially) -- identical here except the host is a fixed
// literal, not an env-configurable one.
httplib::Client& client_for_thread(int timeout_ms) {
    thread_local httplib::Client cli("https://api.anthropic.com");
    cli.set_keep_alive(true);
    cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
    return cli;
}

json build_body(const std::string& prompt,
                 const std::string& system,
                 const std::string& model,
                 bool stream) {
    json body;
    body["model"] = model;
    body["max_tokens"] = kMaxTokens;
    body["stream"] = stream;
    body["messages"] = json::array({{{"role", "user"}, {"content", prompt}}});
    if (!system.empty()) {
        body["system"] = system;
    }
    return body;
}

// Concatenate every text content block, same as claude.lua's to_response().
std::string extract_text(const json& parsed) {
    std::string text;
    for (const auto& block : parsed.value("content", json::array())) {
        if (block.value("type", "") == "text" && block.contains("text") &&
            block["text"].is_string()) {
            text += block["text"].get<std::string>();
        }
    }
    return text;
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
        result.error_message = "anthropic: ANTHROPIC_API_KEY not set";
        return result;
    }

    auto& cli = client_for_thread(timeout_ms);
    httplib::Headers headers = {
        {"x-api-key", key},
        {"anthropic-version", kApiVersion},
    };
    auto body = build_body(prompt, system, model, /*stream=*/false);

    auto res = cli.Post("/v1/messages", headers, body.dump(), "application/json");
    if (!res) {
        result.error_message = "anthropic: " + httplib::to_string(res.error());
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message =
            "anthropic: invalid JSON response (HTTP " + std::to_string(res->status) + ")";
        return result;
    }
    if (parsed.value("type", "") == "error") {
        auto err = parsed.value("error", json::object());
        result.error_message = "anthropic: " + error_text(err);
        return result;
    }
    if (res->status != 200) {
        result.error_message = "anthropic: HTTP " + std::to_string(res->status) + ": " + res->body;
        return result;
    }

    result.text = extract_text(parsed);
    result.stop_reason = parsed.value("stop_reason", "");
    auto usage = parsed.value("usage", json::object());
    result.prompt_tokens = usage.value("input_tokens", 0L);
    result.completion_tokens = usage.value("output_tokens", 0L);
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
        return "anthropic: ANTHROPIC_API_KEY not set";
    }

    auto& cli = client_for_thread(timeout_ms);
    auto body = build_body(prompt, system, model, /*stream=*/true);

    std::string buffer;
    std::string stream_error;
    std::vector<std::string> non_data_lines;
    bool got_content = false;

    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/messages";
    req.set_header("Content-Type", "application/json");
    req.set_header("x-api-key", key);
    req.set_header("anthropic-version", kApiVersion);
    req.body = body.dump();
    // Anthropic's SSE carries both "event: <type>" and "data: {...}" lines
    // per event -- only the payload line is needed, decoded["type"] already
    // says what kind of event it is (same reasoning as claude.lua).
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
            if (payload->empty() || *payload == "[DONE]") {
                continue;
            }

            auto obj = json::parse(*payload, nullptr, false);
            if (obj.is_discarded()) continue;

            std::string type = obj.value("type", "");
            if (type == "content_block_delta") {
                auto delta = obj.value("delta", json::object());
                if (delta.value("type", "") == "text_delta") {
                    std::string chunk = delta.value("text", "");
                    if (!chunk.empty()) {
                        got_content = true;
                        on_delta(chunk);
                    }
                }
            } else if (type == "error") {
                auto err = obj.value("error", json::object());
                stream_error = "anthropic: " + error_text(err);
                return false;
            }
        }
        if (buffer.size() > kMaxLineBufferBytes) {
            stream_error = "anthropic: streaming response line exceeded " +
                            std::to_string(kMaxLineBufferBytes) + " bytes without a newline";
            return false;
        }
        return true;
    };

    auto res = cli.send(req);

    if (!stream_error.empty()) {
        return stream_error;
    }
    if (!res) {
        return "anthropic: " + httplib::to_string(res.error());
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
        if (recovered && recovered->value("type", "") == "error") {
            auto err = recovered->value("error", json::object());
            return "anthropic: " + error_text(err);
        }
        if (res->status != 200) {
            return "anthropic: HTTP " + std::to_string(res->status) + ": " + res->body;
        }
    }
    return "";
}

}  // namespace loomai::anthropic
