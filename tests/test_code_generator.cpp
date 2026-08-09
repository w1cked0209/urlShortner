#include <doctest/doctest.h>

#include <array>
#include <set>
#include <string>

#include "urlshort/code_generator.hpp"
#include "urlshort/errors.hpp"

using namespace urlshort;

TEST_CASE("generated codes have the requested length and charset") {
    const CodeGenerator generator(7);
    for (int i = 0; i < 500; ++i) {
        const std::string code = generator.generate();
        REQUIRE(code.size() == 7);
        CHECK(CodeGenerator::is_valid_code(code));
        // URL-safe means no percent-encoding is ever needed.
        CHECK(code.find_first_not_of(CodeGenerator::kAlphabet) == std::string::npos);
    }
}

TEST_CASE("codes do not repeat across many draws") {
    // 20k draws from a 3.5e12 keyspace: the birthday bound puts the chance of
    // any collision here at roughly 6e-5, so a failure means the generator is
    // broken (fixed seed, biased sampling, shared state), not unlucky.
    const CodeGenerator   generator(7);
    std::set<std::string> seen;
    for (int i = 0; i < 20000; ++i) {
        seen.insert(generator.generate());
    }
    CHECK(seen.size() == 20000);
}

TEST_CASE("short codes still do not repeat within a small sample") {
    // Guards against the generator ignoring its length parameter.
    const CodeGenerator   generator(4);
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string code = generator.generate();
        CHECK(code.size() == 4);
        seen.insert(code);
    }
    CHECK(seen.size() > 190);  // 62^4 = 14.7M, so near-zero expected collisions
}

TEST_CASE("every alphabet symbol is reachable") {
    // A modulo-bias bug or an off-by-one in the alphabet slice typically shows
    // up as one or two characters that never appear.
    const CodeGenerator generator(8);
    std::set<char>      seen;
    for (int i = 0; i < 5000; ++i) {
        for (char c : generator.generate()) seen.insert(c);
    }
    CHECK(seen.size() == CodeGenerator::kAlphabet.size());
}

TEST_CASE("the distribution is not visibly skewed") {
    // A weak sanity check on uniformity: with 62 symbols and 62k samples the
    // expected count per symbol is 1000, and every symbol should land well
    // inside a generous band. This catches gross bias without being flaky.
    const CodeGenerator     generator(10);
    std::array<int, 128>    counts{};
    constexpr int           kDraws = 6200;
    for (int i = 0; i < kDraws; ++i) {
        for (char c : generator.generate()) {
            counts[static_cast<unsigned char>(c)]++;
        }
    }
    const int expected = (kDraws * 10) / static_cast<int>(CodeGenerator::kAlphabet.size());
    for (char c : CodeGenerator::kAlphabet) {
        const int count = counts[static_cast<unsigned char>(c)];
        CAPTURE(c);
        CAPTURE(count);
        CHECK(count > expected / 2);
        CHECK(count < expected * 2);
    }
}

TEST_CASE("generate_unique retries past taken codes") {
    const CodeGenerator generator(7);

    int         calls = 0;
    std::string result = generator.generate_unique([&](const std::string&) {
        // Report the first three candidates as taken, then accept.
        return ++calls <= 3;
    });

    CHECK(calls == 4);
    CHECK(result.size() == 7);
}

TEST_CASE("generate_unique gives up rather than returning a duplicate") {
    // The failure mode we must never have is "returned a code we know is
    // taken". Exhausting the attempt budget is the correct, loud alternative.
    const CodeGenerator generator(7, /*max_attempts=*/5);
    CHECK_THROWS_AS(generator.generate_unique([](const std::string&) { return true; }),
                    ServiceError);

    try {
        generator.generate_unique([](const std::string&) { return true; });
    } catch (const ServiceError& error) {
        CHECK(error.code() == ErrorCode::CodeExhausted);
    }
}

TEST_CASE("rejects nonsensical construction") {
    CHECK_THROWS(CodeGenerator(2));
    CHECK_THROWS(CodeGenerator(7, 0));
}

TEST_CASE("alias validation is looser than code validation but still strict") {
    CHECK(CodeGenerator::is_valid_alias("my-link"));
    CHECK(CodeGenerator::is_valid_alias("Q3_launch"));
    CHECK(CodeGenerator::is_valid_alias("abc"));

    CHECK_FALSE(CodeGenerator::is_valid_alias("ab"));                  // too short
    CHECK_FALSE(CodeGenerator::is_valid_alias(std::string(33, 'a')));  // too long
    CHECK_FALSE(CodeGenerator::is_valid_alias("has space"));
    CHECK_FALSE(CodeGenerator::is_valid_alias("slash/es"));
    CHECK_FALSE(CodeGenerator::is_valid_alias("percent%20"));
    CHECK_FALSE(CodeGenerator::is_valid_alias("dot.ted"));
    CHECK_FALSE(CodeGenerator::is_valid_alias("-leading"));
    CHECK_FALSE(CodeGenerator::is_valid_alias("trailing_"));
    CHECK_FALSE(CodeGenerator::is_valid_alias(""));

    // '-' and '_' are fine in an alias but are not in the generated alphabet.
    CHECK_FALSE(CodeGenerator::is_valid_code("my-link"));
}
