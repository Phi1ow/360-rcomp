# XAudio render driver and platform output

The runtime owns the guest API, BE conversion, client argument lifetime and
the 256-frame/48 kHz callback clock. The platform owns actual output and
bounded durable queues through `include/rcomp/audio_output.h`. This is original
R-comp code. Public research was read for the protocol; no rexglue/Xenia audio
subsystem was imported.

* Register (0x1F3) validates `{be32 callback, be32 argument}` and its writable
  result, allocates an argument wrapper, and opens a real platform stream.
  Unsupported/error output fails explicitly, releases the wrapper, leaves the
  driver output unchanged and starts no callback. A successful open precedes
  publication of `0x41550000 | slot`. Eight clients are supported. The first
  registration creates one host worker, with STARTING published under the
  state mutex before pthread creation. Lifecycle operations serialize through
  a separate mutex; platform calls never hold the client-state mutex.
* The callback receives the address of the BE32 argument wrapper. The public
  pinned protocol uses this indirection. A bounded registration log records
  callback, argument, wrapper and stream so the actual title's callback can be
  inspected in AOT output. This title-specific ABI remains NOT TESTED until
  that address is observed and checked; generated C++ is never edited.
* A normal callback return preserves its host guest-thread identity, PCR, TLS
  and scheduler state. ExTerminateThread retains actual thread retirement; a
  subsequent callback receives a freshly allocated context. The host worker
  retires and destroys its guest context when it really ends.
* Submit (0x1F5) reads 0x1800 bytes: six BE float planes, each containing 256
  samples. Conversion produces native-endian interleaved FL, FR, FC, LFE,
  surround-left and surround-right. An active stream lease protects this
  conversion and platform submission from close. The platform copies all
  samples into its owned bounded queue before OK. The runtime increments its
  submitted counter only after that OK. BUSY/errors accept no frame and are
  returned explicitly. Queue acceptance is not proof the device played it.
* The first actual Submit failure per registered client is logged with its
  exact HRESULT, driver, guest frame pointer and stream. If the backend was
  called, its actual return code is included; input/state rejection explicitly
  reports backend_called=0. There are no per-frame clocks, retries, fake
  acknowledgements or repeated failure logs. Registration starts a fresh group.
* Unregister (0x1F4) prevents new callback/submit leases, waits for existing
  leases, then closes the platform stream synchronously. Close failure keeps
  the stream token and wrapper for retry. Wrapper memory is freed only after
  successful close. A call from the audio callback thread returns BUSY rather
  than waiting on its own callback lease. Its client remains active. This
  explicit failure is a current limitation for self-unregistering callbacks.
* Shutdown stops and joins the worker before closing all streams and freeing
  guest wrappers. A request from that same callback thread is fatal before
  pthread_join(self). Join/close failure is fatal and retains ownership rather
  than proceeding into runtime memory destruction.

The platform return values map to HRESULTs: OK=0, INVALID_ARGUMENT=0x80070057,
UNSUPPORTED=0x80004001, BUSY=0x800700AA, DEVICE_ERROR/STOPPED=0x80004005.
The exact Xbox HRESULT mapping beyond E_INVALIDARG remains NOT TESTED on PS5.
Invalid/free driver handles return E_INVALIDARG.

The callback cadence currently follows an absolute monotonic host clock at
256/48000 seconds, with real queue pressure reported by submit. Device
completion does not yet drive callback scheduling. Clock/source correctness
and audible output need separate PS5 evidence.

`XAudioGetSpeakerConfig` (0x1FF) retains the fixed guest profile 0x00010001,
a compatibility value that does not query attached speakers. Category-volume
change-mask (0x1F7) is zero because there is no system mixer; category volume
(0x1F8) is unity. XMA decoding is described independently in `XMA.md`.

Primary protocol references read without copying subsystem source:

* [Pinned audio conversion layout](https://github.com/rexglue/rexglue-sdk/blob/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518/include/rex/audio/conversion.h).
* [Pinned registration and argument lifetime](https://github.com/rexglue/rexglue-sdk/blob/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518/src/audio/audio_system.cpp).
* [Microsoft channel mapping](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping).

The host tests use TESTDOUBLE_audio_output under tests and check rejection,
copy ownership, bounded queues, close retries, callback lifetime and races.
These are PASS host evidence; device output and PS5 audio remain NOT TESTED.

## Ducker

`runtime/src/hle_xboxkrnl_audio_ducker.cpp` (NARUTO STORM 3). The ducker lowers
title audio while voice chat is heard; R-comp has no voice device, so it never
engages. Kept per Runtime generation: `XAudioEnableDucker(BOOL)` (0x34D, `S_OK`)
sets the flag `XAudioIsDuckerEnabled()` (0x34F) returns. `XAudioGetDuckerLevel`
(0x350), `Threshold` (0x351), `AttackTime` (0x353), `ReleaseTime` (0x355) and
`HoldTime` (0x357) write the setting to their single output pointer and return
`S_OK`. The console's initial values are not established; R-comp reports the
all-zero word (0 and 0.0f alike) and starts disabled. The getter ABI follows the
kernel XAudio query convention without a title call site (NARUTO is not in the
local catalog): an output that is not writable guest memory is an explicit fatal,
not an answer. The setters are not registered: some take a FLOAT (passed in
f1), and no call site establishes which. Host evidence:
`tests/test_xaudio_ducker.cpp`. PS5: NOT TESTED.
