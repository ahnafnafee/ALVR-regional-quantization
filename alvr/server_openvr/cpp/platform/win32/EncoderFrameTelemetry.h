#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <cstdint>

namespace gvl::streaming::alvr {
// Explicit diagnostic only. No extra GPU wait or disk access on the encoder thread.
// NVENC's unique input/output timestamp joins submissions even when pose IDs repeat.
class EncoderFrameTelemetry {
    static constexpr size_t Capacity = 1024;
    struct Sample {
        uint64_t id = 0, target = 0, submitted = 0, completed = 0, sequence = 0;
        uint32_t bytes = 0, averageQp = 0, producer = 0, unityFrame = 0;
        uint32_t width = 0, height = 0;
        int minimum = 0, maximum = 0;
        uint32_t encoderStatus = 0;
        bool map = false, confirmed = false, accepted = false, applied = false;
    };
    std::array<Sample, Capacity> inflight{}, queue{};
    std::atomic<size_t> head{0}, tail{0};
    std::atomic<uint64_t> invalid{0};
    std::atomic<bool> ready{false}, stop{false}, failed{false}, finished{false};
    std::atomic<uint64_t> pendingOutputs{0};
    uint64_t next = 1, frequency = 0;
    std::filesystem::path root;
    std::mutex waitMutex;
    std::condition_variable wake;
    std::thread writer;

    static bool Write(HANDLE file, const std::string& value) {
        DWORD written = 0;
        return WriteFile(file, value.data(), DWORD(value.size()), &written, nullptr)
            && written == value.size();
    }
    static void ClockAnchor(std::ostream& output) {
        LARGE_INTEGER before{}, after{};
        FILETIME utc{};
        QueryPerformanceCounter(&before);
        GetSystemTimePreciseAsFileTime(&utc);
        QueryPerformanceCounter(&after);
        const uint64_t filetime = (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
        output << "\"utc_filetime_100ns\":" << filetime << ",\"utc_qpc_before\":" << before.QuadPart
            << ",\"utc_qpc_after\":" << after.QuadPart;
    }
    void Writer() noexcept {
        HANDLE file = INVALID_HANDLE_VALUE;
        try {
            file = CreateFileW((root / L"encoder-frames.jsonl").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE) { failed = true; return; }
            std::ostringstream header;
            header << "{\"schema\":\"cs692.live-encoder-frames.v1\",\"kind\":\"header\",\"pid\":"
                << GetCurrentProcessId() << ",\"qpc_frequency\":" << frequency
                << ",\"scope\":\"NVENC_output_and_host_clock_diagnostic\","
                << "\"extra_gpu_readback\":false,\"periodic_clock_anchors\":true,\"physical_motion_to_photon_ms\":null,"
                << "\"absolute_semantic_region_bits\":null,";
            ClockAnchor(header); header << "}\n";
            if (!Write(file, header.str())) { failed = true; CloseHandle(file); return; }
            ready = true;
            uint64_t count = 0;
            uint64_t clocks = 0;
            ULONGLONG finishAt = 0;
            ULONGLONG clockAt = GetTickCount64() + 1000;
            for (;;) {
                if (!stop.load() && GetFileAttributesW((root / L"telemetry.stop").c_str()) != INVALID_FILE_ATTRIBUTES)
                    Finish();
                if (stop.load() && !finishAt) finishAt = GetTickCount64() + 1000;
                if (GetTickCount64() >= clockAt) {
                    std::ostringstream clock;
                    clock << "{\"kind\":\"clock\",";
                    ClockAnchor(clock); clock << "}\n";
                    if (!Write(file, clock.str())) { failed = true; break; }
                    ++clocks;
                    clockAt = GetTickCount64() + 1000;
                }
                const size_t begin = head.load(std::memory_order_relaxed);
                const size_t end = tail.load(std::memory_order_acquire);
                if (begin == end) {
                    if (stop.load() && pendingOutputs.load() == 0
                        && tail.load(std::memory_order_acquire) == begin) break;
                    if (finishAt && GetTickCount64() >= finishAt) {
                        invalid += pendingOutputs.load(); break;
                    }
                    std::unique_lock<std::mutex> lock(waitMutex);
                    wake.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                const auto sample = queue[begin % Capacity];
                head.store(begin + 1, std::memory_order_release);
                std::ostringstream row;
                row.precision(12);
                row << "{\"kind\":\"frame\",\"frame_id\":" << sample.id
                    << ",\"target_timestamp_ns\":" << sample.target
                    << ",\"submit_qpc\":" << sample.submitted << ",\"bitstream_lock_qpc\":" << sample.completed
                    << ",\"submit_to_bitstream_lock_ms\":" << 1000.0 * double(sample.completed - sample.submitted) / double(frequency)
                    << ",\"bitstream_bytes\":" << sample.bytes << ",\"frame_average_qp\":" << sample.averageQp
                    << ",\"width\":" << sample.width << ",\"height\":" << sample.height
                    << ",\"map_valid\":" << (sample.map ? "true" : "false")
                    << ",\"submission_confirmed\":" << (sample.confirmed ? "true" : "false")
                    << ",\"encoder_status\":" << sample.encoderStatus
                    << ",\"map_applied\":" << (sample.applied ? "true" : "false")
                    << ",\"producer_pid\":" << sample.producer << ",\"packet_sequence\":" << sample.sequence
                    << ",\"unity_frame\":" << sample.unityFrame
                    << ",\"requested_qp_min\":" << sample.minimum << ",\"requested_qp_max\":" << sample.maximum << "}\n";
                if (!Write(file, row.str())) { failed = true; break; }
                ++count;
            }
            std::ostringstream footer;
            footer << "{\"kind\":\"footer\",\"frames\":" << count << ",\"invalid_or_missing_outputs\":"
                << invalid.load() << ",\"write_failed\":" << (failed.load() ? "true" : "false")
                << ",\"clock_samples\":" << clocks << ',';
            ClockAnchor(footer); footer << "}\n";
            if (!Write(file, footer.str()) || !FlushFileBuffers(file)) failed = true;
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            const auto completion = root / L"encoder-complete.json";
            const auto pendingReceipt = root / L"encoder-complete.pending.json";
            HANDLE done = CreateFileW(pendingReceipt.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (done != INVALID_HANDLE_VALUE) {
                std::ostringstream receipt;
                receipt << "{\"schema\":\"cs692.live-encoder-completion.v1\",\"frames\":" << count
                    << ",\"invalid_or_missing_outputs\":" << invalid.load() << ",\"write_failed\":"
                    << (failed.load() ? "true" : "false") << "}\n";
                if (!Write(done, receipt.str())) failed = true;
                FlushFileBuffers(done); CloseHandle(done);
                if (!MoveFileW(pendingReceipt.c_str(), completion.c_str())) failed = true;
            } else failed = true;
            ready = false; finished = true;
        } catch (...) { failed = true; if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    }
public:
    static EncoderFrameTelemetry& Instance() { static EncoderFrameTelemetry instance; return instance; }
    EncoderFrameTelemetry() noexcept {
        wchar_t path[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", path, 32768);
        if (!size || size >= 32768) return;
        try {
            root = path;
            LARGE_INTEGER counter{};
            if (!root.is_absolute() || !std::filesystem::is_directory(root)
                || !QueryPerformanceFrequency(&counter) || counter.QuadPart <= 0) return;
            frequency = uint64_t(counter.QuadPart);
            writer = std::thread([this] { Writer(); });
        } catch (...) { failed = true; }
    }
    ~EncoderFrameTelemetry() {
        Finish();
        if (writer.joinable()) writer.join();
    }
    EncoderFrameTelemetry(const EncoderFrameTelemetry&) = delete;
    EncoderFrameTelemetry& operator=(const EncoderFrameTelemetry&) = delete;
    bool Ready() const { return ready.load() && !failed.load(); }
    uint64_t InvalidOutputs() const { return invalid.load(); }
    void Finish() noexcept { stop = true; ready = false; wake.notify_one(); }
    uint64_t Begin(uint64_t target, uint32_t width, uint32_t height, bool map,
        uint32_t producer, uint64_t sequence, uint32_t unityFrame, int minimum, int maximum) noexcept {
        if (!Ready()) return 0;
        ++pendingOutputs;
        if (!Ready() || stop.load()) { --pendingOutputs; return 0; }
        auto& slot = inflight[next % Capacity];
        if (slot.id) { --pendingOutputs; ++invalid; failed = true; return 0; }
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) { --pendingOutputs; ++invalid; failed = true; return 0; }
        slot = {};
        slot.id = next; slot.target = target; slot.submitted = uint64_t(now.QuadPart);
        slot.sequence = map ? sequence : 0; slot.producer = map ? producer : 0; slot.unityFrame = map ? unityFrame : 0;
        slot.width = width; slot.height = height; slot.minimum = map ? minimum : 0; slot.maximum = map ? maximum : 0;
        slot.map = map;
        return next++;
    }
    void SubmissionResult(uint64_t id, bool accepted, bool mapApplied, uint32_t status) noexcept {
        if (!id || !frequency || failed.load() || finished.load()) return;
        auto& slot = inflight[id % Capacity];
        if (slot.id != id || slot.confirmed) { ++invalid; return; }
        slot.confirmed = true; slot.accepted = accepted; slot.applied = accepted && mapApplied;
        slot.encoderStatus = status;
    }
    void Complete(uint64_t id, uint32_t bytes, uint32_t averageQp) noexcept {
        if (!id || !frequency || failed.load() || finished.load()) return;
        auto& slot = inflight[id % Capacity];
        if (slot.id != id) { ++invalid; return; }
        LARGE_INTEGER now{};
        if (!slot.confirmed || !slot.accepted || !bytes || averageQp > 51
            || !QueryPerformanceCounter(&now) || uint64_t(now.QuadPart) < slot.submitted) {
            slot.id = 0; --pendingOutputs; ++invalid; return;
        }
        const size_t end = tail.load(std::memory_order_relaxed);
        if (end - head.load(std::memory_order_acquire) == Capacity) {
            slot.id = 0; --pendingOutputs; ++invalid; failed = true; return;
        }
        slot.bytes = bytes; slot.averageQp = averageQp; slot.completed = uint64_t(now.QuadPart);
        queue[end % Capacity] = slot;
        slot.id = 0;
        tail.store(end + 1, std::memory_order_release);
        --pendingOutputs;
        wake.notify_one();
    }
};
}
