# Objects, threads and time conversions

Runtime workstream, September 28, 2026. This work adds only the two calendar
conversions actually imported. It publishes no thread-object descriptor and
defines no kernel variable whose structure has not been established.

## ABI and sources

The XenonRecomp export table used by the project names `RtlTimeFieldsToTime` at
ordinal `0x013F` and `RtlTimeToTimeFields` at `0x0140`. HLE registration checks
these ordinals before publishing the implementations.

`TIME_FIELDS` is treated as eight signed 16-bit integers, in the order
`Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday`. As with other
guest structures in this runtime, fields and the 64-bit `LARGE_INTEGER` are
read and written in big-endian order.

Calendar behavior is checked against `ntdll.dll` on Windows using
`tests/xex/ntdll_calendar_oracle.py`. This independent oracle confirms the
calendar values used in `runtime/tests/test_time.cpp`; it is an NT reference,
not Xbox 360 or PS5 hardware proof. Ordinals come from the project's export
table, not this Windows oracle.

## RtlTimeFieldsToTime

Input must be a readable 16-byte structure and output a writable 8-byte integer.
An access fault produces the `guest_access` diagnostic before any output write.
Input may overlap output: all eight fields are read before the first write.

The validated subset accepts years 1601 through 30827 inclusive and applies
the Gregorian calendar, with hours 0..23, minutes/seconds 0..59 and milliseconds
0..999. Input `Weekday` is unused. An invalid date within this domain returns
`FALSE` and leaves output intact. Years above 30827 trigger `unimplemented`:
the Windows version used by the oracle rejects them during direct conversion,
but that behavior does not establish the Xbox kernel domain. The runtime
therefore does not turn this lack of evidence into a false hardware contract.

The result is the number of 100 ns periods since January 1, 1601 UTC. Pinned
cases include the epoch, 1970, leap-century rules, year 10000, and
`30827-12-31 23:59:59.999`.

## RtlTimeToTimeFields

Input is read completely before output, so exact input/output aliasing is
supported. The validated domain is `0..INT64_MAX`, interpreted as absolute time
in 100 ns periods. Sub-millisecond fractions are truncated, as in the oracle.
`Weekday` is calculated with Monday = 1 for `1601-01-01`; `INT64_MAX` yields
`30828-09-14 02:48:05.477`, weekday 4.

A negative value when `LARGE_INTEGER` is interpreted as signed triggers
`unimplemented`. The observed Windows implementation returns inconsistent
fields for some of these negative values; copying them would not constitute
an established Xbox contract.

## System time and limitations of this work

`KeQuerySystemTime` remains a separate export: it writes a FILETIME obtained
from `CLOCK_REALTIME` after explicitly validating RW output. The host test
bounds its value between two readings of that same clock; it does not claim
to provide a deterministic hardware-time oracle.

This work creates no `ExThreadObjectType`, `KeTimeStampBundle`, guest KTHREAD
or current-thread pseudo-handle. Their form and lifetime remain outside the
contract until the corresponding ABI research is sufficient.
