#pragma once
#include "ReplayGeometry.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstring>

namespace gvl::streaming::alvr {
// Explicit diagnostic only. Readback and disk I/O invalidate live timing measurements.
// With no environment opt-in, Capture performs no texture work or filesystem access.
class EncoderReferenceCapture {
    std::filesystem::path root, output;
    std::string group;
    bool enabled = false, armed = false, failed = false;
    uint32_t count = 0, eligible = 0;
    ULONGLONG nextPoll = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC staged{};
    static constexpr uint32_t MaxFrames = 32, FrameStride = 18;
    static void Write(const std::filesystem::path& path, const void* data, size_t size) {
        if (std::filesystem::exists(path)) throw std::runtime_error("Reference output already exists");
        std::ofstream f(path, std::ios::binary); f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f) throw std::runtime_error("Reference output write failed");
    }
public:
    EncoderReferenceCapture() noexcept {
        wchar_t path[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"GVL_ALVR_REFERENCE_CAPTURE", path, 32768);
        if (!n || n >= 32768) return;
        try { root = path; enabled = root.is_absolute(); } catch (...) {}
    }
    void Capture(ID3D11Texture2D* texture, ID3D11DeviceContext* context, uint64_t timestamp,
                 const Packet& packet, const Geometry& geometry) noexcept {
        if (!enabled || failed || !texture || !context) return;
        try {
            const ULONGLONG now = GetTickCount64();
            if (now >= nextPoll) {
                nextPoll = now + 500;
                std::ifstream request(root / "capture.arm"); std::string token; request >> token;
                armed = token == "development" || token == "holdout";
                if (!armed) return;
                if (token != group) {
                    output = root / token;
                    if (!std::filesystem::create_directory(output)) throw std::runtime_error("Reference group already exists");
                    group = token; count = eligible = 0;
                }
            }
            if (!armed || group.empty() || count >= MaxFrames) return;
            // This is a predeclared task-region diagnostic, so collect only views
            // in which all three tested semantic categories are visible in both eyes.
            uint32_t categoryEyes[3]{};
            for (uint32_t i = 0; i < packet.count; ++i) {
                const uint32_t tag = packet.regions[i].kind & 255u;
                const int category = tag == 1 ? 0 : tag == 2 ? 1 : tag == 4 ? 2 : -1;
                if (category >= 0) categoryEyes[category] |= 1u << packet.regions[i].eye;
            }
            if (categoryEyes[0] != 3 || categoryEyes[1] != 3 || categoryEyes[2] != 3
                || (++eligible % FrameStride) != 0) return;
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if ((!rgba && !bgra) || desc.Width != geometry.width || desc.Height != geometry.height
                || !desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192
                || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1)
                throw std::runtime_error("Unsupported reference texture layout");
            if (!staging) {
                staged = desc; staged.Usage = D3D11_USAGE_STAGING; staged.BindFlags = 0;
                staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ; staged.MiscFlags = 0;
                Microsoft::WRL::ComPtr<ID3D11Device> device; texture->GetDevice(&device);
                if (FAILED(device->CreateTexture2D(&staged, nullptr, &staging))) throw std::runtime_error("Reference staging allocation failed");
                const ReplayGeometry saved(geometry); Write(output / "geometry.bin", &saved, sizeof(saved));
            } else if (desc.Width != staged.Width || desc.Height != staged.Height || desc.Format != staged.Format)
                throw std::runtime_error("Reference geometry changed during capture");
            if (count == 0 && std::filesystem::exists(output / "geometry.bin") == false) {
                const ReplayGeometry saved(geometry); Write(output / "geometry.bin", &saved, sizeof(saved));
            }
            context->CopyResource(staging.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("Reference GPU readback failed");
            std::vector<uint8_t> pixels;
            try {
                pixels.resize(size_t(desc.Width) * desc.Height * 4);
                for (uint32_t y = 0; y < desc.Height; ++y)
                    std::memcpy(pixels.data() + size_t(y) * desc.Width * 4, static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4);
            } catch (...) { context->Unmap(staging.Get(), 0); throw; }
            context->Unmap(staging.Get(), 0);
            if (bgra) for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            const std::string stem = "frame_" + std::to_string(count);
            Write(output / (stem + ".rgba"), pixels.data(), pixels.size());
            Write(output / (stem + ".packet"), &packet, sizeof(packet));
            std::ofstream metadata(output / (stem + ".json"));
            metadata << "{\"scope\":\"observer_affected_encoder_input_reference\",\"index\":" << count
                << ",\"target_timestamp_ns\":" << timestamp << ",\"producer_pid\":" << packet.producerPid
                << ",\"sequence\":" << packet.sequence << ",\"qpc\":" << packet.qpc
                << ",\"width\":" << desc.Width << ",\"height\":" << desc.Height
                << ",\"dxgi_format\":" << unsigned(desc.Format) << ",\"stored_pixel_format\":\"RGBA8\"}\n";
            if (!metadata) throw std::runtime_error("Reference metadata write failed");
            ++count;
            if (count == MaxFrames) {
                std::ofstream done(output / "complete.json");
                done << "{\"frames\":" << count << ",\"valid_frame_stride\":" << FrameStride
                    << ",\"timing_evidence\":false}\n";
            }
        } catch (const std::exception& e) {
            failed = true;
            try { std::ofstream error(root / "capture-error.txt"); error << e.what() << '\n'; } catch (...) {}
        } catch (...) { failed = true; }
    }
};
}
