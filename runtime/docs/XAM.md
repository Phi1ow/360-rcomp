# XAM runtime services

R-comp registers XAM functions only when it has a concrete runtime contract for
their state or side effects. Every identity, device and completion below is
backed by real state: one local offline profile, a hard drive backed by a host
directory, and handles or guest threads that really exist. Nothing is returned
merely to satisfy an import.

## Memory

`XamAlloc` (`0x01EA`) allocates from the active title's ordinary guest heap and
writes the resulting guest pointer to the caller's output cell. The allocation
is non-executable and is tracked separately as XAM-owned state in the active
`Runtime`. `XamFree` (`0x01EC`) only releases an address returned by `XamAlloc`
during that same runtime lifetime; foreign, stale, null, or repeated frees are
guest-access failures instead of freeing some other subsystem's allocation.

The preserved GTA IV AOT call sites in the local build evidence use allocation
flags `0` and `0x18000000`; the public export table establishes the function and
ordinal but does not define those flag bits. R-comp therefore accepts only
those two observed values and gives `0x18000000` the conservative guarantee of
ordinary writable, non-executable guest storage. This is deliberately not a
claim that the flag is semantically equivalent to zero: no extra alignment,
zeroing, protection, or heap-selection behavior is attributed to it until that
contract has independent evidence. Any other flag traps as unimplemented. A
zero size returns `E_INVALIDARG`; guest-heap exhaustion returns `E_OUTOFMEMORY`.
Output memory is checked writable before allocation.

## XEX execution identity

`XamGetExecutionId` (`0x0280`) exposes the exact 24-byte XEX optional-header
payload with key `0x00040006` retained by main-module finalization. This is the
same layout already parsed by `tools/m6_inventory.py`: media ID, version, base
version, title ID, four one-byte platform/executable/disc fields, and savegame
ID. The returned pointer references the runtime's read-only copy of the loaded
XEX header. No title ID, version, edition, or disc metadata is inferred from a
filename or configuration string. A title without that header remains
unimplemented for this call.

## Console/profile and video configuration

`XGetAVPack` (`0x03CB`), `XGetGameRegion` (`0x03CC`), `XGetLanguage`
(`0x03CD`), and `XGetVideoMode` (`0x03D1`) read `XamRuntimeConfig`. The owner
must call `runtime_configure_xam()` once for each `Runtime`; there are no
implicit language, region, AV-pack, or display defaults. `XGetVideoMode` writes
six big-endian 32-bit fields: width, height, interlaced flag, widescreen flag,
high-definition flag, and the IEEE-754 bits of the refresh rate. Its full
24-byte output is validated writable before the first write.

The R-comp tree currently provides only one independently named language value:
`XDBF_LANGUAGE_ENGLISH = 1` in XenonRecomp's XDBF definitions. It does not
contain an authoritative enum table for `XGetGameRegion` or `XGetAVPack`.
Consequently those raw values must be selected explicitly by the title/harness
from a separately established console profile; test values are not production
defaults.

## Controller lifetime and buffer rights

Controller packet history now belongs to `XamState`, so destroying `Runtime`
also destroys packet counters and prior pad state. `XamInputGetState` validates
all 16 output bytes as read/write, `XamInputGetCapabilities` validates all 20,
and `XamInputSetState` validates its four-byte input as readable before calling
the backend. This prevents partial writes and prevents a later title from
inheriting packet history from an earlier runtime.

## The local profile

The PS5 has no Xbox gamertag or LIVE account, so the title sees one local,
offline profile in slot 0 (`rcomp/runtime/xam_profile.h`). It is signed in
locally (state 1), its XUID is the offline XUID `0xE000000052434F4D`, and its
gamertag is "Player". Slots 1-3 are empty.

- `XamUserGetSigninState` (0x210) answers 1 for slot 0 and "any user" (0xFF),
  and 0 for the others.
- `XamUserGetXUID` (0x20A) gives the offline XUID when the type mask asks for an
  offline XUID (bit 0), and `X_E_NO_SUCH_USER` for an online-only mask.
- `XamUserGetSigninInfo` (0x227) fills X_USER_SIGNIN_INFO: XUID (unless
  online-only), state 1, name "Player".
- `XamUserGetName` (0x20E) copies the name, NUL-terminated, truncated to the
  buffer.
- `XamUserCheckPrivilege` (0x212) answers FALSE: the privileges are Xbox LIVE
  ones.

Every LIVE-only service answers as a console whose profile is not signed in to
LIVE. This covers presence, friends, marketplace, QoS, statistics,
`XNetLogon*`, gamercards of other players and player review
(`runtime/src/hle_xam_misc.cpp`, mostly `ERROR_NOT_LOGGED_ON`). The empty slots
1-3 report `X_E_NO_SUCH_USER` / `ERROR_NO_SUCH_USER`.

## Saves: content packages on the hard drive

One storage device exists: the hard drive (device id 1). It is backed by the
host directory the title owner configures with `runtime_configure_save_root`.
On the PS5 that directory is `/app0/savedata`, inside the title's own app
folder, which R-comp Installer never deletes, so saves survive updates
(`runtime/src/hle_xam_content.cpp`, layout in `rcomp/runtime/xam_content.h`).

| Service | Behaviour |
| --- | --- |
| `XamContentCreateEx` (0x259) | Creates and opens a real directory for the package, following the disposition (CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING). Package file names match case-insensitively. The package is mounted read-write under the title's root name (for example `save:`). |
| `XamContentClose` (0x25A) | Unmounts the root name (`Vfs::unmount`). |
| `XamContentCreateEnumerator` / `XamEnumerate` (0x25C / 0x250) | List the packages that exist. |
| `XamContentGetCreator` (0x262) | Reads the creator recorded at creation. |
| `XamContentSetThumbnail` (0x260) | Stores the thumbnail beside the package. |
| `XamContentGetDeviceData` (0x25E) | Reports the drive's real total and free bytes (`statvfs`). |
| `XamShowDeviceSelectorUI` (0x2CB) | Completes with the hard drive, the default a console offers with one device, and logs the call (owner decision: system dialogs complete with their default). |

Without a configured save root no device exists, and the calls answer
`ERROR_DEVICE_NOT_CONNECTED`.

On the PS5, a title can neither measure its file system nor use `lstat()` (all
measured on the console, 3 Oct 2026):

- `statvfs()` and `fstatvfs()` answer `ENOSYS`.
- `statfs()` is exported only to system processes, so a call from a title jumps
  to address 0.
- `lstat()` is refused with `EPERM`; package directories are cleared using the
  `readdir()` entry type, else `stat()`.

The drive's capacity therefore comes from `savedata/.rcomp-capacity`. R-comp
Installer's un-sandboxed chmod payload measures it with `statfs("/data")` at
every install. GTA IV reads the free bytes before it saves.

The diagnostic build option `RCOMP_M6_CONTENT_SELFTEST` (`app/src/content_selftest.cpp`)
runs every save service once on the console's real storage before the guest
starts. On 3 Oct 2026 it passed all 13 steps on the PS5 with GTA IV.

Profile settings (`XamUserReadProfileSettings` 0x219,
`XamUserWriteProfileSettings` 0x21A, `runtime/src/hle_xam_profile.cpp`):

- A setting the title wrote is persisted per title and returned with source
  TITLE.
- A known system setting gives the console default (source DEFAULT).
- A title-specific setting never written has no value.
- An unknown id is `ERROR_INVALID_PARAMETER`.

Achievement enumerators are valid and empty: R-comp keeps no achievement
records.

Gamer pictures (`XamWriteGamerTile` 0x2F0, the import behind
`XUserAwardGamerPicture`, `runtime/src/hle_xam_profile.cpp`): the title awards
a picture by its ids in its own SPA, the XDBF database stored as the XEX
resource named after the title id (resource-info header 0x000002FF). Both PNG
images of the picture (namespace 2, big and small id) are copied into the local
profile:

    <save root>/<XUID>/gamertile/<TitleID>/<ImageID>.png

Awarding the same picture again rewrites the same files. Results:

| Case | Result |
| --- | --- |
| User index past 3 | `E_INVALIDARG`, returned directly (as Xenia does) |
| Slots 1-3 | `ERROR_NO_SUCH_USER` |
| No save root | `ERROR_DEVICE_NOT_CONNECTED` |
| Id not in the SPA | `ERROR_NOT_FOUND` (R-comp's choice; the console's code is not established) |
| Write failure | `ERROR_FUNCTION_FAILED` |

The results after the first one go through the overlapped (`ERROR_IO_PENDING`)
when the title passes one. Only flag 1 (what `XUserAwardGamerPicture` passes)
and the running title's id (or 0) are established; anything else stops.

## Launch data

`runtime/src/hle_xam_loader.cpp`: XAM keeps one launch-data buffer per Runtime.
`XamLoaderSetLaunchData` (0x1A6) copies `(Data, Size)` into it (size 0 clears it;
unreadable data is `ERROR_INVALID_PARAMETER`; above 64 KiB, a bound R-comp sets
because no console limit is established, is an explicit fatal).
`XamLoaderGetLaunchDataSize` (0x1A7) reports the stored size with `ERROR_SUCCESS`,
or 0 and `ERROR_NOT_FOUND`; `XamLoaderGetLaunchData` (0x1A8) copies
`min(Size, stored)` bytes, `ERROR_NOT_FOUND` (buffer untouched) when there is none.
R-comp starts the title directly, so nothing is stored until the title sets it.
An unwritable size pointer or buffer is `ERROR_INVALID_PARAMETER`.

## Dialogs, tasks, sessions and voice

`runtime/src/hle_xam_misc.cpp` covers these:

- **Message boxes** complete with the button the title designated as the default
  (owner decision), and are logged as `RCOMP-XAM-UI`.
- **`XamTaskSchedule`** runs the title's callback on a real guest thread.
- **Sessions** are real handle-table objects.
- **Voice** reports that no headset is connected.

`runtime/src/hle_xam_ui_more.cpp` adds the Guide screens Gears of War 2 opens,
under the same owner rule:

| Dialog | Answer |
| --- | --- |
| `XamShowKeyboardUI` (0x2C1) | Local. The title's default text (empty for NULL) is the entered text, cut to buffer length - 1 WCHARs and NUL-terminated. The overlapped completes with `ERROR_SUCCESS` (`ERROR_IO_PENDING` returned). Logged with title, description and default. |
| `XamShowAchievementsUI` (0x2C5) | Local. Closes at once as viewed, `ERROR_SUCCESS` (R-comp keeps no achievement records). |
| `XamShowMessagesUI` (0x2C0) | LIVE-only |
| `XamShowPlayersUI` (0x2C8) | LIVE-only |
| `XamShowGameInviteUI` (0x2CD) | LIVE-only |
| `XamShowFriendRequestUI` (0x2CE) | LIVE-only |
| `XamShowCustomPlayerListUI` (0x2E6) | LIVE-only, through its overlapped |

A LIVE-only screen answers `ERROR_NOT_LOGGED_ON` for the local profile,
`ERROR_NO_SUCH_USER` for slots 1-3 and `ERROR_INVALID_PARAMETER` past them. All
are logged as `RCOMP-XAM-UI`.

## Second title wave (Halo 3, NARUTO STORM 3, Fallout: New Vegas)

`runtime/src/hle_xam_more.cpp` and `XamShowMessageBoxUI` in `hle_xam_misc.cpp`.
Halo 3 ABI from its recompiled call sites (addresses in the source header);
the others from Xenia 95a5c3e / rexglue-sdk c94f5eb (read only).

| Export | Answer |
| --- | --- |
| `XamShowMessageBoxUI` (0x2CA) | The `Ex` box without `dwUnknown` (result in r10, overlapped in the first stack slot): the designated default button, owner decision. |
| `XamShowCustomMessageComposeUI` (0x2E5) | LIVE-only (`ERROR_NOT_LOGGED_ON` / `ERROR_NO_SUCH_USER` / `ERROR_INVALID_PARAMETER`), logged as `RCOMP-XAM-UI`. |
| `XamGetOverlappedResult` (0x1FB) | `(XOVERLAPPED*, PDWORD, bWait)`: a completed request returns its result and `InternalHigh`; a pending one `ERROR_IO_INCOMPLETE`, or with `bWait` blocks in `NtWaitForSingleObjectEx` on `hEvent`. `bWait` without an event has no contract: explicit fatal. |
| `XMsgCompleteIORequest` (0x1F5) | `(XOVERLAPPED*, result, extended error, length)`: writes length, extended error, then result, then signals the event. `ERROR_INVALID_HANDLE` (nothing written) for an `hEvent` that is not an event; completion routines are an explicit fatal. |
| `XamUserGetDeviceContext` (0x208) | The controller port of a connected user's controller (`XUSER_INDEX_ANY` = port 0, the reference's value for user 0), `S_OK`; otherwise 0 and `HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)`; `E_INVALIDARG` past index 3. The console's encoding of the context is not established. |
| `XamVoiceIsActiveProcess` (0x497) | FALSE: no voice device exists. |
| `XamContentGetLicenseMask` (0x266) | Mask 0 (no marketplace licence), `ERROR_SUCCESS` or through the overlapped. |
| `XNotifyDelayUI` (0x28D) | Records the delay (0x7FFFFFFF = indefinitely); no popup is ever shown. `ERROR_SUCCESS`. |
| `XCustomRegisterDynamicActions` / `XCustomUnregisterDynamicActions` (0x1DD / 0x1DE) | Join / leave dynamic gamercard actions; leaving forgets the action set. |
| `XCustomSetDynamicActions` (0x1DA) | `(user, XUID, XCUSTOMACTION*, WORD count)`: copies the 52-byte actions (`+0 WORD id, +2 WCHAR text[23], +0x30 DWORD flags`, from Halo 3's array stride). Before registration: explicit fatal. |
| `XCustomGetLastActionPressEx` (0x1DC) | No Guide, so nothing is pressed: `ERROR_NOT_FOUND` (Halo 3 tests only for non-zero; the console's exact code is not established). Outputs untouched. |
| `XCustomGetCurrentGamercard` (0x1DF) | FALSE: no gamercard is displayed. |

State (launch data, notification delay, action set) belongs to one Runtime
generation. Host evidence: `runtime/tests/test_xam_more.cpp`. PS5: NOT TESTED.

## Deliberately unresolved offline services

Any XAM service not listed in this document stays unregistered until R-comp
has real state and error semantics for it. Returning
success from these without the corresponding identity, storage, lifecycle, or
completion work would create false boot progress. Network-specific XAM/NetDll
services are owned and reviewed separately; this document makes no claim that
those services are already implemented.
