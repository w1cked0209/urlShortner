#include <exception>
#include <iostream>

#include "urlshort/config.hpp"
#include "urlshort/http_api.hpp"
#include "urlshort/shortener_service.hpp"
#include "urlshort/sqlite_link_store.hpp"

int main() {
    try {
        const urlshort::Config config = urlshort::Config::from_environment();

        urlshort::SqliteLinkStore  store(config.database_path);
        urlshort::ShortenerService service(config, store);
        urlshort::HttpApi          api(config, service);

        std::cout << "url_shortener listening on http://" << config.host << ":" << config.port
                  << "\n  database:       " << config.database_path
                  << "\n  base url:       " << config.base_url
                  << "\n  code length:    " << config.code_length
                  << "\n  private hosts:  " << (config.block_private_hosts ? "blocked" : "allowed")
                  << "\n  links stored:   " << store.count_links() << std::endl;

        if (!api.listen()) {
            std::cerr << "error: could not bind " << config.host << ":" << config.port << "\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        // Startup failures (bad env var, unwritable database) should be a clear
        // one-line message and a non-zero exit, not a stack trace.
        std::cerr << "fatal: " << error.what() << "\n";
        return 1;
    }
}
