#include "catalog_parse.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "models.hpp"

using json = nlohmann::json;

namespace loomai::catalog {

namespace {

constexpr size_t kMaxDescriptionLength = 300;

bool starts_with_any(const std::string& text, const std::vector<std::string>& prefixes) {
    for (const auto& prefix : prefixes) {
        if (text.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string string_field(const json& object, const char* key) {
    return (object.is_object() && object.contains(key) && object[key].is_string()) ? object[key].get<std::string>()
                                                                                   : std::string();
}

long long integer_field(const json& object, const char* key) {
    if (object.is_object() && object.contains(key) && object[key].is_number_integer()) {
        return object[key].get<long long>();
    }
    return 0;
}

json entry(const std::string& id, const std::string& name, const char* category, const char* provider,
           json details) {
    return {{"id", id},
            {"name", name.empty() ? id : name},
            {"category", category},
            {"provider", provider},
            {"available", true},
            {"details", std::move(details)}};
}

// Shared empty object so optional sub-objects can be read through a const
// reference without building a temporary.
const json& empty_object() {
    static const json kEmpty = json::object();
    return kEmpty;
}

std::string truncated(std::string text) {
    if (text.size() > kMaxDescriptionLength) {
        text.resize(kMaxDescriptionLength);
        text += "...";
    }
    return text;
}

}  // namespace

std::string iso_date(long long epoch_seconds) {
    if (epoch_seconds <= 0) {
        return "";
    }
    // Civil-from-days (Howard Hinnant): pure arithmetic, so no gmtime() data race
    // when several list fetches run on different threads.
    long long days = epoch_seconds / 86400;
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long year = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2) {
        ++year;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld", year, month, day);
    return buffer;
}

json parse_ollama_tags(const json& response) {
    json out = json::array();
    if (!response.is_object() || !response.contains("models") || !response["models"].is_array()) {
        return out;
    }
    for (const auto& model : response["models"]) {
        std::string id = string_field(model, "name");
        if (id.empty()) {
            id = string_field(model, "model");
        }
        if (id.empty()) {
            continue;
        }
        json details = json::object();
        const json& info = model.contains("details") ? model["details"] : empty_object();
        for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
                 {"family", "family"}, {"parameter_size", "parameter_size"}, {"quantization_level", "quantization"},
                 {"format", "format"}}) {
            std::string value = string_field(info, from);
            if (!value.empty()) {
                details[to] = value;
            }
        }
        if (long long size = integer_field(model, "size"); size > 0) {
            details["size_bytes"] = size;
        }
        std::string modified = string_field(model, "modified_at");
        if (modified.size() >= 10) {
            details["modified"] = modified.substr(0, 10);
        }
        out.push_back(entry(id, id, "local", "ollama", std::move(details)));
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a["id"].get<std::string>() < b["id"].get<std::string>();
    });
    return out;
}

json parse_openai_models(const json& response) {
    json out = json::array();
    if (!response.is_object() || !response.contains("data") || !response["data"].is_array()) {
        return out;
    }
    const auto& prefixes = models::prefixes("openai");
    std::vector<std::pair<long long, json>> found;
    for (const auto& model : response["data"]) {
        const std::string id = string_field(model, "id");
        if (id.empty() || !starts_with_any(id, prefixes)) {
            continue;
        }
        if (contains_any(id, {"embedding", "whisper", "tts", "transcribe", "realtime", "audio", "image",
                              "moderation", "dall-e", "instruct", "diarize"})) {
            continue;
        }
        json details = json::object();
        const long long created = integer_field(model, "created");
        if (created > 0) {
            details["created"] = iso_date(created);
        }
        if (std::string owner = string_field(model, "owned_by"); !owner.empty()) {
            details["owned_by"] = owner;
        }
        found.emplace_back(created, entry(id, id, "cloud", "openai", std::move(details)));
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& item : found) {
        out.push_back(std::move(item.second));
    }
    return out;
}

json parse_anthropic_models(const json& response) {
    json out = json::array();
    if (!response.is_object() || !response.contains("data") || !response["data"].is_array()) {
        return out;
    }
    for (const auto& model : response["data"]) {
        const std::string id = string_field(model, "id");
        if (id.empty()) {
            continue;
        }
        json details = json::object();
        std::string created = string_field(model, "created_at");
        if (created.size() >= 10) {
            details["created"] = created.substr(0, 10);
        }
        out.push_back(entry(id, string_field(model, "display_name"), "cloud", "anthropic", std::move(details)));
    }
    return out;  // the API already lists newest first
}

json parse_gemini_models(const json& response) {
    json out = json::array();
    if (!response.is_object() || !response.contains("models") || !response["models"].is_array()) {
        return out;
    }
    const auto& prefixes = models::prefixes("gemini");
    for (const auto& model : response["models"]) {
        std::string id = string_field(model, "name");
        if (id.rfind("models/", 0) == 0) {
            id = id.substr(7);
        }
        if (id.empty() || !starts_with_any(id, prefixes)) {
            continue;
        }
        bool can_generate = false;
        if (model.contains("supportedGenerationMethods") && model["supportedGenerationMethods"].is_array()) {
            for (const auto& method : model["supportedGenerationMethods"]) {
                if (method.is_string() && method.get<std::string>() == "generateContent") {
                    can_generate = true;
                }
            }
        }
        if (!can_generate) {
            continue;
        }
        json details = json::object();
        if (std::string description = string_field(model, "description"); !description.empty()) {
            details["description"] = truncated(description);
        }
        if (long long limit = integer_field(model, "inputTokenLimit"); limit > 0) {
            details["context_length"] = limit;
        }
        if (long long limit = integer_field(model, "outputTokenLimit"); limit > 0) {
            details["max_output_tokens"] = limit;
        }
        if (std::string version = string_field(model, "version"); !version.empty()) {
            details["version"] = version;
        }
        out.push_back(entry(id, string_field(model, "displayName"), "cloud", "gemini", std::move(details)));
    }
    return out;
}

json parse_ollama_show(const json& response) {
    json details = json::object();
    if (!response.is_object()) {
        return details;
    }
    const json& info = response.contains("details") ? response["details"] : empty_object();
    for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
             {"family", "family"}, {"parameter_size", "parameter_size"}, {"quantization_level", "quantization"}}) {
        std::string value = string_field(info, from);
        if (!value.empty()) {
            details[to] = value;
        }
    }
    if (response.contains("model_info") && response["model_info"].is_object()) {
        for (const auto& item : response["model_info"].items()) {
            const std::string& key = item.key();
            if (key.size() > 15 && key.compare(key.size() - 15, 15, ".context_length") == 0 &&
                item.value().is_number_integer()) {
                details["context_length"] = item.value().get<long long>();
            } else if (key == "general.parameter_count" && item.value().is_number_integer()) {
                details["parameter_count"] = item.value().get<long long>();
            } else if (key == "general.architecture" && item.value().is_string()) {
                details["architecture"] = item.value();
            }
        }
    }
    if (response.contains("capabilities") && response["capabilities"].is_array()) {
        details["capabilities"] = response["capabilities"];
    }
    return details;
}

json presets(const std::string& provider) {
    json out = json::array();
    auto add = [&](std::initializer_list<std::pair<const char*, const char*>> list) {
        for (const auto& preset : list) {
            json e = entry(preset.first, preset.second, "cloud", provider.c_str(), {{"preset", true}});
            e["available"] = false;
            out.push_back(std::move(e));
        }
    };
    if (provider == "openai") {
        add({{"gpt-4o", "GPT-4o"}, {"gpt-4o-mini", "GPT-4o mini"}, {"o3-mini", "o3-mini"}});
    } else if (provider == "anthropic") {
        add({{"claude-opus-5-5", "Claude Opus 5.5"},
             {"claude-sonnet-5-5", "Claude Sonnet 5.5"},
             {"claude-haiku-4-5-20251001", "Claude Haiku 4.5"}});
    } else if (provider == "gemini") {
        add({{"gemini-2.5-pro", "Gemini 2.5 Pro"}, {"gemini-2.5-flash", "Gemini 2.5 Flash"}});
    }
    return out;
}

}  // namespace loomai::catalog
