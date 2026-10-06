#!/usr/bin/env python3
"""Gate the 2026-09-29 Xenos bring-up instrumentation in a prepared build tree.

The copied GPU sources contain exploratory diagnostics absent from the pinned
patch series. This migration recognises those blocks exactly, preserves their
contents behind the R-comp instrumentation flags, and refuses unknown layouts.
It never touches a reference checkout or generated guest C++. Run without
--apply to check all transformations before changing any file.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MARKER = '// R-comp opt-in diagnostics adaptation v1.\n'
HEADER = MARKER + '#include <rcomp_xenos/diagnostics.h>\n'
CP = 'src/graphics/command_processor.cpp'
VKCP = 'src/graphics/vulkan/command_processor.cpp'
PIPELINE = 'src/graphics/vulkan/pipeline_cache.cpp'


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('expected one occurrence: ' + old[:90])
    return text.replace(old, new, 1)


def span(text, start, end):
    if text.count(start) != 1:
        raise ValueError('ambiguous instrumentation block: ' + start[:90])
    first = text.index(start)
    last = text.index(end, first)
    return text[first:last]


def guard(text, block, flag):
    return once(text, block, '#if ' + flag + '\n' + block + '#endif\n')


def scope_alias(text):
    block = span(text,
                 '#include <atomic>\n#include <chrono>\nextern "C" std::atomic<uint64_t> rcomp_prof[16];',
                 '}  // namespace\n') + '}  // namespace\n'
    if 'struct RcompProfScope {' not in block or 'rcomp_prof[slot + 1].fetch_add' not in block:
        raise ValueError('unknown timing-scope implementation')
    return once(text, block, 'using RcompProfScope = ::rcomp::xenos::ProfScope;\n')


def adapt_cp(text):
    counters = span(text, '  rcomp_cp_last_packet.store(packet,', '  const uint64_t rcomp_t0 =')
    text = guard(text, counters, 'RCOMP_XENOS_DIAGNOSTICS')
    timers = span(text, '  const uint64_t rcomp_t0 =', '  // Type-3 packet.\n')
    if 'rcomp_op_scope{(packet >> 8) & 0x7F, rcomp_t0};' not in timers:
        raise ValueError('unknown packet timing scope')
    text = guard(text, timers, 'RCOMP_XENOS_PROFILE_TIMINGS')
    skipped = span(text, '      {  // bring-up tracing: predicated packets that are skipped\n',
                   '      reader->AdvanceRead(count * sizeof(uint32_t));\n')
    text = guard(text, skipped, 'RCOMP_XENOS_DIAGNOSTICS')
    wait = span(text, '  rcomp_cp_wait[0] = wait_info;', '\n  bool matched = false;\n')
    text = guard(text, wait + '\n', 'RCOMP_XENOS_DIAGNOSTICS')
    text = guard(text, '    rcomp_cp_wait[4] = value;\n', 'RCOMP_XENOS_DIAGNOSTICS')
    text = guard(text, '  const auto rcomp_w0 = std::chrono::steady_clock::now();\n',
                 'RCOMP_XENOS_DIAGNOSTICS')
    event_time = span(text, '  {\n    const auto us = std::chrono::duration_cast<std::chrono::microseconds>',
                      '  uint32_t data_value;\n')
    text = guard(text, event_time, 'RCOMP_XENOS_DIAGNOSTICS')
    event_log = span(text, '  {  // bring-up tracing\n',
                     '  memory::store(memory_->TranslatePhysical(address), data_value);\n')
    text = guard(text, event_log, 'RCOMP_XENOS_DIAGNOSTICS')
    return HEADER + text


def adapt_vkcp(text):
    text = scope_alias(text)
    helpers = span(text, 'namespace {\nstd::atomic<uint64_t> g_rcomp_draw_reason[8];\n',
                   '\nbool VulkanCommandProcessor::IssueDraw(')
    text = once(text, helpers,
                '#if RCOMP_XENOS_DIAGNOSTICS\n' + helpers +
                '#else\nnamespace { inline void RcompDrawReason(unsigned) {} }\n#endif\n')
    signatures = span(text, '  if (false) {  // bring-up tracing disabled for performance runs\n',
                      '  RcompDrawReason(0);\n')
    text = guard(text, signatures, 'RCOMP_XENOS_DIAGNOSTICS')
    return HEADER + text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()
    if (ROOT / 'build').resolve() not in source.parents:
        parser.error('source must be a prepared directory below R-comp/build')
    manifest = json.loads((source / '.rcomp-source-manifest.json').read_text())
    pin = manifest['dependencies']['rexglue-sdk']['commit']
    if pin != 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518':
        parser.error('unsupported source pin')
    transforms = {CP: adapt_cp, VKCP: adapt_vkcp,
                  PIPELINE: lambda value: HEADER + scope_alias(value)}
    contents = {name: (source / name).read_text(encoding='utf-8') for name in transforms}
    already = [value.startswith(MARKER) for value in contents.values()]
    if all(already):
        print('PASS Xenos instrumentation already gated:', source)
        return
    if any(already):
        parser.error('partially migrated source; no files changed')
    # Compute and check the complete migration before the first write.
    results = {name: transform(contents[name]) for name, transform in transforms.items()}
    if not args.apply:
        print('PASS checked instrumentation migration; no files changed:', source)
        return
    evidence = {'version': 1, 'pin': pin, 'files': {}}
    for name, value in results.items():
        (source / name).write_text(value, encoding='utf-8', newline='\n')
        evidence['files'][name] = {
            'before_sha256': hashlib.sha256(contents[name].encode()).hexdigest(),
            'after_sha256': hashlib.sha256(value.encode()).hexdigest()}
    (source / '.rcomp-perf-adaptation.json').write_text(
        json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print('PASS gated Xenos instrumentation:', source)


if __name__ == '__main__':
    main()
