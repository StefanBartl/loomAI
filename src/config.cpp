#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace loomai::config {

namespace {

constexpr int kVersion = 1;
constexpr size_t kMaxModelNameLength = 200;

struct SettingDef {
    const char* key;
    const char* env;       // environment variable that locks the setting, or nullptr
    const char* fallback;  // built-in default
};

constexpr SettingDef kSettings[] = {
    {"theme", nullptr, "system"},
    {"default_model", "LOOMAI_DEFAULT_MODEL", "llama3.1:8b-q5_K_M"},
    {"ollama_host", "LOOMAI_OLLAMA_HOST", "http://127.0.0.1:11434"},
    {"language", nullptr, "de"},
};

std::mutex g_mutex;
bool g_loaded = false;
json g_doc;

const SettingDef* find_setting(const std::string& key) {
    for (const auto& def : kSettings) {
        if (key == def.key) {
            return &def;
        }
    }
    return nullptr;
}

// Non-empty value of an environment variable, or nullptr. An empty variable
// counts as unset (the usual way to "clear" one).
const char* env_value(const char* name) {
    if (!name) {
        return nullptr;
    }
    const char* value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

#ifdef _WIN32
// Paths are read through the wide API: getenv() hands back the ANSI code page,
// which turns a user name with an umlaut into a path that does not exist.
fs::path wide_env_path(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return (value && *value) ? fs::path(value) : fs::path();
}
#endif

fs::path directory_path() {
#ifdef _WIN32
    if (auto dir = wide_env_path(L"LOOMAI_CONFIG_DIR"); !dir.empty()) {
        return dir;
    }
    if (auto appdata = wide_env_path(L"APPDATA"); !appdata.empty()) {
        return appdata / "loomai";
    }
    if (auto profile = wide_env_path(L"USERPROFILE"); !profile.empty()) {
        return profile / "AppData" / "Roaming" / "loomai";
    }
    return fs::path("loomai-config");
#else
    if (const char* dir = env_value("LOOMAI_CONFIG_DIR")) {
        return fs::path(dir);
    }
    if (const char* xdg = env_value("XDG_CONFIG_HOME")) {
        return fs::path(xdg) / "loomai";
    }
    if (const char* home = env_value("HOME")) {
#ifdef __APPLE__
        return fs::path(home) / "Library" / "Application Support" / "loomai";
#else
        return fs::path(home) / ".config" / "loomai";
#endif
    }
    return fs::path("loomai-config");
#endif
}

fs::path file_path() {
    return directory_path() / "config.json";
}

json fresh_doc() {
    return json{{"version", kVersion}, {"settings", json::object()}};
}

// Caller holds g_mutex.
void load_locked() {
    g_loaded = true;
    g_doc = fresh_doc();

    const fs::path path = file_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;
    }

    // Read in its own scope: Windows refuses to rename a file that is still open,
    // and the corrupt-file path below renames it.
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // Never overwrite what we cannot read: it may be the user's only copy.
        fs::path aside = path;
        aside += ".corrupt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        fs::rename(path, aside, ec);
        std::cerr << "loomai: " << path.u8string() << " is not valid JSON; "
                  << (ec ? "could not move it aside" : "moved it to " + aside.u8string())
                  << ", using defaults\n";
        return;
    }

    g_doc = std::move(parsed);
    if (!g_doc.contains("settings") || !g_doc["settings"].is_object()) {
        g_doc["settings"] = json::object();
    }
    if (!g_doc.contains("version")) {
        g_doc["version"] = kVersion;
    }
}

void ensure_loaded_locked() {
    if (!g_loaded) {
        load_locked();
    }
}

// Caller holds g_mutex. Temp file + rename, so a crash mid-write never leaves a
// half-written config.json behind.
Status save_locked() {
    const fs::path path = file_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return {500, "cannot create config directory: " + ec.message()};
    }

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return {500, "cannot write " + tmp.u8string()};
        }
        out << g_doc.dump(2) << "\n";
        out.flush();
        if (!out) {
            return {500, "failed while writing " + tmp.u8string()};
        }
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        // Some platforms refuse to rename over an existing file.
        std::error_code ignored;
        fs::remove(path, ignored);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return {500, "cannot replace " + path.u8string() + ": " + ec.message()};
    }
    return {};
}

std::string trimmed(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Validates and normalises one setting value in place.
Status validate(const std::string& key, std::string& value) {
    if (key == "theme") {
        if (value != "system" && value != "light" && value != "dark") {
            return {400, "theme: must be one of system, light, dark"};
        }
    } else if (key == "language") {
        if (value != "de" && value != "en") {
            return {400, "language: must be one of de, en"};
        }
    } else if (key == "default_model") {
        value = trimmed(value);
        if (value.size() > kMaxModelNameLength) {
            return {400, "default_model: longer than 200 characters"};
        }
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7f) {
                return {400, "default_model: must not contain control characters"};
            }
        }
    } else if (key == "ollama_host") {
        if (!normalize_http_origin(value)) {
            return {400, "ollama_host: expected an http(s) address like http://127.0.0.1:11434"};
        }
    }
    return {};
}

}  // namespace

bool normalize_http_origin(std::string& url) {
    url = trimmed(url);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    std::string rest;
    if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else if (url.rfind("https://", 0) == 0) {
        rest = url.substr(8);
    } else {
        return false;
    }
    if (rest.empty()) {
        return false;
    }
    // An origin only: no path, query, fragment or credentials, no whitespace.
    for (unsigned char c : rest) {
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' || c == '@' || c == '\\') {
            return false;
        }
    }

    std::string host;
    std::string port;
    if (rest[0] == '[') {
        size_t close = rest.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        host = rest.substr(1, close - 1);
        std::string tail = rest.substr(close + 1);
        if (!tail.empty()) {
            if (tail[0] != ':' || tail.size() == 1) {
                return false;
            }
            port = tail.substr(1);
        }
        for (unsigned char c : host) {
            if (!std::isxdigit(c) && c != ':' && c != '.') {
                return false;
            }
        }
    } else {
        size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            host = rest;
        } else {
            host = rest.substr(0, colon);
            port = rest.substr(colon + 1);
            if (port.empty()) {
                return false;
            }
        }
        if (host.empty()) {
            return false;
        }
        for (unsigned char c : host) {
            if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') {
                return false;
            }
        }
    }

    if (!port.empty()) {
        if (port.size() > 5) {
            return false;
        }
        for (unsigned char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        int number = std::stoi(port);
        if (number < 1 || number > 65535) {
            return false;
        }
    }
    return true;
}

std::string directory() {
    return directory_path().u8string();
}

std::string effective(const std::string& key) {
    const SettingDef* def = find_setting(key);
    if (!def) {
        return "";
    }
    if (const char* from_env = env_value(def->env)) {
        return from_env;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    const json& stored = g_doc["settings"];
    if (stored.contains(key) && stored[key].is_string()) {
        return stored[key].get<std::string>();
    }
    return def->fallback;
}

json settings_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    json out = json::object();
    for (const auto& def : kSettings) {
        json entry;
        entry["default"] = def.fallback;
        if (const char* from_env = env_value(def.env)) {
            entry["value"] = from_env;
            entry["source"] = "env";
            entry["locked"] = true;
            entry["env"] = def.env;
        } else if (g_doc["settings"].contains(def.key) && g_doc["settings"][def.key].is_string()) {
            entry["value"] = g_doc["settings"][def.key];
            entry["source"] = "config";
            entry["locked"] = false;
        } else {
            entry["value"] = def.fallback;
            entry["source"] = "default";
            entry["locked"] = false;
        }
        out[def.key] = std::move(entry);
    }
    return out;
}

Status update_settings(const json& patch) {
    if (!patch.is_object()) {
        return {400, "expected a JSON object of settings"};
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();

    json next = g_doc;
    for (const auto& item : patch.items()) {
        const std::string& key = item.key();
        const json& raw = item.value();
        const SettingDef* def = find_setting(key);
        if (!def) {
            return {400, "unknown setting '" + key + "'"};
        }
        if (env_value(def->env)) {
            return {409, key + " is set by the environment variable " + def->env + " and cannot be changed here"};
        }
        if (raw.is_null()) {
            next["settings"].erase(key);
            continue;
        }
        if (!raw.is_string()) {
            return {400, key + ": expected a string"};
        }
        std::string value = raw.get<std::string>();
        Status checked = validate(key, value);
        if (!checked.ok()) {
            return checked;
        }
        if (value.empty()) {
            // An empty default model means "use the built-in default", so store
            // nothing rather than a value that would make requests name no model.
            next["settings"].erase(key);
            continue;
        }
        next["settings"][key] = value;
    }

    json before = std::move(g_doc);
    g_doc = std::move(next);
    Status saved = save_locked();
    if (!saved.ok()) {
        g_doc = std::move(before);
    }
    return saved;
}

json snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();
    return g_doc;
}

Status mutate(const std::function<Status(json&)>& edit) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_loaded_locked();

    json copy = g_doc;
    Status result = edit(copy);
    if (!result.ok()) {
        return result;
    }

    json before = std::move(g_doc);
    g_doc = std::move(copy);
    Status saved = save_locked();
    if (!saved.ok()) {
        g_doc = std::move(before);
    }
    return saved;
}

void reload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = false;
}

}  // namespace loomai::config
