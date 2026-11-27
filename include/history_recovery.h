#pragma once
#include <memory>
#include <new>
#include "ride_session.h"

namespace session {
constexpr size_t RECOVERY_MAX_BYTES = 8192;
enum class BackupResult { FAILED, READABLE, SAVED };

// Preserve the raw bytes, even if the damaged blob has an unexpected size.
template<class Store, class Validate>
BackupResult backupUnreadableHistory(Store& store, const char* source, const char* backup, Validate validate) {
    const size_t length = store.getBytesLength(source);
    if (!length || length > RECOVERY_MAX_BYTES) return BackupResult::FAILED;
    std::unique_ptr<uint8_t[]> original(new (std::nothrow) uint8_t[length]);
    std::unique_ptr<uint8_t[]> readback(new (std::nothrow) uint8_t[length]);
    if (!original || !readback || store.getBytes(source, original.get(), length) != length) return BackupResult::FAILED;
    if (validate(original.get(), length)) return BackupResult::READABLE;

    if (store.getBytesLength(backup) == length &&
        store.getBytes(backup, readback.get(), length) == length &&
        memcmp(original.get(), readback.get(), length) == 0) return BackupResult::SAVED;

    const bool verified = store.putBytes(backup, original.get(), length) == length &&
        store.getBytes(backup, readback.get(), length) == length &&
        memcmp(original.get(), readback.get(), length) == 0;
    return verified ? BackupResult::SAVED : BackupResult::FAILED;
}
}
