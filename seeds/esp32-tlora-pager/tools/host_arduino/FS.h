#pragma once
/*
 * Host mock of the Arduino FS layer — compile-only surface for
 * src/secret_store.cpp in tools/test_secret_store.sh. The migration ops that
 * touch fs::FS are NOT exercised through this mock (the migration driver is
 * host-tested through injected SecretMigrateOps instead); every file here
 * reads as absent so an accidental call is a visible no-op, not a fake pass.
 */

#include <cstddef>
#include <cstdint>

namespace fs {

class File {
public:
    explicit operator bool() const { return false; }
    size_t size() const { return 0; }
    size_t read(uint8_t *, size_t) { return 0; }
    void close() {}
};

class FS {
public:
    bool exists(const char *) { return false; }
    File open(const char *, const char *) { return File(); }
};

}  // namespace fs

/* The real Arduino FS.h exports File into the global namespace; the device
 * code (secret_store.cpp) relies on that spelling. */
using File = fs::File;
