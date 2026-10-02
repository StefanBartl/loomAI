#include <filesystem>

#include "config.hpp"
#include "minitest.hpp"
#include "secrets.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace secrets = loomai::secrets;

namespace {

// Fresh config dir, file backend (never the real Credential Manager), no
// provider keys in the environment.
struct Sandbox {
    fs::path dir;
    explicit Sandbox(const std::string& tag) : dir(minitest::make_temp_dir(tag)) {
        minitest::set_env("LOOMAI_CONFIG_DIR", dir.string());
        minitest::set_env("LOOMAI_SECRET_BACKEND", "file");
        for (const char* name : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY"}) {
            minitest::set_env(name, "");
        }
        loomai::config::reload();
        secrets::reload();
    }
    ~Sandbox() {
        for (const char* name : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY"}) {
            minitest::set_env(name, "");
        }
        minitest::set_env("LOOMAI_SECRET_BACKEND", "");
        loomai::config::reload();
        secrets::reload();
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    fs::path file() const { return dir / "secrets.json"; }
};

}  // namespace

TEST(secret_names_are_an_allowlist) {
    for (const char* ok : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY", "custom:my-model",
                           "custom:meta-llama/llama-3.1-8b", "custom:llama3.1:8b-q5_K_M"}) {
        CHECK(secrets::valid_name(ok));
    }
    for (const char* bad : {"", "PATH", "openai_api_key", "OPENAI_API_KEY ", "custom:", "custom:has space",
                            "custom:quote\"", "custom:" "x" "\n", "other:thing"}) {
        CHECK(!secrets::valid_name(bad));
    }
}

TEST(set_get_remove_round_trip_through_the_file_backend) {
    Sandbox box("roundtrip");
    CHECK_EQ(secrets::backend_name(), std::string("file"));
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string(""));
    CHECK_EQ(secrets::source("OPENAI_API_KEY"), std::string("none"));

    CHECK(secrets::set("OPENAI_API_KEY", "sk-test-123").ok());
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string("sk-test-123"));
    CHECK_EQ(secrets::source("OPENAI_API_KEY"), std::string("store"));

    secrets::reload();  // drop the cache: it must come back from disk
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string("sk-test-123"));

    CHECK(secrets::remove("OPENAI_API_KEY").ok());
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string(""));
    CHECK_EQ(secrets::source("OPENAI_API_KEY"), std::string("none"));
    CHECK(secrets::remove("OPENAI_API_KEY").ok());  // idempotent
}

TEST(custom_model_keys_work_like_provider_keys) {
    Sandbox box("custom");
    CHECK(secrets::set("custom:my-model", "abc123").ok());
    CHECK_EQ(secrets::get("custom:my-model"), std::string("abc123"));
    CHECK_EQ(secrets::get("custom:other"), std::string(""));
}

TEST(values_are_trimmed_and_validated) {
    Sandbox box("values");
    CHECK(secrets::set("GEMINI_API_KEY", "  AIza-key\n").ok());
    CHECK_EQ(secrets::get("GEMINI_API_KEY"), std::string("AIza-key"));  // a pasted trailing newline is harmless
    CHECK_EQ(secrets::set("GEMINI_API_KEY", "").http, 400);
    CHECK_EQ(secrets::set("GEMINI_API_KEY", "   ").http, 400);
    CHECK_EQ(secrets::set("GEMINI_API_KEY", "two words").http, 400);
    CHECK_EQ(secrets::set("GEMINI_API_KEY", std::string("a\tb")).http, 400);
    CHECK_EQ(secrets::set("GEMINI_API_KEY", std::string(2049, 'k')).http, 400);
    CHECK_EQ(secrets::set("NOT_A_KEY", "x").http, 400);
    CHECK_EQ(secrets::get("GEMINI_API_KEY"), std::string("AIza-key"));  // failed writes changed nothing
}

TEST(environment_wins_and_blocks_set_but_not_remove) {
    Sandbox box("env");
    CHECK(secrets::set("ANTHROPIC_API_KEY", "stored-value").ok());
    minitest::set_env("ANTHROPIC_API_KEY", "env-value");
    CHECK_EQ(secrets::get("ANTHROPIC_API_KEY"), std::string("env-value"));
    CHECK_EQ(secrets::source("ANTHROPIC_API_KEY"), std::string("env"));
    CHECK_EQ(secrets::set("ANTHROPIC_API_KEY", "new").http, 409);
    CHECK(secrets::remove("ANTHROPIC_API_KEY").ok());  // only the stored copy goes
    CHECK_EQ(secrets::get("ANTHROPIC_API_KEY"), std::string("env-value"));
    minitest::set_env("ANTHROPIC_API_KEY", "");
    CHECK_EQ(secrets::get("ANTHROPIC_API_KEY"), std::string(""));  // and it really is gone underneath
}

TEST(keys_json_reports_presence_only_never_a_value) {
    Sandbox box("json");
    CHECK(secrets::set("OPENAI_API_KEY", "sk-super-secret-value").ok());
    minitest::set_env("GEMINI_API_KEY", "gemini-env-secret");
    json out = secrets::keys_json();
    const std::string text = out.dump();
    CHECK(text.find("sk-super-secret-value") == std::string::npos);
    CHECK(text.find("gemini-env-secret") == std::string::npos);
    CHECK_EQ(out["store"].get<std::string>(), std::string("file"));
    CHECK(out["keys"]["OPENAI_API_KEY"]["set"].get<bool>());
    CHECK_EQ(out["keys"]["OPENAI_API_KEY"]["source"].get<std::string>(), std::string("store"));
    CHECK(!out["keys"]["OPENAI_API_KEY"]["locked"].get<bool>());
    CHECK_EQ(out["keys"]["GEMINI_API_KEY"]["source"].get<std::string>(), std::string("env"));
    CHECK(out["keys"]["GEMINI_API_KEY"]["locked"].get<bool>());
    CHECK(!out["keys"]["ANTHROPIC_API_KEY"]["set"].get<bool>());
    CHECK_EQ(out["keys"].size(), static_cast<size_t>(3));  // custom keys are not listed
}

TEST(a_corrupt_secrets_file_is_moved_aside_and_the_store_recovers) {
    Sandbox box("corrupt");
    minitest::write_file(box.file(), "not json at all");
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string(""));
    CHECK(secrets::set("OPENAI_API_KEY", "fresh").ok());
    CHECK_EQ(secrets::get("OPENAI_API_KEY"), std::string("fresh"));
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(box.dir)) {
        if (entry.path().filename().string().rfind("secrets.json.corrupt-", 0) == 0) {
            kept = true;
            CHECK_EQ(minitest::read_file(entry.path()), std::string("not json at all"));
        }
    }
    CHECK(kept);
}

#ifndef _WIN32
TEST(the_secrets_file_is_owner_only) {
    Sandbox box("perms");
    CHECK(secrets::set("OPENAI_API_KEY", "k").ok());
    auto perms = fs::status(box.file()).permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}
#endif

#ifdef _WIN32
// The one test that touches the real Windows Credential Manager: it is the only
// way to know the CredWriteW/CredReadW code works. It uses a throw-away name and
// always deletes it again.
TEST(windows_credential_manager_round_trip) {
    Sandbox box("wincred");
    minitest::set_env("LOOMAI_SECRET_BACKEND", "");  // back to the default backend
    secrets::reload();
    CHECK_EQ(secrets::backend_name(), std::string("wincred"));

    const std::string name = "custom:loomai-unit-test-" + std::to_string(::std::rand());
    CHECK(secrets::set(name, "wincred-secret-value").ok());
    secrets::reload();  // force a real read from the Credential Manager
    CHECK_EQ(secrets::get(name), std::string("wincred-secret-value"));
    CHECK_EQ(secrets::source(name), std::string("store"));
    CHECK(!fs::exists(box.file()));  // and nothing landed in a plain file

    CHECK(secrets::remove(name).ok());
    secrets::reload();
    CHECK_EQ(secrets::get(name), std::string(""));
    CHECK(secrets::remove(name).ok());  // deleting an absent credential is fine
}
#endif
