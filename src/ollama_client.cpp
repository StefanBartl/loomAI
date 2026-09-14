#include "ollama_client.hpp"

#include <cstdlib>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace loomai::ollama {

using json = nlohmann::json;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// Ollama's own error field is normally a plain string, but nothing
// guarantees that -- LOOMAI_OLLAMA_HOST can point at any backend speaking
// this same shape, and json::value<std::string>() throws a type_error if
// "error" exists but isn't a string. That throw is fatal on the streaming
// path (see ask_stream): it happens inside a response content-provider
// callback, which runs outside cpp-httplib's request-routing try/catch, so
// an uncaught exception there crashes the whole process, not just one
// request. Never call .value("error", ...) directly; always go through this.
std::string error_text(const json& error_value) {
    if (error_value.is_string()) {
        return error_value.get<std::string>();
    }
    return error_value.dump();
}

httplib::Client make_client(int timeout_ms) {
    httplib::Client cli(host());
    cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
    return cli;
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

std::string host() {
    std::string raw = env_or("LOOMAI_OLLAMA_HOST", "http://127.0.0.1:11434");
    if (raw.rfind("http://", 0) != 0 && raw.rfind("https://", 0) != 0) {
        raw = "http://" + raw;
    }
    return raw;
}

std::string default_model() {
    return env_or("LOOMAI_DEFAULT_MODEL", "llama3.1:8b-q5_K_M");
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms) {
    AskResult result;

    auto cli = make_client(timeout_ms);
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
    result.prompt_eval_count = parsed.value("prompt_eval_count", 0L);
    result.eval_count = parsed.value("eval_count", 0L);
    return result;
}

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta) {
    auto cli = make_client(timeout_ms);
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
