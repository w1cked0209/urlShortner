#include "urlshort/shortener_service.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <string>

#include "urlshort/errors.hpp"

namespace urlshort {
namespace {

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(::time(nullptr));
}

/// Names that would shadow a real route or a browser convention. Checked
/// case-insensitively because SQLite's default collation is case-sensitive but
/// users' expectations are not.
constexpr std::array<std::string_view, 12> kReservedAliases = {
    "api", "admin", "health", "healthz", "metrics", "shorten",
    "static", "assets", "favicon", "robots", "login", "stats"};

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(c))); });
    return out;
}

/// Referrers are stored as a bare host, never the full URL: the path of the
/// page someone clicked from is frequently sensitive (internal wikis, password
/// reset pages) and we have no use for it.
std::string referrer_host(std::string_view referrer) {
    if (referrer.empty()) return {};
    ParsedUrl parsed;
    if (!UrlValidator::parse(referrer, parsed)) return {};
    return parsed.host;
}

}  // namespace

ShortenerService::ShortenerService(const Config& config, LinkStore& store)
    : config_(config),
      store_(store),
      validator_(config),
      generator_(config.code_length) {}

bool ShortenerService::is_reserved(std::string_view alias) {
    const std::string lowered = to_lower(alias);
    return std::find(kReservedAliases.begin(), kReservedAliases.end(), lowered) !=
           kReservedAliases.end();
}

Link ShortenerService::build_link(const std::string& code,
                                  const std::string& original,
                                  const std::string& normalized,
                                  bool               custom) const {
    Link link;
    link.code           = code;
    link.original_url   = original;
    link.normalized_url = normalized;
    link.custom_alias   = custom;
    link.created_at     = now_seconds();
    return link;
}

ShortenResult ShortenerService::shorten(const ShortenRequest& request) {
    // Validate before touching storage: a bad URL should cost one function
    // call, not a database round-trip.
    const ParsedUrl   parsed     = validator_.validate(request.url);
    const std::string normalized = UrlValidator::normalize(parsed);

    // ---- Custom alias path -------------------------------------------------
    if (request.custom_alias) {
        const std::string& alias = *request.custom_alias;

        if (!CodeGenerator::is_valid_alias(alias)) {
            throw ServiceError(ErrorCode::InvalidAlias,
                               "alias must be 3-32 characters of [A-Za-z0-9_-] and may not "
                               "start or end with '-' or '_'");
        }
        if (is_reserved(alias)) {
            throw ServiceError(ErrorCode::ReservedAlias,
                               "'" + alias + "' is reserved by the service");
        }

        auto stored = store_.insert(build_link(alias, request.url, normalized, /*custom=*/true));
        if (!stored) {
            // Deliberately a conflict even when the existing alias points at
            // the same URL. "Your alias was created" and "someone else's alias
            // happens to match" are different facts, and conflating them would
            // let a caller believe they control a name they do not.
            throw ServiceError(ErrorCode::AliasTaken,
                               "alias '" + alias + "' is already in use");
        }
        return ShortenResult{*stored, /*created=*/true};
    }

    // ---- Duplicate collapsing ---------------------------------------------
    if (!request.force_new) {
        if (auto existing = store_.find_reusable_by_normalized_url(normalized)) {
            // Idempotent by default. Shortening the same link twice is the
            // single most common accidental request, and returning a stable
            // code makes retries safe and keeps the table from filling with
            // synonyms. Callers who want separate analytics per placement pass
            // force_new.
            return ShortenResult{*existing, /*created=*/false};
        }
    }

    // ---- Mint a new code ---------------------------------------------------
    // The pre-check is only an optimisation; `insert` returning nullopt on the
    // UNIQUE constraint is what actually guarantees uniqueness under
    // concurrency, so we loop on that too.
    constexpr int kInsertAttempts = 8;
    for (int attempt = 0; attempt < kInsertAttempts; ++attempt) {
        const std::string code = generator_.generate_unique(
            [this](const std::string& candidate) { return store_.code_exists(candidate); });

        if (auto stored = store_.insert(build_link(code, request.url, normalized, false))) {
            return ShortenResult{*stored, /*created=*/true};
        }
        // Lost the race: another request inserted this exact code in the
        // microseconds between our check and our insert. Redraw.
    }

    throw ServiceError(ErrorCode::CodeExhausted,
                       "could not allocate a short code after repeated collisions");
}

Link ShortenerService::resolve(std::string_view code) const {
    auto link = store_.find_by_code(code);
    if (!link) {
        throw ServiceError(ErrorCode::NotFound, "no link for code '" + std::string(code) + "'");
    }
    return *link;
}

Link ShortenerService::resolve_and_track(std::string_view code,
                                         std::string_view referrer,
                                         std::string_view user_agent) {
    Link link = resolve(code);

    ClickEvent event;
    event.code          = link.code;
    event.clicked_at    = now_seconds();
    event.referrer_host = referrer_host(referrer);
    event.user_agent    = std::string(user_agent);

    try {
        store_.record_click(event);
    } catch (const std::exception&) {
        // Analytics is best-effort. A user who clicked a valid link gets their
        // redirect even if the stats write fails; losing a row is strictly
        // better than serving a 500.
    }

    // Return the pre-click snapshot. Reading the row back purely to report an
    // incremented counter would double the work on the hottest path for no
    // benefit - the redirect response does not carry the count.
    return link;
}

LinkStats ShortenerService::stats(std::string_view code, int days) const {
    auto stats = store_.stats_for(code, days);
    if (!stats) {
        throw ServiceError(ErrorCode::NotFound, "no link for code '" + std::string(code) + "'");
    }
    return *stats;
}

std::string ShortenerService::short_url_for(std::string_view code) const {
    return config_.base_url + "/" + std::string(code);
}

}  // namespace urlshort
