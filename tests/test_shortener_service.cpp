#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "urlshort/errors.hpp"
#include "urlshort/shortener_service.hpp"
#include "urlshort/sqlite_link_store.hpp"

using namespace urlshort;

namespace {

/// Real service over a real (in-memory) SQLite database. No mocks: the
/// interesting behaviour here is the interaction between the policy and the
/// UNIQUE constraint, and a mock store would only test the mock.
struct Fixture {
    Config                           config;
    std::unique_ptr<SqliteLinkStore> store;
    std::unique_ptr<ShortenerService> service;

    Fixture() {
        config.database_path       = ":memory:";
        config.base_url            = "https://sho.rt";
        config.block_private_hosts = false;
        store                      = std::make_unique<SqliteLinkStore>(config.database_path);
        service                    = std::make_unique<ShortenerService>(config, *store);
    }

    ShortenResult shorten(const std::string& url) {
        ShortenRequest request;
        request.url = url;
        return service->shorten(request);
    }
};

ErrorCode code_of(const std::function<void()>& action) {
    try {
        action();
    } catch (const ServiceError& error) {
        return error.code();
    }
    return ErrorCode::Internal;  // "did not throw" fails the CHECK below
}

}  // namespace

TEST_CASE("shorten then resolve round-trips") {
    Fixture fixture;

    const auto result = fixture.shorten("https://example.com/a/very/long/path?q=1");
    CHECK(result.created);
    CHECK(result.link.code.size() == fixture.config.code_length);
    CHECK(result.link.original_url == "https://example.com/a/very/long/path?q=1");

    const Link resolved = fixture.service->resolve(result.link.code);
    CHECK(resolved.original_url == "https://example.com/a/very/long/path?q=1");
    CHECK(fixture.service->short_url_for(result.link.code) ==
          "https://sho.rt/" + result.link.code);
}

TEST_CASE("unknown codes raise NotFound") {
    Fixture fixture;
    CHECK(code_of([&] { fixture.service->resolve("missing"); }) == ErrorCode::NotFound);
    CHECK(code_of([&] { fixture.service->stats("missing"); })   == ErrorCode::NotFound);
}

TEST_CASE("the same URL shortens to the same code by default") {
    Fixture fixture;

    const auto first  = fixture.shorten("https://example.com/page");
    const auto second = fixture.shorten("https://example.com/page");

    CHECK(first.created);
    CHECK_FALSE(second.created);          // reused, and the caller can tell
    CHECK(first.link.code == second.link.code);
    CHECK(fixture.store->count_links() == 1);
}

TEST_CASE("deduplication uses the normalised form, not the raw string") {
    Fixture fixture;

    const auto canonical = fixture.shorten("https://example.com/page");
    // Each of these is a different string but provably the same destination.
    for (const char* equivalent : {"HTTPS://EXAMPLE.COM/page",
                                   "https://example.com:443/page",
                                   "https://example.com/page#section"}) {
        CAPTURE(equivalent);
        const auto result = fixture.shorten(equivalent);
        CHECK_FALSE(result.created);
        CHECK(result.link.code == canonical.link.code);
    }
    CHECK(fixture.store->count_links() == 1);
}

TEST_CASE("URLs that differ meaningfully are not collapsed") {
    Fixture fixture;

    const std::set<std::string> distinct = {
        fixture.shorten("https://example.com/page").link.code,
        fixture.shorten("http://example.com/page").link.code,       // scheme matters
        fixture.shorten("https://example.com/page/").link.code,     // trailing slash matters
        fixture.shorten("https://example.com/Page").link.code,      // path case matters
        fixture.shorten("https://example.com/page?a=1").link.code,  // query matters
        fixture.shorten("https://example.com:8080/page").link.code, // non-default port matters
    };
    CHECK(distinct.size() == 6);
}

TEST_CASE("force_new opts out of deduplication") {
    Fixture fixture;

    const auto first = fixture.shorten("https://example.com/campaign");

    ShortenRequest request;
    request.url       = "https://example.com/campaign";
    request.force_new = true;
    const auto second = fixture.service->shorten(request);

    CHECK(second.created);
    CHECK(second.link.code != first.link.code);
    CHECK(fixture.store->count_links() == 2);

    // Both codes still resolve to the same destination, and each tracks
    // separately - that is the entire point of the flag.
    CHECK(fixture.service->resolve(first.link.code).original_url ==
          fixture.service->resolve(second.link.code).original_url);

    // A later default request reuses the *first* code, not the forced one.
    const auto third = fixture.shorten("https://example.com/campaign");
    CHECK_FALSE(third.created);
    CHECK(third.link.code == first.link.code);
}

TEST_CASE("custom aliases are honoured and resolve") {
    Fixture fixture;

    ShortenRequest request;
    request.url          = "https://example.com/docs";
    request.custom_alias = "team-docs";

    const auto result = fixture.service->shorten(request);
    CHECK(result.created);
    CHECK(result.link.code == "team-docs");
    CHECK(result.link.custom_alias);
    CHECK(fixture.service->resolve("team-docs").original_url == "https://example.com/docs");
}

TEST_CASE("a taken alias is a conflict even when the URL matches") {
    Fixture fixture;

    ShortenRequest request;
    request.url          = "https://example.com/docs";
    request.custom_alias = "team-docs";
    REQUIRE(fixture.service->shorten(request).created);

    // Same alias, same URL: still a conflict. Succeeding here would tell a
    // second caller they own a name that someone else created.
    CHECK(code_of([&] { fixture.service->shorten(request); }) == ErrorCode::AliasTaken);

    ShortenRequest other;
    other.url          = "https://example.com/other";
    other.custom_alias = "team-docs";
    CHECK(code_of([&] { fixture.service->shorten(other); }) == ErrorCode::AliasTaken);

    CHECK(fixture.store->count_links() == 1);
}

TEST_CASE("aliases never participate in deduplication in either direction") {
    Fixture fixture;

    ShortenRequest aliased;
    aliased.url          = "https://example.com/launch";
    aliased.custom_alias = "launch";
    REQUIRE(fixture.service->shorten(aliased).created);

    // Someone shortening the same URL without asking for the alias must not be
    // handed the alias - they did not create it and it may be renamed or
    // revoked by its owner.
    const auto anonymous = fixture.shorten("https://example.com/launch");
    CHECK(anonymous.created);
    CHECK(anonymous.link.code != "launch");
    CHECK_FALSE(anonymous.link.custom_alias);

    // And requesting an alias always mints a row, never reuses the generated one.
    ShortenRequest second_alias;
    second_alias.url          = "https://example.com/launch";
    second_alias.custom_alias = "launch-2";
    const auto result = fixture.service->shorten(second_alias);
    CHECK(result.created);
    CHECK(result.link.code == "launch-2");
    CHECK(fixture.store->count_links() == 3);
}

TEST_CASE("invalid and reserved aliases are rejected with distinct errors") {
    Fixture fixture;

    auto attempt = [&](const std::string& alias) {
        ShortenRequest request;
        request.url          = "https://example.com/x";
        request.custom_alias = alias;
        return code_of([&] { fixture.service->shorten(request); });
    };

    CHECK(attempt("ab")        == ErrorCode::InvalidAlias);
    CHECK(attempt("has space") == ErrorCode::InvalidAlias);
    CHECK(attempt("bad/slash") == ErrorCode::InvalidAlias);
    CHECK(attempt("-leading")  == ErrorCode::InvalidAlias);

    // Reserved names would shadow real routes; the check is case-insensitive
    // because a browser will happily request /API.
    CHECK(attempt("api")     == ErrorCode::ReservedAlias);
    CHECK(attempt("API")     == ErrorCode::ReservedAlias);
    CHECK(attempt("healthz") == ErrorCode::ReservedAlias);
    CHECK(attempt("shorten") == ErrorCode::ReservedAlias);

    CHECK(fixture.store->count_links() == 0);
}

TEST_CASE("bad URLs never reach the datastore") {
    Fixture fixture;

    CHECK(code_of([&] { fixture.shorten("not a url"); })            == ErrorCode::InvalidUrl);
    CHECK(code_of([&] { fixture.shorten("javascript:alert(1)"); })  == ErrorCode::UnsupportedScheme);
    CHECK(code_of([&] { fixture.shorten(""); })                     == ErrorCode::InvalidUrl);
    CHECK(fixture.store->count_links() == 0);
}

TEST_CASE("private hosts are refused when the guard is on") {
    Config config;
    config.database_path       = ":memory:";
    config.block_private_hosts = true;
    SqliteLinkStore  store(config.database_path);
    ShortenerService service(config, store);

    ShortenRequest request;
    request.url = "http://169.254.169.254/latest/meta-data/";
    CHECK(code_of([&] { service.shorten(request); }) == ErrorCode::BlockedHost);
    CHECK(store.count_links() == 0);
}

TEST_CASE("resolving with tracking records a click without changing the redirect") {
    Fixture fixture;
    const auto created = fixture.shorten("https://example.com/tracked");

    const Link link = fixture.service->resolve_and_track(
        created.link.code, "https://news.example.org/story", "Mozilla/5.0");
    CHECK(link.original_url == "https://example.com/tracked");

    const LinkStats stats = fixture.service->stats(created.link.code);
    CHECK(stats.link.click_count == 1);
    REQUIRE(stats.top_referrers.size() == 1);
    // Host only: the path of the referring page is none of our business.
    CHECK(stats.top_referrers[0].host == "news.example.org");
}

TEST_CASE("a malformed or absent referrer is stored as direct traffic") {
    Fixture fixture;
    const auto created = fixture.shorten("https://example.com/tracked");

    fixture.service->resolve_and_track(created.link.code, "", "ua");
    fixture.service->resolve_and_track(created.link.code, "not-a-url", "ua");

    const LinkStats stats = fixture.service->stats(created.link.code);
    CHECK(stats.link.click_count == 2);
    CHECK(stats.top_referrers.empty());
}

TEST_CASE("tracking an unknown code fails before it records anything") {
    Fixture fixture;
    CHECK(code_of([&] { fixture.service->resolve_and_track("nosuch", "", ""); }) ==
          ErrorCode::NotFound);
}

TEST_CASE("generated codes stay unique across many shortens") {
    Fixture fixture;

    std::set<std::string> codes;
    for (int i = 0; i < 2000; ++i) {
        codes.insert(fixture.shorten("https://example.com/page/" + std::to_string(i)).link.code);
    }
    CHECK(codes.size() == 2000);
    CHECK(fixture.store->count_links() == 2000);
}
