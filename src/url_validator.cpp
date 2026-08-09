#include "urlshort/url_validator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>

#include "urlshort/errors.hpp"

namespace urlshort {
namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

bool is_ctrl_or_space(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u <= 0x20 || u == 0x7F;
}

// Host label charset per RFC 1123, plus '_' which appears in the wild often
// enough (and harmlessly) that rejecting it causes more support tickets than
// it prevents attacks.
bool is_host_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_';
}

bool parse_ipv4(std::string_view host, std::array<unsigned, 4>& octets) {
    unsigned    value  = 0;
    int         digits = 0;
    std::size_t index  = 0;

    for (char c : host) {
        if (c == '.') {
            if (digits == 0 || index >= 3) return false;
            octets[index++] = value;
            value = 0;
            digits = 0;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            if (++digits > 3) return false;
            value = value * 10 + static_cast<unsigned>(c - '0');
            if (value > 255) return false;
        } else {
            return false;
        }
    }
    if (digits == 0 || index != 3) return false;
    octets[3] = value;
    return true;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

UrlValidator::UrlValidator(const Config& config) : config_(config) {}

bool UrlValidator::parse(std::string_view raw, ParsedUrl& out) {
    out = ParsedUrl{};

    // scheme ":" "//"
    const auto scheme_end = raw.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return false;

    const auto scheme = raw.substr(0, scheme_end);
    if (!std::isalpha(static_cast<unsigned char>(scheme.front()))) return false;
    for (char c : scheme) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    out.scheme = to_lower(scheme);

    std::string_view rest = raw.substr(scheme_end + 3);

    // The authority ends at the first of '/', '?' or '#'. Getting this order
    // wrong is the classic parser bug that turns "http://evil.com#@good.com"
    // into a trusted-looking host.
    const auto authority_end = rest.find_first_of("/?#");
    std::string_view authority =
        authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    std::string_view remainder =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    if (authority.empty()) return false;

    // userinfo@host - last '@' wins, since '@' is legal inside userinfo.
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) {
        out.has_userinfo = true;
        authority        = authority.substr(at + 1);
        if (authority.empty()) return false;
    }

    // Host, with IPv6 literals in brackets.
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        out.host = to_lower(authority.substr(0, close + 1));

        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return false;
            const auto port_text = after.substr(1);
            if (port_text.empty()) return false;
            for (char c : port_text) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            }
            out.port = std::atoi(std::string(port_text).c_str());
        }
    } else {
        const auto colon = authority.find(':');
        if (colon == std::string_view::npos) {
            out.host = to_lower(authority);
        } else {
            out.host = to_lower(authority.substr(0, colon));
            const auto port_text = authority.substr(colon + 1);
            if (port_text.empty() || port_text.size() > 5) return false;
            for (char c : port_text) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            }
            out.port = std::atoi(std::string(port_text).c_str());
            if (out.port < 1 || out.port > 65535) return false;
        }
    }

    if (out.host.empty()) return false;

    // path / query / fragment
    if (!remainder.empty()) {
        auto fragment_start = remainder.find('#');
        if (fragment_start != std::string_view::npos) {
            out.fragment = std::string(remainder.substr(fragment_start + 1));
            remainder    = remainder.substr(0, fragment_start);
        }
        auto query_start = remainder.find('?');
        if (query_start != std::string_view::npos) {
            out.query = std::string(remainder.substr(query_start + 1));
            remainder = remainder.substr(0, query_start);
        }
        out.path = std::string(remainder);
    }

    return true;
}

bool UrlValidator::is_private_host(std::string_view host) {
    const std::string h = to_lower(host);

    if (h == "localhost" || ends_with(h, ".localhost") || ends_with(h, ".local") ||
        ends_with(h, ".internal") || ends_with(h, ".home.arpa")) {
        return true;
    }

    // IPv6 literal.
    if (!h.empty() && h.front() == '[') {
        const std::string inner = h.substr(1, h.size() >= 2 ? h.size() - 2 : 0);
        if (inner == "::1" || inner == "::") return true;
        if (starts_with(inner, "fe80:") || starts_with(inner, "fc") ||
            starts_with(inner, "fd")) {
            return true;
        }
        // IPv4-mapped (::ffff:10.0.0.1) - fall through to the v4 check.
        const auto last_colon = inner.rfind(':');
        if (last_colon != std::string::npos) {
            return is_private_host(inner.substr(last_colon + 1));
        }
        return false;
    }

    std::array<unsigned, 4> o{};
    if (!parse_ipv4(h, o)) return false;

    if (o[0] == 0) return true;                                  // 0.0.0.0/8
    if (o[0] == 10) return true;                                 // RFC1918
    if (o[0] == 127) return true;                                // loopback
    if (o[0] == 169 && o[1] == 254) return true;                 // link-local
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return true;    // RFC1918
    if (o[0] == 192 && o[1] == 168) return true;                 // RFC1918
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return true;   // CGNAT
    if (o[0] >= 224) return true;                                // multicast + reserved
    return false;
}

ParsedUrl UrlValidator::validate(std::string_view raw) const {
    if (raw.empty()) {
        throw ServiceError(ErrorCode::InvalidUrl, "url must not be empty");
    }
    if (raw.size() > config_.max_url_length) {
        throw ServiceError(ErrorCode::UrlTooLong,
                           "url exceeds " + std::to_string(config_.max_url_length) + " characters");
    }
    // Control characters and raw spaces let an attacker smuggle a second
    // request line past a naive proxy. Reject rather than escape: we do not
    // want to guess what the caller meant.
    for (char c : raw) {
        if (is_ctrl_or_space(c)) {
            throw ServiceError(ErrorCode::InvalidUrl,
                               "url must not contain spaces or control characters");
        }
    }

    // Opaque schemes - "javascript:alert(1)", "data:text/html,...", "mailto:x"
    // - have no "//" authority, so the parser below would reject them as
    // unparseable garbage. They are not garbage; they are the dangerous case,
    // and the caller deserves to be told which of their assumptions is wrong.
    // So we sniff the scheme first and report it precisely.
    const auto colon = raw.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        bool scheme_like = std::isalpha(static_cast<unsigned char>(raw.front())) != 0;
        for (std::size_t i = 0; scheme_like && i < colon; ++i) {
            const char c = raw[i];
            scheme_like = std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' ||
                          c == '.';
        }
        if (scheme_like) {
            const std::string scheme = to_lower(raw.substr(0, colon));
            if (scheme != "http" && scheme != "https") {
                throw ServiceError(ErrorCode::UnsupportedScheme,
                                   "only http and https are supported, got: " + scheme);
            }
        }
    }

    ParsedUrl parsed;
    if (!parse(raw, parsed)) {
        throw ServiceError(ErrorCode::InvalidUrl,
                           "url must be absolute, e.g. https://example.com/page");
    }

    if (parsed.scheme != "http" && parsed.scheme != "https") {
        // javascript:, data: and file: are the interesting ones - a shortener
        // that redirects to them is a stored-XSS delivery service.
        throw ServiceError(ErrorCode::UnsupportedScheme,
                           "only http and https are supported, got: " + parsed.scheme);
    }

    if (parsed.has_userinfo) {
        // "https://user:pass@host" in a shared short link leaks credentials to
        // everyone who clicks it, and is a well-known phishing disguise.
        throw ServiceError(ErrorCode::InvalidUrl, "url must not embed credentials");
    }

    const bool is_ipv6 = parsed.host.front() == '[';
    if (!is_ipv6) {
        if (parsed.host.size() > 253) {
            throw ServiceError(ErrorCode::InvalidUrl, "hostname is too long");
        }
        for (char c : parsed.host) {
            if (!is_host_char(c)) {
                throw ServiceError(ErrorCode::InvalidUrl,
                                   "hostname contains invalid characters");
            }
        }
        if (parsed.host.front() == '.' || parsed.host.back() == '.' ||
            parsed.host.find("..") != std::string::npos) {
            throw ServiceError(ErrorCode::InvalidUrl, "malformed hostname");
        }

        std::array<unsigned, 4> octets{};
        const bool is_ipv4 = parse_ipv4(parsed.host, octets);
        // A dotless host is either an intranet name or a typo. Both are things
        // we would rather not hand a permanent public redirect to.
        if (!is_ipv4 && parsed.host.find('.') == std::string::npos) {
            throw ServiceError(ErrorCode::InvalidUrl,
                               "hostname must be fully qualified, e.g. example.com");
        }
        std::size_t label_length = 0;
        for (char c : parsed.host) {
            label_length = (c == '.') ? 0 : label_length + 1;
            if (label_length > 63) {
                throw ServiceError(ErrorCode::InvalidUrl, "hostname label is too long");
            }
        }
    }

    if (config_.block_private_hosts && is_private_host(parsed.host)) {
        throw ServiceError(ErrorCode::BlockedHost,
                           "refusing to shorten a link to a private or loopback address");
    }

    return parsed;
}

std::string UrlValidator::normalize(const ParsedUrl& parsed) {
    std::string out = parsed.scheme + "://" + parsed.host;

    const bool default_port = (parsed.scheme == "http"  && parsed.port == 80) ||
                              (parsed.scheme == "https" && parsed.port == 443);
    if (parsed.port > 0 && !default_port) {
        out += ":" + std::to_string(parsed.port);
    }

    out += parsed.path.empty() ? "/" : parsed.path;

    // An empty query ("http://x/?") is equivalent to no query at all.
    if (!parsed.query.empty()) {
        out += "?" + parsed.query;
    }
    // Fragment intentionally dropped: it never reaches the origin server, so
    // two URLs differing only by fragment are the same resource to us.
    return out;
}

}  // namespace urlshort
