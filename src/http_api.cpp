#include "urlshort/http_api.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <string>

#include "urlshort/errors.hpp"

namespace urlshort {
namespace {

using json = nlohmann::json;

/// Unix seconds -> RFC 3339 in UTC. Timestamps leave the service in one format
/// only; letting each endpoint pick its own is how APIs end up with three.
std::string iso8601(std::int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(2) + "\n", "application/json");
}

void send_error(httplib::Response& response, ErrorCode code, const std::string& message) {
    send_json(response, status_for(code),
              json{{"error", to_string(code)}, {"message", message}});
}

json to_json(const Link& link, const std::string& short_url, bool reused) {
    return json{
        {"code",           link.code},
        {"short_url",      short_url},
        {"url",            link.original_url},
        {"normalized_url", link.normalized_url},
        {"custom_alias",   link.custom_alias},
        {"created_at",     iso8601(link.created_at)},
        // Explicit rather than inferred from the status code: clients that only
        // look at the body should still be able to tell a fresh code from a
        // recycled one.
        {"reused",         reused},
    };
}

/// One place where service exceptions become HTTP responses, so no handler has
/// to remember the mapping and no unexpected exception can escape into
/// httplib's generic 500 without a log line.
template <typename Handler>
void guarded(const httplib::Request& request, httplib::Response& response, Handler&& handler) {
    (void)request;
    try {
        handler();
    } catch (const ServiceError& error) {
        send_error(response, error.code(), error.what());
    } catch (const std::exception& error) {
        send_error(response, ErrorCode::Internal, error.what());
    }
}

}  // namespace

HttpApi::HttpApi(const Config& config, ShortenerService& service)
    : config_(config), service_(service) {}

void HttpApi::register_routes(httplib::Server& server) const {
    // ---- POST /shorten -----------------------------------------------------
    server.Post("/shorten", [this](const httplib::Request& request, httplib::Response& response) {
        guarded(request, response, [&] {
            json body;
            try {
                body = json::parse(request.body);
            } catch (const json::exception&) {
                throw ServiceError(ErrorCode::MalformedRequest, "request body must be JSON");
            }
            if (!body.is_object()) {
                throw ServiceError(ErrorCode::MalformedRequest, "request body must be a JSON object");
            }

            ShortenRequest shorten_request;

            const auto url = body.find("url");
            if (url == body.end() || !url->is_string()) {
                throw ServiceError(ErrorCode::MalformedRequest,
                                   "field 'url' is required and must be a string");
            }
            shorten_request.url = url->get<std::string>();

            // Both spellings accepted: "alias" is what people type, "custom_alias"
            // is what the schema calls it. Cheap kindness, one line.
            for (const char* key : {"custom_alias", "alias"}) {
                const auto alias = body.find(key);
                if (alias != body.end() && !alias->is_null()) {
                    if (!alias->is_string()) {
                        throw ServiceError(ErrorCode::MalformedRequest,
                                           std::string("field '") + key + "' must be a string");
                    }
                    shorten_request.custom_alias = alias->get<std::string>();
                    break;
                }
            }

            const auto force_new = body.find("force_new");
            if (force_new != body.end() && !force_new->is_null()) {
                if (!force_new->is_boolean()) {
                    throw ServiceError(ErrorCode::MalformedRequest,
                                       "field 'force_new' must be a boolean");
                }
                shorten_request.force_new = force_new->get<bool>();
            }

            const ShortenResult result = service_.shorten(shorten_request);
            const std::string   short_url = service_.short_url_for(result.link.code);

            // 201 for a new resource, 200 for one that already existed. The
            // Location header is set either way so clients can follow it
            // without parsing the body.
            response.set_header("Location", short_url);
            send_json(response, result.created ? 201 : 200,
                      to_json(result.link, short_url, !result.created));
        });
    });

    // ---- GET /api/stats/{code} --------------------------------------------
    server.Get(R"(/api/stats/([A-Za-z0-9_-]+))",
               [this](const httplib::Request& request, httplib::Response& response) {
                   guarded(request, response, [&] {
                       const std::string code = request.matches[1];

                       int days = 7;
                       if (request.has_param("days")) {
                           try {
                               days = std::stoi(request.get_param_value("days"));
                           } catch (const std::exception&) {
                               throw ServiceError(ErrorCode::MalformedRequest,
                                                  "query parameter 'days' must be an integer");
                           }
                           if (days < 1 || days > 365) {
                               throw ServiceError(ErrorCode::MalformedRequest,
                                                  "query parameter 'days' must be in 1..365");
                           }
                       }

                       const LinkStats stats = service_.stats(code, days);

                       json referrers = json::array();
                       for (const auto& referrer : stats.top_referrers) {
                           referrers.push_back({{"host", referrer.host}, {"clicks", referrer.count}});
                       }
                       json by_day = json::array();
                       for (const auto& day : stats.clicks_by_day) {
                           by_day.push_back({{"day", day.day}, {"clicks", day.count}});
                       }

                       json body = to_json(stats.link, service_.short_url_for(stats.link.code),
                                           /*reused=*/false);
                       body.erase("reused");  // meaningless on a read endpoint
                       body["total_clicks"]    = stats.link.click_count;
                       body["last_clicked_at"] = stats.last_clicked_at
                                                     ? json(iso8601(*stats.last_clicked_at))
                                                     : json(nullptr);
                       body["window_days"]     = days;
                       body["top_referrers"]   = referrers;
                       body["clicks_by_day"]   = by_day;

                       send_json(response, 200, body);
                   });
               });

    // ---- GET /healthz ------------------------------------------------------
    server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
        send_json(response, 200, json{{"status", "ok"}});
    });

    // ---- GET /{code} -------------------------------------------------------
    // Registered last: httplib matches in registration order, and this pattern
    // would otherwise swallow /healthz and /api/*.
    server.Get(R"(/([A-Za-z0-9_-]{1,32}))",
               [this](const httplib::Request& request, httplib::Response& response) {
                   guarded(request, response, [&] {
                       const std::string code = request.matches[1];

                       const Link link = service_.resolve_and_track(
                           code, request.get_header_value("Referer"),
                           request.get_header_value("User-Agent"));

                       // 301 is what the brief asks for. It is also the reason
                       // repeat visits from the same browser will not appear in
                       // the analytics: a permanent redirect is cached by the
                       // client and never reaches us again. `no-store` asks
                       // browsers not to do that; compliance is patchy, so if
                       // click accuracy mattered more than downstream SEO the
                       // right answer would be 302. Documented in the README.
                       response.set_header("Cache-Control", "no-store, max-age=0");
                       response.set_redirect(link.original_url, 301);
                   });
               });

    // Anything that reaches here is not a valid code shape at all. Answering in
    // JSON keeps the API consistent instead of returning httplib's HTML.
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404 && response.body.empty()) {
            send_error(response, ErrorCode::NotFound, "no such short link");
        }
    });

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr ep) {
            std::string message = "unexpected error";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& error) {
                message = error.what();
            }
            send_error(response, ErrorCode::Internal, message);
        });
}

bool HttpApi::listen() const {
    httplib::Server server;
    server.new_task_queue = [this] {
        return new httplib::ThreadPool(static_cast<std::size_t>(config_.thread_count));
    };
    register_routes(server);
    return server.listen(config_.host.c_str(), config_.port);
}

}  // namespace urlshort
