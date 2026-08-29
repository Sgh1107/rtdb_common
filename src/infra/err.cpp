#include "rtdb/err.hpp"

namespace rtdb {

const char* ErrorMessage(Err e) noexcept {
    switch (e) {
        case Err::kOk:
            return "ok";
        // ---- 1xx ----
        case Err::kInvalidArgument:
            return "invalid argument";
        case Err::kNotFound:
            return "not found";
        case Err::kAlreadyExists:
            return "already exists";
        case Err::kIoError:
            return "io error";
        case Err::kUnsupported:
            return "operation not supported";
        case Err::kOutOfMemory:
            return "out of memory";
        case Err::kTimeout:
            return "timeout";
        // ---- 2xx ----
        case Err::kBadSuperBlock:
            return "bad super block (magic mismatch or region too small)";
        case Err::kVersionMismatch:
            return "shm format version mismatch";
        case Err::kIncompatibleLayout:
            return "incompatible shm layout (pointer width / layout crc)";
        case Err::kReadOnlyHandle:
            return "handle is read-only";
        case Err::kCorruption:
            return "persistent structure corrupted";
        // ---- 3xx ----
        case Err::kPoisoned:
            return "engine poisoned (unrecoverable wal state, writes rejected)";
        case Err::kTableExists:
            return "table already exists";
        case Err::kTableNotFound:
            return "table not found";
        case Err::kSchemaMismatch:
            return "schema layout version mismatch";
        case Err::kBatchAborted:
            return "atomic batch aborted (no commit record)";
        case Err::kLimitExceeded:
            return "configured limit exceeded (tables / columns / indexes)";
        // ---- 4xx ----
        case Err::kNotImplemented:
            return "not implemented yet";
        case Err::kNetworkError:
            return "network error";
        default:
            break;
    }
    return IsOk(e) ? "ok" : "unknown error";
}

}  // namespace rtdb
