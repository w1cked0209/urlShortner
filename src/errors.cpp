#include "urlshort/errors.hpp"

namespace urlshort {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidUrl:        return "invalid_url";
        case ErrorCode::UnsupportedScheme: return "unsupported_scheme";
        case ErrorCode::UrlTooLong:        return "url_too_long";
        case ErrorCode::BlockedHost:       return "blocked_host";
        case ErrorCode::InvalidAlias:      return "invalid_alias";
        case ErrorCode::ReservedAlias:     return "reserved_alias";
        case ErrorCode::AliasTaken:        return "alias_taken";
        case ErrorCode::NotFound:          return "not_found";
        case ErrorCode::CodeExhausted:     return "code_exhausted";
        case ErrorCode::MalformedRequest:  return "malformed_request";
        case ErrorCode::Internal:          return "internal_error";
    }
    return "internal_error";
}

int status_for(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidUrl:
        case ErrorCode::UnsupportedScheme:
        case ErrorCode::UrlTooLong:
        case ErrorCode::BlockedHost:
        case ErrorCode::InvalidAlias:
        case ErrorCode::ReservedAlias:
        case ErrorCode::MalformedRequest:
            return 400;

        case ErrorCode::NotFound:
            return 404;

        // The alias is a namespace conflict, not bad input - 409 tells the
        // caller "try a different name", which is actionable.
        case ErrorCode::AliasTaken:
            return 409;

        // We could not mint a code. That is our problem, not the caller's, and
        // it is retryable - 503 with the expectation that it never fires.
        case ErrorCode::CodeExhausted:
            return 503;

        case ErrorCode::Internal:
            return 500;
    }
    return 500;
}

}  // namespace urlshort
