cmake_minimum_required(VERSION 3.20)
project(loomai CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Header-only third-party deps, vendored into third_party/ (see
# wkdbook-loomai/Guides/setup-guide.md (author notes, not in this repo), Phase 2 -- gitignored, fetch locally).
add_library(third_party_headers INTERFACE)
target_include_directories(third_party_headers INTERFACE third_party)

# Logic that needs no HTTP stack (settings store, secret store, model registry),
# kept in its own library so the unit tests can link it without OpenSSL or
# cpp-httplib.
add_library(loomai_core STATIC
    src/config.cpp
    src/models.cpp
    src/secrets.cpp
)
target_include_directories(loomai_core PUBLIC src)
target_link_libraries(loomai_core PUBLIC third_party_headers)
if(WIN32)
    target_link_libraries(loomai_core PUBLIC advapi32) # Credential Manager (secrets.cpp)
endif()

add_executable(loomai
    src/main.cpp
    src/ollama_client.cpp
    src/openai_client.cpp
    src/anthropic_client.cpp
    src/gemini_client.cpp
)
target_include_directories(loomai PRIVATE src)
target_link_libraries(loomai PRIVATE loomai_core third_party_headers)

# The cloud ModelRouter backends (openai/anthropic/gemini) talk to real
# HTTPS endpoints, unlike ollama_client's localhost-only HTTP -- cpp-httplib
# needs CPPHTTPLIB_OPENSSL_SUPPORT plus OpenSSL to dispatch an "https://"
# httplib::Client to its SSL-capable code path. Required, not optional: the
# router falls back to Ollama when a cloud model prefix's key is missing,
# but the binary must still link successfully regardless of which keys are
# set at runtime.
find_package(OpenSSL REQUIRED)
target_compile_definitions(loomai PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(loomai PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(loomai PRIVATE ws2_32 crypt32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(loomai PRIVATE Threads::Threads)
endif()

# Unit tests (plain asserts, no framework): `ctest --test-dir build`. The HTTP
# API is covered separately by the Node tests under tests/.
option(LOOMAI_BUILD_TESTS "Build the unit tests" ON)
if(LOOMAI_BUILD_TESTS)
    enable_testing()
    add_executable(loomai_tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_models.cpp
        tests/unit/test_secrets.cpp
    )
    target_link_libraries(loomai_tests PRIVATE loomai_core)
    add_test(NAME loomai_unit COMMAND loomai_tests)
endif()

# Flat install layout: the server mounts "./dashboard" relative to its working
# directory, so the binary and the dashboard folder must sit side by side and
# the server must be started from there. scripts/setup.ps1 and scripts/setup.sh
# install into <prefix>/app and write a launcher that does the cd.
install(TARGETS loomai RUNTIME DESTINATION .)
install(DIRECTORY dashboard DESTINATION .)
