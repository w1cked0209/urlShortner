#include <doctest/doctest.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <thread>

#include "urlshort/http_api.hpp"
#include "urlshort/shortener_service.hpp"
#include "urlshort/sqlite_link_store.hpp"

using namespace urlshort;
using json = nlohmann::json;

namespace {

/// Spins up the real server on an ephemeral port and talks to it over a real
/// socket. Slower than calling the handlers directly, but it is the only way to
/// test the things that actually break in production: status codes, headers,
/// route precedence, and the 301 itself.
class ServerFixture {
public:
    ServerFixture() {
        config_.database_path       = ":memory:";
        config_.block_private_hosts = false;

        store_   = std::make_unique<SqliteLinkStore>(config_.database_path);
        service_ = std::make_unique<ShortenerService>(config_, *store_);
        api_     = std::make_unique<HttpApi>(config_, *service_);

        api_->register_routes(server_);

        port_ = server_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        config_.base_url = "http://127.0.0.1:" + std::to_string(port_);

        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~ServerFixture() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    /// A client that does NOT follow redirects - the redirect is the thing
    /// under test.
    httplib::Client client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_follow_location(false);
        client.set_read_timeout(5, 0);
        return client;
    }

    httplib::Result post_shorten(const json& body) const {
        auto c = client();
        return c.Post("/shorten", body.dump(), "application/json");
    }

    ShortenerService& service() { return *service_; }

private:
    Config                            config_;
    std::unique_ptr<SqliteLinkStore>  store_;
    std::unique_ptr<ShortenerService> service_;
    std::unique_ptr<HttpApi>          api_;
    httplib::Server                   server_;
    int                               port_ = 0;
    std::thread                       thread_;
};

}  // namespace

TEST_CASE("POST /shorten returns 201 and a usable short_url") {
    ServerFixture fixture;

    auto response = fixture.post_shorten({{"url", "https://example.com/hello"}});
    REQUIRE(response);
    CHECK(response->status == 201);
    CHECK(response->get_header_value("Content-Type").find("application/json") == 0);

    const auto body = json::parse(response->body);
    CHECK(body["url"]          == "https://example.com/hello");
    CHECK(body["custom_alias"] == false);
    CHECK(body["reused"]       == false);
    CHECK(body["code"].get<std::string>().size() == 7);
    CHECK(body["short_url"].get<std::string>().find(body["code"].get<std::string>()) !=
          std::string::npos);
    CHECK(response->get_header_value("Location") == body["short_url"].get<std::string>());
}

TEST_CASE("GET /{code} redirects with 301 to the original URL") {
    ServerFixture fixture;

    auto created = fixture.post_shorten({{"url", "https://example.com/destination?x=1"}});
    REQUIRE(created);
    const auto code = json::parse(created->body)["code"].get<std::string>();

    auto client   = fixture.client();
    auto redirect = client.Get("/" + code);
    REQUIRE(redirect);
    CHECK(redirect->status == 301);
    CHECK(redirect->get_header_value("Location") == "https://example.com/destination?x=1");
    // 301 is cached aggressively by browsers, which would hide repeat visits
    // from analytics. We ask them not to.
    CHECK(redirect->get_header_value("Cache-Control").find("no-store") != std::string::npos);
}

TEST_CASE("unknown codes return 404 with a JSON body") {
    ServerFixture fixture;
    auto client = fixture.client();

    auto response = client.Get("/doesnot");
    REQUIRE(response);
    CHECK(response->status == 404);

    const auto body = json::parse(response->body);
    CHECK(body["error"] == "not_found");

    // A path that cannot be a code at all still answers in JSON rather than
    // httplib's default HTML.
    auto weird = client.Get("/this/is/not/a/code");
    REQUIRE(weird);
    CHECK(weird->status == 404);
    json weird_body;
    CHECK_NOTHROW(weird_body = json::parse(weird->body));
    CHECK(weird_body["error"] == "not_found");
}

TEST_CASE("a duplicate URL returns 200 and reused=true") {
    ServerFixture fixture;

    auto first = fixture.post_shorten({{"url", "https://example.com/same"}});
    REQUIRE(first);
    CHECK(first->status == 201);

    auto second = fixture.post_shorten({{"url", "https://example.com/same"}});
    REQUIRE(second);
    // 200 rather than 201: nothing was created. The distinction is what lets a
    // client tell "my request made this" from "this already existed".
    CHECK(second->status == 200);

    const auto first_body  = json::parse(first->body);
    const auto second_body = json::parse(second->body);
    CHECK(second_body["code"]   == first_body["code"]);
    CHECK(second_body["reused"] == true);
}

TEST_CASE("force_new produces a second independent code over HTTP") {
    ServerFixture fixture;

    auto first  = fixture.post_shorten({{"url", "https://example.com/campaign"}});
    auto second = fixture.post_shorten({{"url", "https://example.com/campaign"},
                                        {"force_new", true}});
    REQUIRE(first);
    REQUIRE(second);
    CHECK(second->status == 201);
    CHECK(json::parse(second->body)["code"] != json::parse(first->body)["code"]);
}

TEST_CASE("custom aliases work end to end and conflict with 409") {
    ServerFixture fixture;

    auto created = fixture.post_shorten({{"url", "https://example.com/docs"},
                                         {"custom_alias", "team-docs"}});
    REQUIRE(created);
    CHECK(created->status == 201);
    CHECK(json::parse(created->body)["code"] == "team-docs");
    CHECK(json::parse(created->body)["custom_alias"] == true);

    auto client   = fixture.client();
    auto redirect = client.Get("/team-docs");
    REQUIRE(redirect);
    CHECK(redirect->status == 301);
    CHECK(redirect->get_header_value("Location") == "https://example.com/docs");

    auto conflict = fixture.post_shorten({{"url", "https://example.com/elsewhere"},
                                          {"custom_alias", "team-docs"}});
    REQUIRE(conflict);
    CHECK(conflict->status == 409);
    CHECK(json::parse(conflict->body)["error"] == "alias_taken");
}

TEST_CASE("the short 'alias' spelling is accepted too") {
    ServerFixture fixture;

    auto created = fixture.post_shorten({{"url", "https://example.com/x"}, {"alias", "shorthand"}});
    REQUIRE(created);
    CHECK(created->status == 201);
    CHECK(json::parse(created->body)["code"] == "shorthand");
}

TEST_CASE("reserved aliases cannot shadow real routes") {
    ServerFixture fixture;

    auto response = fixture.post_shorten({{"url", "https://example.com/x"},
                                          {"custom_alias", "healthz"}});
    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(json::parse(response->body)["error"] == "reserved_alias");

    // ...and /healthz still does its job.
    auto client = fixture.client();
    auto health = client.Get("/healthz");
    REQUIRE(health);
    CHECK(health->status == 200);
    CHECK(json::parse(health->body)["status"] == "ok");
}

TEST_CASE("bad input is rejected with a specific error code") {
    ServerFixture fixture;

    struct Case {
        json        body;
        int         status;
        const char* error;
    };

    const Case cases[] = {
        {{{"url", "not a url"}},                    400, "invalid_url"},
        {{{"url", "javascript:alert(1)"}},          400, "unsupported_scheme"},
        {{{"url", ""}},                             400, "invalid_url"},
        {{{"nourl", "https://example.com"}},        400, "malformed_request"},
        {{{"url", 42}},                             400, "malformed_request"},
        {{{"url", "https://example.com"}, {"force_new", "yes"}}, 400, "malformed_request"},
        {{{"url", "https://example.com"}, {"custom_alias", "ab"}}, 400, "invalid_alias"},
    };

    for (const auto& test : cases) {
        CAPTURE(test.body.dump());
        auto response = fixture.post_shorten(test.body);
        REQUIRE(response);
        CHECK(response->status == test.status);
        CHECK(json::parse(response->body)["error"] == test.error);
    }
}

TEST_CASE("a non-JSON body is a 400, not a 500") {
    ServerFixture fixture;
    auto client   = fixture.client();
    auto response = client.Post("/shorten", "this is not json", "application/json");
    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(json::parse(response->body)["error"] == "malformed_request");
}

TEST_CASE("stats accumulate across redirects") {
    ServerFixture fixture;

    auto created = fixture.post_shorten({{"url", "https://example.com/tracked"}});
    REQUIRE(created);
    const auto code = json::parse(created->body)["code"].get<std::string>();

    auto client = fixture.client();
    for (int i = 0; i < 3; ++i) {
        httplib::Headers headers{{"Referer", "https://news.example.org/story"},
                                 {"User-Agent", "doctest/1.0"}};
        auto redirect = client.Get("/" + code, headers);
        REQUIRE(redirect);
        CHECK(redirect->status == 301);
    }
    // One visit with no Referer at all.
    client.Get("/" + code);

    auto stats = client.Get("/api/stats/" + code);
    REQUIRE(stats);
    CHECK(stats->status == 200);

    const auto body = json::parse(stats->body);
    CHECK(body["code"]         == code);
    CHECK(body["total_clicks"] == 4);
    CHECK(body["window_days"]  == 7);
    CHECK(body["last_clicked_at"].is_string());
    REQUIRE(body["top_referrers"].size() == 1);
    CHECK(body["top_referrers"][0]["host"]   == "news.example.org");
    CHECK(body["top_referrers"][0]["clicks"] == 3);
    REQUIRE(body["clicks_by_day"].size() == 1);
    CHECK(body["clicks_by_day"][0]["clicks"] == 4);
}

TEST_CASE("stats for an unknown code are 404") {
    ServerFixture fixture;
    auto client   = fixture.client();
    auto response = client.Get("/api/stats/nosuch1");
    REQUIRE(response);
    CHECK(response->status == 404);
    CHECK(json::parse(response->body)["error"] == "not_found");
}

TEST_CASE("the days window is validated") {
    ServerFixture fixture;
    auto created = fixture.post_shorten({{"url", "https://example.com/w"}});
    REQUIRE(created);
    const auto code = json::parse(created->body)["code"].get<std::string>();

    auto client = fixture.client();

    auto ok = client.Get("/api/stats/" + code + "?days=30");
    REQUIRE(ok);
    CHECK(ok->status == 200);
    CHECK(json::parse(ok->body)["window_days"] == 30);

    for (const char* bad : {"0", "9999", "abc", "-1"}) {
        CAPTURE(bad);
        auto response = client.Get("/api/stats/" + code + "?days=" + bad);
        REQUIRE(response);
        CHECK(response->status == 400);
    }
}

TEST_CASE("route precedence keeps /api and /healthz out of the code namespace") {
    // The catch-all /{code} route is registered last for exactly this reason;
    // if someone reorders it, these break loudly.
    ServerFixture fixture;
    auto client = fixture.client();

    auto health = client.Get("/healthz");
    REQUIRE(health);
    CHECK(health->status == 200);

    auto stats = client.Get("/api/stats/whatever");
    REQUIRE(stats);
    CHECK(stats->status == 404);
    CHECK(json::parse(stats->body)["error"] == "not_found");
}

TEST_CASE("a redirect target containing quotes and newlines cannot break the response") {
    ServerFixture fixture;

    // Header injection attempt: the newline must never survive into Location.
    auto response = fixture.post_shorten(
        {{"url", "https://example.com/x\r\nX-Injected: yes"}});
    REQUIRE(response);
    CHECK(response->status == 400);
}
