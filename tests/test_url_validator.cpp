#include <doctest/doctest.h>

#include "urlshort/config.hpp"
#include "urlshort/errors.hpp"
#include "urlshort/url_validator.hpp"

using namespace urlshort;

namespace {

Config permissive_config() {
    Config config;
    config.block_private_hosts = false;  // exercised separately below
    return config;
}

std::string normalized(const UrlValidator& validator, const std::string& url) {
    return UrlValidator::normalize(validator.validate(url));
}

}  // namespace

TEST_CASE("accepts ordinary http and https URLs") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    CHECK_NOTHROW(validator.validate("https://example.com"));
    CHECK_NOTHROW(validator.validate("http://example.com/a/b?c=d#e"));
    CHECK_NOTHROW(validator.validate("https://sub.domain.example.co.uk:8443/path"));
    CHECK_NOTHROW(validator.validate("https://192.0.2.10/page"));
    CHECK_NOTHROW(validator.validate("https://[2001:db8::1]/page"));
}

TEST_CASE("rejects URLs that are not absolute http(s)") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    CHECK_THROWS_AS(validator.validate(""), ServiceError);
    CHECK_THROWS_AS(validator.validate("example.com"), ServiceError);
    CHECK_THROWS_AS(validator.validate("/just/a/path"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://"), ServiceError);
    CHECK_THROWS_AS(validator.validate("://example.com"), ServiceError);
}

TEST_CASE("rejects dangerous schemes rather than merely unknown ones") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    // javascript: and data: are the ones that turn a shortener into an XSS
    // delivery mechanism, so this test is about security, not tidiness.
    for (const char* url : {"javascript:alert(1)//x.com",
                            "data:text/html,<script>alert(1)</script>",
                            "file:///etc/passwd",
                            "ftp://example.com/file"}) {
        CAPTURE(url);
        CHECK_THROWS_AS(validator.validate(url), ServiceError);
    }
}

TEST_CASE("scheme rejection reports UnsupportedScheme, not a generic parse error") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    try {
        validator.validate("ftp://example.com/file");
        FAIL("expected ftp:// to be rejected");
    } catch (const ServiceError& error) {
        CHECK(error.code() == ErrorCode::UnsupportedScheme);
    }
}

TEST_CASE("rejects embedded credentials") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    CHECK_THROWS_AS(validator.validate("https://user:pass@example.com/"), ServiceError);
}

TEST_CASE("the authority ends at the first of / ? #") {
    // The classic parser bug: "http://evil.com#@good.com" must resolve to
    // evil.com, not good.com. Getting this backwards is how open redirects and
    // spoofed link previews happen.
    ParsedUrl parsed;
    REQUIRE(UrlValidator::parse("http://evil.com#@good.com", parsed));
    CHECK(parsed.host == "evil.com");

    REQUIRE(UrlValidator::parse("http://evil.com?@good.com", parsed));
    CHECK(parsed.host == "evil.com");

    REQUIRE(UrlValidator::parse("http://evil.com/@good.com", parsed));
    CHECK(parsed.host == "evil.com");
}

TEST_CASE("rejects control characters and whitespace") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    CHECK_THROWS_AS(validator.validate("https://example.com/a b"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://example.com/a\nHost: evil"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://example.com/\t"), ServiceError);
}

TEST_CASE("enforces the length ceiling") {
    Config config = permissive_config();
    config.max_url_length = 64;
    UrlValidator validator(config);

    const std::string long_url = "https://example.com/" + std::string(100, 'a');
    try {
        validator.validate(long_url);
        FAIL("expected the length check to fire");
    } catch (const ServiceError& error) {
        CHECK(error.code() == ErrorCode::UrlTooLong);
    }
}

TEST_CASE("rejects malformed hostnames") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    CHECK_THROWS_AS(validator.validate("https://.example.com/"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://example..com/"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://exa mple.com/"), ServiceError);
    CHECK_THROWS_AS(validator.validate("https://intranet/"), ServiceError);  // dotless
    CHECK_THROWS_AS(
        validator.validate("https://" + std::string(64, 'a') + ".com/"), ServiceError);
}

TEST_CASE("identifies private and loopback hosts") {
    CHECK(UrlValidator::is_private_host("localhost"));
    CHECK(UrlValidator::is_private_host("app.localhost"));
    CHECK(UrlValidator::is_private_host("printer.local"));
    CHECK(UrlValidator::is_private_host("127.0.0.1"));
    CHECK(UrlValidator::is_private_host("10.1.2.3"));
    CHECK(UrlValidator::is_private_host("172.16.0.1"));
    CHECK(UrlValidator::is_private_host("172.31.255.255"));
    CHECK(UrlValidator::is_private_host("192.168.1.1"));
    CHECK(UrlValidator::is_private_host("169.254.169.254"));  // cloud metadata
    CHECK(UrlValidator::is_private_host("100.64.0.1"));       // CGNAT
    CHECK(UrlValidator::is_private_host("0.0.0.0"));
    CHECK(UrlValidator::is_private_host("[::1]"));
    CHECK(UrlValidator::is_private_host("[fd00::1]"));

    // 172.32.x is *outside* RFC1918 - off-by-one in that range is a common bug.
    CHECK_FALSE(UrlValidator::is_private_host("172.32.0.1"));
    CHECK_FALSE(UrlValidator::is_private_host("8.8.8.8"));
    CHECK_FALSE(UrlValidator::is_private_host("example.com"));
    CHECK_FALSE(UrlValidator::is_private_host("192.169.1.1"));
}

TEST_CASE("blocks private hosts only when configured to") {
    Config blocking;
    blocking.block_private_hosts = true;
    UrlValidator strict(blocking);

    try {
        strict.validate("http://169.254.169.254/latest/meta-data/");
        FAIL("expected the metadata endpoint to be blocked");
    } catch (const ServiceError& error) {
        CHECK(error.code() == ErrorCode::BlockedHost);
    }

    const Config allowing = permissive_config();
    UrlValidator relaxed(allowing);
    CHECK_NOTHROW(relaxed.validate("http://169.254.169.254/latest/meta-data/"));
}

TEST_CASE("normalisation folds only what cannot change the destination") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    // Case of scheme and host is not significant; case of the path is.
    CHECK(normalized(validator, "HTTPS://EXAMPLE.COM/Path") == "https://example.com/Path");

    // Default ports are equivalent to no port.
    CHECK(normalized(validator, "http://example.com:80/x")  == "http://example.com/x");
    CHECK(normalized(validator, "https://example.com:443/x") == "https://example.com/x");
    CHECK(normalized(validator, "http://example.com:8080/x") == "http://example.com:8080/x");

    // Missing path means root.
    CHECK(normalized(validator, "https://example.com") == "https://example.com/");

    // Fragments never reach the origin server.
    CHECK(normalized(validator, "https://example.com/x#section") == "https://example.com/x");

    // An empty query is no query.
    CHECK(normalized(validator, "https://example.com/x?") == "https://example.com/x");
}

TEST_CASE("normalisation deliberately preserves query semantics") {
    const Config config = permissive_config();
    UrlValidator validator(config);

    // Reordering parameters or stripping utm_* would be a normalisation that
    // changes where the link points for some sites. We do not do it, and this
    // test pins that decision so nobody "helpfully" adds it later.
    CHECK(normalized(validator, "https://example.com/x?a=1&b=2") !=
          normalized(validator, "https://example.com/x?b=2&a=1"));
    CHECK(normalized(validator, "https://example.com/x?utm_source=news") ==
          "https://example.com/x?utm_source=news");

    // Trailing slash is significant to plenty of servers, so it stays.
    CHECK(normalized(validator, "https://example.com/x/") != normalized(validator, "https://example.com/x"));
}
