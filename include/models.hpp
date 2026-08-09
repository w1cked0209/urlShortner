#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace urlshort {

/// A stored short-link.
///
/// `original_url` is what we redirect to and what we echo back to humans.
/// `normalized_url` is what we *index* for duplicate detection. Keeping both
/// means normalisation can get smarter later without rewriting history or
/// silently changing where existing links point.
struct Link {
    std::int64_t id             = 0;
    std::string  code;
    std::string  original_url;
    std::string  normalized_url;
    bool         custom_alias   = false;
    std::int64_t created_at     = 0;  ///< Unix seconds, UTC.
    std::int64_t click_count    = 0;  ///< Denormalised counter, see LinkStore.
};

/// One recorded visit. Deliberately free of IP addresses - see README
/// "What we do not store".
struct ClickEvent {
    std::string  code;
    std::int64_t clicked_at = 0;
    std::string  referrer_host;  ///< Host only, never the full referring URL.
    std::string  user_agent;     ///< Truncated to 512 bytes on write.
};

struct ReferrerCount {
    std::string  host;
    std::int64_t count = 0;
};

struct DailyCount {
    std::string  day;  ///< YYYY-MM-DD, UTC.
    std::int64_t count = 0;
};

/// Aggregated analytics for a single code.
struct LinkStats {
    Link                       link;
    std::optional<std::int64_t> last_clicked_at;
    std::vector<ReferrerCount>  top_referrers;
    std::vector<DailyCount>     clicks_by_day;
};

/// Input to ShortenerService::shorten.
struct ShortenRequest {
    std::string                url;
    std::optional<std::string> custom_alias;
    /// Opt out of duplicate collapsing when you want a second, independently
    /// tracked link to the same destination.
    bool                       force_new = false;
};

struct ShortenResult {
    Link link;
    /// false => we returned a pre-existing link instead of minting one.
    /// The HTTP layer turns this into 201 vs 200.
    bool created = true;
};

}  // namespace urlshort
