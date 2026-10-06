# AudioOut PS5 — backend original R-comp

`audio_output_ps5.cpp` implements the public `rcomp/audio_output.h` contract.
It is a separate static library proposed as `rcomp_platform_audio`; adding its
sources does not enable audio in an existing title. PRIME owns build and runtime
integration. No rexglue/Xenia audio code, SDK binary or game data is included.

## Device and data contract

- Open initializes AudioOut, then opens the SYSTEM route (user 0xFF), MAIN port
  (type 0, index 0), 256 frames, 48,000 Hz, signed 16-bit stereo (format 1).
  The platform interface carries no guest user identity. This backend requires
  no UserService initialization, initial-user selection or UserService import.
- Open publishes its token only after a real silent output succeeds and its
  worker exists. Any failed native init/open/prime or worker startup returns
  DEVICE_ERROR with the output argument unchanged.
- Input is 256 frames of six native-endian IEEE binary32 floats, interleaved
  FL FR FC LFE SL SR. Runtime owns guest big-endian / planar conversion.
- Each active stream owns a queue of eight complete input blocks (49,152
  bytes), one complete in-flight block, and two durable native PCM buffers.
  Submit copies all 6,144 input bytes before OK. It retains no caller pointer.
- Eight independently owned streams are supported. Their queues, PCM buffers
  and workers are distinct. There is no software mixing between clients.
- Downmix is original: L = FL + 0.70710678 FC + 0.5 LFE + 0.70710678 SL;
  R = FR + 0.70710678 FC + 0.5 LFE + 0.70710678 SR. Both use gain
  1/(1 + 0.70710678 + 0.5 + 0.70710678), then round and saturate to S16.
  The constant headroom keeps simultaneous unit inputs within native PCM.
- NaN/infinity is INVALID_ARGUMENT and accepts nothing. Finite oversized
  samples are saturated during conversion. A full pending queue is BUSY and
  accepts nothing. DeviceFailed rejects further submissions with DEVICE_ERROR.
- A successful submit means ownership transferred into an active device queue;
  it does not establish playback. A later native device error may prevent
  already accepted blocks from playing. There is no fabricated completion.
- With no queued guest block, the worker sends real silence through AudioOut.
  Native Output provides device pacing; no frame-rate or game clock is changed.

## Lifetime and failure contract

Tokens contain a slot and monotonically increasing generation. A closed token
cannot submit into a reused slot, and its close stays idempotent. Unknown future
tokens are INVALID_ARGUMENT; submissions to closed tokens are STOPPED.

Close serializes against another close/open for that slot, rejects new submit,
cancels the pending queue, joins the output worker, then calls native Close.
No native operation holds the queue mutex. Join/Close failure keeps the port,
buffers, generation and ownership for retry; new submissions remain STOPPED.
Storage has process lifetime, so a failed cleanup does not free buffers still
owned by native code. Native output completion is required for synchronous join;
behaviour of a hung native device has not been tested on PS5.

If rollback after a failed open cannot close its port, the unpublished resource
remains bounded in that slot. A later open retries its cleanup. No stream token
is fabricated and the failed caller's output argument remains untouched.

Each failed native operation logs one `RCOMP-AUDIO native` line with operation,
port and signed/hex result. Failed Open additionally logs all six arguments:
user, port type, index, frames, frequency and format. Successful per-block output
is not logged.

## ABI provenance and validation

Minimal function declarations are written independently in `audio_native.h`.
The public [OpenOrbis AudioOut declarations](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/AudioOut.h)
support the six-argument legacy Open ABI, and its
[audio types](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/audio_out.h)
define MAIN=0 and S16_STEREO=1. These establish a source for the legacy ABI;
they do not prove parameter support or behaviour on PS5.

The public [SDL PS5 audio backend](https://github.com/ps5-payload-dev/SDL/blob/release-2.30.x-ps5/src/audio/ps5/SDL_ps5audio.c)
uses the same six-argument Open with SYSTEM, MAIN, index 0 and 48 kHz. Its
[native ABI header](https://github.com/ps5-payload-dev/SDL/blob/release-2.30.x-ps5/src/audio/ps5/SDL_ps5audio.h)
defines SYSTEM=0xFF, MAIN=0 and S16_STEREO=1. R-comp's implementation remains
original; no SDL subsystem or source is imported. SDL's source notice uses the
zlib license.

PRIME's phase 10 title with the initial-user route failed Open with 0x80260011.
The public [OpenOrbis error definitions](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/errors.h)
name this legacy AudioOut code SYSTEM_RESOURCE. This does not identify which
resource failed. The SYSTEM route is an isolated change requiring a new PS5
test; no native resource failure is converted to success.

The local PS5 SDK import libraries compile and link these declarations. Firmware
9.40 ELF dynamic symbol metadata independently contains all required exports:

| Module | Import | NID |
| --- | --- | --- |
| libSceAudioOut | sceAudioOutInit | JfEPXVxhFqA |
| libSceAudioOut | sceAudioOutOpen | ekNvsT22rsY |
| libSceAudioOut | sceAudioOutOutput | QOQtbeDqsT4 |
| libSceAudioOut | sceAudioOutClose | s1--uE9mBFw |

Reproducible read-only audit and results live under
`build/platform-audioout-20260930/audit_exports.py` and `exports-9.40.json`.
The firmware modules and SDK are read only and are never copied into source.
Export presence and SDK linkage do not prove title module loading or audibility.

## Verification

`build/platform-audioout-20260930/verify.sh`: seven host suites with native
TESTDOUBLE, seven with UBSan trap, explicit host UNSUPPORTED backend, PS5 SDK
compile, isolated synthetic payload link and firmware NID audit. PASS, exit 0.

`build/platform-audioout-20260930/verify_cmake.sh`: proposed patch application
check, isolated CMake host audio tests (2/2), PS5 static library build, and
PRIME's `app/m6/native_audio_probe.cpp` compilation. PASS, exit 0.

Host tests exercise ownership after caller-buffer reuse, full queue, input
validation, native output failure, rollback/Close failures and retry, blocked
consumer during close, eight independent ports, stale tokens, concurrent
submit/close and slot reuse, and numerical routing/saturation. TESTDOUBLE native
functions exist solely in `platform/tests/TESTDOUBLE_audio_native.*`.

PS5 title execution, route negotiation, real device pacing, game integration,
and audibility: NOT TESTED. Only PRIME runs the console. The standalone SDK
probe is a compile/link artifact; use PRIME's native title probe for proof.
