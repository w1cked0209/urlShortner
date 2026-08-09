#include "urlshort/sqlite_link_store.hpp"

#include <sqlite3.h>

#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>

#include "urlshort/errors.hpp"

namespace urlshort {
namespace {

/// The schema *is* the data model, so it lives in one readable block rather
/// than being assembled by a migration framework we do not need yet.
///
/// Two constraints carry real weight:
///   * links.code UNIQUE - the authority on short-code uniqueness. Every
///     collision argument in CodeGenerator ultimately cashes out here.
///   * the partial index on normalized_url WHERE custom_alias = 0 - makes
///     duplicate lookup an index seek while structurally excluding aliases
///     from dedupe, so the policy cannot be violated by a careless query.
constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS links (
    id             INTEGER PRIMARY KEY,
    code           TEXT    NOT NULL UNIQUE,
    original_url   TEXT    NOT NULL,
    normalized_url TEXT    NOT NULL,
    custom_alias   INTEGER NOT NULL DEFAULT 0 CHECK (custom_alias IN (0, 1)),
    created_at     INTEGER NOT NULL,
    click_count    INTEGER NOT NULL DEFAULT 0 CHECK (click_count >= 0)
);

CREATE INDEX IF NOT EXISTS idx_links_dedupe
    ON links (normalized_url) WHERE custom_alias = 0;

CREATE TABLE IF NOT EXISTS clicks (
    id            INTEGER PRIMARY KEY,
    link_id       INTEGER NOT NULL REFERENCES links (id) ON DELETE CASCADE,
    clicked_at    INTEGER NOT NULL,
    referrer_host TEXT    NOT NULL DEFAULT '',
    user_agent    TEXT    NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_clicks_link_time ON clicks (link_id, clicked_at);
)sql";

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
    throw ServiceError(ErrorCode::Internal,
                       what + ": " + (db ? sqlite3_errmsg(db) : "unknown sqlite error"));
}

/// Minimal RAII wrapper over sqlite3_stmt.
///
/// Exists so that every query site is bind-and-step with no manual finalize,
/// which is what keeps this file free of leaks on the throwing paths. All
/// values go through sqlite3_bind_*, so there is no string concatenation
/// anywhere near the SQL and therefore no injection surface.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            fail(db, "failed to prepare statement");
        }
    }

    ~Stmt() { sqlite3_finalize(stmt_); }

    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int index, std::string_view value) {
        // SQLITE_TRANSIENT: sqlite copies the bytes, so the caller's buffer
        // does not have to outlive the step().
        if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            fail(db_, "failed to bind text");
        }
        return *this;
    }

    Stmt& bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            fail(db_, "failed to bind integer");
        }
        return *this;
    }

    /// true when a row is available, false at end of result set.
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW)  return true;
        if (rc == SQLITE_DONE) return false;
        fail(db_, "failed to execute statement");
    }

    /// Like step(), but reports a UNIQUE violation instead of throwing so the
    /// caller can treat "code taken" as an expected outcome.
    bool step_allowing_constraint(bool& constraint_violation) {
        const int rc = sqlite3_step(stmt_);
        constraint_violation = false;
        if (rc == SQLITE_ROW)  return true;
        if (rc == SQLITE_DONE) return false;
        if (rc == SQLITE_CONSTRAINT) {
            constraint_violation = true;
            return false;
        }
        fail(db_, "failed to execute statement");
    }

    std::int64_t column_int(int index) const { return sqlite3_column_int64(stmt_, index); }

    std::string column_text(int index) const {
        const auto* text = sqlite3_column_text(stmt_, index);
        if (text == nullptr) return {};
        return std::string(reinterpret_cast<const char*>(text),
                           static_cast<std::size_t>(sqlite3_column_bytes(stmt_, index)));
    }

    bool column_is_null(int index) const {
        return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
    }

private:
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

Link read_link(const Stmt& row) {
    Link link;
    link.id             = row.column_int(0);
    link.code           = row.column_text(1);
    link.original_url   = row.column_text(2);
    link.normalized_url = row.column_text(3);
    link.custom_alias   = row.column_int(4) != 0;
    link.created_at     = row.column_int(5);
    link.click_count    = row.column_int(6);
    return link;
}

constexpr const char* kLinkColumns =
    "id, code, original_url, normalized_url, custom_alias, created_at, click_count";

}  // namespace

struct SqliteLinkStore::Impl {
    sqlite3*           db = nullptr;
    mutable std::mutex write_mutex;  ///< Serialises multi-statement transactions.

    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "unknown error";
            sqlite3_free(error);
            throw ServiceError(ErrorCode::Internal, "sqlite exec failed: " + message);
        }
    }
};

SqliteLinkStore::SqliteLinkStore(const std::string& path) : impl_(std::make_unique<Impl>()) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &impl_->db, flags, nullptr) != SQLITE_OK) {
        const std::string message = impl_->db ? sqlite3_errmsg(impl_->db) : "unknown";
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
        throw ServiceError(ErrorCode::Internal, "cannot open database '" + path + "': " + message);
    }

    // WAL lets readers proceed during a write, which matters because every
    // redirect performs one. Ignored (harmlessly) for :memory: databases.
    impl_->exec("PRAGMA journal_mode = WAL;");
    // Without this, a concurrent writer surfaces as SQLITE_BUSY to the user
    // instead of a 20 ms wait.
    impl_->exec("PRAGMA busy_timeout = 5000;");
    // NORMAL trades "lose the last few click rows if the machine loses power"
    // for a large write speedup. Acceptable for analytics; FULL would be the
    // right call if this table were billing data.
    impl_->exec("PRAGMA synchronous = NORMAL;");
    impl_->exec("PRAGMA foreign_keys = ON;");
    impl_->exec(kSchema);
}

SqliteLinkStore::~SqliteLinkStore() {
    if (impl_ && impl_->db) {
        sqlite3_close(impl_->db);
    }
}

std::optional<Link> SqliteLinkStore::insert(const Link& link) {
    Stmt stmt(impl_->db,
              "INSERT INTO links (code, original_url, normalized_url, custom_alias, created_at, "
              "click_count) VALUES (?, ?, ?, ?, ?, 0);");
    stmt.bind(1, link.code)
        .bind(2, link.original_url)
        .bind(3, link.normalized_url)
        .bind(4, static_cast<std::int64_t>(link.custom_alias ? 1 : 0))
        .bind(5, link.created_at);

    bool constraint_violation = false;
    stmt.step_allowing_constraint(constraint_violation);
    if (constraint_violation) {
        // Someone else took this code between our check and our insert. That is
        // the race the UNIQUE index exists to lose safely.
        return std::nullopt;
    }

    Link stored = link;
    stored.id   = sqlite3_last_insert_rowid(impl_->db);
    stored.click_count = 0;
    return stored;
}

std::optional<Link> SqliteLinkStore::find_by_code(std::string_view code) const {
    Stmt stmt(impl_->db,
              ("SELECT " + std::string(kLinkColumns) + " FROM links WHERE code = ?;").c_str());
    stmt.bind(1, code);
    if (!stmt.step()) return std::nullopt;
    return read_link(stmt);
}

std::optional<Link> SqliteLinkStore::find_reusable_by_normalized_url(
    std::string_view normalized_url) const {
    // custom_alias = 0 matches the partial index and enforces the policy that
    // named links are never recycled into anonymous shorten requests.
    // Oldest-first so the answer is stable no matter how many duplicates exist.
    Stmt stmt(impl_->db,
              ("SELECT " + std::string(kLinkColumns) +
               " FROM links WHERE normalized_url = ? AND custom_alias = 0 "
               "ORDER BY id LIMIT 1;")
                  .c_str());
    stmt.bind(1, normalized_url);
    if (!stmt.step()) return std::nullopt;
    return read_link(stmt);
}

bool SqliteLinkStore::code_exists(std::string_view code) const {
    Stmt stmt(impl_->db, "SELECT 1 FROM links WHERE code = ? LIMIT 1;");
    stmt.bind(1, code);
    return stmt.step();
}

bool SqliteLinkStore::record_click(const ClickEvent& event) {
    // Two writes that must agree, so they go in one IMMEDIATE transaction: the
    // denormalised counter on `links` is only trustworthy if it can never drift
    // from the `clicks` rows.
    std::lock_guard<std::mutex> guard(impl_->write_mutex);

    impl_->exec("BEGIN IMMEDIATE;");
    try {
        std::int64_t link_id = 0;
        {
            Stmt lookup(impl_->db, "SELECT id FROM links WHERE code = ?;");
            lookup.bind(1, event.code);
            if (!lookup.step()) {
                impl_->exec("ROLLBACK;");
                return false;
            }
            link_id = lookup.column_int(0);
        }

        {
            // Truncate rather than reject: a hostile User-Agent header should
            // cost us 512 bytes, not a failed redirect.
            std::string user_agent = event.user_agent.substr(0, 512);
            Stmt insert(impl_->db,
                        "INSERT INTO clicks (link_id, clicked_at, referrer_host, user_agent) "
                        "VALUES (?, ?, ?, ?);");
            insert.bind(1, link_id)
                .bind(2, event.clicked_at)
                .bind(3, event.referrer_host.substr(0, 253))
                .bind(4, user_agent);
            insert.step();
        }

        {
            Stmt bump(impl_->db,
                      "UPDATE links SET click_count = click_count + 1 WHERE id = ?;");
            bump.bind(1, link_id);
            bump.step();
        }

        impl_->exec("COMMIT;");
        return true;
    } catch (...) {
        impl_->exec("ROLLBACK;");
        throw;
    }
}

std::optional<LinkStats> SqliteLinkStore::stats_for(std::string_view code, int days) const {
    auto link = find_by_code(code);
    if (!link) return std::nullopt;

    LinkStats stats;
    stats.link = *link;

    {
        Stmt stmt(impl_->db, "SELECT MAX(clicked_at) FROM clicks WHERE link_id = ?;");
        stmt.bind(1, link->id);
        if (stmt.step() && !stmt.column_is_null(0)) {
            stats.last_clicked_at = stmt.column_int(0);
        }
    }

    {
        // Direct traffic (no Referer header) is stored as '' and excluded here
        // so the "top referrers" list is not dominated by a meaningless bucket.
        Stmt stmt(impl_->db,
                  "SELECT referrer_host, COUNT(*) AS n FROM clicks "
                  "WHERE link_id = ? AND referrer_host <> '' "
                  "GROUP BY referrer_host ORDER BY n DESC, referrer_host ASC LIMIT 5;");
        stmt.bind(1, link->id);
        while (stmt.step()) {
            stats.top_referrers.push_back({stmt.column_text(0), stmt.column_int(1)});
        }
    }

    {
        // Bucketing is done by SQLite's date() in UTC rather than in C++: it
        // keeps the aggregation in the index scan and avoids pulling every
        // click row into the process just to count them.
        Stmt stmt(impl_->db,
                  "SELECT date(clicked_at, 'unixepoch') AS day, COUNT(*) AS n FROM clicks "
                  "WHERE link_id = ? AND clicked_at >= ? GROUP BY day ORDER BY day;");
        const std::int64_t cutoff =
            static_cast<std::int64_t>(::time(nullptr)) -
            static_cast<std::int64_t>(days) * 86400;
        stmt.bind(1, link->id).bind(2, cutoff);
        while (stmt.step()) {
            stats.clicks_by_day.push_back({stmt.column_text(0), stmt.column_int(1)});
        }
    }

    return stats;
}

std::int64_t SqliteLinkStore::count_links() const {
    Stmt stmt(impl_->db, "SELECT COUNT(*) FROM links;");
    return stmt.step() ? stmt.column_int(0) : 0;
}

}  // namespace urlshort
