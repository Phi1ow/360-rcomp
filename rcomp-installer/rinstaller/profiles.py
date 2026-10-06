"""Build profiles for mode 3: the R-comp CMake options a title is built with.

Every profile turns the diagnostic defaults of R-comp's exploratory title off for players: the 1 kHz PC
sampler (it signals four threads ~4000 times a second), the periodic GPU readback that writes a 24 MB
frame capture into the app folder and the scripted autopilot input; and it keeps the launch options menu
(resolution) on. The title profiles add the options of the fastest *clean* measured
builds (4K, scripted runs, no black frame; see R-comp docs/PERF_4K30_20260929.md and
docs/TBOGT_BRINGUP_20261002.md). The synthetic native-audio probe and the timebase observer of those
measurement builds are diagnostics and stay off.
"""

PLAYER = {
    'RCOMP_M6_PC_SAMPLER': 'OFF',
    'RCOMP_M6_CAPTURE_INTERVAL_SECONDS': '0',
    'RCOMP_M6_NATIVE_AUDIO_PROBE': 'OFF',
    'RCOMP_M6_TIMEBASE_OBSERVER': 'OFF',
    # Only the player's controller drives the game: no /app0/autopilot.txt script.
    'RCOMP_PS5_AUTOPILOT': 'OFF',
    # The launch options menu (internal resolution, remembered in /app0/savedata) before every start.
    'RCOMP_M6_LAUNCH_MENU': 'ON',
    # Console diagnostics never ship: the save-service self-test stays off.
    'RCOMP_M6_CONTENT_SELFTEST': 'OFF',
}

# RAGE (GTA IV engine) options of Grand Theft Auto IV, measured on the console: RADV wave32 + no FMASK/NGG, the title's main guest
# thread alone on physical core 0 (RCOMP_CPU_MASK_GUEST_MAIN, its hyperthread sibling left idle), the other
# guest threads on CPUs 2-7, replay on CPU 8, bridge on CPU 12, the runtime's service threads on their own
# mask, asynchronous submission, rolling revalidation of tracked uploads every 8 frames, the fused resolve
# (bit-identical, +8.2 %) and the generated code at -O3. This is the configuration of arm 197 (4 October
# 2026): in the owner's city play at 4K, 54.18 fps over the final 90 s at his heaviest spot against 48.67 for
# the previous placement, minimums 46-50 instead of 40-46; the smoothest session by his account.
RAGE_197 = {
    'RCOMP_M6_RADV_PERFTEST': 'cswave32,pswave32,gewave32',
    'RCOMP_M6_RADV_DEBUG': 'nofmask,nonggc',
    'RCOMP_M6_TUNING_ENV': 'RCOMP_CPU_MASK_GUEST_MAIN=0x1,RCOMP_CPU_MASK_GUEST=0xFC,RCOMP_CPU_MASK_BRIDGE=0x1000,RCOMP_CPU_MASK_REPLAY=0x100',
    'RCOMP_M6_XENOS_CVARS': 'rcomp_async_submit=true,rcomp_revalidate_frames=8,rcomp_fused_resolve=true',
    'RCOMP_M6_GENERATED_OPT': '-O3',
}
# The RAGE configuration measured before 4 October (waves 5-7 and the TBoGT bring-up): guest threads on CPUs 0-7,
# replay on CPU 8, bridge on CPU 12, generated code at -O2. Kept for Episodes from Liberty City, whose measurements
# (55-56 fps, TBoGT bring-up 31-44) were made with it; the arm-197 placement is NOT TESTED on that title.
RAGE = {
    'RCOMP_M6_RADV_PERFTEST': 'cswave32,pswave32,gewave32',
    'RCOMP_M6_RADV_DEBUG': 'nofmask,nonggc',
    'RCOMP_M6_TUNING_ENV': 'RCOMP_CPU_MASK_GUEST=0xFF,RCOMP_CPU_MASK_BRIDGE=0x1000,RCOMP_CPU_MASK_REPLAY=0x100',
    'RCOMP_M6_XENOS_CVARS': 'rcomp_async_submit=true,rcomp_revalidate_frames=8,rcomp_fused_resolve=true',
    'RCOMP_M6_GENERATED_OPT': '-O2',
}

# Generator configuration of the tuned titles (XenonRecomp options, passed to tools/m6_inventory.py as --recomp-option): the
# condition register, CTR, XER and the load reservation are locals of each recompiled function instead of fields of the PPC
# context ("group 1" of R-comp docs/OPTIMIZATION_AUDIT_20260930.md C2: linked .text -20 %, busy cores -6 %). Every GTA IV and
# EFLC measurement of 2-4 October 2026, arm 197 included, ran code generated with it. "Group 2" (non_argument_as_local,
# non_volatile_as_local) is rejected there: some functions read those registers as inputs (tools/scan_register_inputs.py).
# NOT TESTED on other titles: the generic profile keeps the generator defaults.
RAGE_GENERATOR = {
    'cr_as_local': 'true',
    'ctr_as_local': 'true',
    'xer_as_local': 'true',
    'reserved_as_local': 'true',
}

# Test builds of 4 October 2026 (the owner asked for "the maximum" before stopping): the same games and generator options, plus what the
# instruction-level profile of Episodes from Liberty City (tools/pc_ppc_profile.py) and the earlier GTA IV profiles pointed at.
#   * generated code at -O3 without frame pointers (the SDK compiler keeps rbp frames: three instructions per function and a register);
#   * an inline direct-mapped cache of the indirect-call lookup (include/rcomp/indirect_cache.h, in every build since the prelude carries it);
#   * a linker symbol-ordering file (tools/hot_function_order.py): the functions the guest threads actually ran, hottest first;
#   * 'maxcap' only: RCOMP_SLEEP_CAP_NS wakes the main guest thread after at most 0.25 ms from the ~1 ms sleeps of its polling loops
#     (an experiment outside the Sleep contract: the only change of behaviour of the three profiles).
# NOT MEASURED against the owner's play; the scripted routes carry a spread of about 3 fps between identical runs.
MAX_CODEGEN = '-O3;-fomit-frame-pointer'
GTA4_MAX = {**RAGE_197, 'RCOMP_M6_GENERATED_OPT': MAX_CODEGEN}
GTA4_MAXCAP = {**GTA4_MAX, 'RCOMP_M6_TUNING_ENV': RAGE_197['RCOMP_M6_TUNING_ENV'] + ',RCOMP_SLEEP_CAP_NS=250000'}
EFLC_MAX = {**RAGE, 'RCOMP_M6_GENERATED_OPT': MAX_CODEGEN}

PROFILES = {
    'generic': {
        'label': 'Generic (R-comp defaults, diagnostics off)',
        'name': '',
        # Console layout of the physical windows (0xE0000000 shift): what XDK Direct3D and Unreal Engine 3 expect
        # (Gears of War 2). The tuned GTA IV / EFLC profiles keep the layout their performance was measured with.
        'options': {**PLAYER, 'RCOMP_M6_PHYSICAL_4K_WINDOW_OFFSET': 'ON'},
        'generator': {},
        'exploratory': False,
        'measured': 'no title-specific tuning',
    },
    'gta4': {
        'label': 'Grand Theft Auto IV (tuned 4K)',
        'name': 'Grand Theft Auto IV',
        'options': {**PLAYER, **RAGE_197},
        'generator': RAGE_GENERATOR,
        'exploratory': True,
        'measured': '54.18 fps average at 3840x2160 over the final 90 s of city free play at the heaviest spot (arm 197, '
                    '4 October 2026; 54.69 on the scripted run, phase 173); constant 60 fps not reached in the city',
    },
    'gta4max': {
        'label': 'Grand Theft Auto IV (max test build)',
        'name': 'Grand Theft Auto IV (max)',
        'options': {**PLAYER, **GTA4_MAX},
        'generator': RAGE_GENERATOR,
        'link_order': 'gta4_hot_order.txt',
        'exploratory': True,
        'measured': 'NOT MEASURED (test build of 4 October 2026): the arm-197 configuration plus -fomit-frame-pointer, the indirect-call cache and '
                    'the hot-function link order',
    },
    'gta4maxcap': {
        'label': 'Grand Theft Auto IV (max test build + short main-thread sleeps)',
        'name': 'Grand Theft Auto IV (max+cap)',
        'options': {**PLAYER, **GTA4_MAXCAP},
        'generator': RAGE_GENERATOR,
        'link_order': 'gta4_hot_order.txt',
        'exploratory': True,
        'measured': 'NOT MEASURED (test build of 4 October 2026): the max build plus RCOMP_SLEEP_CAP_NS=250000 (main-thread sleeps of about 1 ms '
                    'end after at most 0.25 ms)',
    },
    'eflcmax': {
        'label': 'Episodes from Liberty City (max test build)',
        'name': 'Grand Theft Auto: Episodes from Liberty City',
        'options': {**PLAYER, **EFLC_MAX, 'RCOMP_M6_CACHE_ROOT_PATH': '/app0/cache', 'RCOMP_M6_CACHE_READ_ONLY': 'ON'},
        'generator': RAGE_GENERATOR,
        'link_order': 'eflc_hot_order.txt',
        'exploratory': True,
        'measured': 'NOT MEASURED (test build of 4 October 2026): the previous EFLC configuration (guest threads on CPUs 0-7) plus -fomit-frame-pointer, '
                    'the indirect-call cache and the hot-function link order',
    },
    'eflc': {
        'label': 'Episodes from Liberty City (tuned 4K)',
        'name': 'Grand Theft Auto: Episodes from Liberty City',
        # RAGE's HDD cache devices (cache:, cache1:) are required by EFLC; mounted read-only so the title streams
        # from the disc instead of installing archives in the background (which halved its frame rate).
        'options': {**PLAYER, **RAGE, 'RCOMP_M6_CACHE_ROOT_PATH': '/app0/cache', 'RCOMP_M6_CACHE_READ_ONLY': 'ON'},
        'generator': RAGE_GENERATOR,
        'exploratory': True,
        'measured': 'about 55-56 fps at 3840x2160 in The Ballad of Gay Tony (scripted 90 s windows, '
                    'TBoGT bring-up measurements 31-44, with this configuration; the GTA IV arm-197 placement '
                    'is NOT TESTED on this title and is not applied)',
    },
}

# Known discs: (Xbox title id, media id) -> profile. Both games share the title id 545407F2.
KNOWN = {
    ('545407F2', '4A53F9F6'): 'gta4',  # Grand Theft Auto IV (Europe, Asia)
    ('545407F2', '06759F9C'): 'eflc',  # Grand Theft Auto: Episodes from Liberty City
}


def detect(title_id, media_id, disc_dir=None):
    """(profile key, reason) for a disc; falls back on the disc layout for other regions of the two games."""
    key = KNOWN.get((title_id, media_id))
    if key:
        return key, f'known disc {title_id}/{media_id}'
    if title_id == '545407F2':
        if disc_dir is not None and (disc_dir / 'DLC2').is_dir():
            return 'eflc', f'title {title_id} with DLC1/DLC2 folders (Episodes from Liberty City layout), media {media_id} not measured'
        return 'gta4', f'title {title_id} (GTA IV engine), media {media_id} not measured'
    return 'generic', f'no tuned profile for title {title_id}'
