#include "api.hpp"

#include "catalog.hpp"
#include "config.hpp"
#include "models.hpp"
#include "secrets.hpp"

using json = nlohmann::json;

namespace loomai::api {

namespace {

// Parses the request body as a JSON object; answers 400 itself when it is not one.
bool parse_object(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        send_error(res, 400, "the request body must be a JSON object");
        return false;
    }
    return true;
}

json settings_response() {
    return {{"settings", config::settings_json()}, {"config_dir", config::directory()}};
}

// The three provider keys are managed here; the key of a custom model belongs to
// the model and is set through /models/custom.
bool is_provider_key(const std::string& name) {
    return secrets::valid_name(name) && name.rfind("custom:", 0) != 0;
}

}  // namespace

void register_routes(httplib::Server& server) {
    // ----- models ----------------------------------------------------------

    // Model listing: local (Ollama), cloud (provider list endpoints, only with a
    // key), custom. ?refresh=1 bypasses the cache.
    server.Get("/models", [](const httplib::Request& req, httplib::Response& res) {
        const std::string refresh = req.get_param_value("refresh");
        send_json(res, 200, catalog::list(refresh == "1" || refresh == "true"));
    });

    // Details for the hover popup: GET /models/info?id=<model id>
    server.Get("/models/info", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("id")) {
            send_error(res, 400, "query parameter 'id' is required");
            return;
        }
        auto result = catalog::info(req.get_param_value("id"));
        send_json(res, result.http, result.body);
    });

    // ----- custom models ----------------------------------------------------

    server.Get("/models/custom", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, {{"models", models::custom_list()}});
    });

    server.Post("/models/custom", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        json created;
        config::Status st = models::custom_add(body, created);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 201, created);
    });

    // The id may contain '/' and ':' (URL-decoded before routing), hence `.+`.
    server.Delete(R"(/models/custom/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        config::Status st = models::custom_remove(req.matches[1].str());
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        res.status = 204;
    });

    // ----- settings ---------------------------------------------------------

    server.Get("/settings", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, settings_response());
    });

    // Flat partial update, e.g. {"theme":"dark"}; null resets a key.
    server.Put("/settings", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        config::Status st = config::update_settings(body);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, settings_response());
    });

    // ----- keys (presence only, never a value) ------------------------------

    server.Get("/keys", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, secrets::keys_json());
    });

    server.Put(R"(/keys/([A-Za-z0-9_:.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (!is_provider_key(name)) {
            send_error(res, 400, "not a provider key name (OPENAI_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY)");
            return;
        }
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        if (!body.contains("value") || !body["value"].is_string()) {
            send_error(res, 400, "'value' (string) is required");
            return;
        }
        config::Status st = secrets::set(name, body["value"].get<std::string>());
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, secrets::keys_json());
    });

    server.Delete(R"(/keys/([A-Za-z0-9_:.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (!is_provider_key(name)) {
            send_error(res, 400, "not a provider key name (OPENAI_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY)");
            return;
        }
        config::Status st = secrets::remove(name);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, secrets::keys_json());
    });
}

}  // namespace loomai::api
