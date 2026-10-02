#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "httplib.h"

namespace loomai {

// One persistent, keep-alive client per (worker thread, base URL) instead of a
// fresh TCP/TLS connection for every /ask and /ask/stream request.
//
// main.cpp's httplib Server hands each request to one of a fixed pool of worker
// threads that handle requests sequentially, so a thread_local map needs no
// locking. Keying by base URL (not one client per thread, as the backends did
// before) is what lets the Ollama host and user-defined OpenAI-compatible
// endpoints change while the server runs: a new URL simply gets a new client.
//
// `base_url` is "scheme://host[:port]" with no path. The map is bounded: the
// URL set is user-controlled, and a long-running server must not accumulate
// connections for endpoints that were removed long ago.
inline httplib::Client& client_for(const std::string& base_url, int timeout_ms) {
    constexpr size_t kMaxClientsPerThread = 16;
    thread_local std::map<std::string, std::unique_ptr<httplib::Client>> clients;

    auto found = clients.find(base_url);
    if (found == clients.end()) {
        if (clients.size() >= kMaxClientsPerThread) {
            clients.clear();  // nothing holds a reference: callers use the client within one call
        }
        found = clients.emplace(base_url, std::make_unique<httplib::Client>(base_url)).first;
    }
    httplib::Client& client = *found->second;
    client.set_keep_alive(true);
    client.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    client.set_read_timeout(std::chrono::milliseconds(timeout_ms));
    return client;
}

}  // namespace loomai
