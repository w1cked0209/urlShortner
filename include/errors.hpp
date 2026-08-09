#pragma once

#include <stdexcept>
#include <string>

namespace urlshort {

/// Every way a request can fail that is *not* a bug in our code.
///
/// The enum is the single source of truth: the HTTP layer maps it to a status
/// code, the JSON body echoes the name back to the client, and tests assert on
/// it rather than on strings. Adding a failure mode therefore forces you to
/// decide its HTTP status in exactly one place (`status_for`).
enum class ErrorCode {
    InvalidUrl,          ///< Not parseable as an absolute URL.
    UnsupportedScheme,   ///< Parsed, but not http/https.
    UrlTooLong,          ///< Exceeds the configured length ceiling.
    BlockedHost,         ///< Loopback / private / link-local target.
    InvalidAlias,        ///< Custom alias fails the charset or length rules.
    ReservedAlias,       ///< Custom alias would shadow one of our own routes.
    AliasTaken,          ///< Custom alias already maps to a different URL.
    NotFound,            ///< No link for this code.
    CodeExhausted,       ///< Generator could not find a free code (see README).
    MalformedRequest,    ///< Body was not JSON, or a field had the wrong type.
    Internal             ///< Anything we failed to anticipate.
};

const char* to_string(ErrorCode code) noexcept;

/// HTTP status for an error code. Kept next to the enum so the mapping cannot
/// drift between call sites.
int status_for(ErrorCode code) noexcept;

/// Thrown by the service layer; caught and serialised by the HTTP layer.
class ServiceError : public std::runtime_error {
public:
    ServiceError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace urlshort
