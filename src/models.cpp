#include "models.hpp"

#include <cstdlib>
#include <initializer_list>

#include "ids.hpp"
#include "net_util.hpp"
#include "secrets.hpp"

using json = nlohmann::json;

namespace loomai::models {

namespace {

constexpr size_t kMaxCustomModels = 100;
constexpr size_t kMaxNameLength = 100;

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        size_t len = (comma == std::string::npos) ? std::string::npos : comma - start;
        std::string part = value.substr(start, len);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return parts;
}

// `env_var`, if set, REPLACES `fallback` wholesale (comma-separated), e.g.
// LOOMAI_PREFIXES_OPENAI=gpt-,o1-,o3-,o4- to pick up a new model family without
// a rebuild. Read once per provider (function-local static).
std::vector<std::string> prefixes_for(const char* env_var, std::initializer_list<const char*> fallback) {
    const char* override_value = std::getenv(env_var);
    if (override_value && *override_value) {
        return split_csv(override_value);
    }
    return std::vector<std::string>(fallback.begin(), fallback.end());
}

bool matches_any(const std::string& model, const std::vector<std::string>& list) {
    for (const auto& prefix : list) {
        if (model.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool valid_provider(const std::string& provider) {
    return provider == "ollama" || provider == "openai" || provider == "anthropic" || provider == "gemini" ||
           provider == "openai-compatible";
}

bool has_control_chars(const std::string& text) {
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

std::string key_name_for_provider(const std::string& provider) {
    if (provider == "openai") return "OPENAI_API_KEY";
    if (provider == "anthropic") return "ANTHROPIC_API_KEY";
    if (provider == "gemini") return "GEMINI_API_KEY";
    return "";
}

Route route_from_custom(const json& record) {
    Route route;
    route.custom = true;
    route.provider = record.value("provider", "ollama");
    const std::string id = record.value("id", "");
    route.upstream_model = record.value("upstream_model", "");
    if (route.upstream_model.empty()) {
        route.upstream_model = id;
    }
    route.label = id;
    if (route.provider == "openai-compatible") {
        std::string origin;
        std::string path;
        if (split_base_url(record.value("base_url", ""), origin, path)) {
            route.base_url = origin;
            route.path_prefix = path;
            route.local = loomai::is_loopback_host(loomai::host_of(origin));
        }
        route.key_name = "custom:" + id;
    } else {
        route.key_name = key_name_for_provider(route.provider);
        route.local = route.provider == "ollama";
    }
    return route;
}

json public_view(const json& record) {
    json view = {{"id", record.value("id", "")},
                 {"name", record.value("name", record.value("id", ""))},
                 {"provider", record.value("provider", "")},
                 {"upstream_model", record.value("upstream_model", "")}};
    if (record.contains("base_url") && record["base_url"].is_string()) {
        view["base_url"] = record["base_url"];
    }
    const std::string id = record.value("id", "");
    view["has_key"] = record.value("provider", "") == "openai-compatible" &&
                      secrets::source("custom:" + id) != "none";
    return view;
}

}  // namespace

const std::vector<std::string>& prefixes(const std::string& provider) {
    static const std::vector<std::string> anthropic = prefixes_for("LOOMAI_PREFIXES_CLAUDE", {"claude-"});
    static const std::vector<std::string> openai =
        prefixes_for("LOOMAI_PREFIXES_OPENAI", {"gpt-", "o1-", "o3-"});
    static const std::vector<std::string> gemini = prefixes_for("LOOMAI_PREFIXES_GEMINI", {"gemini-"});
    static const std::vector<std::string> none;
    if (provider == "anthropic") return anthropic;
    if (provider == "openai") return openai;
    if (provider == "gemini") return gemini;
    return none;
}

bool split_base_url(const std::string& url, std::string& origin, std::string& path_prefix) {
    std::string text = url;
    size_t begin = text.find_first_not_of(" \t\r\n");
    size_t end = text.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return false;
    }
    text = text.substr(begin, end - begin + 1);

    size_t scheme = text.find("://");
    if (scheme == std::string::npos) {
        return false;
    }
    size_t slash = text.find('/', scheme + 3);
    std::string origin_part = text.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "" : text.substr(slash);
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    if (!config::normalize_http_origin(origin_part)) {
        return false;
    }
    for (unsigned char c : path) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '~' || c == '/' || c == '-';
        if (!ok) {
            return false;
        }
    }
    if (path.find("..") != std::string::npos || path.find("//") != std::string::npos) {
        return false;
    }
    origin = origin_part;
    path_prefix = path;
    return true;
}

Route resolve(const std::string& model) {
    if (!model.empty()) {
        Route found;
        bool hit = false;
        config::inspect([&](const json& doc) {
            if (!doc.contains("custom_models") || !doc["custom_models"].is_array()) {
                return;
            }
            for (const auto& record : doc["custom_models"]) {
                if (record.is_object() && record.value("id", "") == model) {
                    found = route_from_custom(record);
                    hit = true;
                    return;
                }
            }
        });
        if (hit) {
            return found;
        }
    }

    Route route;
    route.upstream_model = model;
    if (matches_any(model, prefixes("anthropic"))) {
        route.provider = "anthropic";
    } else if (matches_any(model, prefixes("openai"))) {
        route.provider = "openai";
    } else if (matches_any(model, prefixes("gemini"))) {
        route.provider = "gemini";
    } else {
        route.provider = "ollama";
        route.local = true;
    }
    route.label = route.provider;
    route.key_name = key_name_for_provider(route.provider);
    return route;
}

json custom_list() {
    json out = json::array();
    config::inspect([&](const json& doc) {
        if (!doc.contains("custom_models") || !doc["custom_models"].is_array()) {
            return;
        }
        for (const auto& record : doc["custom_models"]) {
            if (record.is_object()) {
                out.push_back(public_view(record));
            }
        }
    });
    return out;
}

config::Status custom_add(const json& body, json& created) {
    if (!body.is_object()) {
        return {400, "expected a JSON object"};
    }
    for (const auto& item : body.items()) {
        const std::string& key = item.key();
        if (key != "id" && key != "provider" && key != "name" && key != "upstream_model" && key != "base_url" &&
            key != "api_key") {
            return {400, "unknown field '" + key + "'"};
        }
        if (!item.value().is_string() && !item.value().is_null()) {
            return {400, key + ": expected a string"};
        }
    }
    auto text = [&](const char* key) {
        return (body.contains(key) && body[key].is_string()) ? body[key].get<std::string>() : std::string();
    };

    const std::string id = text("id");
    const std::string provider = text("provider");
    std::string name = text("name");
    std::string upstream = text("upstream_model");
    std::string base_url = text("base_url");
    const std::string api_key = text("api_key");

    if (!valid_model_id(id)) {
        return {400, "id: 1-100 characters from letters, digits and . _ : / -"};
    }
    if (!valid_provider(provider)) {
        return {400, "provider: one of ollama, openai, anthropic, gemini, openai-compatible"};
    }
    if (name.size() > kMaxNameLength || has_control_chars(name)) {
        return {400, "name: at most 100 characters, no control characters"};
    }
    if (name.empty()) {
        name = id;
    }
    if (!upstream.empty() && !valid_model_id(upstream)) {
        return {400, "upstream_model: 1-100 characters from letters, digits and . _ : / -"};
    }

    json record = {{"id", id}, {"name", name}, {"provider", provider}};
    if (!upstream.empty()) {
        record["upstream_model"] = upstream;
    }
    if (provider == "openai-compatible") {
        std::string origin;
        std::string path;
        if (!split_base_url(base_url, origin, path)) {
            return {400, "base_url: expected an http(s) address like http://127.0.0.1:1234/v1"};
        }
        record["base_url"] = origin + path;
    } else {
        if (!base_url.empty()) {
            return {400, "base_url: only for provider openai-compatible"};
        }
        if (!api_key.empty()) {
            return {400, "api_key: only for provider openai-compatible (the other providers use their own key)"};
        }
    }

    config::Status added = config::mutate([&](json& doc) -> config::Status {
        if (!doc.contains("custom_models") || !doc["custom_models"].is_array()) {
            doc["custom_models"] = json::array();
        }
        auto& list = doc["custom_models"];
        if (list.size() >= kMaxCustomModels) {
            return {400, "at most 100 custom models"};
        }
        for (const auto& existing : list) {
            if (existing.is_object() && existing.value("id", "") == id) {
                return {409, "a custom model with id '" + id + "' already exists"};
            }
        }
        list.push_back(record);
        return {};
    });
    if (!added.ok()) {
        return added;
    }

    if (!api_key.empty()) {
        config::Status stored = secrets::set("custom:" + id, api_key);
        if (!stored.ok()) {
            // Do not leave a model behind whose key could not be saved.
            config::mutate([&](json& doc) -> config::Status {
                auto& list = doc["custom_models"];
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].value("id", "") == id) {
                        list.erase(i);
                        break;
                    }
                }
                return {};
            });
            return stored;
        }
    }
    created = public_view(record);
    return {};
}

config::Status custom_remove(const std::string& id) {
    if (!valid_model_id(id)) {
        return {400, "not a valid model id"};
    }
    config::Status removed = config::mutate([&](json& doc) -> config::Status {
        if (!doc.contains("custom_models") || !doc["custom_models"].is_array()) {
            return {404, "unknown custom model '" + id + "'"};
        }
        auto& list = doc["custom_models"];
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].is_object() && list[i].value("id", "") == id) {
                list.erase(i);
                return {};
            }
        }
        return {404, "unknown custom model '" + id + "'"};
    });
    if (!removed.ok()) {
        return removed;
    }
    return secrets::remove("custom:" + id);
}

}  // namespace loomai::models
