#pragma once

#include <string>

namespace loomai {

// Host part of "scheme://host[:port][/path]" (brackets kept for IPv6).
inline std::string host_of(const std::string& url) {
    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    if (start < url.size() && url[start] == '[') {
        size_t close = url.find(']', start);
        return url.substr(start, close == std::string::npos ? std::string::npos : close - start + 1);
    }
    size_t end = url.find_first_of(":/", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// True for the machine itself: localhost, 127.x.x.x, ::1.
inline bool is_loopback_host(const std::string& host) {
    return host == "localhost" || host == "[::1]" || host.rfind("127.", 0) == 0;
}

}  // namespace loomai
