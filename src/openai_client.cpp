#include "openai_client.hpp"

#include <cstdlib>

#include "httplib.h"
#include "secrets.hpp"

namespace loomai::openai {

using json = nlohmann::json;

namespace {

// A legitimate SSE chunk is at most a few KB -- this only guards against a
// malformed/adversarial response that never sends a newline, which would
// otherwise grow `buffer` in ask_stream()'s content_receiver without bound
// for the life of the request. Mirrors gemini_client.cpp's kMaxLineBufferBytes.
constexpr size_t kMaxLineBufferBytes = 1 << 20;  // 1 MiB

// Environment variable first, then the key saved from the settings dialog.
std::string api_key() {
    return secrets::get("OPENAI_API_KEY");
}

// One persistent, keep-alive client (TCP + TLS session) per worker thread
// instead of a fresh connection and handshake on every /ask and
// /ask/stream request. See ollama_client.cpp's client_for_thread() for the
// full reasoning (thread_local avoids any locking, since main.cpp's
// httplib Server hands each request to one worker thread that handles
// requests sequentially) -- identical here except the host is a fixed
// literal, not an env-configurable one.
httplib::Client& client_for_thread(int timeout_ms) {
    thread_local httplib::Client cli("https://api.openai.com");
    cli.set_keep_alive(true);
    cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
    return cli;
}

json build_body(const std::string& prompt,
                 const std::string& system,
                 const std::string& model,
                 bool stream) {
    json messages = json::array();
    if (!system.empty()) {
        messages.push_back({{"role", "system"}, {"content", system}});
    }
    messages.push_back({{"role", "user"}, {"content", prompt}});

    json body;
    body["model"] = model;
    body["messages"] = messages;
    body["stream"] = stream;
    return body;
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
        result.error_message = "openai: OPENAI_API_KEY not set";
        return result;
    }

    auto& cli = client_for_thread(timeout_ms);
    httplib::Headers headers = {{"Authorization", "Bearer " + key}};
    auto body = build_body(prompt, system, model, /*stream=*/false);

    auto res = cli.Post("/v1/chat/completions", headers, body.dump(), "application/json");
    if (!res) {
        result.error_message = "openai: " + httplib::to_string(res.error());
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message =
            "openai: invalid JSON response (HTTP " + std::to_string(res->status) + ")";
        return result;
    }
    if (parsed.contains("error")) {
        result.error_message = "openai: " + error_text(parsed["error"]);
        return result;
    }
    if (res->status != 200) {
        result.error_message = "openai: HTTP " + std::to_string(res->status) + ": " + res->body;
        return result;
    }

    auto choices = parsed.value("choices", json::array());
    if (!choices.empty()) {
        auto message = choices[0].value("message", json::object());
        result.text = message.value("content", "");
        result.stop_reason = choices[0].value("finish_reason", "");
    }
    auto usage = parsed.value("usage", json::object());
    result.prompt_tokens = usage.value("prompt_tokens", 0L);
    result.completion_tokens = usage.value("completion_tokens", 0L);
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
        return "openai: OPENAI_API_KEY not set";
    }

    auto& cli = client_for_thread(timeout_ms);
    auto body = build_body(prompt, system, model, /*stream=*/true);

    std::string buffer;
    std::string stream_error;
    std::vector<std::string> non_data_lines;
    bool got_content = false;

    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.set_header("Content-Type", "application/json");
    req.set_header("Authorization", "Bearer " + key);
    req.body = body.dump();
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

            if (obj.contains("error")) {
                stream_error = "openai: " + error_text(obj["error"]);
                return false;
            }
            auto choices = obj.value("choices", json::array());
            if (!choices.empty()) {
                auto delta = choices[0].value("delta", json::object());
                std::string chunk = delta.value("content", "");
                if (!chunk.empty()) {
                    got_content = true;
                    on_delta(chunk);
                }
            }
        }
        if (buffer.size() > kMaxLineBufferBytes) {
            stream_error = "openai: streaming response line exceeded " +
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
        return "openai: " + httplib::to_string(res.error());
    }
    // A fatal error body has no guaranteed trailing newline (unlike a
    // well-formed SSE/NDJSON stream, which always ends each line with one),
    // so the last line can still be sitting unflushed in `buffer` once the
    // connection closes -- verified live: a real 401 response left the
    // whole error body stuck here, and recover_error_body() silently saw
    // an empty non_data_lines without this.
    if (!buffer.empty()) {
        non_data_lines.push_back(buffer);
        buffer.clear();
    }
    if (!got_content) {
        auto recovered = recover_error_body(non_data_lines);
        if (recovered && recovered->contains("error")) {
            return "openai: " + error_text((*recovered)["error"]);
        }
        if (res->status != 200) {
            return "openai: HTTP " + std::to_string(res->status) + ": " + res->body;
        }
    }
    return "";
}

}  // namespace loomai::openai
