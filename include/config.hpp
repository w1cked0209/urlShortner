#pragma once

#include <cstddef>
#include <string>

namespace urlshort {

/// Runtime configuration. Defaults are the "safe for a public deployment"
/// choices; every field can be overridden from the environment so that tests
/// and local development can relax them explicitly rather than by accident.
struct Config {
    std::string host          = "0.0.0.0";
    int         port          = 8080;

    /// SQLite file. ":memory:" is supported and used by the test suite.
    std::string database_path = "urlshortener.db";

    /// Origin used to build the `short_url` we hand back. Purely cosmetic -
    /// redirects work regardless of what this says.
    std::string base_url      = "http://localhost:8080";

    /// Length of generated codes. 7 Base62 chars ~= 3.5e12 possibilities.
    std::size_t code_length   = 7;

    /// Longest URL we accept. 2048 is the pragmatic ceiling every browser and
    /// proxy honours, even though the RFC sets no limit.
    std::size_t max_url_length = 2048;

    /// Refuse to shorten loopback / RFC1918 / link-local targets. On by
    /// default: an open redirector into a private network is an SSRF gadget.
    bool block_private_hosts = true;

    /// Worker threads for the HTTP server.
    int  thread_count = 8;

    /// Populate from environment variables (URLSHORT_PORT, URLSHORT_DB, ...).
    /// Unset variables leave the default in place; malformed values throw.
    static Config from_environment();
};

}  // namespace urlshort
