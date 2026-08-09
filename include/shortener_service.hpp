#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "urlshort/code_generator.hpp"
#include "urlshort/config.hpp"
#include "urlshort/link_store.hpp"
#include "urlshort/models.hpp"
#include "urlshort/url_validator.hpp"

namespace urlshort {

/// All the business rules, and nothing about HTTP.
///
/// Everything the take-home asks you to "decide intentionally" is decided here
/// and only here, which is also what makes it directly testable.
class ShortenerService {
public:
    ShortenerService(const Config& config, LinkStore& store);

    /// Create (or reuse) a short link.
    ///
    /// DUPLICATE POLICY - idempotent by default, opt out per request:
    ///   * Same URL twice          -> same code, `created == false`.
    ///   * `force_new: true`       -> a second, independently tracked code.
    ///   * Custom alias            -> always a new row; never dedupe-matched,
    ///                                and never returned to someone who did not
    ///                                ask for it by name.
    /// Rationale in the README; the short version is that the common case
    /// (someone pasting the same link twice) should be free and stable, while
    /// per-campaign analytics stays available to anyone who asks for it.
    ///
    /// ALIAS POLICY: taking an alias that already exists is a 409, even if it
    /// points at the same URL. Silently succeeding would tell the caller they
    /// own a name they do not.
    ShortenResult shorten(const ShortenRequest& request);

    /// Resolve for redirect. Throws ErrorCode::NotFound for unknown codes.
    Link resolve(std::string_view code) const;

    /// Resolve *and* record the visit. Recording failures are swallowed: a
    /// broken analytics write must never cost a user their redirect.
    Link resolve_and_track(std::string_view code,
                           std::string_view referrer,
                           std::string_view user_agent);

    LinkStats stats(std::string_view code, int days = 7) const;

    /// Absolute short URL for a code, using Config::base_url.
    std::string short_url_for(std::string_view code) const;

    /// Route names a custom alias may not shadow.
    static bool is_reserved(std::string_view alias);

private:
    Link build_link(const std::string& code,
                    const std::string& original,
                    const std::string& normalized,
                    bool               custom) const;

    const Config&  config_;
    LinkStore&     store_;
    UrlValidator   validator_;
    CodeGenerator  generator_;
};

}  // namespace urlshort
