#pragma once

#include <string>
#include <string_view>

#include "urlshort/config.hpp"

namespace urlshort {

/// The pieces of a URL we care about. Not a general-purpose URL type - it
/// carries exactly what validation and normalisation need.
struct ParsedUrl {
    std::string scheme;    ///< Lower-cased.
    std::string host;      ///< Lower-cased; IPv6 keeps its brackets.
    int         port = -1; ///< -1 when absent.
    std::string path;      ///< "" when absent (normalised to "/" later).
    std::string query;     ///< Without the leading '?'.
    std::string fragment;  ///< Without the leading '#'.
    bool        has_userinfo = false;
};

/// Validates and normalises URLs before they enter the datastore.
///
/// Written by hand rather than with a regex: URL grammar has enough context
/// sensitivity (userinfo vs host, IPv6 literals, the first of "/?#" ending the
/// authority) that a one-line regex is either wrong or unreadable, and usually
/// both.
class UrlValidator {
public:
    explicit UrlValidator(const Config& config);

    /// Parse without judging. Returns false if the string is not an absolute
    /// URL at all.
    static bool parse(std::string_view raw, ParsedUrl& out);

    /// Full check. Throws ServiceError with a specific ErrorCode on rejection,
    /// so callers get a usable message instead of a bare bool.
    ParsedUrl validate(std::string_view raw) const;

    /// Canonical form used as the duplicate-detection key.
    ///
    /// We fold only the parts that provably cannot change the response:
    /// scheme/host case, the default port, an empty path, and the fragment
    /// (never sent to the server). We deliberately do NOT sort or strip query
    /// parameters - "?a=1&b=2" and "?b=2&a=1" are the same page for most sites
    /// but not all, and dropping utm_* would rewrite someone's campaign.
    static std::string normalize(const ParsedUrl& parsed);

    /// True for loopback, RFC1918, link-local, CGNAT, and the usual internal
    /// hostname suffixes. Exposed for testing.
    static bool is_private_host(std::string_view host);

private:
    const Config& config_;
};

}  // namespace urlshort
