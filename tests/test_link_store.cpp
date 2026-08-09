#include <doctest/doctest.h>

#include <atomic>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "urlshort/sqlite_link_store.hpp"

using namespace urlshort;

namespace {

Link make_link(std::string code, std::string url, bool custom = false) {
    Link link;
    link.code           = std::move(code);
    link.original_url   = url;
    link.normalized_url = std::move(url);
    link.custom_alias   = custom;
    link.created_at     = static_cast<std::int64_t>(::time(nullptr));
    return link;
}

}  // namespace

TEST_CASE("insert then read back round-trips every field") {
    SqliteLinkStore store(":memory:");

    Link link          = make_link("abc1234", "https://example.com/page");
    link.original_url  = "https://EXAMPLE.com/page";  // differs from normalized

    const auto stored = store.insert(link);
    REQUIRE(stored.has_value());
    CHECK(stored->id > 0);
    CHECK(stored->click_count == 0);

    const auto found = store.find_by_code("abc1234");
    REQUIRE(found.has_value());
    CHECK(found->code           == "abc1234");
    CHECK(found->original_url   == "https://EXAMPLE.com/page");
    CHECK(found->normalized_url == "https://example.com/page");
    CHECK(found->custom_alias   == false);
    CHECK(found->created_at     == link.created_at);
}

TEST_CASE("unknown codes are absent, not errors") {
    SqliteLinkStore store(":memory:");
    CHECK_FALSE(store.find_by_code("nope123").has_value());
    CHECK_FALSE(store.code_exists("nope123"));
    CHECK_FALSE(store.stats_for("nope123", 7).has_value());
    CHECK(store.count_links() == 0);
}

TEST_CASE("the UNIQUE constraint makes a duplicate code a recoverable outcome") {
    // This is the mechanism the whole no-collisions argument rests on: a losing
    // insert must return empty rather than throwing or, far worse, overwriting.
    SqliteLinkStore store(":memory:");

    REQUIRE(store.insert(make_link("dup1234", "https://a.example.com/")).has_value());
    const auto second = store.insert(make_link("dup1234", "https://b.example.com/"));
    CHECK_FALSE(second.has_value());

    // The original survived untouched.
    const auto found = store.find_by_code("dup1234");
    REQUIRE(found.has_value());
    CHECK(found->original_url == "https://a.example.com/");
    CHECK(store.count_links() == 1);
}

TEST_CASE("dedupe lookup ignores custom aliases") {
    SqliteLinkStore   store(":memory:");
    const std::string url = "https://example.com/shared";

    REQUIRE(store.insert(make_link("my-alias", url, /*custom=*/true)).has_value());

    // Only an alias exists, so there is nothing reusable yet.
    CHECK_FALSE(store.find_reusable_by_normalized_url(url).has_value());

    REQUIRE(store.insert(make_link("gen1234", url)).has_value());
    const auto reusable = store.find_reusable_by_normalized_url(url);
    REQUIRE(reusable.has_value());
    CHECK(reusable->code == "gen1234");
}

TEST_CASE("dedupe lookup is stable when several duplicates exist") {
    SqliteLinkStore   store(":memory:");
    const std::string url = "https://example.com/x";

    REQUIRE(store.insert(make_link("first00", url)).has_value());
    REQUIRE(store.insert(make_link("second0", url)).has_value());
    REQUIRE(store.insert(make_link("third00", url)).has_value());

    // Oldest wins, every time - otherwise the "same URL gives the same code"
    // promise would depend on row order.
    for (int i = 0; i < 5; ++i) {
        const auto reusable = store.find_reusable_by_normalized_url(url);
        REQUIRE(reusable.has_value());
        CHECK(reusable->code == "first00");
    }
}

TEST_CASE("recording a click updates the counter and the event log together") {
    SqliteLinkStore store(":memory:");
    REQUIRE(store.insert(make_link("clk1234", "https://example.com/")).has_value());

    ClickEvent event;
    event.code          = "clk1234";
    event.clicked_at    = static_cast<std::int64_t>(::time(nullptr));
    event.referrer_host = "news.example.org";
    event.user_agent    = "curl/8.0";

    CHECK(store.record_click(event));
    CHECK(store.record_click(event));

    const auto link = store.find_by_code("clk1234");
    REQUIRE(link.has_value());
    CHECK(link->click_count == 2);

    const auto stats = store.stats_for("clk1234", 7);
    REQUIRE(stats.has_value());
    CHECK(stats->link.click_count == 2);
    REQUIRE(stats->last_clicked_at.has_value());
    CHECK(*stats->last_clicked_at == event.clicked_at);
    REQUIRE(stats->top_referrers.size() == 1);
    CHECK(stats->top_referrers[0].host  == "news.example.org");
    CHECK(stats->top_referrers[0].count == 2);
    REQUIRE(stats->clicks_by_day.size() == 1);
    CHECK(stats->clicks_by_day[0].count == 2);
}

TEST_CASE("clicks on an unknown code are rejected, not silently counted") {
    SqliteLinkStore store(":memory:");

    ClickEvent event;
    event.code       = "ghost00";
    event.clicked_at = static_cast<std::int64_t>(::time(nullptr));

    CHECK_FALSE(store.record_click(event));
}

TEST_CASE("direct traffic is excluded from the referrer breakdown") {
    SqliteLinkStore store(":memory:");
    REQUIRE(store.insert(make_link("ref1234", "https://example.com/")).has_value());

    const auto now = static_cast<std::int64_t>(::time(nullptr));
    for (int i = 0; i < 3; ++i) {
        store.record_click({"ref1234", now, "", "ua"});  // no Referer header
    }
    store.record_click({"ref1234", now, "t.example.com", "ua"});

    const auto stats = store.stats_for("ref1234", 7);
    REQUIRE(stats.has_value());
    CHECK(stats->link.click_count == 4);          // all four counted
    REQUIRE(stats->top_referrers.size() == 1);    // but only one has a source
    CHECK(stats->top_referrers[0].host == "t.example.com");
}

TEST_CASE("the daily window excludes older clicks") {
    SqliteLinkStore store(":memory:");
    REQUIRE(store.insert(make_link("win1234", "https://example.com/")).has_value());

    const auto now = static_cast<std::int64_t>(::time(nullptr));
    store.record_click({"win1234", now, "", "ua"});
    store.record_click({"win1234", now - 30 * 86400, "", "ua"});  // outside 7 days

    const auto stats = store.stats_for("win1234", 7);
    REQUIRE(stats.has_value());
    CHECK(stats->link.click_count == 2);       // lifetime total keeps both
    CHECK(stats->clicks_by_day.size() == 1);   // the window shows one day

    const auto wide = store.stats_for("win1234", 60);
    REQUIRE(wide.has_value());
    CHECK(wide->clicks_by_day.size() == 2);
}

TEST_CASE("long user agents are truncated instead of rejected") {
    SqliteLinkStore store(":memory:");
    REQUIRE(store.insert(make_link("big1234", "https://example.com/")).has_value());

    ClickEvent event;
    event.code       = "big1234";
    event.clicked_at = static_cast<std::int64_t>(::time(nullptr));
    event.user_agent = std::string(50000, 'x');

    CHECK(store.record_click(event));
    const auto link = store.find_by_code("big1234");
    REQUIRE(link.has_value());
    CHECK(link->click_count == 1);
}

TEST_CASE("values are bound, never concatenated, so SQL injection is a non-event") {
    SqliteLinkStore store(":memory:");

    const std::string hostile = "https://example.com/'; DROP TABLE links; --";
    REQUIRE(store.insert(make_link("inj1234", hostile)).has_value());

    const auto found = store.find_by_code("inj1234");
    REQUIRE(found.has_value());
    CHECK(found->original_url == hostile);
    CHECK(store.count_links() == 1);

    // A hostile *code* must also be inert.
    CHECK_FALSE(store.find_by_code("' OR '1'='1").has_value());
}

TEST_CASE("concurrent inserts of the same code produce exactly one winner") {
    // The real test of the uniqueness argument: hammer one code from several
    // threads and require that the database, not our timing, decides.
    SqliteLinkStore store(":memory:");

    constexpr int         kThreads = 8;
    std::atomic<int>      winners{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&store, &winners, i] {
            if (store.insert(make_link("race123", "https://example.com/" + std::to_string(i)))) {
                winners.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK(winners.load() == 1);
    CHECK(store.count_links() == 1);
}

TEST_CASE("concurrent clicks are all accounted for") {
    SqliteLinkStore store(":memory:");
    REQUIRE(store.insert(make_link("hot1234", "https://example.com/")).has_value());

    constexpr int            kThreads = 8;
    constexpr int            kPerThread = 25;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&store] {
            const auto now = static_cast<std::int64_t>(::time(nullptr));
            for (int j = 0; j < kPerThread; ++j) {
                store.record_click({"hot1234", now, "a.example.com", "ua"});
            }
        });
    }
    for (auto& thread : threads) thread.join();

    const auto link = store.find_by_code("hot1234");
    REQUIRE(link.has_value());
    // No lost updates: the counter and the event rows agree because they are
    // written in one transaction.
    CHECK(link->click_count == kThreads * kPerThread);

    const auto stats = store.stats_for("hot1234", 7);
    REQUIRE(stats.has_value());
    REQUIRE(stats->top_referrers.size() == 1);
    CHECK(stats->top_referrers[0].count == kThreads * kPerThread);
}
