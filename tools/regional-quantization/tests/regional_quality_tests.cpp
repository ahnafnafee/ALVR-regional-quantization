#include "RegionalQuality.h"
#include "AttentionMap.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gvl::streaming;
static int checks = 0;
static void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
int main() {
    QualityConfig config; config.mode = uint32_t(QualityMode::Adaptive);
    RegionalQualityController controller;
    double now = 1000;
    uint64_t sequence = 0, loss = 0;
    auto frame = [&](float network = 5, float decode = 1) {
        now += 14;
        QualityFeedback feedback{++sequence, uint64_t(now * 1000), loss, 35000, network, decode, 14};
        Check(controller.Observe(feedback, now), "valid feedback rejected");
        controller.Step(config, now);
        Check(controller.status.appliedExtraQp >= 0 && controller.status.appliedExtraQp <= config.maxExtraQp
            && controller.status.appliedScale >= config.minimumScale && controller.status.appliedScale <= 1,
            "controller escaped configured bounds");
    };
    controller.Step(config, now);
    Check(controller.status.state == uint32_t(QualityState::Waiting), "missing feedback changed quality");
    for (int i = 0; i < 100; ++i) frame();
    Check(controller.status.appliedScale == 1 && controller.status.appliedExtraQp == 0, "healthy stream penalized");
    for (int i = 0; i < 25; ++i) frame(40);
    Check(controller.status.appliedExtraQp > 0 && controller.status.appliedScale == 1, "periphery not reduced first");
    for (int i = 0; i < 200; ++i) frame(40);
    Check(controller.status.appliedExtraQp == config.maxExtraQp && controller.status.appliedScale == config.minimumScale,
        "persistent queue did not reach bounded floor");
    auto held = controller.status;
    now += 1000; controller.Step(config, now);
    Check(controller.status.state == uint32_t(QualityState::Stale) && !controller.status.fresh
        && controller.status.appliedScale == held.appliedScale && controller.status.appliedExtraQp == held.appliedExtraQp,
        "missing feedback probed upward or changed the last decision");
    for (int i = 0; i < 200; ++i) frame();
    Check(controller.status.appliedScale == config.minimumScale, "recovery ignored hold period");
    for (int i = 0; i < 1800; ++i) frame();
    Check(controller.status.appliedScale == 1 && controller.status.appliedExtraQp == 0, "healthy recovery did not finish");
    ++loss; frame();
    Check(controller.status.appliedExtraQp > 0, "packet loss ignored");
    loss = 0; frame();
    Check(controller.status.packetsLost == 0, "packet counter reset rejected");
    for (int i = 0; i < 200; ++i) frame(5, 35);
    Check(controller.status.appliedScale < 1, "decoder queue ignored");
    config.mode = uint32_t(QualityMode::Observe); frame(40);
    Check(controller.status.appliedScale == 1 && controller.status.appliedExtraQp == 0
        && controller.status.recommendedExtraQp > 0, "observe mode changes encoder quality");
    QualityFeedback invalid{sequence, uint64_t(now * 1000), 0, 35000, 5, 1, 14};
    Check(!controller.Observe(invalid, now), "duplicate timestamp accepted");
    invalid.sourceTimestamp = sequence - 1;
    Check(!controller.Observe(invalid, now), "regressing timestamp accepted");
    invalid.sourceTimestamp = sequence + 1; invalid.networkMs = std::numeric_limits<float>::quiet_NaN();
    Check(!controller.Observe(invalid, now), "NaN feedback accepted");
    invalid.networkMs = 5; invalid.frameBytes = 0;
    Check(!controller.Observe(invalid, now), "unmatched frame accepted");
    config.mode = 0; controller.Step(config, now);
    Check(controller.status.recommendedScale == 1 && controller.status.recommendedExtraQp == 0, "disable does not reset controller");
    config.mode = 2; config.minimumScale = .1f;
    Check(!Valid(config), "unsafe configuration accepted");
    controller.Step(config, now);
    Check(controller.status.appliedScale == 1, "invalid configuration affected encoder");

    using namespace gvl::streaming::alvr;
    // Leave room below the QP ceiling so this fixture exercises an added penalty,
    // rather than requiring an already saturated background to worsen further.
    Packet p{}; p.count = 2; p.importantWeight = 70; p.backgroundWeight = 10;
    p.regions[0] = {.375f, .375f, .625f, .625f, 0, 1};
    p.regions[1] = {.375f, .375f, .625f, .625f, 1, 1};
    MapBuilder map; Geometry geometry; geometry.width = 1024; geometry.height = 512; geometry.block = 32;
    map.Configure(geometry); map.Build(p, 20, 0);
    auto original = map.deltas;
    map.Build(p, 20, 6);
    Check(map.deltas[8 * 32 + 8] == original[8 * 32 + 8]
        && map.deltas[8 * 32 + 24] == original[8 * 32 + 24], "network policy penalizes protected center");
    Check(map.deltas[0] > original[0], "network policy did not reduce background quality");
    p.regions[0] = {0, 0, 1, 1, 0, 1}; p.regions[1] = {0, 0, 1, 1, 1, 1}; map.Build(p, 20, 6);
    Check(std::all_of(map.deltas.begin(), map.deltas.end(), [](int8_t q) { return q == 0; }), "full-view fallback is penalized");
    std::cout << "{\"regional_quality_checks\":" << checks << "}\n";
}
