# Guest virtual reservations and commit

The actual title-entry diagnostic reached `NtAllocateVirtualMemory` with
`0x60002000` at return address `0x829A61F4`. The pinned public reference names
these bits `MEM_HEAP | MEM_LARGE_PAGES | MEM_RESERVE`, not MEM_COMMIT.
References: rexglue-sdk `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`,
`include/rex/system/xtypes.h` and `src/kernel/xboxkrnl/xboxkrnl_memory.cpp`;
the corresponding public Xenia definitions are in `src/xenia/xbox.h`.

R-comp now separates ownership from backing. A reserve-only request removes a
whole-page range from the guest allocator and ensures it has no committed
backing or read/write access. Query reports MEM_RESERVE with the original
allocation base/protection. A later commit must fit the same registered virtual
allocation. Only newly backed pages are initialized; recommitting a previously
committed page preserves its bytes. Decommit drops backing but retains address
ownership. Releasing a reserve-created VM allocation drops all remaining backing
and then retires the allocation/protection metadata.

The guest heap flag selects ordinary title-heap virtual ownership; it does not
select physical/GPU memory. The backing implementation uses 64 KiB pages as the
rest of GuestMemory. Executable guest protection, reset, 16 MiB page requests,
debug-memory selection and unsupported flag combinations remain explicit failures.
No host executable mapping, host virtual address or arbitrary memory is exposed.
Small fixed commit requests are rounded to this backend's 64 KiB granularity;
independent sub-page protection is not claimed. Address arithmetic uses 64 bits.

Metadata is allocated before publication. Platform mapping failures return an
error while allocation ownership is retained; partial platform rollback is not
claimed. The caller cannot receive SUCCESS unless the requested backing and
protection operation succeeded. The tests query reservation/commit transitions,
recommit preservation, decommit zeroing, read-only pages, bounds and full release.

## Address ranges by page size (3 Oct 2026)

The runtime virtual heap covers `0x00010000..0x7F000000` (the first 64 KiB stay
unmapped). As on the Xbox 360 kernel, `NtAllocateVirtualMemory` without a base
address places:

- 4 KiB-page allocations (no `MEM_LARGE_PAGES`) in `0x00010000..0x3FFFFFFF`;
- `MEM_LARGE_PAGES` (64 KiB) allocations in `0x40000000..0x7F000000`.

`MEM_TOP_DOWN` applies inside that window. A request with a base address is
placed at that base anywhere in the heap. Runtime-owned blocks
(`GuestHeap::alloc`: stacks, HLE buffers) stay at or above `0x40000000`
(`GuestHeap::runtime_lo()`), so they never take the title's 4 KiB range.

Why: Unreal Engine 3's allocator (Gears of War 2, `sub_827F80C8`) frees a
pointer below `0x40000000` as a direct `VirtualAlloc` block, and looks up
anything above it in its own 64 KiB page table. A 4 KiB-page block placed at
`0x405B0000` made it follow a null link: PS5 crash `addr=guest_base+0`,
before this change. Host test: `tests/test_hle.cpp`.

Commit granularity stays 64 KiB (`kGuestPageSize`): a 4 KiB-page request
is rounded up to 64 KiB.
