#!/usr/bin/env python3
"""Emit SYNTHETIC Xenos shader containers authored from scratch by R-comp.

Purpose: exercise XenosRecomp's parser/translator end to end on this host
without any game content. Every byte written here is derived from the field
layout XenosRecomp itself declares (XenosRecomp/shader.h, constant_table.h,
shader_code.h @990d03b2) -- NOT from any Xbox 360 shader binary, SDK or game.

These are NOT Xbox 360 shaders and prove nothing about real titles: they are
parser fixtures. The real corpus stays BLOCKED (no authorized content).

Usage: synth_xenos.py OUT_DIR        -> writes SYNTH_*.xso files
Layout reminder (all words big-endian, bit-fields are LSB-first per 32-bit
unit, which is how clang/gcc lay out XenosRecomp's structs on x86-64):
  [virtual part]  ShaderContainer header | constant table | Shader struct
  [physical part] microcode (control-flow triples, then 96-bit instructions)
"""
import struct
import sys
from pathlib import Path

MAGIC = 0x102A1100          # (flags & 0xFFFFFF00) == 0x102A1100, main.cpp
FLAG_VERTEX = 0x1           # isPixelShader = (flags & 1) == 0, shader_recompiler.cpp

# RegisterSet (constant_table.h)
RS_BOOL, RS_INT4, RS_FLOAT4, RS_SAMPLER = 0, 1, 2, 3
# DeclUsage (shader.h)
U_POSITION, U_TEXCOORD = 0, 5
# ControlFlowOpcode / FetchOpcode / Alu opcodes (shader_code.h)
CF_NOP, CF_EXEC, CF_EXEC_END, CF_COND_JMP = 0, 1, 2, 11
FO_VFETCH, FO_TFETCH = 0, 1
VO_MUL, VO_DP4 = 1, 15
SO_RETAIN_PREV = 50
EXP_VS_POSITION = 62
TEX_2D = 1
VF_32_32_32_32_FLOAT = 38  # VertexFormat (Xenos)
SWZ_XYZW = 0 | (1 << 3) | (2 << 6) | (3 << 9)   # fetch dst swizzle, 3 bits/comp


def bits(*fields):
    """fields: (value, width) LSB first -> one 32-bit word."""
    word, pos = 0, 0
    for value, width in fields:
        assert 0 <= value < (1 << width), (value, width)
        word |= value << pos
        pos += width
    assert pos <= 32
    return word


def cf_exec(opcode, address, count, sequence):
    lo = bits((address, 12), (count, 3), (0, 1), (sequence, 12), (0, 4))
    hi = bits((0, 2), (0, 7), (0, 1), (0, 1), (0, 1), (opcode, 4))
    return lo, hi


def cf_cond_jmp(address, bool_address, condition):
    lo = bits((address, 13), (0, 1), (0, 1), (0, 17))
    hi = bits((0, 1), (0, 1), (bool_address, 8), (condition, 1), (0, 1), (CF_COND_JMP, 4))
    return lo, hi


def cf_nop():
    return 0, bits((0, 12), (CF_NOP, 4))


def cf_pair(a, b):
    """Two 48-bit CF instructions -> three 32-bit words (shader_recompiler.cpp
    reads code1 = w1 & 0xFFFF, code2 = (w1 >> 16) | (w2 << 16), code3 = w2 >> 16)."""
    (a0, a1), (b0, b1) = a, b
    assert a1 < 0x10000 and b1 < 0x10000
    return [a0, a1 | ((b0 & 0xFFFF) << 16), (b0 >> 16) | (b1 << 16)]


def vfetch(dst, src, const_index):
    w0 = bits((FO_VFETCH, 5), (src, 6), (0, 1), (dst, 6), (0, 1), (1, 1),
              (const_index, 5), (0, 2), (0, 3), (0, 2))
    # format 38 = k_32_32_32_32_FLOAT (a float4 position; format 0 is not a
    # valid Xenos vertex format and Xenia's translator rejects it).
    w1 = bits((SWZ_XYZW, 12), (0, 1), (0, 1), (0, 1), (0, 1), (VF_32_32_32_32_FLOAT, 6), (0, 2), (0, 6), (0, 1), (0, 1))
    w2 = bits((16, 8), (0, 23), (0, 1))
    return [w0, w1, w2]


def tfetch2d(dst, src, const_index):
    src_swz = 0 | (1 << 2)  # .xy
    w0 = bits((FO_TFETCH, 5), (src, 6), (0, 1), (dst, 6), (0, 1), (0, 1),
              (const_index, 5), (0, 1), (src_swz, 6))
    w1 = bits((SWZ_XYZW, 12), (1, 2), (1, 2), (0, 2), (0, 3), (0, 3), (0, 2), (0, 2),
              (0, 1), (0, 1), (0, 1), (0, 1))
    w2 = bits((0, 1), (0, 1), (0, 7), (0, 5), (TEX_2D, 2), (0, 5), (0, 5), (0, 5), (0, 1))
    return [w0, w1, w2]


def alu(vop, vdst, wmask, src1, src2, export=False):
    """src = (register, is_temp). Scalar half is RetainPrev (no-op)."""
    w0 = bits((vdst, 6), (0, 1), (0, 1), (0, 6), (0, 1), (1 if export else 0, 1),
              (wmask, 4), (0, 4), (0, 1), (0, 1), (SO_RETAIN_PREV, 6))
    w1 = bits((0, 8), (0, 8), (0, 8), (0, 1), (0, 1), (0, 1), (0, 1), (0, 1),
              (0, 1), (0, 1), (0, 1))
    w2 = bits((0, 8), (src2[0], 8), (src1[0], 8), (vop, 5), (0, 1),
              (1 if src2[1] else 0, 1), (1 if src1[1] else 0, 1))
    return [w0, w1, w2]


def be_words(words):
    return b"".join(struct.pack(">I", w) for w in words)


def constant_table(constants):
    """constants: list of (name, register_set, register_index, register_count).
    Returns ConstantTableContainer bytes (size word + D3DXSHADER_CONSTANTTABLE +
    infos + names); offsets are relative to the ConstantTable start."""
    header_size = 28
    info_off = header_size
    names_off = info_off + 20 * len(constants)
    names = b""
    infos = b""
    for name, rset, rindex, rcount in constants:
        noff = names_off + len(names)
        names += name.encode() + b"\0"
        infos += struct.pack(">IHHHHII", noff, rset, rindex, rcount, 0, 0, 0)
    body_len = names_off + len(names)
    body_len = (body_len + 3) & ~3
    table = struct.pack(">IIIIIII", header_size, 0, 0, len(constants), info_off, 0, 0)
    body = (table + infos + names).ljust(body_len, b"\0")
    return struct.pack(">I", len(body)) + body


def container(is_vertex, constants, shader_struct_words, code_words):
    head_size = 0x24
    ctab = constant_table(constants)
    ctab_off = head_size
    shader_off = ctab_off + len(ctab)
    shader_bytes = be_words(shader_struct_words)
    virtual_size = shader_off + len(shader_bytes)
    physical = be_words(code_words)
    flags = MAGIC | (FLAG_VERTEX if is_vertex else 0)
    header = struct.pack(">IIIIIIIII", flags, virtual_size, len(physical), 0,
                         ctab_off, 0, shader_off, 0, 0)
    return header + ctab + shader_bytes + physical


def pixel_shader_textured():
    # r0 = TEXCOORD0; r1 = tfetch2D(g_Tex0, r0.xy); oC0 = r1 * g_Tint
    code = cf_pair(cf_exec(CF_EXEC_END, 1, 2, 0b0001), cf_nop())
    code += tfetch2d(dst=1, src=0, const_index=0)
    code += alu(VO_MUL, 0, 0xF, (1, True), (0, False), export=True)
    interp_texcoord0_r0 = 0 | (U_TEXCOORD << 4) | (0 << 8)
    shader = [0, len(code) * 4, 0, 0xFF00, 0, 1 << 5,   # Shader
              0, 0x1,                                   # field18, outputs=COLOR0
              interp_texcoord0_r0]
    consts = [("g_Tint", RS_FLOAT4, 0, 1), ("g_Tex0", RS_SAMPLER, 0, 1)]
    return container(False, consts, shader, code)


def pixel_shader_bool_branch():
    # if (b0) oC0 = g_A * g_A;  oC0 = g_B * g_B (end)
    code = cf_pair(cf_cond_jmp(2, 0, 1), cf_exec(CF_EXEC, 2, 1, 0))
    code += cf_pair(cf_exec(CF_EXEC_END, 3, 1, 0), cf_nop())
    code += alu(VO_MUL, 0, 0xF, (0, False), (0, False), export=True)
    code += alu(VO_MUL, 0, 0xF, (1, False), (1, False), export=True)
    shader = [0, len(code) * 4, 0, 0xFF00, 0, 0, 0, 0x1]
    consts = [("g_A", RS_FLOAT4, 0, 1), ("g_B", RS_FLOAT4, 1, 1), ("g_UseA", RS_BOOL, 0, 1)]
    return container(False, consts, shader, code)


def vertex_shader_transform():
    # r0 = vfetch POSITION0; oPos.{x,y,z,w} = dot(r0, g_Mtx[i]); oTexCoord0 = r0 * g_Scale
    code = cf_pair(cf_exec(CF_EXEC_END, 1, 6, 0b000000000001), cf_nop())
    code += vfetch(dst=0, src=0, const_index=0)
    for i in range(4):
        code += alu(VO_DP4, EXP_VS_POSITION, 1 << i, (0, True), (4 + i, False), export=True)
    code += alu(VO_MUL, 0, 0xF, (0, True), (8, False), export=True)
    vertex_element_pos0_at_1 = 1 | (U_POSITION << 12) | (0 << 16)
    interp_texcoord0 = 0 | (U_TEXCOORD << 4)
    shader = [0, len(code) * 4, 0, 0, 0, 1 << 5,        # Shader
              0, 1, 0,                                   # field18, vertexElementCount, field20
              vertex_element_pos0_at_1, interp_texcoord0]
    consts = [("g_Mtx", RS_FLOAT4, 4, 4), ("g_Scale", RS_FLOAT4, 8, 1)]
    return container(True, consts, shader, code)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, blob in (("SYNTH_ps_textured.xso", pixel_shader_textured()),
                       ("SYNTH_ps_bool_branch.xso", pixel_shader_bool_branch()),
                       ("SYNTH_vs_transform.xso", vertex_shader_transform())):
        (out / name).write_bytes(blob)
        print(f"{name} {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
