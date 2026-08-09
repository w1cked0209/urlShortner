#include "urlshort/config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace urlshort {
namespace {

const char* env_or_null(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return nullptr;
    return value;
}

std::string env_string(const char* name, const std::string& fallback) {
    const char* value = env_or_null(name);
    return value ? std::string(value) : fallback;
}

// Fail loudly on garbage rather than silently falling back: a typo in
// URLSHORT_PORT should stop the process, not quietly bind the wrong port.
long env_long(const char* name, long fallback, long min, long max) {
    const char* value = env_or_null(name);
    if (!value) return fallback;

    char* end   = nullptr;
    long  parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        throw std::runtime_error(std::string(name) + " must be an integer, got: " + value);
    }
    if (parsed < min || parsed > max) {
        throw std::runtime_error(std::string(name) + " out of range [" + std::to_string(min) +
                                 ", " + std::to_string(max) + "]: " + value);
    }
    return parsed;
}

bool env_bool(const char* name, bool fallback) {
    const char* value = env_or_null(name);
    if (!value) return fallback;

    const std::string v(value);
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "FALSE" || v == "no" || v == "off") return false;
    throw std::runtime_error(std::string(name) + " must be a boolean, got: " + v);
}

}  // namespace

Config Config::from_environment() {
    Config config;
    config.host                = env_string("URLSHORT_HOST", config.host);
    config.port                = static_cast<int>(env_long("URLSHORT_PORT", config.port, 1, 65535));
    config.database_path       = env_string("URLSHORT_DB", config.database_path);
    config.code_length =
        static_cast<std::size_t>(env_long("URLSHORT_CODE_LENGTH",
                                          static_cast<long>(config.code_length), 4, 32));
    config.max_url_length =
        static_cast<std::size_t>(env_long("URLSHORT_MAX_URL_LENGTH",
                                          static_cast<long>(config.max_url_length), 16, 65536));
    config.block_private_hosts = env_bool("URLSHORT_BLOCK_PRIVATE_HOSTS",
                                          config.block_private_hosts);
    config.thread_count =
        static_cast<int>(env_long("URLSHORT_THREADS", config.thread_count, 1, 512));

    // Derived after port, so the default stays consistent when only the port
    // is overridden.
    config.base_url = env_string("URLSHORT_BASE_URL",
                                 "http://localhost:" + std::to_string(config.port));

    // Trailing slashes here would produce "http://host//abc123".
    while (!config.base_url.empty() && config.base_url.back() == '/') {
        config.base_url.pop_back();
    }
    return config;
}

}  // namespace urlshort
