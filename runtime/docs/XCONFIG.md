# Xbox XConfig query contract

This tranche implements `xboxkrnl.exe!ExGetXConfigSetting` as a read-only view
of an explicit guest profile. It does not read host locale, timezone, display,
or region settings, and it does not fill unknown XConfig fields with zeros.

## Public layout evidence

Free60's archived XConfig description names category `0x2` as secured and
category `0x3` as user. The secured record stores `AVRegion` as four bytes at
offset 40. The user record stores timezone settings 1..7, language at offset
44, `VideoFlags` at 48, and `RetailFlags` at 56; every setting used here is
four bytes. `XCONFIG_TIMEZONE_DATE` is four raw bytes: month, day, day-of-week,
hour.

Free60 libxenon's physical-config reader identifies the display-standard byte
inside `AVRegion` as NTSC-M=1, NTSC-J=2, PAL50=3, PAL60=4. The public
`xenia-edge` XConfig definitions give the complete guest values used here:
`0x00400100`, `0x00400200`, `0x00800300`, and `0x00400400`, respectively.
They also define `XCONFIG_USER_VIDEO_FLAGS.Widescreen` as `0x00010000`.
Consequently a configured 1280x720, 60 Hz, progressive HDMI profile exposes
the widescreen bit through setting `0xA`; resolution, scan mode and connector
remain profile facts and are not invented as extra `VideoFlags` bits.

References:

- https://github.com/Free60Project/wiki/blob/master/docs/System-Software/XConfig.md
- https://github.com/Free60Project/libxenon/blob/master/libxenon/drivers/xenon_nand/xenon_config.h
- https://github.com/Free60Project/libxenon/blob/master/libxenon/drivers/xenon_nand/xenon_config.c
- https://github.com/has207/xenia-edge/blob/edge/src/xenia/kernel/xconfig.h

The pinned rexglue c94f source in `build/prime-mihawk-audit` is used only for
the exported ABI and status family: category in `r3`, setting in `r4`, buffer
in `r5`, 16-bit buffer size in `r6`, optional 16-bit required-size pointer in
`r7`; ordinal `0x0010`; `STATUS_BUFFER_TOO_SMALL=0xC0000023` and
`STATUS_INVALID_PARAMETER_1/2/3=0xC00000EF/F0/F1`. Its placeholder zero
timezone fields and fixed video value are not copied.

## Runtime profile and query behavior

`XConfigProfile` has no usable default. Bootstrap must supply language,
`XConfigAvRegion`, and a concrete display profile. `XConfigTimeZoneProfile`
and retail flags are separately marked configured because GTA IV has static
call sites for user settings 1..7 and `0xC`, but a runtime cannot claim those
values until the title owner selected them.

The currently implemented settings are:

| Category | Setting | Encoding/source |
| --- | ---: | --- |
| secured `2` | `2` | `AVRegion`, BE32 from `profile.av_region` |
| user `3` | `1` | timezone bias, BE32 |
| user `3` | `2` | timezone standard name, four raw bytes |
| user `3` | `3` | timezone daylight name, four raw bytes |
| user `3` | `4` | timezone standard date, four raw bytes |
| user `3` | `5` | timezone daylight date, four raw bytes |
| user `3` | `6` | timezone standard bias, BE32 |
| user `3` | `7` | timezone daylight bias, BE32 |
| user `3` | `9` | language, BE32 |
| user `3` | `A` | `0x00010000` iff the configured video is widescreen |
| user `3` | `C` | explicitly configured retail flags, BE32 |

An unknown category returns `STATUS_INVALID_PARAMETER_1`; an unknown setting
in a known category returns `STATUS_INVALID_PARAMETER_2`; null buffer with a
nonzero size returns `STATUS_INVALID_PARAMETER_3`; an undersized non-null
buffer returns `STATUS_BUFFER_TOO_SMALL`; an inaccessible output or
required-size pointer returns `STATUS_ACCESS_VIOLATION`. A known setting whose
profile field is not configured returns `STATUS_NOT_SUPPORTED`. Failures do
not modify either output. A successful size-only query uses `buffer=0,size=0`
and writes the BE16 required size when that pointer is non-null.

All target ranges are checked before the first guest write, so an invalid
required-size pointer cannot leave a partially updated value buffer.

## Integration proposal after the services freeze

Keep one `XConfigState` per `Runtime` once shared headers are writable. Until
then the isolated source binds its state to `Runtime::generation`, preventing
a later title from seeing a stale profile even if reset was missed.

PRIME can integrate this tranche with four small calls/fields after the freeze:

1. Add `XConfigProfile xconfig_profile` plus `bool configure_xconfig` to
   `TitleConfig`, populated beside `xam_profile`. Copy language and all display
   facts from the same explicit title configuration; select `av_region`,
   timezone and retail flags explicitly rather than from host hardware.
2. After `runtime_init`, call `register_xboxkrnl_xconfig_hle()` and then
   `runtime_configure_xconfig(cfg.xconfig_profile)` before guest threads start.
3. For the intended virtual 720p60 progressive HDMI profile, set width=1280,
   height=720, refresh=60000 millihertz, interlaced=false, widescreen=true,
   high_definition=true, connector=HDMI, and choose AV region/language from
   the title's explicit profile.
4. Call `runtime_reset_xconfig()` during runtime shutdown until the state is
   moved into `Runtime`, at which point normal object destruction owns reset.

This source does not register `ExSetXConfigSetting` or any other XConfig
mutation API.
