#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

using namespace rcomp;
using namespace rcomp::rt;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* file = std::fopen(argv[1], "r");
  if (!file) return 2;

  GuestMemory memory;
  if (memory.reserve() != MemStatus::Ok || runtime_init(&memory) != Status::Ok ||
      register_xboxkrnl_hle() != Status::Ok ||
      memory.commit(0x30000000, 0x10000, Protect::ReadWrite) != MemStatus::Ok) {
    std::fclose(file);
    return 2;
  }

  const auto forward = find_import(kModuleXboxkrnl, 0x013F);
  const auto inverse = find_import(kModuleXboxkrnl, 0x0140);
  if (!forward || !inverse) {
    std::fclose(file);
    runtime_shutdown();
    clear_imports();
    return 2;
  }

  unsigned checks = 0;
  unsigned failures = 0;
  char mode = 0;
  while (std::fscanf(file, " %c", &mode) == 1) {
    alignas(64) PPCContext context{};
    context.r3.u64 = 0x30000000;
    context.r4.u64 = 0x30000100;
    auto* input = memory.base() + context.r3.u32;
    auto* output = memory.base() + context.r4.u32;
    int fields[8]{};
    uint64_t ticks = 0;
    bool valid = true;

    if (mode == 'F') {
      for (int& value : fields) {
        if (std::fscanf(file, "%d", &value) != 1) return 2;
      }
      int expected_return = 0;
      if (std::fscanf(file, "%d %" SCNu64, &expected_return, &ticks) != 2) return 2;
      for (unsigned i = 0; i < 8; ++i) {
        input[2 * i] = uint16_t(fields[i]) >> 8;
        input[2 * i + 1] = uint16_t(fields[i]);
      }
      constexpr uint64_t sentinel = 0x0123456789ABCDEF;
      for (unsigned i = 0; i < 8; ++i) output[i] = sentinel >> (56 - 8 * i);
      forward(context, memory.base());
      valid = context.r3.u64 == uint64_t(expected_return);
      for (unsigned i = 0; i < 8; ++i) valid &= output[i] == uint8_t(ticks >> (56 - 8 * i));
    } else if (mode == 'I') {
      if (std::fscanf(file, "%" SCNu64, &ticks) != 1) return 2;
      for (int& value : fields) {
        if (std::fscanf(file, "%d", &value) != 1) return 2;
      }
      for (unsigned i = 0; i < 8; ++i) input[i] = ticks >> (56 - 8 * i);
      std::memset(output, 0xCC, 16);
      inverse(context, memory.base());
      for (unsigned i = 0; i < 8; ++i) {
        const uint16_t actual = uint16_t((uint16_t(output[2 * i]) << 8) | output[2 * i + 1]);
        valid &= actual == uint16_t(fields[i]);
      }
    } else {
      return 2;
    }

    ++checks;
    if (!valid) {
      ++failures;
      std::printf("FAIL vector=%u mode=%c\n", checks, mode);
    }
  }

  const bool read_ok = std::ferror(file) == 0;
  std::fclose(file);
  runtime_shutdown();
  clear_imports();
  if (!read_ok) return 2;
  std::printf("NTDLL-DIFFERENTIAL checks=%u pass=%u fail=%u scope=host\n",
              checks, checks - failures, failures);
  return checks == 4005 && failures == 0 ? 0 : 1;
}
