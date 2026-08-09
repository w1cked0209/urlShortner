#pragma once

#include <string>

#include "urlshort/config.hpp"
#include "urlshort/shortener_service.hpp"

namespace httplib {
class Server;
}

namespace urlshort {

/// Binds the service to HTTP routes.
///
/// Split from main() so the integration tests can start a real server on an
/// ephemeral port and exercise the same routing code the binary runs.
class HttpApi {
public:
    HttpApi(const Config& config, ShortenerService& service);

    /// Register routes on an existing server (used by tests).
    void register_routes(httplib::Server& server) const;

    /// Bind, listen, block. Returns false if the port could not be bound.
    bool listen() const;

private:
    const Config&     config_;
    ShortenerService& service_;
};

}  // namespace urlshort
