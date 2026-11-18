#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gvl::streaming {
constexpr uint32_t QualityMagic = 0x31514c47, QualityVersion = 1;
constexpr size_t QualityOffset = 6528; // After the four version-1 attention packets.
enum class QualityMode : uint32_t { Disabled, Observe, Adaptive };
enum class QualityState : uint32_t { Waiting, Stable, Congested, Recovering, Stale };
struct QualityConfig {
    uint32_t mode = 0;
    float maxExtraQp = 6, minimumScale = .65f, highQueueMs = 12, lowQueueMs = 4;
    float decoderQueueMs = 20, recoveryHoldMs = 3000, feedbackTimeoutMs = 500;
    uint32_t producerPid = 0, reserved0 = 0;
    uint64_t updatedQpc = 0;
    uint32_t reserved[4]{};
};
inline bool Valid(const QualityConfig& c) {
    return c.mode <= 2 && std::isfinite(c.maxExtraQp) && c.maxExtraQp >= 0 && c.maxExtraQp <= 12
        && std::isfinite(c.minimumScale) && c.minimumScale >= .5f && c.minimumScale <= 1
        && std::isfinite(c.highQueueMs) && c.highQueueMs >= 4 && c.highQueueMs <= 100
        && std::isfinite(c.lowQueueMs) && c.lowQueueMs >= 0 && c.lowQueueMs < c.highQueueMs
        && std::isfinite(c.decoderQueueMs) && c.decoderQueueMs >= 5 && c.decoderQueueMs <= 100
        && std::isfinite(c.recoveryHoldMs) && c.recoveryHoldMs >= 1000 && c.recoveryHoldMs <= 30000
        && std::isfinite(c.feedbackTimeoutMs) && c.feedbackTimeoutMs >= 100 && c.feedbackTimeoutMs <= 2000;
}
struct QualityFeedback {
    uint64_t sourceTimestamp = 0, receivedQpc = 0, packetsLost = 0, frameBytes = 0;
    float networkMs = 0, decoderQueueMs = 0, frameIntervalMs = 0;
};
struct QualityStatus {
    uint32_t state = 0, feedbackCount = 0, invalidFeedback = 0, fresh = 0;
    uint64_t sourceTimestamp = 0, feedbackQpc = 0;
    float networkMs = 0, excessMs = 0, decoderQueueMs = 0, jitterMs = 0;
    float recommendedExtraQp = 0, recommendedScale = 1, appliedExtraQp = 0, appliedScale = 1;
    float nominalMbps = 0, appliedMbps = 0, encodedFrameKBytes = 0, frameIntervalMs = 0;
    uint64_t packetsLost = 0;
};
struct QualityControl {
    uint32_t magic = QualityMagic, version = QualityVersion, bytes = sizeof(QualityControl), backendPid = 0;
    uint64_t heartbeat = 0, frequency = 0;
    QualityConfig config;
    QualityStatus status;
};
static_assert(sizeof(QualityConfig) == 64 && sizeof(QualityStatus) == 88 && sizeof(QualityControl) == 184,
    "Quality control layout changed");

// Network residual is ALVR's estimate, not an absolute one-way queue measurement.
// All state updates use the receiver's monotonic clock, never a headset clock epoch.
class RegionalQualityController {
public:
    QualityStatus status;
    void Reset() { *this = RegionalQualityController{}; }
    bool Observe(const QualityFeedback& f, double nowMs) {
        if (!std::isfinite(nowMs) || nowMs < 0 || !f.sourceTimestamp ||
            (status.sourceTimestamp && f.sourceTimestamp <= status.sourceTimestamp) ||
            !std::isfinite(f.networkMs) || f.networkMs <= 0 || f.networkMs > 2000 ||
            !std::isfinite(f.decoderQueueMs) || f.decoderQueueMs < 0 || f.decoderQueueMs > 2000 ||
            !std::isfinite(f.frameIntervalMs) || f.frameIntervalMs <= 0 || f.frameIntervalMs > 2000 ||
            !f.frameBytes || f.frameBytes > 64 * 1024 * 1024 || (seen && nowMs < lastFeedbackMs)) {
            ++status.invalidFeedback; return false;
        }
        const double dt = seen ? std::min(nowMs - lastFeedbackMs, 1000.0) : 0;
        if (!seen) { baseline = mean = previous = f.networkMs; lastStepMs = nowMs; }
        else {
            const float alpha = float(1 - std::exp(-dt / 150.0));
            mean += alpha * (f.networkMs - mean);
            jitter += alpha * (std::abs(f.networkMs - previous) - jitter);
            // Slowly forget an obsolete path minimum; rising queues cannot immediately
            // redefine themselves as the uncongested baseline.
            baseline = std::min(f.networkMs, baseline + float(dt * .0005));
        }
        if (seen && f.packetsLost > status.packetsLost) lossUntilMs = nowMs + 500;
        previous = f.networkMs;
        lastFeedbackMs = nowMs; seen = true;
        status.sourceTimestamp = f.sourceTimestamp; status.feedbackQpc = f.receivedQpc;
        status.networkMs = f.networkMs; status.decoderQueueMs = f.decoderQueueMs;
        status.excessMs = std::max(0.0f, mean - baseline); status.jitterMs = jitter;
        status.encodedFrameKBytes = float(f.frameBytes) / 1000; status.frameIntervalMs = f.frameIntervalMs;
        status.packetsLost = f.packetsLost; ++status.feedbackCount;
        return true;
    }
    void Step(const QualityConfig& config, double nowMs) {
        if (!Valid(config) || config.mode == uint32_t(QualityMode::Disabled)) {
            status.appliedExtraQp = status.recommendedExtraQp = 0;
            status.appliedScale = status.recommendedScale = 1;
            status.state = uint32_t(QualityState::Waiting); status.fresh = 0; healthySinceMs = -1;
            lastStepMs = nowMs; return;
        }
        if (!std::isfinite(nowMs) || nowMs < lastStepMs) return;
        const double elapsed = std::min(nowMs - lastStepMs, 250.0);
        lastStepMs = nowMs;
        status.fresh = seen && nowMs >= lastFeedbackMs && nowMs - lastFeedbackMs <= config.feedbackTimeoutMs;
        if (!seen) status.state = uint32_t(QualityState::Waiting);
        else if (!status.fresh) {
            // Hold the last bounded decision during missing feedback; never probe upward.
            status.state = uint32_t(QualityState::Stale); healthySinceMs = -1;
        } else {
            const bool bad = status.excessMs + status.jitterMs > config.highQueueMs
                || status.decoderQueueMs > config.decoderQueueMs || nowMs < lossUntilMs;
            const bool good = status.excessMs + status.jitterMs < config.lowQueueMs
                && status.decoderQueueMs < config.decoderQueueMs * .5f && nowMs >= lossUntilMs;
            if (bad) {
                status.state = uint32_t(QualityState::Congested); healthySinceMs = -1;
                status.recommendedExtraQp += float(elapsed / 200.0);
                // Reduce peripheral quality first. Persistent congestion then limits
                // total encoder load, while retaining the core's regional QP boost.
                if (status.recommendedExtraQp >= std::min(3.0f, config.maxExtraQp))
                    status.recommendedScale -= float(elapsed * .0002);
            } else if (good) {
                if (healthySinceMs < 0) healthySinceMs = nowMs;
                if (nowMs - healthySinceMs >= config.recoveryHoldMs) {
                    status.recommendedExtraQp -= float(elapsed / 1000.0);
                    status.recommendedScale += float(elapsed * .00002);
                }
                status.state = uint32_t(status.recommendedExtraQp > 0 || status.recommendedScale < 1
                    ? QualityState::Recovering : QualityState::Stable);
            } else { healthySinceMs = -1; status.state = uint32_t(QualityState::Recovering); }
        }
        status.recommendedExtraQp = std::clamp(status.recommendedExtraQp, 0.0f, config.maxExtraQp);
        status.recommendedScale = std::clamp(status.recommendedScale, config.minimumScale, 1.0f);
        const bool apply = config.mode == uint32_t(QualityMode::Adaptive);
        status.appliedExtraQp = apply ? status.recommendedExtraQp : 0;
        status.appliedScale = apply ? status.recommendedScale : 1;
    }
private:
    bool seen = false;
    float baseline = 0, mean = 0, previous = 0, jitter = 0;
    double lastFeedbackMs = 0, lastStepMs = 0, healthySinceMs = -1, lossUntilMs = 0;
};
} // namespace gvl::streaming
