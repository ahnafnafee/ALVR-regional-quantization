#include "AttentionBridge.h"
#include <cstring>
#include <filesystem>

namespace gvl::streaming::alvr {
AttentionBridge& AttentionBridge::Instance() { static AttentionBridge bridge; return bridge; }
AttentionBridge::AttentionBridge() {
    wchar_t value[8]{};
    enabled = GetEnvironmentVariableW(L"FLYINGBIKE_ALVR_ATTENTION", value, 8) == 1 && value[0] == L'1';
    HMODULE ownModule = nullptr;
    wchar_t ownPath[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&AttentionBridge::Instance), &ownModule)
        && GetModuleFileNameW(ownModule, ownPath, 32768)) {
        const auto marker = std::filesystem::path(ownPath).parent_path().parent_path().parent_path() / L"attention.enabled";
        enabled = enabled || GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!enabled) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f); frequency = uint64_t(f.QuadPart);
    mutex = CreateMutexW(nullptr, FALSE, MutexName);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, MappingBytes, MappingName);
    if (mapping) view = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, MappingBytes));
    if (!mutex || !view) { enabled = false; return; }
    control.consumerPid = GetCurrentProcessId();
    control.enabled = 1;
    quality.backendPid = GetCurrentProcessId(); quality.frequency = frequency;
    const DWORD initial = WaitForSingleObject(mutex, 0);
    if (initial == WAIT_OBJECT_0 || initial == WAIT_ABANDONED) {
        std::memcpy(view + QualityOffset, &quality, sizeof(quality));
        ReleaseMutex(mutex);
    }
    PublishStatus();
}
AttentionBridge::~AttentionBridge() {
    control.enabled = 0; PublishStatus();
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
}
void AttentionBridge::Configure(const Geometry& geometry) {
    if (!Enabled()) return;
    builder.Configure(geometry);
    configuredGeometry = geometry;
    qualityController.Reset();
    AcquireSRWLockExclusive(&feedbackLock);
    hasFeedback = false;
    ReleaseSRWLockExclusive(&feedbackLock);
    control.width = geometry.width; control.height = geometry.height; control.block = geometry.block;
    capturedValid = false;
    PublishStatus();
}
void AttentionBridge::PublishStatus() {
    if (!view || !mutex) return;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    control.heartbeat = uint64_t(now.QuadPart);
    control.scenePid = scenePid.load();
    std::memcpy(view, &control, offsetof(Control, producerDiagnostics));
    std::memcpy(view + offsetof(Control, highPriorityFraction), &control.highPriorityFraction, 3 * sizeof(float));
    QualityControl received{};
    std::memcpy(&received, view + QualityOffset, sizeof(received));
    if (received.magic == QualityMagic && received.version == QualityVersion && received.bytes == sizeof(received)
        && received.backendPid == GetCurrentProcessId() && Valid(received.config)) quality.config = received.config;
    quality.heartbeat = control.heartbeat;
    quality.status = qualityController.status;
    // The host owns the config only. Status updates never overwrite a concurrent choice.
    std::memcpy(view + QualityOffset, &quality, offsetof(QualityControl, config));
    std::memcpy(view + QualityOffset + offsetof(QualityControl, status), &quality.status, sizeof(quality.status));
    ReleaseMutex(mutex);
}
void AttentionBridge::Capture(uint64_t frameTimestamp, int layerCount) {
    capturedValid = false; capturedFrame = frameTimestamp;
    if (!Enabled() || !view || layerCount != 1) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    if (wait != WAIT_ABANDONED) {
        auto* fence = reinterpret_cast<volatile LONG64*>(view + offsetof(Control, requestedQpc));
        const uint64_t requested = uint64_t(InterlockedCompareExchange64(fence, 0, 0));
        uint64_t newest = 0;
        for (uint32_t i = 0; i < Slots; ++i) {
            Packet candidate;
            std::memcpy(&candidate, view + ControlBytes + i * sizeof(Packet), sizeof(Packet));
            if (candidate.magic == Magic && candidate.version == Version
                && candidate.producerPid == scenePid.load() && candidate.qpc <= uint64_t(now.QuadPart)
                && candidate.qpc >= newest) {
                newest = candidate.qpc; captured = candidate;
            }
        }
        capturedValid = newest != 0 && MatchesRequest(captured, requested)
            && requested == uint64_t(InterlockedCompareExchange64(fence, 0, 0))
            && Valid(captured, uint64_t(now.QuadPart), frequency, scenePid.load());
    }
    ReleaseMutex(mutex);
}
bool AttentionBridge::Build(uint64_t frameTimestamp, float streamMbps) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const bool valid = Enabled() && capturedValid && capturedFrame == frameTimestamp
        && Valid(captured, uint64_t(now.QuadPart), frequency, scenePid.load());
    if (valid) {
        builder.Build(captured, streamMbps, qualityController.status.appliedExtraQp);
        LARGE_INTEGER finished; QueryPerformanceCounter(&finished);
        control.mapBuildMs = float(1000.0 * double(finished.QuadPart - now.QuadPart) / double(frequency));
        control.producerAgeMs = float(1000.0 * double(now.QuadPart - captured.qpc) / double(frequency));
        control.regions = captured.count; control.producerPid = captured.producerPid;
        ++control.applied;
        control.lastSequence = captured.sequence;
        control.protectedFraction = builder.protectedFraction;
        control.highPriorityFraction = builder.highPriorityFraction;
        control.intermediateFraction = builder.intermediateFraction;
        control.backgroundFraction = builder.backgroundFraction;
        control.minDelta = builder.minDelta; control.maxDelta = builder.maxDelta;
    } else {
        ++control.rejected;
        control.protectedFraction = 0;
        control.highPriorityFraction = control.intermediateFraction = control.backgroundFraction = 0;
        control.minDelta = control.maxDelta = 0;
        control.mapBuildMs = 0; control.regions = 0;
    }
    PublishStatus();
    return valid;
}
void AttentionBridge::Disable(uint32_t error) {
    failed = true; control.enabled = 0; control.encoderError = error;
    PublishStatus();
}
void AttentionBridge::ReportNetworkFeedback(QualityFeedback feedback) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    feedback.receivedQpc = uint64_t(now.QuadPart);
    if (!TryAcquireSRWLockExclusive(&feedbackLock)) return;
    incomingFeedback = feedback; hasFeedback = true;
    ReleaseSRWLockExclusive(&feedbackLock);
}
float AttentionBridge::UpdateQuality(float nominalMbps) {
    if (!Enabled() || !frequency || !std::isfinite(nominalMbps) || nominalMbps <= 0) return nominalMbps;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    QualityFeedback sample{}; bool available = false;
    if (TryAcquireSRWLockExclusive(&feedbackLock)) {
        available = hasFeedback; sample = incomingFeedback; hasFeedback = false;
        ReleaseSRWLockExclusive(&feedbackLock);
    }
    if (available) qualityController.Observe(sample, double(sample.receivedQpc) * 1000 / double(frequency));
    auto activeConfig = quality.config;
    // A policy belongs to the current publishing application. Do not retain its
    // bitrate cap in the SteamVR dashboard or in another game after it exits.
    if (!activeConfig.producerPid || activeConfig.producerPid != scenePid.load()
        || uint64_t(now.QuadPart) < activeConfig.updatedQpc
        || double(uint64_t(now.QuadPart) - activeConfig.updatedQpc) * 1000 / double(frequency) > 2500)
        activeConfig.mode = uint32_t(QualityMode::Disabled);
    qualityController.Step(activeConfig, double(now.QuadPart) * 1000 / double(frequency));
    auto& status = qualityController.status;
    status.nominalMbps = nominalMbps;
    // The existing NVENC backend uses integral Mbps. Never exceed ALVR's requested budget.
    status.appliedMbps = std::min(nominalMbps, std::max(1.0f, std::ceil(nominalMbps * status.appliedScale)));
    return status.appliedMbps;
}
}

extern "C" void GvlReportRegionalFeedback(unsigned long long timestamp, float networkMs,
    float decoderQueueMs, unsigned long long lost, unsigned long long bytes, float intervalMs) {
    gvl::streaming::QualityFeedback feedback{};
    feedback.sourceTimestamp = timestamp; feedback.networkMs = networkMs;
    feedback.decoderQueueMs = decoderQueueMs; feedback.packetsLost = lost;
    feedback.frameBytes = bytes; feedback.frameIntervalMs = intervalMs;
    gvl::streaming::alvr::AttentionBridge::Instance().ReportNetworkFeedback(feedback);
}
