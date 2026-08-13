#include "storage/storage.hpp"

namespace storage {

const char *error_code(Error error)
{
    switch (error) {
    case Error::Ok: return "OK";
    case Error::NotFound: return "NOT_FOUND";
    case Error::AlreadyExists: return "ALREADY_EXISTS";
    case Error::InvalidPath: return "INVALID_PATH";
    case Error::NotAFile: return "NOT_A_FILE";
    case Error::NotADirectory: return "NOT_A_DIRECTORY";
    case Error::ParentNotFound: return "PARENT_NOT_FOUND";
    case Error::NoSpace: return "NO_SPACE";
    case Error::ReadFailed: return "READ_FAILED";
    case Error::WriteFailed: return "WRITE_FAILED";
    case Error::CommitFailed: return "COMMIT_FAILED";
    case Error::Busy: return "BUSY";
    case Error::OutOfRange: return "OUT_OF_RANGE";
    case Error::Unsupported: return "UNSUPPORTED";
    }
    return "INTERNAL_ERROR";
}

} // namespace storage
