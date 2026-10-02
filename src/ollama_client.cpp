#include "ollama_client.hpp"

#include <cstdlib>

#include "config.hpp"
#include "http_pool.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace loomai::ollama {

using json = nlohmann::json;

namespace {

// A legitimate NDJSON chunk is at most a few KB -- this only guards against a
// malformed/adversarial response that never sends a newline, which would
// otherwise grow `buffer` in ask_stream()'s content_receiver without bound
// for the life of the request. Mirrors gemini_client.cpp's kMaxLineBufferBytes.
// Most relevant here since LOOMAI_OLLAMA_HOST is user-configurable and can
// point at a non-local host, unlike the hardcoded cloud-provider hosts.
constexpr size_t kMaxLineBufferBytes = 1 << 20;  // 1 MiB

// Keep-alive client for the *current* Ollama host (see http_pool.hpp). host()
// is read on every request, so a host changed in the settings dialog takes
// effect immediately instead of staying pinned to the first request's value.
httplib::Client& client_for_thread(int timeout_ms) {
    return loomai::client_for(host(), timeout_ms);
}

json build_request(const std::string& prompt,
                    const std::string& system,
                    const std::string& model,
                    bool stream) {
    json body;
    body["model"] = model.empty() ? default_model() : model;
    body["prompt"] = prompt;
    body["stream"] = stream;
    if (!system.empty()) {
        body["system"] = system;
    }
    return body;
}

}  // namespace

// Both come from the config store, which applies LOOMAI_OLLAMA_HOST /
// LOOMAI_DEFAULT_MODEL first, then config.json (settings dialog), then the
// built-in default.
std::string host() {
    std::string raw = config::effective("ollama_host");
    if (raw.rfind("http://", 0) != 0 && raw.rfind("https://", 0) != 0) {
        raw = "http://" + raw;
    }
    return raw;
}

std::string default_model() {
    return config::effective("default_model");
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms) {
    AskResult result;

    auto& cli = client_for_thread(timeout_ms);
    auto body = build_request(prompt, system, model, /*stream=*/false);

    auto res = cli.Post("/api/generate", body.dump(), "application/json");
    if (!res) {
        result.error_message =
            "ollama: " + httplib::to_string(res.error()) + " (" + host() + ")";
        return result;
    }
    if (res->status != 200) {
        result.error_message =
            "ollama: HTTP " + std::to_string(res->status) + ": " + res->body;
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message = "ollama: invalid JSON response";
        return result;
    }
    if (parsed.contains("error")) {
        result.error_message = "ollama: " + error_text(parsed["error"]);
        return result;
    }

    result.ok = true;
    result.text = parsed.value("response", "");
    result.stop_reason = parsed.value("done_reason", "");
    result.prompt_tokens = parsed.value("prompt_eval_count", 0L);
    result.completion_tokens = parsed.value("eval_count", 0L);
    return result;
}

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta) {
    auto& cli = client_for_thread(timeout_ms);
    auto body = build_request(prompt, system, model, /*stream=*/true);

    // Ollama streams NDJSON (one JSON object per line), not SSE -- reframe
    // it into individual text deltas for the caller, who turns those into
    // this server's own SSE format. cpp-httplib's high-level Post()
    // overloads don't expose a response content_receiver, so the request is
    // built manually and sent via Client::send().
    std::string buffer;
    std::string stream_error;

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/generate";
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t len, uint64_t,
                                uint64_t) -> bool {
        buffer.append(data, len);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty()) continue;

            auto obj = json::parse(line, nullptr, false);
            if (obj.is_discarded()) continue;

            if (obj.contains("error")) {
                stream_error = "ollama: " + error_text(obj["error"]);
                return false;
            }

            std::string chunk = obj.value("response", "");
            if (!chunk.empty()) {
                on_delta(chunk);
            }
        }
        if (buffer.size() > kMaxLineBufferBytes) {
            stream_error = "ollama: streaming response line exceeded " +
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
        return "ollama: " + httplib::to_string(res.error()) + " (" + host() + ")";
    }
    if (res->status != 200) {
        return "ollama: HTTP " + std::to_string(res->status) + ": " + res->body;
    }
    return "";
}

}  // namespace loomai::ollama
