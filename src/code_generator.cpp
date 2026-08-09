#include "urlshort/code_generator.hpp"

#include <cctype>
#include <limits>
#include <random>
#include <stdexcept>

#include "urlshort/errors.hpp"

namespace urlshort {
namespace {

/// Uniform index into a 62-symbol alphabet.
///
/// The obvious `rng() % 62` is biased: 2^32 is not a multiple of 62, so the
/// first 2^32 mod 62 symbols come up slightly more often. Irrelevant for a toy,
/// but bias in an identifier space is exactly the kind of thing that turns into
/// a real problem at scale, and rejection sampling costs four lines.
unsigned uniform_index(std::random_device& rd, unsigned bound) {
    static_assert(std::random_device::min() == 0, "expected a full-range random_device");

    const unsigned limit = std::numeric_limits<unsigned>::max() -
                           (std::numeric_limits<unsigned>::max() % bound) - 1;
    unsigned draw = 0;
    do {
        draw = static_cast<unsigned>(rd());
    } while (draw > limit);
    return draw % bound;
}

}  // namespace

CodeGenerator::CodeGenerator(std::size_t length, int max_attempts)
    : length_(length), max_attempts_(max_attempts) {
    if (length_ < 4) {
        throw std::invalid_argument("code length must be at least 4");
    }
    if (max_attempts_ < 1) {
        throw std::invalid_argument("max_attempts must be at least 1");
    }
}

std::string CodeGenerator::generate() const {
    // Thread-local so concurrent requests never share entropy state. On Linux
    // this is backed by getrandom(2)/RDRAND, so codes are unpredictable as well
    // as uniform - a sequential or PRNG-derived scheme would let anyone
    // enumerate every link in the system.
    thread_local std::random_device rd;

    std::string code;
    code.reserve(length_);
    for (std::size_t i = 0; i < length_; ++i) {
        code.push_back(kAlphabet[uniform_index(rd, static_cast<unsigned>(kAlphabet.size()))]);
    }
    return code;
}

std::string CodeGenerator::generate_unique(
    const std::function<bool(const std::string&)>& is_taken) const {
    for (int attempt = 0; attempt < max_attempts_; ++attempt) {
        std::string code = generate();
        if (!is_taken(code)) {
            return code;
        }
    }
    // Reaching here means either the keyspace is genuinely saturated or
    // something is badly wrong upstream. Either way it is not the caller's
    // fault, and silently returning a duplicate would be far worse.
    throw ServiceError(ErrorCode::CodeExhausted,
                       "could not generate an unused short code; consider increasing "
                       "URLSHORT_CODE_LENGTH");
}

bool CodeGenerator::is_valid_code(std::string_view code) {
    if (code.empty() || code.size() > 32) return false;
    for (char c : code) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool CodeGenerator::is_valid_alias(std::string_view alias) {
    if (alias.size() < 3 || alias.size() > 32) return false;
    for (char c : alias) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        if (!ok) return false;
    }
    // Leading/trailing punctuation reads as a typo and makes the link look
    // broken when it is auto-linkified in chat clients.
    if (alias.front() == '-' || alias.front() == '_' ||
        alias.back()  == '-' || alias.back()  == '_') {
        return false;
    }
    return true;
}

}  // namespace urlshort
