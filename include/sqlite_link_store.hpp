#pragma once

#include <memory>
#include <string>

#include "urlshort/link_store.hpp"

struct sqlite3;

namespace urlshort {

/// SQLite-backed LinkStore.
///
/// Concurrency model: one shared connection opened with SQLITE_OPEN_FULLMUTEX,
/// so SQLite serialises access internally, plus WAL journaling and a busy
/// timeout. That is a real ceiling on read parallelism and it is a conscious
/// choice for this exercise - a connection pool is ~60 lines and noted in the
/// README as the next step. What matters here is that the invariants
/// (uniqueness, click accounting) live in the schema, so no amount of
/// concurrency can corrupt them.
class SqliteLinkStore final : public LinkStore {
public:
    /// `path` may be ":memory:". Creates the schema if absent.
    explicit SqliteLinkStore(const std::string& path);
    ~SqliteLinkStore() override;

    SqliteLinkStore(const SqliteLinkStore&)            = delete;
    SqliteLinkStore& operator=(const SqliteLinkStore&) = delete;

    std::optional<Link> insert(const Link& link) override;
    std::optional<Link> find_by_code(std::string_view code) const override;
    std::optional<Link> find_reusable_by_normalized_url(
        std::string_view normalized_url) const override;
    bool                code_exists(std::string_view code) const override;
    bool                record_click(const ClickEvent& event) override;
    std::optional<LinkStats> stats_for(std::string_view code, int days) const override;
    std::int64_t        count_links() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace urlshort
