#include <filesystem>

#include "config.hpp"
#include "minitest.hpp"
#include "models.hpp"
#include "secrets.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace models = loomai::models;
namespace secrets = loomai::secrets;

namespace {

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
        minitest::set_env("LOOMAI_SECRET_BACKEND", "");
        loomai::config::reload();
        secrets::reload();
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

json compat(const std::string& id, const std::string& base_url) {
    return {{"id", id}, {"provider", "openai-compatible"}, {"base_url", base_url}};
}

}  // namespace

TEST(split_base_url_accepts_origin_plus_safe_path) {
    struct Case {
        const char* in;
        const char* origin;
        const char* path;
    };
    const Case good[] = {
        {"http://127.0.0.1:1234/v1", "http://127.0.0.1:1234", "/v1"},
        {"https://api.groq.com/openai/v1/", "https://api.groq.com", "/openai/v1"},
        {"https://openrouter.ai/api/v1", "https://openrouter.ai", "/api/v1"},
        {"http://localhost:8000", "http://localhost:8000", ""},
        {"  http://localhost:8000/  ", "http://localhost:8000", ""},
    };
    for (const auto& c : good) {
        std::string origin, path;
        CHECK(models::split_base_url(c.in, origin, path));
        CHECK_EQ(origin, std::string(c.origin));
        CHECK_EQ(path, std::string(c.path));
    }
}

TEST(split_base_url_rejects_unsafe_input) {
    for (const char* bad : {"", "localhost:1234/v1", "ftp://host/v1", "http://host/v1?x=1", "http://host/v1#frag",
                            "http://user@host/v1", "http://host/../etc", "http://host//v1", "http://host/a b",
                            "http://host:99999/v1", "http://host/v1%2e", "http://host/a\\b"}) {
        std::string origin, path;
        CHECK(!models::split_base_url(bad, origin, path));
    }
}

TEST(resolve_keeps_the_prefix_routing_of_the_old_router) {
    Sandbox box("prefix");
    struct Case {
        const char* model;
        const char* provider;
    };
    const Case cases[] = {
        {"claude-sonnet-x", "anthropic"}, {"gpt-4o", "openai"},      {"o1-mini", "openai"},
        {"o3-pro", "openai"},             {"gemini-2.5-pro", "gemini"}, {"llama3.1:8b", "ollama"},
        {"mistral", "ollama"},            {"", "ollama"},
    };
    for (const auto& c : cases) {
        auto route = models::resolve(c.model);
        CHECK_EQ(route.provider, std::string(c.provider));
        CHECK_EQ(route.upstream_model, std::string(c.model));  // passed through untouched
        CHECK(!route.custom);
    }
    CHECK(models::resolve("llama3.1:8b").local);
    CHECK(!models::resolve("gpt-4o").local);
    CHECK_EQ(models::resolve("gpt-4o").key_name, std::string("OPENAI_API_KEY"));
    CHECK_EQ(models::resolve("claude-x").label, std::string("anthropic"));
}

TEST(a_custom_openai_compatible_model_routes_to_its_endpoint) {
    Sandbox box("compat");
    json created;
    auto st = models::custom_add({{"id", "my-lmstudio"}, {"provider", "openai-compatible"},
                                  {"base_url", "http://127.0.0.1:1234/v1/"}, {"name", "LM Studio"},
                                  {"upstream_model", "qwen2.5-7b"}},
                                 created);
    CHECK(st.ok());
    CHECK_EQ(created["id"].get<std::string>(), std::string("my-lmstudio"));
    CHECK_EQ(created["base_url"].get<std::string>(), std::string("http://127.0.0.1:1234/v1"));  // normalised
    CHECK(!created["has_key"].get<bool>());

    auto route = models::resolve("my-lmstudio");
    CHECK(route.custom);
    CHECK_EQ(route.provider, std::string("openai-compatible"));
    CHECK_EQ(route.upstream_model, std::string("qwen2.5-7b"));
    CHECK_EQ(route.base_url, std::string("http://127.0.0.1:1234"));
    CHECK_EQ(route.path_prefix, std::string("/v1"));
    CHECK_EQ(route.key_name, std::string("custom:my-lmstudio"));
    CHECK(route.local);  // loopback: the long default timeout applies
}

TEST(upstream_model_defaults_to_the_id_and_a_remote_endpoint_is_not_local) {
    Sandbox box("remote");
    json created;
    CHECK(models::custom_add(compat("groq-llama", "https://api.groq.com/openai/v1"), created).ok());
    auto route = models::resolve("groq-llama");
    CHECK_EQ(route.upstream_model, std::string("groq-llama"));
    CHECK(!route.local);
}

TEST(a_custom_id_beats_the_prefix_so_a_model_can_be_rerouted) {
    Sandbox box("override");
    CHECK_EQ(models::resolve("gpt-4o").provider, std::string("openai"));
    json created;
    CHECK(models::custom_add(compat("gpt-4o", "https://openrouter.ai/api/v1"), created).ok());
    CHECK_EQ(models::resolve("gpt-4o").provider, std::string("openai-compatible"));
    CHECK_EQ(models::resolve("gpt-4o-mini").provider, std::string("openai"));  // only the exact id moves
}

TEST(the_api_key_goes_to_the_secret_store_and_never_into_the_listing) {
    Sandbox box("key");
    json created;
    auto st = models::custom_add({{"id", "keyed"}, {"provider", "openai-compatible"},
                                  {"base_url", "https://example.com/v1"}, {"api_key", "sk-very-secret"}},
                                 created);
    CHECK(st.ok());
    CHECK(created["has_key"].get<bool>());
    CHECK_EQ(secrets::get("custom:keyed"), std::string("sk-very-secret"));
    CHECK(models::custom_list().dump().find("sk-very-secret") == std::string::npos);
    CHECK(loomai::config::snapshot().dump().find("sk-very-secret") == std::string::npos);  // not in config.json either
}

TEST(custom_add_validates_everything) {
    Sandbox box("validate");
    json created;
    CHECK_EQ(models::custom_add(json::array(), created).http, 400);
    CHECK_EQ(models::custom_add({{"provider", "ollama"}}, created).http, 400);                    // no id
    CHECK_EQ(models::custom_add({{"id", "bad id"}, {"provider", "ollama"}}, created).http, 400);  // space
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "nope"}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "openai-compatible"}}, created).http, 400);  // no base_url
    CHECK_EQ(models::custom_add(compat("x", "ftp://host/v1"), created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "ollama"}, {"base_url", "http://h"}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "openai"}, {"api_key", "k"}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "ollama"}, {"surprise", "1"}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "ollama"}, {"name", std::string(101, 'n')}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", "x"}, {"provider", "ollama"}, {"upstream_model", "bad name"}}, created).http, 400);
    CHECK_EQ(models::custom_add({{"id", 5}, {"provider", "ollama"}}, created).http, 400);
    CHECK_EQ(models::custom_list().size(), static_cast<size_t>(0));  // every rejection left nothing behind
}

TEST(duplicate_ids_conflict_and_the_list_is_bounded) {
    Sandbox box("dups");
    json created;
    CHECK(models::custom_add({{"id", "a"}, {"provider", "ollama"}}, created).ok());
    CHECK_EQ(models::custom_add({{"id", "a"}, {"provider", "gemini"}}, created).http, 409);
    CHECK_EQ(models::custom_list().size(), static_cast<size_t>(1));
    for (int i = 0; i < 99; ++i) {
        CHECK(models::custom_add({{"id", "m" + std::to_string(i)}, {"provider", "ollama"}}, created).ok());
    }
    CHECK_EQ(models::custom_add({{"id", "one-too-many"}, {"provider", "ollama"}}, created).http, 400);
}

TEST(custom_remove_deletes_the_model_and_its_key) {
    Sandbox box("remove");
    json created;
    CHECK(models::custom_add({{"id", "gone"}, {"provider", "openai-compatible"},
                              {"base_url", "https://example.com/v1"}, {"api_key", "k1"}},
                             created)
              .ok());
    CHECK_EQ(models::custom_remove("missing").http, 404);
    CHECK_EQ(models::custom_remove("bad id").http, 400);
    CHECK(models::custom_remove("gone").ok());
    CHECK_EQ(secrets::get("custom:gone"), std::string(""));
    CHECK(!models::resolve("gone").custom);  // back to prefix/Ollama routing
    CHECK_EQ(models::custom_remove("gone").http, 404);
}

TEST(custom_models_survive_a_restart) {
    Sandbox box("persist");
    json created;
    CHECK(models::custom_add(compat("kept", "http://localhost:8080/v1"), created).ok());
    loomai::config::reload();  // as after a restart: the list must come back from disk
    CHECK_EQ(models::custom_list().size(), static_cast<size_t>(1));
    CHECK(models::resolve("kept").custom);
}
