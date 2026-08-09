#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "urlshort/models.hpp"

namespace urlshort {

/// Persistence boundary.
///
/// The service layer talks only to this interface, which keeps the SQLite
/// specifics (and sqlite3.h) out of the business logic and lets the tests run
/// against an in-memory database without any mocking framework.
class LinkStore {
public:
    virtual ~LinkStore() = default;

    /// Insert a new link.
    ///
    /// Returns std::nullopt when the code is already taken - the caller decides
    /// whether that means "retry with a new code" (generated) or "409"
    /// (custom alias). Any other failure throws. The UNIQUE constraint on
    /// `code` is what makes this safe under concurrency; there is no
    /// check-then-insert window.
    virtual std::optional<Link> insert(const Link& link) = 0;

    virtual std::optional<Link> find_by_code(std::string_view code) const = 0;

    /// Duplicate detection. Only auto-generated links are candidates: a custom
    /// alias is a deliberate, named thing and must never be handed to a caller
    /// who merely asked to shorten the same URL.
    virtual std::optional<Link> find_reusable_by_normalized_url(
        std::string_view normalized_url) const = 0;

    virtual bool code_exists(std::string_view code) const = 0;

    /// Record a visit and bump the counter for that code, atomically.
    /// No-op (returns false) if the code does not exist.
    virtual bool record_click(const ClickEvent& event) = 0;

    virtual std::optional<LinkStats> stats_for(std::string_view code, int days) const = 0;

    virtual std::int64_t count_links() const = 0;
};

}  // namespace urlshort
