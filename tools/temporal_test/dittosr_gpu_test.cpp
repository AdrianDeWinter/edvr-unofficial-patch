#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/dittosr_engine.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
std::vector<std::string> lines;

void check(bool ok, const char* message) {
    if (ok) return;
    std::printf("FAIL: %s\n", message);
    ++failures;
}

auto countNotes(const char* text) {
    return std::count_if(lines.begin(), lines.end(), [text](const std::string& line) {
        return line.find(text) != std::string::npos;
    });
}

}  // namespace

namespace edvr {

Config& Config::get() { static Config config; return config; }
float Config::getFloat(const char*, float fallback) const { return fallback; }
std::string Config::getString(const char*, const char* fallback) const { return fallback; }

Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char* format, ...) {
    char line[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    lines.emplace_back(line);
}

}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("dittosr_gpu_test: would run the production compute passes and an allocation refusal on WARP.");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: dittosr_gpu_test --self-test|--dry-run");
        return 2;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const auto create = edvr::systemD3D11CreateDevice();
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        std::puts("FAIL: create a WARP device");
        return 1;
    }

    // Use initialized, disjoint input and output resources. The two input views let
    // the second frame use a different depth resource, as in the VR and flat callers.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    float samples[16 * 16 * 4]{};
    D3D11_SUBRESOURCE_DATA input{samples, 16 * 4 * sizeof(float), 0};
    ComPtr<ID3D11ShaderResourceView> inputs[2];
    for (auto& view : inputs) {
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &input, texture.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()))) {
            std::puts("FAIL: create the input texture and view");
            return 1;
        }
    }
    desc.Width = desc.Height = 32;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, output.GetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(output.Get(), nullptr, outputView.GetAddressOf()))) {
        std::puts("FAIL: create the output texture and view");
        return 1;
    }

    check(!edvr::dittoAvailable(nullptr, nullptr) && !edvr::dittoAvailable(nullptr, nullptr) &&
              countNotes("DittoSR: unavailable") == 1,
          "preflight logs the first device refusal even without a reason output");
    const char* why = nullptr;
    const auto run = [&](uint32_t width, ID3D11ShaderResourceView* depth) {
        return edvr::dittoEvaluate(context.Get(), 0, "flat",
                                   {inputs[0].Get(), depth, inputs[0].Get(), nullptr, outputView.Get()},
                                   16, 16, width, 32, 0, 0, false, &why, false);
    };
    check(run(32, inputs[0].Get()) && run(32, inputs[1].Get()) &&
              countNotes("flat evaluated 16x16 -> 32x32") == 1,
          "DittoSR accepts alternating caller depth views and logs its first evaluation once");

    // The render-size textures fit. The first output-size history texture is rejected before dispatch.
    constexpr uint32_t oversized = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1;
    check(!run(oversized, inputs[0].Get()) && why && std::strstr(why, "history textures") &&
              !run(oversized, inputs[1].Get()) && why && std::strstr(why, "history textures") &&
              countNotes("history allocation failed") == 1 && countNotes("history ready") == 0,
          "a failed resize logs one refusal and stays latched at that size");
    check(run(32, inputs[0].Get()) && !why && countNotes("history ready after size change") == 1 &&
              countNotes("16385x32 becomes 16x16 -> 32x32") == 1,
          "the next valid size recovers and logs the successful rebuild with both sizes");

    edvr::dittoShutdown();
    if (failures) return 1;
    std::puts("DITTOSR GPU TEST PASSED");
    return 0;
}
