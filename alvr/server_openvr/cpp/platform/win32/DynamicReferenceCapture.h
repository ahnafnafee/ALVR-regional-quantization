#pragma once
#include "ReplayGeometry.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gvl::streaming::alvr {
// Opt-in encoder-input observation. GPU readbacks never wait; a bounded writer
// queue fails the capture instead of silently sampling, repeating or dropping frames.
// This diagnostic still affects the host and cannot establish live latency.
class DynamicReferenceCapture {
    static constexpr uint32_t FrameCount = 360, StagingCount = 8, PoolCount = 24;
    struct Frame {
        std::vector<uint8_t> pixels;
        Packet packet{};
        uint64_t timestamp = 0, captureQpc = 0;
        uint32_t index = 0;
        bool valid = false;
    };
    struct Pending {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Frame* frame = nullptr;
        uint32_t pool = 0;
    };
    std::filesystem::path root, output;
    bool enabled = false, armed = false, started = false, bgra = false;
    std::atomic<bool> failed{false};
    std::array<Frame, PoolCount> pool;
    std::array<Pending, StagingCount> staging;
    uint32_t submitted = 0, pending = 0, head = 0, tail = 0;
    ULONGLONG nextPoll = 0;
    uint64_t captureFrequency = 0;
    D3D11_TEXTURE2D_DESC layout{};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<uint32_t> free, ready;
    bool stopping = false;
    std::thread writer;

    static void Write(const std::filesystem::path& path, const void* data, size_t size) {
        if (std::filesystem::exists(path)) throw std::runtime_error("Dynamic reference output already exists");
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char*>(data), std::streamsize(size));
        if (!file) throw std::runtime_error("Dynamic reference write failed");
    }
    void Error(const char* message) noexcept {
        failed = true;
        try { std::ofstream file(root / "capture-error.txt"); file << message << '\n'; } catch (...) {}
    }
    void Writer() noexcept {
        try {
            uint32_t written = 0, fallbacks = 0;
            uint64_t first = 0, last = 0, firstCapture = 0, lastCapture = 0;
            for (;;) {
                uint32_t slot;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] { return stopping || !ready.empty(); });
                    if (ready.empty()) return;
                    slot = ready.front(); ready.pop_front();
                }
                auto& frame = pool[slot];
                if (frame.index != written || !frame.captureQpc || (written && frame.captureQpc <= lastCapture))
                    throw std::runtime_error("Dynamic input frame order or capture clock discontinuity");
                if (bgra) for (size_t i = 0; i < frame.pixels.size(); i += 4)
                    std::swap(frame.pixels[i], frame.pixels[i + 2]);
                const auto stem = "frame_" + std::to_string(frame.index);
                Write(output / (stem + ".rgba"), frame.pixels.data(), frame.pixels.size());
                Write(output / (stem + ".packet"), &frame.packet, sizeof(frame.packet));
                std::ofstream meta(output / (stem + ".json"));
                meta << "{\"scope\":\"observer_affected_continuous_encoder_input\",\"index\":" << frame.index
                    << ",\"target_timestamp_ns\":" << frame.timestamp
                    << ",\"capture_qpc\":" << frame.captureQpc << ",\"capture_frequency\":" << captureFrequency
                    << ",\"map_valid\":" << (frame.valid ? "true" : "false")
                    << ",\"producer_pid\":" << frame.packet.producerPid
                    << ",\"sequence\":" << frame.packet.sequence << ",\"qpc\":" << frame.packet.qpc
                    << ",\"width\":" << layout.Width << ",\"height\":" << layout.Height
                    << ",\"stored_pixel_format\":\"RGBA8\"}\n";
                if (!meta) throw std::runtime_error("Dynamic metadata write failed");
                if (!written) { first = frame.timestamp; firstCapture = frame.captureQpc; }
                last = frame.timestamp; lastCapture = frame.captureQpc; fallbacks += !frame.valid; ++written;
                {
                    std::lock_guard<std::mutex> lock(mutex); free.push_back(slot);
                }
                if (written == FrameCount) {
                    std::ofstream done(output / "complete.json");
                    done << "{\"schema\":\"cs692.dynamic-encoder-reference.v2\",\"frames\":" << written
                        << ",\"fallback_frames\":" << fallbacks << ",\"capture_dropped_frames\":0"
                        << ",\"first_timestamp_ns\":" << first << ",\"last_timestamp_ns\":" << last
                        << ",\"capture_clock\":\"windows_qpc\",\"capture_frequency\":" << captureFrequency
                        << ",\"first_capture_qpc\":" << firstCapture << ",\"last_capture_qpc\":" << lastCapture
                        << ",\"frame_stride\":1,\"nominal_fps\":72,\"timing_evidence\":false}\n";
                    if (!done) throw std::runtime_error("Dynamic completion receipt write failed");
                    return;
                }
            }
        } catch (const std::exception& e) { Error(e.what()); }
        catch (...) { Error("Dynamic writer failed"); }
    }
    void Start(ID3D11Texture2D* texture, const Geometry& geometry, const std::string& token) {
        texture->GetDesc(&layout);
        bgra = layout.Format == DXGI_FORMAT_B8G8R8A8_UNORM || layout.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = layout.Format == DXGI_FORMAT_R8G8B8A8_UNORM || layout.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const ReplayGeometry saved(geometry);
        if (!saved.Valid() || (!bgra && !rgba) || layout.Width != geometry.width || layout.Height != geometry.height
            || layout.MipLevels != 1 || layout.ArraySize != 1 || layout.SampleDesc.Count != 1)
            throw std::runtime_error("Unsupported dynamic reference geometry");
        output = root / token;
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("Dynamic reference group already exists");
        Write(output / "geometry.bin", &saved, sizeof(saved));
        auto desc = layout; desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Device> device; texture->GetDevice(&device);
        for (auto& slot : staging) if (FAILED(device->CreateTexture2D(&desc, nullptr, &slot.texture)))
            throw std::runtime_error("Dynamic staging allocation failed");
        for (uint32_t i = 0; i < PoolCount; ++i) {
            pool[i].pixels.resize(size_t(layout.Width) * layout.Height * 4); free.push_back(i);
        }
        started = true; writer = std::thread([this] { Writer(); });
    }
    void Drain(ID3D11DeviceContext* context) {
        while (pending) {
            auto& slot = staging[head];
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT result = context->Map(slot.texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (FAILED(result)) throw std::runtime_error("Dynamic GPU readback failed");
            for (uint32_t y = 0; y < layout.Height; ++y)
                std::memcpy(slot.frame->pixels.data() + size_t(y) * layout.Width * 4,
                    static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(layout.Width) * 4);
            context->Unmap(slot.texture.Get(), 0);
            { std::lock_guard<std::mutex> lock(mutex); ready.push_back(slot.pool); }
            wake.notify_one(); slot.frame = nullptr;
            head = (head + 1) % StagingCount; --pending;
        }
    }
public:
    DynamicReferenceCapture() noexcept {
        wchar_t path[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"GVL_ALVR_DYNAMIC_CAPTURE", path, 32768);
        if (!n || n >= 32768) return;
        try {
            root = path;
            LARGE_INTEGER frequency{};
            enabled = root.is_absolute() && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
            captureFrequency = uint64_t(frequency.QuadPart);
        } catch (...) {}
    }
    ~DynamicReferenceCapture() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one(); if (writer.joinable()) writer.join();
    }
    DynamicReferenceCapture(const DynamicReferenceCapture&) = delete;
    DynamicReferenceCapture& operator=(const DynamicReferenceCapture&) = delete;
    void Capture(ID3D11Texture2D* texture, ID3D11DeviceContext* context, uint64_t timestamp,
                 const Packet& packet, const Geometry& geometry, bool valid) noexcept {
        if (!enabled || failed || !texture || !context) return;
        try {
            LARGE_INTEGER inputClock{};
            if (!QueryPerformanceCounter(&inputClock)) throw std::runtime_error("Dynamic input capture clock unavailable");
            if (started) Drain(context);
            const auto now = GetTickCount64();
            if (now >= nextPoll) {
                nextPoll = now + 500;
                std::ifstream request(root / "capture.arm"); std::string token; request >> token;
                armed = token == "development" || token == "holdout";
                if (armed && !started) Start(texture, geometry, token);
            }
            if (!armed || !started || submitted == FrameCount) return;
            D3D11_TEXTURE2D_DESC current{}; texture->GetDesc(&current);
            if (current.Width != layout.Width || current.Height != layout.Height || current.Format != layout.Format)
                throw std::runtime_error("Dynamic reference geometry changed");
            if (pending == StagingCount) throw std::runtime_error("Dynamic capture GPU queue overflow; group is invalid");
            uint32_t index;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (free.empty()) throw std::runtime_error("Dynamic capture disk queue overflow; group is invalid");
                index = free.front(); free.pop_front();
            }
            auto& frame = pool[index]; frame.index = submitted; frame.timestamp = timestamp; frame.valid = valid;
            // ALVR matches tracking-history poses to textures. Their target timestamps
            // can repeat or move backwards; they are provenance, not a submission clock.
            frame.captureQpc = uint64_t(inputClock.QuadPart);
            frame.packet = valid ? packet : Packet{}; // A fallback never reuses the previous map.
            auto& slot = staging[tail]; slot.pool = index; slot.frame = &frame;
            context->CopyResource(slot.texture.Get(), texture);
            tail = (tail + 1) % StagingCount; ++pending; ++submitted;
        } catch (const std::exception& e) { Error(e.what()); }
        catch (...) { Error("Dynamic capture failed"); }
    }
};
}
