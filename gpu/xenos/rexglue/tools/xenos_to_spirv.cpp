// Xenos microcode -> SPIR-V through rexglue's (Xenia's) SpirvShaderTranslator,
// the same calls its Vulkan pipeline cache makes (Shader, AnalyzeUcode,
// GetOrCreateTranslation, TranslateAnalyzedShader).
//
//   xenos_to_spirv vs|ps <input> <out.spv> [--container] [--all-features] [--num-reg N]
// <input> is big-endian microcode, or with --container an .xso container
// (gpu/xenos/tools/synth_xenos.py layout: header, virtual part, microcode).
// Default features are the conservative set (Features(false)), closest to
// what PS5_Vulkan exposes. Exit 0 and a SPIR-V module on success.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s vs|ps <input> <out.spv> [--container] [--all-features] [--num-reg N]\n", argv[0]);
        return 2;
    }
    const bool vertex = !strcmp(argv[1], "vs");
    bool container = false, all = false;
    uint32_t num_reg = 4;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--container")) container = true;
        else if (!strcmp(argv[i], "--all-features")) all = true;
        else if (!strcmp(argv[i], "--num-reg") && i + 1 < argc) num_reg = uint32_t(atoi(argv[++i]));
    }
    FILE* f = fopen(argv[2], "rb");
    if (!f) return 2;
    std::vector<uint8_t> in;
    uint8_t b[4096];
    for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) in.insert(in.end(), b, b + n);
    fclose(f);
    size_t off = 0, len = in.size();
    if (container) {
        if (in.size() < 12) return 2;
        off = be32(in.data() + 4);   // virtual part size
        len = be32(in.data() + 8);   // physical part (microcode) size
        if (off + len > in.size()) {
            fprintf(stderr, "container sizes out of range\n");
            return 2;
        }
    }
    if (len % 4) return 2;
    std::vector<uint32_t> ucode(len / 4);
    memcpy(ucode.data(), in.data() + off, len);

    const auto type = vertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel;
    Shader shader(type, /*hash*/ 0x52434F4D50ull, ucode.data(), ucode.size(), std::endian::big);
    rex::string::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    SpirvShaderTranslator translator(SpirvShaderTranslator::Features(all), false, false, false);
    const uint32_t dyn = shader.GetDynamicAddressableRegisterCount(num_reg);
    const uint64_t mod = vertex ? translator.GetDefaultVertexShaderModification(dyn)
                                : translator.GetDefaultPixelShaderModification(dyn);
    Shader::Translation* t = shader.GetOrCreateTranslation(mod);
    if (!translator.TranslateAnalyzedShader(*t) || !t->is_valid()) {
        fprintf(stderr, "translation failed\n");
        return 1;
    }
    const auto& spv = t->translated_binary();
    FILE* o = fopen(argv[3], "wb");
    if (!o || fwrite(spv.data(), 1, spv.size(), o) != spv.size() || fclose(o) != 0) return 2;
    printf("%s: %zu ucode dwords -> %zu bytes of SPIR-V\n", argv[2], ucode.size(), spv.size());
    return 0;
}
