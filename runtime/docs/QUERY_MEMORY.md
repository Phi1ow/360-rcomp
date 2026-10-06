# Virtual-memory query services

This tranche now owns the complete queried-memory path used by GTA IV:

- `MmQueryAddressProtect` (`xboxkrnl.exe` ordinal `0x00C4`)
- `MmSetAddressProtect` (`0x00C7`)
- `NtAllocateVirtualMemory` (`0x00CC`)
- `NtFreeVirtualMemory` (`0x00DC`)
- `NtProtectVirtualMemory` (`0x00E1`)
- `NtQueryVirtualMemory` (`xboxkrnl.exe` ordinal `0x00EE`)

`GuestMemory` remains the platform-owned host-page backing and access map. It
does not store Xbox PAGE_* modifiers. `GuestHeap` now owns guest-allocation
provenance: initial `AllocationProtect`, current protection runs, live
allocation boundaries, and free-list boundaries. Existing Runtime virtual and
physical ownership sets select which GuestHeap allocations are title VM rather
than stacks, pools, XAM storage, or other runtime allocations.

## Observed GTA IV contract

The current R-comp static analysis records two early `NtQueryVirtualMemory`
calls at `0x829A6130` and `0x829A6188`. Both pass a dynamic address in `r3`, a
stack result buffer at caller offset 96 in `r4`, and region type `0` in `r5`.
The caller reads only:

- `+0x00` — `BaseAddress`
- `+0x0C` — `RegionSize`
- `+0x10` — `State`

It compares `State` against `MEM_FREE (0x10000)`, `MEM_COMMIT (0x1000)`, and
`MEM_RESERVE (0x2000)`. This is static call-site evidence; GTA IV execution is
not claimed by this tranche.

The pinned rexglue-sdk `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`
Xbox ABI reference declares the 32-bit result as seven consecutive 32-bit
fields: `BaseAddress`, `AllocationBase`, `AllocationProtect`, `RegionSize`,
`State`, `Protect`, `Type`. R-comp therefore writes exactly **28 bytes**, even
when the caller has a 32-byte stack slot. The test keeps a canary at byte 28.

## NT semantics used

Microsoft documents `NtQueryVirtualMemory` / `MEMORY_BASIC_INFORMATION` as a
page-region query. `BaseAddress` is the base of a region; `RegionSize` covers
consecutive pages with matching attributes. `AllocationBase` identifies the
allocation containing that region. The standard state values are:

- `MEM_COMMIT = 0x1000`
- `MEM_RESERVE = 0x2000`
- `MEM_FREE = 0x10000`

The host access values represented by `GuestMemory` remain:

- `PAGE_NOACCESS = 0x01`
- `PAGE_READONLY = 0x02`
- `PAGE_READWRITE = 0x04`

There is deliberately no synthetic executable/RWX protection. Guest PPC code
is AOT-linked host code; the loaded XEX bytes are guest data and the current
loader commits them read/write.

Guest PAGE_* provenance is separate from those host access rights. The owned
memory allocators preserve the exact accepted `PAGE_NOCACHE (0x200)` and
`PAGE_WRITECOMBINE (0x400)` bits, both for the initial allocation protection
and for later `NtProtectVirtualMemory` / `MmSetAddressProtect` runs. These
modifiers have no host `mprotect` effect but are returned exactly by query APIs.

For a free region, Microsoft declares `AllocationBase`, `AllocationProtect`,
`Protect`, and `Type` undefined. R-comp emits zero in those four fields so no
host address or stale runtime metadata can leak into the guest. GTA's observed
path does not consume them for a `MEM_FREE` result.

## What is actually queryable now

`query_memory_basic()` combines four independent truths:

1. live allocation/free-block boundaries from `GuestHeap`, including
   `allocation_containing()` and `region_containing()`;
2. exact guest PAGE_* allocation/current protection from GuestHeap provenance,
   with `Runtime::virtual_allocations` / `physical_allocations` as family
   ownership authority;
3. the finalized main XEX image, reconstructed from `ModuleState`'s protected
   read-only copy of the already validated XEX header (`loadAddress` / loaded
   image size), never from the guest-writable loader record;
4. GuestMemory commit/base-access state as a consistency check and for image /
   external regions.

The virtual and physical allocation-family mutexes are held together from the
snapshot through region classification, so a concurrent Nt/Mm allocate/free
cannot invalidate the allocation base while it is being reported. As with the
rest of `GuestMemory`, callers still must serialize future direct mapping or
protection mutations that bypass these runtime allocation families.

For Nt/Mm allocations, `AllocationProtect` is the exact accepted initial Xbox
value and `Protect` is the exact current protection run, including NOCACHE or
WRITECOMBINE. `GuestMemory` is checked to ensure the current base access
(NOACCESS/READONLY/READWRITE) agrees with that metadata. The main image is
reported as `MEM_IMAGE`; Nt/Mm allocations are `MEM_PRIVATE`.

Within a tracked allocation the query uses the exact GuestHeap protection-run
boundary, further split when GuestMemory commit state changes. A live tracked
allocation whose backing page is decommitted is therefore `MEM_RESERVE` with
`Protect=0`; the process-wide host reservation made by `GuestMemory::reserve()`
is never evidence of guest `MEM_RESERVE`.

Inside a GuestHeap, a released block is `MEM_FREE` from allocator state even if
its host page remains committed for reuse. Only whole 64-KiB free pages are
advertised as VM-free when a free fragment shares a page with a live runtime
allocation. Outside the heaps, free scanning still requires an uncommitted page
and stops at the image/runtime ranges. Arithmetic stays 64-bit through the
end-of-4-GiB calculation before the 32-bit fields are written.

## Refused provenance instead of fabricated answers

`GuestHeap::free()` intentionally retains committed host pages, but the free
list is authoritative for released title VM, so stale backing is now reported
coherently as `MEM_FREE`. A *live* heap allocation that is not in the virtual or
physical family set may be a stack/TLS/PCR, pool, XAM allocation, video
variable, etc.; that address remains `Status::Unsupported` instead of being
invented as title `MEM_COMMIT`.

The same policy applies to opaque runtime ranges. They reserve address-space
identity for R-comp internals, not Xbox title virtual memory, and are never
reported as guest `MEM_RESERVE`.

Physical allocations retain their GuestHeap boundary even for the Xbox 4-KiB
allocation class. A 4-KiB physical allocation can therefore report a 4-KiB MBI
region while GuestMemory still backs/protects its containing 64-KiB host page.
Base-access changes that cannot be represented at that host granularity remain
explicitly unsupported rather than mutating neighboring allocations.

## `MmQueryAddressProtect`

This export has no Windows NT equivalent with the Xbox ABI, so the pinned
rexglue/Xenia-derived public code is used only to cross-check its one-address,
one-return-value shape. For tracked Nt/Mm allocations the production result now
comes directly from GuestHeap's exact current Xbox PAGE_* run. This includes
the `0x200/0x400` modifier bits GTA IV tests with a `0x600` mask. Free/reserved
memory returns zero. Main-image protection is derived from its fixed loader
contract plus current GuestMemory access.

For a raw committed page with no guest allocation provenance,
`query_address_protect()` can still expose the host base access for diagnostics
but marks the complete Xbox result unknown; production `MmQueryAddressProtect`
does not treat that partial observation as a success.

`NtProtectVirtualMemory` and `MmSetAddressProtect` update both host base access
and the guest protection-run metadata transactionally. `GuestHeap::free()`
restores RO/NOACCESS host pages to RW before recycling the block and erases the
allocation/current protection metadata, preventing protection leakage into a
subsequent allocation.

## Validation

`runtime/tests/test_query_memory.cpp` calls the real import thunks and covers:

- exact 28-byte output layout and a byte-28 canary;
- GTA-consumed offsets 0, 12, and 16;
- `MEM_COMMIT`, synthetic decommit-with-live-provenance `MEM_RESERVE`, and real
  allocator-backed/external `MEM_FREE`;
- initial `PAGE_WRITECOMBINE` / `PAGE_NOCACHE` preservation;
- `NtProtectVirtualMemory` page rounding, old-protection output and exact
  mixed protection runs;
- `MmSetAddressProtect` and exact production `MmQueryAddressProtect`;
- read/write, read-only, and no-access splits inside one allocation;
- `AllocationBase`, `AllocationProtect`, `MEM_PRIVATE`, and `MEM_IMAGE`;
- 64-KiB and 4-KiB physical allocation provenance;
- release-time host-protection restoration followed by exact address reuse;
- `MEM_TOP_DOWN` placement;
- free-region boundaries and an address at `0xFFFFFFFE` without 32-bit
  arithmetic overflow;
- invalid/null/overflowing output pointers and unsupported region type;
- live untracked runtime heap allocations returning `Unsupported`;
- raw committed test-harness memory refused as title MBI because allocation
  provenance is absent.

The production test is validated in three independent host configurations:
normal, `-fsanitize=undefined -fsanitize-trap=undefined`, and
`-fno-exceptions`. All execute the same allocation/protection/free/query path.

Primary references and local evidence hashes are recorded in
`QUERY_MEMORY_REFERENCES.json`.
