#pragma once
#include "AttentionMap.h"
#include "RegionalQuality.h"
#include <atomic>
#include <windows.h>

namespace gvl::streaming::alvr {
// One receiver per SteamVR process. Capture runs at composition submission; the encoder
// consumes that exact captured snapshot, never a newer map for an older queued frame.
class AttentionBridge {
public:
    static AttentionBridge& Instance();
    void SetSceneProcess(uint32_t pid) { scenePid.store(pid); }
    void Configure(const Geometry& geometry);
    void Capture(uint64_t frameTimestamp, int layerCount);
    bool Build(uint64_t frameTimestamp, float streamMbps);
    void ReportNetworkFeedback(QualityFeedback feedback);
    float UpdateQuality(float nominalMbps);
    void Disable(uint32_t encoderError);
    bool Enabled() const { return enabled && !failed; }
    const std::vector<int8_t>& Deltas() const { return builder.deltas; }
    const Packet& CapturedPacket() const { return captured; } // only after Build succeeds for this frame
    const Geometry& ConfiguredGeometry() const { return configuredGeometry; }
    int MinimumDelta() const { return builder.minDelta; }
    int MaximumDelta() const { return builder.maxDelta; }
private:
    AttentionBridge();
    ~AttentionBridge();
    void PublishStatus();
    bool enabled = false, failed = false, capturedValid = false;
    HANDLE mapping = nullptr, mutex = nullptr;
    uint8_t* view = nullptr;
    uint64_t frequency = 0, capturedFrame = 0;
    std::atomic<uint32_t> scenePid{0};
    Control control;
    Packet captured{};
    MapBuilder builder;
    Geometry configuredGeometry;
    SRWLOCK feedbackLock = SRWLOCK_INIT;
    QualityFeedback incomingFeedback{};
    bool hasFeedback = false;
    QualityControl quality;
    RegionalQualityController qualityController;
};
}
