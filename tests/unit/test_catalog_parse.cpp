#include "catalog_parse.hpp"
#include "minitest.hpp"

using json = nlohmann::json;
namespace catalog = loomai::catalog;

TEST(iso_date_converts_unix_time_without_gmtime) {
    CHECK_EQ(catalog::iso_date(0), std::string(""));
    CHECK_EQ(catalog::iso_date(-5), std::string(""));
    CHECK_EQ(catalog::iso_date(86400LL * 365), std::string("1971-01-01"));
    CHECK_EQ(catalog::iso_date(951782400LL), std::string("2000-02-29"));  // leap day
    CHECK_EQ(catalog::iso_date(1700000000LL), std::string("2023-11-14"));
    CHECK_EQ(catalog::iso_date(1735689600LL), std::string("2025-01-01"));
    CHECK_EQ(catalog::iso_date(4102444800LL), std::string("2100-01-01"));  // 2100 is not a leap year
}

TEST(ollama_tags_become_local_entries_with_details) {
    json response = json::parse(R"({"models":[
      {"name":"mistral:7b","model":"mistral:7b","modified_at":"2025-09-01T10:11:12.5+02:00","size":4113301824,
       "details":{"format":"gguf","family":"llama","parameter_size":"7.2B","quantization_level":"Q4_0"}},
      {"name":"llama3.1:8b-q5_K_M","size":5732987392,
       "details":{"family":"llama","parameter_size":"8.0B","quantization_level":"Q5_K_M"}},
      {"model":"only-model-field:1b"},
      {"size":1}
    ]})");
    json out = catalog::parse_ollama_tags(response);
    CHECK_EQ(out.size(), static_cast<size_t>(3));                      // the nameless one is skipped
    CHECK_EQ(out[0]["id"].get<std::string>(), std::string("llama3.1:8b-q5_K_M"));  // sorted by id
    CHECK_EQ(out[0]["category"].get<std::string>(), std::string("local"));
    CHECK_EQ(out[0]["provider"].get<std::string>(), std::string("ollama"));
    CHECK(out[0]["available"].get<bool>());
    CHECK_EQ(out[0]["details"]["quantization"].get<std::string>(), std::string("Q5_K_M"));
    CHECK_EQ(out[0]["details"]["parameter_size"].get<std::string>(), std::string("8.0B"));
    CHECK_EQ(out[0]["details"]["size_bytes"].get<long long>(), 5732987392LL);
    CHECK_EQ(out[1]["id"].get<std::string>(), std::string("mistral:7b"));
    CHECK_EQ(out[1]["details"]["modified"].get<std::string>(), std::string("2025-09-01"));
    CHECK_EQ(out[1]["details"]["format"].get<std::string>(), std::string("gguf"));
    CHECK_EQ(out[2]["id"].get<std::string>(), std::string("only-model-field:1b"));
    CHECK(out[2]["details"].empty());
}

TEST(openai_list_keeps_only_chat_models_newest_first) {
    json response = json::parse(R"({"data":[
      {"id":"gpt-4o","created":1715367049,"owned_by":"system"},
      {"id":"gpt-4o-audio-preview","created":1727460443},
      {"id":"text-embedding-3-small","created":1705948997},
      {"id":"whisper-1","created":1677532384},
      {"id":"o3-mini","created":1737146383,"owned_by":"system"},
      {"id":"dall-e-3","created":1698785189},
      {"id":"gpt-image-1","created":1745517030},
      {"id":"gpt-3.5-turbo-instruct","created":1692901427},
      {"id":"gpt-4o-realtime-preview","created":1727659998},
      {"id":"gpt-4o-mini-tts","created":1742403959},
      {"id":"omni-moderation-latest","created":1731689265},
      {"id":"babbage-002","created":1692634615},
      {"id":"o1-mini","created":1725649008},
      {"created":5}
    ]})");
    json out = catalog::parse_openai_models(response);
    std::vector<std::string> ids;
    for (const auto& item : out) ids.push_back(item["id"].get<std::string>());
    CHECK_EQ(ids.size(), static_cast<size_t>(3));
    CHECK_EQ(ids[0], std::string("o3-mini"));  // newest first
    CHECK_EQ(ids[1], std::string("o1-mini"));
    CHECK_EQ(ids[2], std::string("gpt-4o"));
    CHECK_EQ(out[0]["details"]["created"].get<std::string>(), std::string("2025-01-17"));
    CHECK_EQ(out[0]["details"]["owned_by"].get<std::string>(), std::string("system"));
    CHECK_EQ(out[0]["provider"].get<std::string>(), std::string("openai"));
    CHECK_EQ(out[0]["category"].get<std::string>(), std::string("cloud"));
}

TEST(anthropic_list_uses_the_display_name) {
    json response = json::parse(R"({"data":[
      {"type":"model","id":"claude-sonnet-5-5","display_name":"Claude Sonnet 5.5","created_at":"2026-05-01T00:00:00Z"},
      {"id":"claude-no-name"},
      {"display_name":"no id"}
    ],"has_more":false})");
    json out = catalog::parse_anthropic_models(response);
    CHECK_EQ(out.size(), static_cast<size_t>(2));
    CHECK_EQ(out[0]["name"].get<std::string>(), std::string("Claude Sonnet 5.5"));
    CHECK_EQ(out[0]["details"]["created"].get<std::string>(), std::string("2026-05-01"));
    CHECK_EQ(out[1]["name"].get<std::string>(), std::string("claude-no-name"));  // falls back to the id
}

TEST(gemini_list_strips_models_prefix_and_filters_on_generateContent) {
    std::string long_text(500, 'x');
    json response = {{"models",
                      json::array({
                          {{"name", "models/gemini-2.5-pro"},
                           {"displayName", "Gemini 2.5 Pro"},
                           {"description", long_text},
                           {"inputTokenLimit", 1048576},
                           {"outputTokenLimit", 65536},
                           {"version", "2.5"},
                           {"supportedGenerationMethods", json::array({"generateContent", "countTokens"})}},
                          {{"name", "models/gemini-embedding-001"},
                           {"supportedGenerationMethods", json::array({"embedContent"})}},
                          {{"name", "models/embedding-001"},
                           {"supportedGenerationMethods", json::array({"generateContent"})}},
                          {{"name", "models/gemini-no-methods"}},
                      })}};
    json out = catalog::parse_gemini_models(response);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK_EQ(out[0]["id"].get<std::string>(), std::string("gemini-2.5-pro"));
    CHECK_EQ(out[0]["name"].get<std::string>(), std::string("Gemini 2.5 Pro"));
    CHECK_EQ(out[0]["details"]["context_length"].get<long long>(), 1048576LL);
    CHECK_EQ(out[0]["details"]["max_output_tokens"].get<long long>(), 65536LL);
    CHECK_EQ(out[0]["details"]["description"].get<std::string>().size(), static_cast<size_t>(303));  // 300 + "..."
}

TEST(ollama_show_extracts_context_length_and_capabilities) {
    json response = json::parse(R"({
      "details":{"family":"llama","parameter_size":"8.0B","quantization_level":"Q5_K_M"},
      "model_info":{"general.architecture":"llama","general.parameter_count":8030261248,"llama.context_length":131072,
                    "llama.embedding_length":4096},
      "capabilities":["completion","tools"]})");
    json out = catalog::parse_ollama_show(response);
    CHECK_EQ(out["context_length"].get<long long>(), 131072LL);
    CHECK_EQ(out["parameter_count"].get<long long>(), 8030261248LL);
    CHECK_EQ(out["architecture"].get<std::string>(), std::string("llama"));
    CHECK_EQ(out["quantization"].get<std::string>(), std::string("Q5_K_M"));
    CHECK_EQ(out["capabilities"].size(), static_cast<size_t>(2));
}

TEST(malformed_provider_responses_degrade_to_empty_never_throw) {
    const json garbage[] = {json(nullptr), json("a string"), json(42), json::array(), json::object(),
                            json{{"models", "not an array"}}, json{{"data", 5}}, json{{"models", json::array({1, "x", nullptr})}},
                            json{{"data", json::array({json::array(), "x", 7})}}};
    for (const auto& input : garbage) {
        CHECK(catalog::parse_ollama_tags(input).is_array());
        CHECK(catalog::parse_openai_models(input).is_array());
        CHECK(catalog::parse_anthropic_models(input).is_array());
        CHECK(catalog::parse_gemini_models(input).is_array());
        CHECK(catalog::parse_ollama_show(input).is_object());
    }
    CHECK(catalog::parse_ollama_tags(json(nullptr)).empty());
    // wrong field types inside an otherwise valid entry
    json odd = json::parse(R"({"models":[{"name":5,"details":"str","size":"big"},{"name":"ok:1b","details":7}]})");
    json out = catalog::parse_ollama_tags(odd);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK_EQ(out[0]["id"].get<std::string>(), std::string("ok:1b"));
}

TEST(presets_are_marked_as_suggestions) {
    for (const char* provider : {"openai", "anthropic", "gemini"}) {
        json out = catalog::presets(provider);
        CHECK(!out.empty());
        for (const auto& item : out) {
            CHECK(!item["available"].get<bool>());
            CHECK(item["details"]["preset"].get<bool>());
            CHECK_EQ(item["provider"].get<std::string>(), std::string(provider));
            CHECK_EQ(item["category"].get<std::string>(), std::string("cloud"));
        }
    }
    CHECK(catalog::presets("nobody").empty());
}
