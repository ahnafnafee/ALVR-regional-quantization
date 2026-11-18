#include "AttentionMap.h"
#include "RowCodingCost.h"
#include "ReplayGeometry.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gvl::streaming::alvr;
static int checks = 0;
void Check(bool result, const char* message) { ++checks; if (!result) throw std::runtime_error(message); }
int main() try {
    Packet p{};
    p.magic = Magic; p.version = Version; p.bytes = sizeof(p); p.producerPid = 123;
    p.sequence = 1; p.qpc = 1000; p.frequency = 1000; p.count = 2;
    p.importantWeight = 30; p.backgroundWeight = 10; p.maxAgeMs = 75; p.padding = 0;
    p.regions[0] = {0.25f, 0.25f, 0.75f, 0.75f, 0, 1};
    p.regions[1] = {0.25f, 0.25f, 0.75f, 0.75f, 1, 1};
    Check(Valid(p, 1020, 1000, 123), "valid packet rejected");
    Check(!Valid(p, 1020, 1000, 124), "foreign app accepted");
    Check(!Valid(p, 1076, 1000, 123), "expired packet accepted");
    Check(!Valid(p, 999, 1000, 123), "future packet accepted");
    Check(!Valid(p, 1020, 2000, 123), "wrong clock accepted");
    auto bad = p; bad.count = 65;
    Check(!Valid(bad, 1020, 1000, 123), "oversized packet accepted");
    bad = p; bad.regions[0].left = std::numeric_limits<float>::quiet_NaN();
    Check(!Valid(bad, 1020, 1000, 123), "NaN accepted");
    bad = p; bad.count = 0;
    Check(!Valid(bad, 1020, 1000, 123), "disabled packet accepted");
    MapBuilder map;
    Geometry geometry; geometry.width = 1024; geometry.height = 512; geometry.block = 32;
    map.Configure(geometry); map.Build(p);
    Check(map.deltas.size() == 32 * 16, "HEVC CTB dimensions wrong");
    Check(map.deltas[8 * 32 + 8] < 0, "left important center not protected");
    Check(map.deltas[8 * 32 + 24] < 0, "right important center not protected");
    Check(map.deltas[0] > 0, "background not deprioritized");
    Check(map.rateProxy <= 1.0 && map.minDelta >= -12 && map.maxDelta <= 12,
        "bounded map exceeds its relative rate proxy");
    const auto fixedBudget = map.deltas;
    map.Build(p, 4);
    Check(map.deltas == fixedBudget, "dimensionless priority changed with nominal Mbps");
    auto scaled = p; scaled.importantWeight *= 2; scaled.backgroundWeight *= 2;
    map.Build(scaled, 80);
    Check(map.deltas == fixedBudget, "rescaling both relative weights changed the QP map");
    map.Build(p);
    Check(map.highPriorityFraction == map.protectedFraction, "legacy binary priority changed");
    auto tiered = p;
    tiered.regions[1].kind = 0x66660103u; // Unity wire tag: racer, importance 0.4.
    Check(std::abs(Importance(tiered.regions[1]) - .4f) < .00001f && Importance(tiered.regions[0]) == 1, "wire priority compatibility");
    map.Build(tiered);
    Check(map.deltas[8 * 32 + 8] < map.deltas[8 * 32 + 24]
        && map.deltas[8 * 32 + 24] < map.deltas[31], "intermediate region is not between important and background");
    Check(map.highPriorityFraction > 0 && map.intermediateFraction > 0 && map.backgroundFraction > 0
        && std::abs(map.highPriorityFraction + map.intermediateFraction + map.backgroundFraction - 1) < .00001f,
        "separate coverage counters do not partition blocks");
    tiered.regions[2] = tiered.regions[1]; tiered.regions[2].kind = 4; tiered.count = 3;
    map.Build(tiered);
    Check(map.deltas[8 * 32 + 8] == map.deltas[8 * 32 + 24], "near hazard must override intermediate road");
    auto small = p;
    small.regions[0] = {.45f, .45f, .55f, .55f, 0, 1};
    small.regions[1] = {.45f, .45f, .55f, .55f, 1, 1};
    map.Build(small);
    Check(map.deltas[0] > map.deltas[6 * 32 + 6], "periphery should cost fewer bits than central background");
    small.regions[2] = {0, 0, .125f, .125f, 0, 4}; small.count = 3;
    map.Build(small);
    // A full hazard intersection must receive at least the protection of a partially covered road block.
    Check(map.deltas[0] <= map.deltas[8 * 32 + 8], "edge hazard must override peripheral quality reduction");
    auto cheaperBackground = p; cheaperBackground.backgroundWeight = 6;
    map.Build(p); const auto originalBackground = map.deltas[0];
    map.Build(cheaperBackground);
    Check(map.deltas[0] > originalBackground && map.deltas[8 * 32 + 8] < 0,
        "lower background weight should further reduce background and retain important regions");
    p.count = 1; map.Build(p);
    Check(map.deltas[8 * 32 + 8] < map.deltas[8 * 32 + 24], "eye identities lost");
    p.regions[0] = {0, 0, 1, 1, 0, 1}; p.regions[1] = {0, 0, 1, 1, 1, 1}; p.count = 2;
    map.Build(p);
    Check(std::all_of(map.deltas.begin(), map.deltas.end(), [](int8_t v){return v == 0;}), "all-protected fallback changes global quality");
    Axis identity;
    Check(std::abs(identity.Source(0.3f) - 0.3f) < 1e-6f, "identity warp");
    const Axis warped = AlignedAxis(2624, 0.45f, 0.4f, 4);
    float previous = -1;
    for (int i = 0; i <= 1000; ++i) {
        float value = warped.Source(i / 1000.0f);
        Check(std::isfinite(value) && value >= previous - 1e-5f && value >= -1e-5f && value <= 1.00001f, "warp nonmonotonic or outside source");
        previous = value;
    }
    geometry.foveated = true; geometry.x = warped; geometry.y = AlignedAxis(2860, .4f, .1f, 5);
    map.Configure(geometry); map.Build(p);
    Check(std::all_of(map.deltas.begin(), map.deltas.end(), [](int8_t v){return v == 0;}), "full protection lost through warp");
    p.importantWeight = p.backgroundWeight = 10; p.count = 1;
    map.Build(p);
    Check(std::all_of(map.deltas.begin(), map.deltas.end(), [](int8_t v){return v == 0;}), "equal weights should disable regional difference");
    geometry.x = AlignedAxis(2144, .45f, .4f, 6);
    geometry.y = AlignedAxis(2336, .4f, .1f, 6);
    map.Configure(geometry); p = cheaperBackground; map.Build(p);
    Check(map.minDelta < 0 && map.maxDelta > 0, "stronger peripheral sampling must preserve separate quality levels");
    Region block{.4f, .4f, .5f, .5f, 0, 0};
    Region sliver{.499f, .4f, .6f, .5f, 0, 1};
    Check(RegionCoverage(sliver, block, 0) < .011f, "road sliver boosts an entire intersected block");
    sliver.kind = 2;
    Check(RegionCoverage(sliver, block, 0) == 1, "small HUD text lost block protection");
    sliver.kind = 4;
    Check(RegionCoverage(sliver, block, 0) == 1, "small safety object lost block protection");
    geometry.foveated = false; map.Configure(geometry);
    for (int coverage = 1; coverage < 100; ++coverage) {
        auto dense = cheaperBackground;
        const float right = coverage / 100.0f;
        dense.regions[0] = {0, 0, right, 1, 0, 2};
        dense.regions[1] = {0, 0, right, 1, 1, 2};
        dense.importantWeight = 70; dense.backgroundWeight = 3;
        map.Build(dense);
        Check(map.rateProxy <= 1.000001 && map.minDelta >= -12 && map.maxDelta <= 12,
            "clamped/rounded dense priorities oversubscribe the rate proxy");
        for (int boost : {4, 6, 8, 12}) {
            map.Build(dense, 16, 0, {boost, 0, 1});
            Check(map.rateProxy <= 1.000001 && map.minDelta >= -boost && map.maxDelta <= 12,
                "development policy oversubscribes its frame-local budget");
            const auto candidate = map.deltas;
            auto rescaled = dense; rescaled.importantWeight /= 2; rescaled.backgroundWeight /= 2;
            map.Build(rescaled, 16, 0, {boost, 0, 1});
            Check(candidate == map.deltas, "development policy changed under weight rescaling");
        }
    }
    ReplayGeometry replay(geometry);
    Check(replay.Valid(), "valid replay geometry rejected");
    replay.width = 63; Check(!replay.Valid(), "split-eye unaligned replay accepted");
    replay = ReplayGeometry(geometry); replay.yRatio = std::numeric_limits<float>::quiet_NaN();
    Check(!replay.Valid(), "nonfinite replay warp accepted");
    auto distant = cheaperBackground;
    distant.regions[0].kind = 0x66660101u;
    distant.regions[1].kind = 0x66660104u;
    auto fullSemantic = distant;
    fullSemantic.regions[0].kind = 1; fullSemantic.regions[1].kind = 4;
    map.Build(fullSemantic); const auto fullImportance = map.deltas;
    map.Build(distant);
    Check(map.deltas == fullImportance,
        "selected B weakened low-importance road or hazard blocks instead of retaining their semantic floor");
    distant.regions[1].kind = 0x66660103u; // Racers retain their graded importance.
    map.Build(distant);
    Check(map.deltas[8 * 32 + 8] < map.deltas[8 * 32 + 24],
        "selected B applied the safety floor to an ordinary racer");
    distant.regions[1].kind = 0x66660104u;
    map.Build(distant, 16, 0, {8, 0, 1, .7f, 1});
    Check(map.deltas[8 * 32 + 8] < map.deltas[31], "road priority floor does not protect the distant road");
    Check(map.deltas[8 * 32 + 24] <= map.deltas[8 * 32 + 8], "safety-object floor lost priority to road");
    Check(map.rateProxy <= 1.000001, "semantic priority floors bypassed the frame-local budget");
    for (float target : {.75f, 1.15f, 1.3f}) {
        map.Build(distant, 16, 0, {8, 0, 1, .7f, 1, target});
        Check(map.rateProxy <= target + .000001, "map exceeded configured development proxy bound");
    }
    map.Build(cheaperBackground); const auto unweighted = map.deltas;
    std::vector<float> codingCosts(map.deltas.size(), 1);
    map.Build(cheaperBackground, 16, 0, {}, &codingCosts);
    Check(map.deltas == unweighted, "unit coding costs changed the frozen map");
    codingCosts[0] = std::numeric_limits<float>::quiet_NaN();
    map.Build(cheaperBackground, 16, 0, {}, &codingCosts);
    Check(map.deltas == unweighted, "invalid feedback contaminated the map");
    map.Build(cheaperBackground, 16, 0, {3, 3});
    Check(map.minDelta == *std::min_element(map.deltas.begin(), map.deltas.end())
        && map.maxDelta == *std::max_element(map.deltas.begin(), map.deltas.end()),
        "reported quantizer extrema include an unapplied zero offset");
    for (size_t i = 0; i < codingCosts.size(); ++i) codingCosts[i] = i % 2 ? .05f : 20.f;
    map.Build(cheaperBackground, 16, 0, {12, 0, 1}, &codingCosts);
    Check(map.rateProxy <= 1.000001 && map.minDelta >= -12 && map.maxDelta <= 12,
        "measured coding costs bypassed the bounded budget");
    const auto weighted = map.deltas;
    for (auto& cost : codingCosts) cost *= 2;
    map.Build(cheaperBackground, 16, 0, {12, 0, 1}, &codingCosts);
    Check(map.deltas == weighted, "coding cost scale changed relative allocation");
    RowCodingCost feedback; feedback.Configure(64, 64);
    uint32_t bits[2]{100, 100}; uint8_t qp[2]{30, 36};
    Check(feedback.Observe(bits, qp, 2, 30), "actual coded rows were rejected");
    Check(feedback.blocks[0] < feedback.blocks[2], "coding cost ignored the measured quantizer");
    const auto oldCosts = feedback.blocks;
    qp[1] = 99;
    Check(!feedback.Observe(bits, qp, 2, 30) && feedback.blocks == oldCosts,
        "invalid row statistics altered feedback state");
    feedback.Configure(128, 32);
    Check(feedback.blocks.size() == 4 && std::all_of(feedback.blocks.begin(), feedback.blocks.end(), [](float v){return v == 1;}),
        "geometry change retained stale coding costs");
    std::cout << checks << " checks passed\n";
}
catch (const std::exception& error) {
    std::cerr << "FAIL at check " << checks << ": " << error.what() << '\n';
    return 1;
}
