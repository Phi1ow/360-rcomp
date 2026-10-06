# Calendar/threads proof — 28 September 2026

Original R-comp title `PPSA88360`, build `38d8473ebfa9282d`, run on the PS5
at 20:46:07 UTC. Result: **13/13 platform + 20/20 calendar/threads**,
six PPC returns of `0x70` after 45 checks per entry. The title was stopped
and the temporary controller closed after collection.

`INTEGRATION_RESULTS.json` gathers the verified results and the hashes of
the local artifacts. `EXPECTATIONS.json` and `PRELAUNCH.json` are the
records made before launch: their `NOT TESTED` field deliberately describes
the state before execution, not the final result.
`rcomp_title.log` is the exact capture of the original fixture.

`NEGATIVE_RESULT.json` and `negative_fixture.patch` describe the negative
host check: a single expected constant is changed, the PPC is recompiled
and linked against the same five archives, then the error is detected
(`0xE403`, process exit code 1). The PASS of this proof is about detecting
the error.

Verifying the evidence copies from this folder:

```sh
sha256sum -c SHA256SUMS
```

Redaction (6 October 2026): the console's LAN address was replaced by
`<PS5_IP>` in `EXPECTATIONS.json` and `INTEGRATION_RESULTS.json`, and their
checksums were updated. No other byte changed.

Raw klogs, binaries, XEX files and commercial data are not included.
The complete evidence stays under `build/prime-calendar-resume-20260928/`,
`build/platform-calendar-resume-20260928/` and the test folders cited in
the [session report](../../docs/SESSION_CALENDAR_THREADS_20260928.md).

GTA IV remains **BLOCKED** by 166 missing functions and three missing
variables; game execution and rendering for this slice are **NOT TESTED**.
No source or configuration of Emu-3's obsolete PS5_Vulkan is an input of
this proof.
