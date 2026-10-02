#include "api.hpp"

#include "catalog.hpp"

using json = nlohmann::json;

namespace loomai::api {

void register_routes(httplib::Server& server) {
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
}

}  // namespace loomai::api
