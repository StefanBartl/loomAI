#pragma once

#include <string>

namespace loomai {

// Identifier of a user-defined model. It ends up in URLs (/models/custom/<id>),
// in config.json and in the name of a stored secret, so it is restricted to a
// small safe alphabet: letters, digits and . _ : / -  (1-100 characters). That
// covers Ollama-style names ("llama3.1:8b-q5_K_M") and provider paths
// ("meta-llama/llama-3.1-8b") without admitting whitespace, quotes or '..'-style
// surprises beyond what the alphabet already allows.
inline bool valid_model_id(const std::string& id) {
    if (id.empty() || id.size() > 100) {
        return false;
    }
    for (unsigned char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace loomai
