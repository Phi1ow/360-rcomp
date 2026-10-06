// Manifest-backed directory index retained for future NtQueryDirectoryFile.
#include "rcomp/runtime/io_next.h"
#include "test_util.h"

using namespace rcomp::rt;

int main() {
    IoNextDirectoryIndex index;
    CHECK_ST(index.add_path("game:\\Data\\Foo.BIN"), Status::Ok);
    CHECK_ST(index.add_path("game:\\Data\\bar.txt"), Status::Ok);
    CHECK_ST(index.add_path("game:\\Data\\Alpha.bin"), Status::Ok);
    CHECK_ST(index.add_path("game:\\Data\\foo.bin"), Status::AlreadyExists);
    CHECK_ST(index.finalize(), Status::Ok);
    CHECK_EQ(index.size(), 4u);  // Data + three children.

    IoNextDirectoryCursor cursor;
    IoNextDirectoryEntry entry;
    CHECK(index.query("GAME:", "*", false, &cursor, &entry) ==
          IoNextDirectoryResult::Found);
    CHECK(entry.name == "Data");
    CHECK_EQ(entry.file_index, 0u);

    cursor = {};
    CHECK(index.query("GAME:\\data", "*.BIN", false, &cursor, &entry) ==
          IoNextDirectoryResult::Found);
    CHECK(entry.name == "Alpha.bin");
    CHECK_EQ(entry.file_index, 0u);
    CHECK(index.query("game:\\DATA", "", false, &cursor, &entry) ==
          IoNextDirectoryResult::Found);
    CHECK(entry.name == "Foo.BIN");
    CHECK_EQ(entry.file_index, 2u);
    CHECK(index.query("game:\\data", "", false, &cursor, &entry) ==
          IoNextDirectoryResult::NoMoreFiles);
    CHECK(index.query("game:\\data", "", true, &cursor, &entry) ==
          IoNextDirectoryResult::Found);
    CHECK(entry.name == "Alpha.bin");

    IoNextDirectoryCursor miss;
    CHECK(index.query("game:\\data", "*.rpf", false, &miss, &entry) ==
          IoNextDirectoryResult::NoSuchFile);
    return test_result("rt_test_io_next");
}
