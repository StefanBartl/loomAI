#include "secrets.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>

#include "ids.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace loomai::secrets {

namespace {

constexpr size_t kMaxValueBytes = 2048;  // Credential Manager allows 2560; keys are far shorter
constexpr const char* kBuiltinNames[] = {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY"};

std::mutex g_mutex;
// name -> stored value (nullopt = looked up, nothing there). Reading the
// Credential Manager on every request would be wasteful; set()/remove() keep
// this in step, and nothing else writes the store while the server runs.
std::map<std::string, std::optional<std::string>> g_cache;

bool is_builtin(const std::string& name) {
    for (const char* builtin : kBuiltinNames) {
        if (name == builtin) {
            return true;
        }
    }
    return false;
}

// Environment variable that provides this secret, or nullptr (custom keys have none).
const char* env_name(const std::string& name) {
    return is_builtin(name) ? name.c_str() : nullptr;
}

std::string env_value(const std::string& name) {
    if (!env_name(name)) {
        return "";
    }
    const char* value = std::getenv(name.c_str());
    return (value && *value) ? std::string(value) : std::string();
}

bool use_file_backend() {
#ifdef _WIN32
    const char* forced = std::getenv("LOOMAI_SECRET_BACKEND");
    return forced && std::string(forced) == "file";
#else
    return true;
#endif
}

// ---------------------------------------------------------------------------
// File backend: <config dir>/secrets.json, owner-only on POSIX.
// ---------------------------------------------------------------------------

fs::path secrets_file() {
    return fs::u8path(config::directory()) / "secrets.json";
}

// Caller holds g_mutex. Always returns an object with a "secrets" object.
json load_file_locked() {
    json empty = {{"version", 1}, {"secrets", json::object()}};
    const fs::path path = secrets_file();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return empty;
    }

    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("secrets") ||
        !parsed["secrets"].is_object()) {
        // Do not overwrite something we cannot read; keep it for the user.
        fs::path aside = path;
        aside += ".corrupt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        fs::rename(path, aside, ec);
        std::cerr << "loomai: the secrets file is not valid; "
                  << (ec ? "could not move it aside" : "moved it to " + aside.u8string()) << "\n";
        return empty;
    }
    return parsed;
}

config::Status save_file_locked(const json& doc) {
    const fs::path path = secrets_file();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return {500, "cannot create the config directory: " + ec.message()};
    }

    fs::path tmp = path;
    tmp += ".tmp";
    {
        // Create it empty and restrict it *before* any secret goes in, so there is
        // no moment where the key sits in a world-readable file.
        std::ofstream create(tmp, std::ios::binary | std::ios::trunc);
        if (!create) {
            return {500, "cannot write the secrets file"};
        }
    }
#ifndef _WIN32
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return {500, "cannot restrict the secrets file to its owner"};
    }
#endif
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(2) << "\n";
        out.flush();
        if (!out) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return {500, "failed while writing the secrets file"};
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(path, ignored);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return {500, "cannot replace the secrets file: " + ec.message()};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Windows Credential Manager backend.
// ---------------------------------------------------------------------------
#ifdef _WIN32

std::string target_name(const std::string& name) {
    return "loomai/" + name;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], length);
    return wide;
}

std::optional<std::string> wincred_read(const std::string& name) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(widen(target_name(name)).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return std::nullopt;  // ERROR_NOT_FOUND in the normal case
    }
    std::string value(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return value;
}

config::Status wincred_write(const std::string& name, const std::string& value) {
    std::wstring target = widen(target_name(name));
    std::wstring user = L"loomai";
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = &target[0];
    credential.UserName = &user[0];
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        return {500, "the Windows Credential Manager refused the write (error " + std::to_string(GetLastError()) + ")"};
    }
    return {};
}

config::Status wincred_delete(const std::string& name) {
    if (!CredDeleteW(widen(target_name(name)).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        return {500, "the Windows Credential Manager refused the delete (error " + std::to_string(GetLastError()) + ")"};
    }
    return {};
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Backend dispatch. Caller holds g_mutex.
// ---------------------------------------------------------------------------

std::optional<std::string> store_read_locked(const std::string& name) {
#ifdef _WIN32
    if (!use_file_backend()) {
        return wincred_read(name);
    }
#endif
    json doc = load_file_locked();
    if (doc["secrets"].contains(name) && doc["secrets"][name].is_string()) {
        return doc["secrets"][name].get<std::string>();
    }
    return std::nullopt;
}

config::Status store_write_locked(const std::string& name, const std::string& value) {
#ifdef _WIN32
    if (!use_file_backend()) {
        return wincred_write(name, value);
    }
#endif
    json doc = load_file_locked();
    doc["secrets"][name] = value;
    return save_file_locked(doc);
}

config::Status store_delete_locked(const std::string& name) {
#ifdef _WIN32
    if (!use_file_backend()) {
        return wincred_delete(name);
    }
#endif
    json doc = load_file_locked();
    if (!doc["secrets"].contains(name)) {
        return {};
    }
    doc["secrets"].erase(name);
    return save_file_locked(doc);
}

// Caller holds g_mutex.
std::string stored_value_locked(const std::string& name) {
    auto hit = g_cache.find(name);
    if (hit == g_cache.end()) {
        hit = g_cache.emplace(name, store_read_locked(name)).first;
    }
    return hit->second.value_or("");
}

std::string trimmed(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

bool valid_name(const std::string& name) {
    if (is_builtin(name)) {
        return true;
    }
    static const std::string prefix = "custom:";
    return name.rfind(prefix, 0) == 0 && valid_model_id(name.substr(prefix.size()));
}

std::string get(const std::string& name) {
    if (!valid_name(name)) {
        return "";
    }
    std::string from_env = env_value(name);
    if (!from_env.empty()) {
        return from_env;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return stored_value_locked(name);
}

std::string source(const std::string& name) {
    if (!valid_name(name)) {
        return "none";
    }
    if (!env_value(name).empty()) {
        return "env";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return stored_value_locked(name).empty() ? "none" : "store";
}

config::Status set(const std::string& name, const std::string& raw_value) {
    if (!valid_name(name)) {
        return {400, "not a valid secret name"};
    }
    const std::string value = trimmed(raw_value);
    if (value.empty()) {
        return {400, "the value is empty"};
    }
    if (value.size() > kMaxValueBytes) {
        return {400, "the value is longer than 2048 bytes"};
    }
    for (unsigned char c : value) {
        if (c <= 0x20 || c == 0x7f) {
            return {400, "the value must not contain whitespace or control characters"};
        }
    }
    if (!env_value(name).empty()) {
        return {409, name + " is provided by an environment variable and cannot be changed here"};
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    config::Status written = store_write_locked(name, value);
    if (written.ok()) {
        g_cache[name] = value;
    }
    return written;
}

config::Status remove(const std::string& name) {
    if (!valid_name(name)) {
        return {400, "not a valid secret name"};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    config::Status deleted = store_delete_locked(name);
    if (deleted.ok()) {
        g_cache[name] = std::nullopt;
    }
    return deleted;
}

json keys_json() {
    json keys = json::object();
    for (const char* name : kBuiltinNames) {
        const std::string where = source(name);
        keys[name] = {{"set", where != "none"}, {"source", where}, {"locked", where == "env"}};
    }
    return {{"keys", keys}, {"store", backend_name()}};
}

std::string backend_name() {
    return use_file_backend() ? "file" : "wincred";
}

void reload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
}

}  // namespace loomai::secrets
