#include "openai_client.hpp"

#include <cstdlib>

#include "http_pool.hpp"
#include "httplib.h"
#include "net_util.hpp"
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

// An API key must never travel in clear text across a network: plain http is
// only acceptable to this machine itself. https is always fine.
std::string key_transport_error(const Endpoint& endpoint) {
    if (endpoint.api_key.empty() || endpoint.base_url.rfind("http://", 0) != 0) {
        return "";
    }
    const std::string host = loomai::host_of(endpoint.base_url);
    if (loomai::is_loopback_host(host)) {
        return "";
    }
    return endpoint.label + ": refusing to send an API key over plain http to " + host +
           " (use an https:// base URL)";
}

httplib::Headers auth_headers(const Endpoint& endpoint) {
    httplib::Headers headers;
    if (!endpoint.api_key.empty()) {
        headers.emplace("Authorization", "Bearer " + endpoint.api_key);
    }
    return headers;
}

std::string chat_path(const Endpoint& endpoint) {
    return endpoint.path_prefix + "/chat/completions";
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

Endpoint default_endpoint() {
    Endpoint endpoint;
    endpoint.api_key = api_key();
    return endpoint;
}

bool configured() {
    return !api_key().empty();
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms) {
    return ask(prompt, system, model, timeout_ms, default_endpoint());
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms,
              const Endpoint& endpoint) {
    AskResult result;
    if (endpoint.key_required && endpoint.api_key.empty()) {
        result.error_message = endpoint.label + ": " + endpoint.missing_key_message;
        return result;
    }
    if (std::string refused = key_transport_error(endpoint); !refused.empty()) {
        result.error_message = refused;
        return result;
    }

    auto& cli = loomai::client_for(endpoint.base_url, timeout_ms);
    auto body = build_body(prompt, system, model, /*stream=*/false);

    auto res = cli.Post(chat_path(endpoint), auth_headers(endpoint), body.dump(), "application/json");
    if (!res) {
        result.error_message = endpoint.label + ": " + httplib::to_string(res.error());
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message =
            endpoint.label + ": invalid JSON response (HTTP " + std::to_string(res->status) + ")";
        return result;
    }
    if (parsed.contains("error")) {
        result.error_message = endpoint.label + ": " + error_text(parsed["error"]);
        return result;
    }
    if (res->status != 200) {
        result.error_message = endpoint.label + ": HTTP " + std::to_string(res->status) + ": " + res->body;
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
    return ask_stream(prompt, system, model, timeout_ms, default_endpoint(), on_delta);
}

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const Endpoint& endpoint,
                        const std::function<void(const std::string&)>& on_delta) {
    if (endpoint.key_required && endpoint.api_key.empty()) {
        return endpoint.label + ": " + endpoint.missing_key_message;
    }
    if (std::string refused = key_transport_error(endpoint); !refused.empty()) {
        return refused;
    }

    auto& cli = loomai::client_for(endpoint.base_url, timeout_ms);
    auto body = build_body(prompt, system, model, /*stream=*/true);

    std::string buffer;
    std::string stream_error;
    std::vector<std::string> non_data_lines;
    bool got_content = false;

    httplib::Request req;
    req.method = "POST";
    req.path = chat_path(endpoint);
    req.set_header("Content-Type", "application/json");
    for (const auto& header : auth_headers(endpoint)) {
        req.set_header(header.first.c_str(), header.second.c_str());
    }
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
                stream_error = endpoint.label + ": " + error_text(obj["error"]);
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
            stream_error = endpoint.label + ": streaming response line exceeded " +
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
        return endpoint.label + ": " + httplib::to_string(res.error());
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
            return endpoint.label + ": " + error_text((*recovered)["error"]);
        }
        if (res->status != 200) {
            return endpoint.label + ": HTTP " + std::to_string(res->status) + ": " + res->body;
        }
    }
    return "";
}

}  // namespace loomai::openai
