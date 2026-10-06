// TESTDOUBLE rendering backend for the Xenos command processor (tests only):
// records what the command processor asks the backend to do instead of
// rendering it. Never linked into a title.
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>

struct TESTDOUBLE_Draw {
    rex::graphics::xenos::PrimitiveType prim;
    uint32_t index_count;
    bool indexed;
    uint32_t index_base;
    uint32_t index_count_in_buffer;
};
struct TESTDOUBLE_Swap {
    uint32_t frontbuffer, width, height;
};

class TESTDOUBLE_RecordingBackend final : public rex::graphics::CommandProcessor {
public:
    explicit TESTDOUBLE_RecordingBackend(rex::graphics::GpuHost* host) : CommandProcessor(host) {}
    std::mutex mu;
    std::vector<TESTDOUBLE_Draw> draws;
    std::vector<TESTDOUBLE_Swap> swaps;
    std::vector<std::unique_ptr<rex::graphics::Shader>> shaders;
    uint32_t copies = 0;
    bool context = false;

    void IssueSwap(uint32_t fb, uint32_t w, uint32_t h) override {
        std::lock_guard<std::mutex> lk(mu);
        swaps.push_back({fb, w, h});
    }

protected:
    bool SetupContext() override { return context = true; }
    void ShutdownContext() override { context = false; }
    rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType type, uint32_t guest_address,
                                      const uint32_t* host_address, uint32_t dword_count) override {
        std::lock_guard<std::mutex> lk(mu);
        shaders.push_back(std::make_unique<rex::graphics::Shader>(type, guest_address, host_address, dword_count));
        return shaders.back().get();
    }
    bool IssueDraw(rex::graphics::xenos::PrimitiveType prim, uint32_t index_count, IndexBufferInfo* ib,
                   bool) override {
        std::lock_guard<std::mutex> lk(mu);
        draws.push_back({prim, index_count, ib != nullptr, ib ? ib->guest_base : 0, ib ? ib->count : 0});
        return true;
    }
    bool IssueCopy() override {
        std::lock_guard<std::mutex> lk(mu);
        ++copies;
        return true;
    }
};
