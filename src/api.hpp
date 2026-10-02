#pragma once

#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

// The management API of the dashboard / desktop app: model listing, settings,
// keys and custom models. The ai.nvim contract (/health, /ask, /ask/stream)
// stays in main.cpp, untouched.
namespace loomai::api {

// {"error":{"message":"..."}} with the given status, the error shape every
// endpoint of this server uses.
inline void send_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    nlohmann::json body;
    body["error"]["message"] = message;
    res.set_content(body.dump(), "application/json");
}

inline void send_json(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Registers the management routes on `server`.
void register_routes(httplib::Server& server);

}  // namespace loomai::api
