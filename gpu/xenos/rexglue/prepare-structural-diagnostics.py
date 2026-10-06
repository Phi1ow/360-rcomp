#!/usr/bin/env python3
"""Prepare opt-in diagnostics from the phase-9 adapted copy; never edit that copy."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('unexpected source layout: '+before[:120])
    return text.replace(before, after)


def gated(text):
    return '#if RCOMP_XENOS_RENDER_DIAGNOSTICS\n'+text+'\n#endif\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--helper-baseline', type=Path,
                        help='Archived phase-9 helper, after the production helper migrated')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = args.source.resolve()
    output = args.out.resolve()
    # Reference and shared SDK paths can only be read, never be output targets.
    output.relative_to((root/'build').resolve())
    if not output.name.startswith('xenos-') and not any(p.name.startswith('xenos-') for p in output.parents):
        raise ValueError('output must be in build/xenos-*')
    if output == source or source in output.parents or output in source.parents:
        raise ValueError('output and input must be separate trees')
    changes = {}
    helper_rel = 'gpu/xenos/rexglue/shim/rcomp_xenos/render_diagnostics.h'
    helper_baseline = args.helper_baseline or root/helper_rel
    old = helper_baseline.read_text()
    if hashlib.sha256(helper_baseline.read_bytes()).hexdigest() != 'a814a4c774beb49b179c2645d5d583b2dacadc6a61adeb8e12e3930a00c09e08':
        raise ValueError('phase-9 helper changed; review the new baseline first')
    new = once(old,
        'enum class RenderDiagnosticKind { Draw, Targets, Resolve, Transfer, Dump, SharedBarrier, Count };',
        '''enum class RenderDiagnosticKind {
  Draw, Targets, Resolve, Transfer, Dump, SharedBarrier, Clear, StencilClear,
  HostDepthStore, ResolveCopy, DumpDispatch, TransferDraw, Count
};''')
    start = new.index('  static constexpr uint32_t kStatesPerKind = 64;')
    end = new.index('    const uint32_t index = uint32_t(kind);', start)
    new = new[:start]+'''  static constexpr uint32_t kCapacity = 128;
  // These cvars require restart and are set before GPU startup. Avoid cvar
  // access and key construction outside the selected window.
  RenderDiagnosticGate()
      : enabled_(REXCVAR_GET(rcomp_render_diagnostics)),
        first_(uint64_t(std::max(REXCVAR_GET(rcomp_render_diag_start_frame), 0))),
        frames_(uint64_t(std::clamp(REXCVAR_GET(rcomp_render_diag_frames), 1, 32))) {}
  bool InWindow(uint64_t frame) const {
    return enabled_ && frame >= first_ && frame - first_ < frames_;
  }
  static constexpr uint32_t Limit(RenderDiagnosticKind kind) {
    return kind == RenderDiagnosticKind::Draw || kind == RenderDiagnosticKind::Transfer ? 128 : 64;
  }
  uint32_t sequence() const { return sequence_; }
  bool Accept(RenderDiagnosticKind kind, uint64_t frame, const RenderDiagnosticKey& key) {
    if (!InWindow(frame)) return false;
'''+new[end:]
    new = once(new,
        '"RCOMP-RENDER-DIAG begin frame=%llu frames=%llu exact_states_per_kind=%u\\n",\n                   (unsigned long long)first, (unsigned long long)frames, kStatesPerKind);',
        '"RCOMP-RENDER-DIAG begin frame=%llu frames=%llu structural_draw=128 transfer=128 other=64 max_lines=1805\\n",\n                   (unsigned long long)first_, (unsigned long long)frames_);')
    new = new.replace('counts_[index] == kStatesPerKind', 'counts_[index] == Limit(kind)')
    new = new.replace('index, kStatesPerKind);', 'index, Limit(kind));')
    new = once(new, '    keys_[index][counts_[index]++] = key;',
        '    keys_[index][counts_[index]++] = key;\n    ++sequence_;')
    new = new.replace('RenderDiagnosticKey, kStatesPerKind>', 'RenderDiagnosticKey, kCapacity>')
    new = once(new, '  bool announced_ = false;', '''  bool announced_ = false;
  uint32_t sequence_ = 0;
  const bool enabled_;
  const uint64_t first_;
  const uint64_t frames_;''')
    changes[helper_rel] = (old, new)

    rel = 'src/graphics/vulkan/command_processor.cpp'
    old = (source/rel).read_text()
    new = once(old,
        'const uint32_t indices[] = {0x2000, 0x2001, 0x2002, 0x2006,',
        'const uint32_t indices[] = {0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,')
    new = once(new, '''    key.Add64(vertex_shader->ucode_data_hash());
    key.Add64(pixel_shader ? pixel_shader->ucode_data_hash() : 0);
''', '''    // Group structural state, not individual shaders or DMA packet addresses.
    // Hashes/modifications in the log identify the first representative only.
    key.Add(vertex_shader_modification.vertex.user_clip_plane_count);
    key.Add(vertex_shader_modification.vertex.user_clip_plane_cull);
    key.Add(vertex_shader_modification.vertex.vertex_kill_and);
    key.Add(unsigned(vertex_shader_modification.vertex.host_vertex_shader_type));
    key.Add(unsigned(pixel_shader_modification.pixel.depth_stencil_mode));
    key.Add(unsigned(pixel_shader && pixel_shader->writes_depth()));
    key.Add(unsigned(pixel_shader && pixel_shader->implicit_early_z_write_allowed()));
''')
    new = once(new, '''    for (const auto& plane : system_constants_.user_clip_planes)
      for (float component : plane) key.AddFloat(component);''', '''    for (uint32_t plane = 0; plane < vertex_shader_modification.vertex.user_clip_plane_count; ++plane)
      for (float component : system_constants_.user_clip_planes[plane]) key.AddFloat(component);''')
    new = once(new, '''    key.Add64(vertex_shader_modification.value);
    key.Add64(pixel_shader_modification.value);
''', '')
    new = once(new, '      for (uint32_t plane = 0; plane < 6; ++plane) {',
        '      for (uint32_t plane = 0; plane < vertex_shader_modification.vertex.user_clip_plane_count; ++plane) {')
    # Explicit structural identity; DRAW and DEPTH retain their representative
    # shader hash so existing parsers remain compatible.
    new = once(new, 'RCOMP-RENDER-DRAW frame=%llu sub=%llu path=',
        'RCOMP-RENDER-DRAW frame=%llu sub=%llu structural=1 path=')
    changes[rel] = (old, new)

    rel = 'src/graphics/vulkan/render_target_cache.cpp'
    old = (source/rel).read_text()
    new = once(old, '''              key.Add(transfer_rectangle.x_pixels); key.Add(transfer_rectangle.y_pixels);''',
        '''              key.Add(it_merged->shader_key.key);
              key.Add(it_merged->transfer.host_depth_source ? it_merged->transfer.host_depth_source->key().key : 0);
              key.Add(msaa_2x_attachments_supported_);
              key.Add(transfer_rectangle.x_pixels); key.Add(transfer_rectangle.y_pixels);''')
    new = once(new, 'RCOMP-RENDER-TRANSFER frame=%llu sub=%llu dest=%08X source=%08X rect=',
        'RCOMP-RENDER-TRANSFER frame=%llu sub=%llu prepared=1 dest=%08X source=%08X shader=%08X mode=%u host_depth=%08X host_copy=%u native2x=%u rect=')
    # The cast is present on this statement in the adapted source.
    new = once(new,
        '(unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1],\n                    transfer_rectangle.x_pixels,',
        '''(unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1],
                    it_merged->shader_key.key, unsigned(it_merged->shader_key.mode), key.words[3],
                    unsigned(it_merged->transfer.host_depth_source == dest_rt), unsigned(msaa_2x_attachments_supported_),
                    transfer_rectangle.x_pixels,''')
    # Actual transfer draw emission, separate from rectangle preparation. A
    # missing pipeline emits available=0 before the existing continue.
    anchor = '''        if (!transfer_pipelines) {
          continue;
        }'''
    block = '''        {
          using namespace ::rcomp::xenos;
          if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
            RenderDiagnosticKey key;
            key.Add(dest_rt_key.key); key.Add(source_vulkan_rt.key().key);
            key.Add(transfer_shader_key.key);
            key.Add(host_depth_source_vulkan_rt ? host_depth_source_vulkan_rt->key().key : 0);
            key.Add(transfer_vertex_count); key.Add(transfer_sample_pipeline_count);
            key.Add(transfer_is_stencil_bit); key.Add(unsigned(transfer_pipelines != nullptr));
            if (RenderDiagnostics().Accept(RenderDiagnosticKind::TransferDraw, command_processor_.GetCurrentFrame(), key))
              fprintf(stderr, "RCOMP-RENDER-TRANSFER-DRAW frame=%llu sub=%llu dest=%08X source=%08X shader=%08X host_depth=%08X vertices=%u sample_pipelines=%u stencil_bits=%u pipeline_available=%u\\n",
                  (unsigned long long)command_processor_.GetCurrentFrame(),
                  (unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1],
                  key.words[2], key.words[3], transfer_vertex_count, transfer_sample_pipeline_count,
                  transfer_is_stencil_bit ? 8 : 0, unsigned(transfer_pipelines != nullptr));
          }
        }'''
    new = once(new, anchor, gated(block)+anchor)
    # Host depth scratch copy uses the actual computed constants and groups.
    anchor = '''        command_processor_.SubmitBarriers(true);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();'''
    block = '''        {
          using namespace ::rcomp::xenos;
          if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
            const auto& rect = transfer_rectangles[j];
            RenderDiagnosticKey key;
            key.Add(dest_rt_key.key); key.Add(transfer.source ? transfer.source->key().key : 0);
            key.Add(host_depth_store_rectangle_constant.constant);
            key.Add(rect.x_pixels); key.Add(rect.y_pixels); key.Add(rect.width_pixels); key.Add(rect.height_pixels);
            key.Add(group_count_x); key.Add(group_count_y); key.Add(msaa_2x_attachments_supported_);
            if (RenderDiagnostics().Accept(RenderDiagnosticKind::HostDepthStore, command_processor_.GetCurrentFrame(), key))
              fprintf(stderr, "RCOMP-RENDER-HOST-DEPTH-STORE frame=%llu sub=%llu rt=%08X owner=%08X rect=%u,%u+%u,%u rectangle_constant=%08X groups=%u,%u native2x=%u pitch=%u scale=%u,%u\\n",
                  (unsigned long long)command_processor_.GetCurrentFrame(),
                  (unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1],
                  rect.x_pixels, rect.y_pixels, rect.width_pixels, rect.height_pixels,
                  host_depth_store_rectangle_constant.constant, group_count_x, group_count_y,
                  unsigned(msaa_2x_attachments_supported_), dest_rt_key.pitch_tiles_at_32bpp,
                  draw_resolution_scale_x(), draw_resolution_scale_y());
          }
        }'''
    new = once(new, anchor, gated(block)+anchor)
    # Observe the actual Vk clear values, including raw bits for uint formats.
    anchor = '      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, 1, &resolve_clear_rect);'
    block = '''      {
        using namespace ::rcomp::xenos;
        if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
          uint32_t bits[4];
          std::memcpy(bits, &resolve_clear_attachment.clearValue, sizeof(bits));
          RenderDiagnosticKey key;
          key.Add(dest_rt_key.key); key.Add(resolve_clear_attachment.aspectMask); key.Add64(clear_value);
          key.Add(resolve_clear_rect.rect.offset.x); key.Add(resolve_clear_rect.rect.offset.y);
          key.Add(resolve_clear_rect.rect.extent.width); key.Add(resolve_clear_rect.rect.extent.height);
          for (uint32_t word : bits) key.Add(word);
          if (RenderDiagnostics().Accept(RenderDiagnosticKind::Clear, command_processor_.GetCurrentFrame(), key))
            fprintf(stderr, "RCOMP-RENDER-CLEAR frame=%llu sub=%llu rt=%08X aspects=%u guest=%016llX rect=%d,%d+%u,%u vkbits=%08X,%08X,%08X,%08X framebuffer=%u,%u\\n",
                (unsigned long long)command_processor_.GetCurrentFrame(),
                (unsigned long long)command_processor_.GetCurrentSubmission(), dest_rt_key.key,
                resolve_clear_attachment.aspectMask, (unsigned long long)clear_value,
                resolve_clear_rect.rect.offset.x, resolve_clear_rect.rect.offset.y,
                resolve_clear_rect.rect.extent.width, resolve_clear_rect.rect.extent.height,
                bits[0], bits[1], bits[2], bits[3], transfer_framebuffer->host_extent.width,
                transfer_framebuffer->host_extent.height);
        }
      }'''
    new = once(new, anchor, gated(block)+anchor)
    # Stencil is cleared separately when stencil-export isn't available.
    anchor = '            ++stencil_clear_rect_write_ptr;'
    block = '''            {
              using namespace ::rcomp::xenos;
              if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
                const auto& rect = stencil_clear_rect_write_ptr->rect;
                RenderDiagnosticKey key;
                key.Add(dest_rt_key.key); key.Add(transfer.source ? transfer.source->key().key : 0);
                key.Add(rect.offset.x); key.Add(rect.offset.y); key.Add(rect.extent.width); key.Add(rect.extent.height);
                if (RenderDiagnostics().Accept(RenderDiagnosticKind::StencilClear, command_processor_.GetCurrentFrame(), key))
                  fprintf(stderr, "RCOMP-RENDER-STENCIL-CLEAR frame=%llu sub=%llu rt=%08X source=%08X rect=%d,%d+%u,%u value=0 export=%u\\n",
                      (unsigned long long)command_processor_.GetCurrentFrame(),
                      (unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1],
                      rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height,
                      unsigned(vulkan_device->extensions().ext_EXT_shader_stencil_export));
              }
            }'''
    new = once(new, anchor, gated(block)+anchor)
    # Actual resolve dispatch, after the push constants destination base has
    # been made relative to the descriptor. direct_resolved tells the route
    # that supplied EDRAM; it doesn't mean bypassing this resolve dispatch.
    anchor = '          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y, 1);'
    block = '''          {
            using namespace ::rcomp::xenos;
            if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
              RenderDiagnosticKey key;
              key.Add(resolve_info.copy_dest_base); key.Add(resolve_info.copy_dest_extent_start);
              key.Add(resolve_info.copy_dest_extent_length); key.Add(unsigned(copy_shader));
              key.Add(direct_resolved); key.Add(copy_group_count_x); key.Add(copy_group_count_y);
              key.Add(copy_shader_constants.dest_relative.edram_info.packed);
              key.Add(copy_shader_constants.dest_relative.coordinate_info.packed);
              key.Add(copy_shader_constants.dest_relative.dest_info.value);
              key.Add(copy_shader_constants.dest_relative.dest_coordinate_info.packed);
              key.Add(copy_shader_constants.dest_base);
              if (RenderDiagnostics().Accept(RenderDiagnosticKind::ResolveCopy, command_processor_.GetCurrentFrame(), key))
                fprintf(stderr, "RCOMP-RENDER-RESOLVE-COPY frame=%llu sub=%llu dest=%08X extent=%08X+%08X shader=%u direct_edram=%u groups=%u,%u constants=%08X,%08X,%08X,%08X,%08X descriptor=%llu+%llu use=%llu+%llu scaled=%u\\n",
                    (unsigned long long)command_processor_.GetCurrentFrame(),
                    (unsigned long long)command_processor_.GetCurrentSubmission(), key.words[0], key.words[1], key.words[2],
                    unsigned(copy_shader), unsigned(direct_resolved), copy_group_count_x, copy_group_count_y,
                    key.words[7], key.words[8], key.words[9], key.words[10], key.words[11],
                    (unsigned long long)copy_dest_base, (unsigned long long)copy_dest_range_length,
                    (unsigned long long)copy_dest_use_start, (unsigned long long)copy_dest_use_length,
                    unsigned(draw_resolution_scaled));
            }
          }'''
    new = once(new, anchor, gated(block)+anchor)
    # Dispatch rectangles expose the ownership partition behind a DUMP span.
    anchor = '      offsets.dispatch_first_tile = dump_base + dispatch.offset;'
    block = '''      {
        using namespace ::rcomp::xenos;
        if (RenderDiagnostics().InWindow(command_processor_.GetCurrentFrame())) {
          RenderDiagnosticKey key;
          key.Add(rt_key.key); key.Add(dump_base); key.Add(dispatch.offset);
          key.Add(dispatch.width_tiles); key.Add(dispatch.height_tiles); key.Add(dump_pitch);
          if (RenderDiagnostics().Accept(RenderDiagnosticKind::DumpDispatch, command_processor_.GetCurrentFrame(), key))
            fprintf(stderr, "RCOMP-RENDER-DUMP-DISPATCH frame=%llu sub=%llu rt=%08X dest_first_tile=%u source_base=%u width_tiles=%u height_tiles=%u source_pitch=%u dest_pitch=%u\\n",
                (unsigned long long)command_processor_.GetCurrentFrame(),
                (unsigned long long)command_processor_.GetCurrentSubmission(), rt_key.key,
                dump_base + dispatch.offset, rt_key.base_tiles, dispatch.width_tiles, dispatch.height_tiles,
                rt_key.GetPitchTiles(), dump_pitch);
        }
      }'''
    new = once(new, anchor, gated(block)+anchor)
    changes[rel] = (old, new)

    rel = 'src/graphics/vulkan/shared_memory.cpp'
    old = (source/rel).read_text()
    changes[rel] = (old, old)
    # Gate existing blocks before building keys. New blocks already gate within
    # their lexical scope, and can cheaply receive a second outer gate.
    for rel in ('src/graphics/vulkan/command_processor.cpp', 'src/graphics/vulkan/render_target_cache.cpp', 'src/graphics/vulkan/shared_memory.cpp'):
        old, new = changes[rel]
        frame = 'GetCurrentFrame()' if rel.endswith('/command_processor.cpp') else 'command_processor_.GetCurrentFrame()'
        new = re.sub(r'#if RCOMP_XENOS_RENDER_DIAGNOSTICS\n([ \t]*)\{\n',
                     lambda match: '#if RCOMP_XENOS_RENDER_DIAGNOSTICS\n'+match[1]+'if (::rcomp::xenos::RenderDiagnostics().InWindow('+frame+')) {\n', new)
        changes[rel] = (old, new)

    # Retire the CP sampler on every normal/early return before pthread TLS.
    rel = 'src/graphics/command_processor.cpp'
    old = (source/rel).read_text()
    new = once(old, 'extern "C" void rcomp_profile_register_host_thread() __attribute__((weak));',
        'extern "C" void rcomp_profile_register_host_thread() __attribute__((weak));\nextern "C" void rcomp_profile_unregister_host_thread() __attribute__((weak));')
    new = once(new, '  if (rcomp_profile_register_host_thread) rcomp_profile_register_host_thread();  // bring-up profiling',
        '''  if (rcomp_profile_register_host_thread) rcomp_profile_register_host_thread();  // bring-up profiling
  struct ProfileRetirement {
    ~ProfileRetirement() {
      if (rcomp_profile_unregister_host_thread) rcomp_profile_unregister_host_thread();
    }
  } profile_retirement;''')
    changes[rel] = (old, new)

    patches = {'structural-render-diagnostics.patch': '', 'structural-render-helper.patch': '', 'profile-retirement-cp.patch': ''}
    evidence = {}
    for rel, (old, new) in changes.items():
        target = output/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(new)
        patch_name = ('structural-render-helper.patch' if rel.startswith('gpu/') else
                      'profile-retirement-cp.patch' if rel == 'src/graphics/command_processor.cpp' else
                      'structural-render-diagnostics.patch')
        patches[patch_name] += ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), 'a/'+rel, 'b/'+rel))
        evidence[rel] = {'before_sha256': hashlib.sha256(old.encode()).hexdigest(),
                         'after_sha256': hashlib.sha256(new.encode()).hexdigest()}
    patch_dir = root/'gpu/xenos/rexglue/render-patches'
    for name, content in patches.items():
        (patch_dir/name).write_text(content)
    (output/'preparation.json').write_text(json.dumps({'status': 'PASS', 'source': str(source), 'files': evidence}, indent=2)+'\n')
    print('PASS structural diagnostics prepared; production source/helper untouched; max 1805 diagnostic lines')


if __name__ == '__main__':
    main()
