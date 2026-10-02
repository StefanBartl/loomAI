#include "catalog.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "catalog_parse.hpp"
#include "config.hpp"
#include "httplib.h"
#include "model_client.hpp"
#include "models.hpp"
#include "ollama_client.hpp"
#include "secrets.hpp"

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace loomai::catalog {

namespace {

constexpr int kFetchTimeoutMs = 4000;
constexpr auto kCloudTtl = std::chrono::minutes(10);
constexpr auto kOllamaTtl = std::chrono::seconds(15);
constexpr auto kErrorTtl = std::chrono::seconds(30);
constexpr auto kInfoTtl = std::chrono::seconds(60);
constexpr size_t kMaxInfoCacheEntries = 64;
constexpr const char* kAnthropicVersion = "2023-06-01";

struct Remote {
    std::string fingerprint;  // what the fetch depended on (host / key hash); a change forces a refetch
    json entries = json::array();
    std::string error;
    Clock::time_point at;
    bool valid = false;
};

std::mutex g_mutex;
std::map<std::string, Remote> g_remote;
std::map<std::string, std::pair<Clock::time_point, json>> g_info;

// One GET returning JSON, with the backend's own error text on failure. Never
// includes request headers (they carry the key) in the message.
bool get_json(const std::string& origin, const std::string& path, const httplib::Headers& headers,
              const std::string& label, json& out, std::string& error) {
    httplib::Client client(origin);
    client.set_connection_timeout(std::chrono::milliseconds(kFetchTimeoutMs));
    client.set_read_timeout(std::chrono::milliseconds(kFetchTimeoutMs));
    auto res = client.Get(path, headers);
    if (!res) {
        error = label + ": " + httplib::to_string(res.error());
        return false;
    }
    out = json::parse(res->body, nullptr, false);
    if (out.is_discarded()) {
        error = label + ": invalid JSON response (HTTP " + std::to_string(res->status) + ")";
        return false;
    }
    if (res->status != 200) {
        std::string detail = (out.is_object() && out.contains("error")) ? loomai::error_text(out["error"]) : "";
        if (detail.size() > 200) {
            detail.resize(200);
        }
        error = label + ": HTTP " + std::to_string(res->status) + (detail.empty() ? "" : ": " + detail);
        return false;
    }
    return true;
}

struct Fetched {
    json entries = json::array();
    std::string error;
};

Fetched fetch_ollama() {
    Fetched fetched;
    json body;
    if (get_json(ollama::host(), "/api/tags", {}, "ollama", body, fetched.error)) {
        fetched.entries = parse_ollama_tags(body);
    }
    return fetched;
}

Fetched fetch_provider(const std::string& provider, const std::string& key) {
    Fetched fetched;
    json body;
    bool ok = false;
    if (provider == "openai") {
        ok = get_json("https://api.openai.com", "/v1/models", {{"Authorization", "Bearer " + key}}, "openai", body,
                      fetched.error);
        if (ok) fetched.entries = parse_openai_models(body);
    } else if (provider == "anthropic") {
        ok = get_json("https://api.anthropic.com", "/v1/models?limit=1000",
                      {{"x-api-key", key}, {"anthropic-version", kAnthropicVersion}}, "anthropic", body, fetched.error);
        if (ok) fetched.entries = parse_anthropic_models(body);
    } else if (provider == "gemini") {
        ok = get_json("https://generativelanguage.googleapis.com", "/v1beta/models?pageSize=1000",
                      {{"x-goog-api-key", key}}, "gemini", body, fetched.error);
        if (ok) fetched.entries = parse_gemini_models(body);
    }
    return fetched;
}

// Cached fetch. The network call runs outside the lock.
Remote cached(const std::string& name, const std::string& fingerprint, Clock::duration ok_ttl, bool refresh,
              const std::function<Fetched()>& fetch) {
    if (!refresh) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto hit = g_remote.find(name);
        if (hit != g_remote.end() && hit->second.valid && hit->second.fingerprint == fingerprint) {
            const auto ttl = hit->second.error.empty() ? ok_ttl : Clock::duration(kErrorTtl);
            if (Clock::now() - hit->second.at < ttl) {
                return hit->second;
            }
        }
    }
    Fetched fetched = fetch();
    Remote fresh;
    fresh.fingerprint = fingerprint;
    fresh.entries = std::move(fetched.entries);
    fresh.error = std::move(fetched.error);
    fresh.at = Clock::now();
    fresh.valid = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_remote[name] = fresh;
    return fresh;
}

std::string key_fingerprint(const std::string& key) {
    return std::to_string(std::hash<std::string>{}(key));  // in memory only; the key itself is never stored here
}

json custom_entry(const json& view) {
    const std::string provider = view.value("provider", "");
    bool available = true;
    if (provider == "openai") available = !secrets::get("OPENAI_API_KEY").empty();
    if (provider == "anthropic") available = !secrets::get("ANTHROPIC_API_KEY").empty();
    if (provider == "gemini") available = !secrets::get("GEMINI_API_KEY").empty();
    json details = {{"upstream_model", view.value("upstream_model", view.value("id", ""))}};
    if (view.contains("base_url")) {
        details["base_url"] = view["base_url"];
    }
    details["has_key"] = view.value("has_key", false);
    return {{"id", view.value("id", "")},
            {"name", view.value("name", view.value("id", ""))},
            {"category", "custom"},
            {"provider", provider},
            {"available", available},
            {"details", details}};
}

}  // namespace

json list(bool refresh) {
    struct Provider {
        const char* name;
        const char* key_name;
    };
    const Provider providers[] = {{"anthropic", "ANTHROPIC_API_KEY"}, {"openai", "OPENAI_API_KEY"},
                                  {"gemini", "GEMINI_API_KEY"}};

    // Everything that needs the network runs at once, so a slow or dead provider
    // costs one timeout, not the sum of them.
    auto ollama_future = std::async(std::launch::async, [&] {
        return cached("ollama", ollama::host(), kOllamaTtl, refresh, fetch_ollama);
    });
    std::vector<std::pair<std::string, std::future<Remote>>> cloud_futures;
    std::map<std::string, bool> has_key;
    for (const auto& provider : providers) {
        const std::string key = secrets::get(provider.key_name);
        has_key[provider.name] = !key.empty();
        if (key.empty()) {
            continue;
        }
        const std::string name = provider.name;
        cloud_futures.emplace_back(name, std::async(std::launch::async, [name, key, refresh] {
                                       return cached(name, key_fingerprint(key), kCloudTtl, refresh,
                                                     [&] { return fetch_provider(name, key); });
                                   }));
    }

    json custom = json::array();
    std::set<std::string> custom_ids;
    for (const auto& view : models::custom_list()) {
        custom_ids.insert(view.value("id", ""));
        custom.push_back(custom_entry(view));
    }

    json models = json::array();
    json errors = json::object();
    auto add = [&](const json& entries) {
        for (const auto& item : entries) {
            // A custom model with the same id wins (it is what routing uses).
            if (!custom_ids.count(item.value("id", ""))) {
                models.push_back(item);
            }
        }
    };

    Remote local = ollama_future.get();
    add(local.entries);
    if (!local.error.empty()) {
        errors["ollama"] = local.error;
    }

    std::map<std::string, Remote> cloud;
    for (auto& [name, future] : cloud_futures) {
        cloud[name] = future.get();
    }
    for (const auto& provider : providers) {
        const std::string name = provider.name;
        if (!has_key[name]) {
            add(presets(name));
            continue;
        }
        const Remote& remote = cloud[name];
        if (!remote.error.empty()) {
            errors[name] = remote.error;
        }
        if (remote.entries.empty() && !remote.error.empty()) {
            // The key exists but the list could not be fetched: offer the
            // suggestions as usable, the user can still try them.
            json fallback = presets(name);
            for (auto& item : fallback) {
                item["available"] = true;
            }
            add(fallback);
        } else {
            add(remote.entries);
        }
    }
    for (const auto& item : custom) {
        models.push_back(item);
    }

    return {{"default", config::effective("default_model")}, {"models", models}, {"errors", errors}};
}

InfoResult info(const std::string& id) {
    InfoResult result;
    if (id.empty() || id.size() > 200) {
        result.http = 400;
        result.body = {{"error", {{"message", "id: 1-200 characters expected"}}}};
        return result;
    }

    const models::Route route = models::resolve(id);

    // Local models: ask Ollama for the details a list does not carry.
    if (route.provider == "ollama") {
        const std::string host = ollama::host();
        const std::string cache_key = host + "|" + route.upstream_model;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto hit = g_info.find(cache_key);
            if (hit != g_info.end() && Clock::now() - hit->second.first < kInfoTtl) {
                result.body = hit->second.second;
                return result;
            }
        }
        httplib::Client client(host);
        client.set_connection_timeout(std::chrono::milliseconds(kFetchTimeoutMs));
        client.set_read_timeout(std::chrono::milliseconds(kFetchTimeoutMs));
        json request = {{"model", route.upstream_model.empty() ? ollama::default_model() : route.upstream_model}};
        auto res = client.Post("/api/show", request.dump(), "application/json");
        if (!res) {
            result.http = 502;
            result.body = {{"error", {{"message", "ollama: " + httplib::to_string(res.error())}}}};
            return result;
        }
        json parsed = json::parse(res->body, nullptr, false);
        if (res->status == 404) {
            result.http = 404;
            result.body = {{"error", {{"message", "ollama does not know the model '" + id + "'"}}}};
            return result;
        }
        if (res->status != 200 || parsed.is_discarded()) {
            result.http = 502;
            result.body = {{"error", {{"message", "ollama: HTTP " + std::to_string(res->status)}}}};
            return result;
        }
        result.body = {{"id", id},
                       {"category", route.custom ? "custom" : "local"},
                       {"provider", "ollama"},
                       {"details", parse_ollama_show(parsed)}};
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_info.size() >= kMaxInfoCacheEntries) {
            g_info.clear();
        }
        g_info[cache_key] = {Clock::now(), result.body};
        return result;
    }

    // Everything else: what the (cached) list already knows.
    const json listing = list(false);
    for (const auto& item : listing["models"]) {
        if (item.value("id", "") == id) {
            result.body = {{"id", id},
                           {"category", item.value("category", "")},
                           {"provider", item.value("provider", "")},
                           {"details", item.value("details", json::object())}};
            return result;
        }
    }
    result.http = 404;
    result.body = {{"error", {{"message", "unknown model '" + id + "'"}}}};
    return result;
}

}  // namespace loomai::catalog
