#pragma once
#include "AttentionPacket.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gvl::streaming::alvr {
using namespace gvl::streaming;

struct Axis {
    float center = 1, shift = 0, ratio = 1, sizeRatio = 1;
    // Matches ALVR 20.14.1 CompressAxisAlignedPixelShader.hlsl, including its
    // quadratic edge transition. Input is the ENCODED coordinate; output is source UV.
    float Source(float encoded) const {
        const float u = std::clamp(encoded / sizeRatio, 0.0f, 1.0f);
        const float c0 = (1 - center) * 0.5f;
        const float c1 = (ratio - 1) * c0 * (shift + 1) / ratio;
        const float c2 = (ratio - 1) * center + 1;
        const float lo = c0 * (shift + 1) / c2;
        const float hi = c0 * (shift - 1) / c2 + 1;
        const float mid = u * c2 / ratio + c1;
        if (u < lo && lo > 0) {
            const float g = u / lo;
            return g * mid + (1 - g) * u * c2;
        }
        if (u > hi && hi < 1) {
            const float g = (1 - u) / (1 - hi);
            return g * mid + (1 - g) * ((u - 1) * c2 + 1);
        }
        return mid;
    }
};
inline Axis AlignedAxis(float pixels, float center, float shift, float ratio) {
    Axis a;
    a.ratio = std::max(1.0f, ratio);
    a.center = 1 - std::ceil((pixels - center * pixels) / (a.ratio * 2)) * (a.ratio * 2) / pixels;
    const float edge = pixels * (1 - a.center);
    a.shift = edge > 0 ? std::ceil(shift * edge / (a.ratio * 2)) * (a.ratio * 2) / edge : 0;
    const float optimized = (a.center + (1 - a.center) / a.ratio) * pixels;
    a.sizeRatio = optimized / (std::ceil(optimized / 32) * 32);
    return a;
}
struct Geometry {
    uint32_t width = 0, height = 0, block = 32;
    Axis x, y;
    bool foveated = false;
};

inline float RegionCoverage(const Region& r, const Region& b, float padding) {
    const float dx = std::max({r.left - b.right, b.left - r.right, 0.0f});
    const float dy = std::max({r.top - b.bottom, b.top - r.bottom, 0.0f});
    if ((r.kind & 255u) != 1u) {
        // Text and safety objects remain protected even when smaller than a coding block.
        const float distance = std::max(dx, dy);
        return distance == 0 ? 1.0f : padding > 0 ? std::max(0.0f, 1 - distance / padding) : 0;
    }
    const float area = (b.right - b.left) * (b.bottom - b.top);
    if (area <= 0) return 0;
    auto coverage = [&](float expand) {
        const float w = std::max(0.0f, std::min(r.right + expand, b.right) - std::max(r.left - expand, b.left));
        const float h = std::max(0.0f, std::min(r.bottom + expand, b.bottom) - std::max(r.top - expand, b.top));
        return std::clamp(w * h / area, 0.0f, 1.0f);
    };
    // Road bands contain empty corner space. A glancing intersection should not
    // promote an entire block; retain a modest feather without weakening small HUD text.
    return std::max(coverage(0), padding > 0 ? .25f * coverage(padding) : 0);
}

// The selected B_floor12 policy uses the same bounded, frame-local budget solver.
// Selection does not qualify the policy: independent holdout and live gates remain pending.
struct MapTuning {
    int maximumBoost = 12, headroom = 1;
    float contrastExponent = 1, minimumRoadImportance = 1, minimumHazardImportance = 1;
    float rateProxyTarget = 1;
};
class MapBuilder {
public:
    std::vector<int8_t> deltas;
    float protectedFraction = 0;
    float highPriorityFraction = 0, intermediateFraction = 0, backgroundFraction = 0;
    int minDelta = 0, maxDelta = 0;
    double rateProxy = 1;
    int budgetShift = 0;

    void Configure(const Geometry& g) {
        geometry = g;
        columns = (g.width + g.block - 1) / g.block;
        rows = (g.height + g.block - 1) / g.block;
        deltas.resize(size_t(columns) * rows);
        protection.resize(deltas.size());
        weights.resize(deltas.size());
        backgroundScale.resize(deltas.size());
        footprints.resize(deltas.size());
        const float eyeWidth = g.width / 2.0f;
        for (uint32_t row = 0; row < rows; ++row) for (uint32_t col = 0; col < columns; ++col) {
            const float px0 = float(col * g.block), px1 = std::min(float((col + 1) * g.block), float(g.width));
            const uint32_t eye = px0 >= eyeWidth ? 1 : 0;
            float x0 = (px0 - eye * eyeWidth) / eyeWidth;
            float x1 = (px1 - eye * eyeWidth) / eyeWidth;
            float y0 = float(row * g.block) / g.height;
            float y1 = std::min(float((row + 1) * g.block) / g.height, 1.0f);
            if (g.foveated) {
                if (eye == 1) { x0 = 1 - x0; x1 = 1 - x1; }
                x0 = g.x.Source(x0); x1 = g.x.Source(x1);
                if (eye == 1) { x0 = 1 - x0; x1 = 1 - x1; }
                y0 = g.y.Source(y0); y1 = g.y.Source(y1);
            }
            const size_t index = size_t(row) * columns + col;
            footprints[index] = {
                std::min(x0, x1), y0, std::max(x0, x1), y1, eye, 0
            };
            // Follow the head/lens-aligned central region already reconstructed by
            // the client. Importance overrides this falloff, including at the edges.
            float cx = (x0 + x1) * 0.5f;
            if (eye == 1) cx = 1 - cx;
            const float cy = (y0 + y1) * 0.5f;
            auto edgeDistance = [](float u, float center, float shift) {
                const float lo = (1 - center) * 0.5f * (shift + 1);
                const float hi = lo + center;
                if (u < lo) return lo > 0 ? (lo - u) / lo : 0.0f;
                if (u > hi) return hi < 1 ? (u - hi) / (1 - hi) : 0.0f;
                return 0.0f;
            };
            const float distance = std::clamp(std::max(
                edgeDistance(cx, g.foveated ? g.x.center : .5f, g.foveated ? g.x.shift : 0),
                edgeDistance(cy, g.foveated ? g.y.center : .5f, g.foveated ? g.y.shift : 0)), 0.0f, 1.0f);
            const float smooth = distance * distance * (3 - 2 * distance);
            backgroundScale[index] = 1 - .5f * smooth;
        }
    }

    void Build(const Packet& p, float streamMbps = 20.0f, float extraPeripheralQp = 0, MapTuning tuning = {},
               const std::vector<float>* codingCosts = nullptr) {
        tuning.maximumBoost = std::clamp(tuning.maximumBoost, 1, 12);
        tuning.headroom = std::clamp(tuning.headroom, 0, 3);
        tuning.contrastExponent = std::isfinite(tuning.contrastExponent)
            ? std::clamp(tuning.contrastExponent, .5f, 1.0f) : .5f;
        auto clampImportance = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; };
        tuning.minimumRoadImportance = clampImportance(tuning.minimumRoadImportance);
        tuning.minimumHazardImportance = clampImportance(tuning.minimumHazardImportance);
        tuning.rateProxyTarget = std::isfinite(tuning.rateProxyTarget)
            ? std::clamp(tuning.rateProxyTarget, .75f, 1.3f) : 1;
        extraPeripheralQp = std::isfinite(extraPeripheralQp) ? std::clamp(extraPeripheralQp, 0.0f, 12.0f) : 0;
        std::fill(protection.begin(), protection.end(), 0.0f);
        std::array<float, MaxRegions> importance{};
        for (uint32_t j = 0; j < p.count; ++j) {
            const uint32_t tag = p.regions[j].kind & 255u;
            const float minimum = tag == 1 ? tuning.minimumRoadImportance : tag == 4 ? tuning.minimumHazardImportance : 0;
            importance[j] = std::max(Importance(p.regions[j]), minimum);
        }
        // Geometry is cached by Configure. At frame time, reject regions once per
        // row/eye instead of testing every region against every coding block.
        // Preserve packet order and the exact feather arithmetic within each row.
        struct Candidate { uint32_t region; float dy; };
        std::array<std::array<Candidate, MaxRegions>, 2> candidates{};
        for (uint32_t row = 0; columns > 0 && row < rows; ++row) {
            std::array<uint32_t, 2> count{};
            const auto& rowBounds = footprints[size_t(row) * columns];
            for (uint32_t j = 0; j < p.count; ++j) {
                const auto& r = p.regions[j];
                const float dy = std::max({r.top - rowBounds.bottom, rowBounds.top - r.bottom, 0.0f});
                if (dy == 0 || (p.padding > 0 && dy < p.padding))
                    candidates[r.eye][count[r.eye]++] = {j, dy};
            }
            for (uint32_t col = 0; col < columns; ++col) {
                const size_t i = size_t(row) * columns + col;
                const auto& b = footprints[i];
                for (uint32_t k = 0; k < count[b.eye]; ++k) {
                    const auto& candidate = candidates[b.eye][k];
                    const auto& r = p.regions[candidate.region];
                    // Block intersection protects thin text and small hazards; padding feathers
                    // quality into adjacent blocks instead of making a hard rectangular seam.
                    const float value = RegionCoverage(r, b, p.padding);
                    protection[i] = std::max(protection[i], value * importance[candidate.region]);
                    if (protection[i] >= 1) break;
                }
            }
        }
        double total = 0;
        size_t high = 0, intermediate = 0, background = 0;
        for (float v : protection) {
            total += v;
            if (v >= .99999f) ++high;
            else if (v > 0) ++intermediate;
            else ++background;
        }
        protectedFraction = protection.empty() ? 0 : float(total / protection.size());
        highPriorityFraction = protection.empty() ? 0 : float(high) / protection.size();
        intermediateFraction = protection.empty() ? 0 : float(intermediate) / protection.size();
        backgroundFraction = protection.empty() ? 0 : float(background) / protection.size();
        // Optional development feedback uses measured prior-frame coding costs.
        // Costs affect the budget only; the current game's priority packet stays current.
        bool validCosts = codingCosts && codingCosts->size() == protection.size();
        if (validCosts) for (float cost : *codingCosts)
            if (!std::isfinite(cost) || cost <= 0 || cost > 100) { validCosts = false; break; }
        auto costAt = [&](size_t index) { return validCosts ? double((*codingCosts)[index]) : 1.0; };
        double totalWeight = 0, totalCost = 0;
        for (size_t i = 0; i < protection.size(); ++i) {
            const float background = p.backgroundWeight * backgroundScale[i];
            const float value = background + (p.importantWeight - background) * protection[i];
            weights[i] = tuning.contrastExponent == .5f ? std::sqrt(value) : std::pow(value, tuning.contrastExponent);
            totalWeight += weights[i] * costAt(i);
            totalCost += costAt(i);
        }
        const float meanWeight = totalCost <= 0 ? p.backgroundWeight : float(totalWeight / totalCost);
        // Importance is dimensionless. Normalize by this frame's mean weight, never
        // by the Mbps target. Rescaling all weights must leave the QP request unchanged.
        (void)streamMbps;
        const bool uniform = protectedFraction >= .99999f || p.importantWeight == p.backgroundWeight;
        const float normalizer = std::max(meanWeight, .00001f);
        // Bounded gains prevent a tiny region from asking for an eight-QP boost.
        // NVENC still owns actual rate control; this proxy is not a bit-rate measurement.
        constexpr int floor = -12, maximum = 12;
        const int minimum = -tuning.maximumBoost;
        std::array<double, maximum - floor + 1> histogram{};
        minDelta = 0; maxDelta = 0;
        for (size_t i = 0; i < deltas.size(); ++i) {
            // Temper the authored weight contrast: extreme 70/3 inputs should not
            // collapse intermediate detail into the same saturated boost as critical text.
            const float weight = uniform ? normalizer : weights[i];
            // Rate weights, NOT independent Mbps channels. The 6*log2 mapping is a
            // tunable quantizer heuristic; NVENC's global rate control owns actual bits.
            // One QP step of headroom reduced overshoot in the hardware stress test.
            const int baseDelta = uniform ? 0 : std::clamp(int(std::lround(-6 * std::log2(weight / normalizer))), minimum, maximum);
            // Network pressure never removes the full-priority center/HUD/hazard boost.
            // It changes intermediate/background quantization before reducing global load.
            const int delta = uniform ? 0 : std::clamp(baseDelta
                + int(std::lround(extraPeripheralQp * (1 - protection[i]))), minimum, maximum);
            deltas[i] = int8_t(delta);
            histogram[size_t(delta - floor)] += costAt(i);
        }
        // Clamping and integer rounding can undo mean-weight normalization. Solve on
        // the small QP histogram rather than iterating exp/log for every block. One
        // fixed QP of headroom limits overshoot; there is no history, lookahead or queue.
        budgetShift = uniform ? 0 : tuning.headroom;
        auto proxy = [&](int shift) {
            double totalRate = 0;
            for (int d = minimum; d <= maximum; ++d)
                totalRate += double(histogram[size_t(d - floor)]) * std::exp2(-std::min(d + shift, maximum) / 6.0);
            return totalCost <= 0 ? 1.0 : totalRate / totalCost;
        };
        while (!uniform && budgetShift < maximum - minimum && proxy(budgetShift) > tuning.rateProxyTarget)
            ++budgetShift;
        rateProxy = proxy(budgetShift);
        minDelta = deltas.empty() ? 0 : maximum;
        maxDelta = deltas.empty() ? 0 : minimum;
        for (auto& delta : deltas) {
            delta = int8_t(std::min(int(delta) + budgetShift, maximum));
            minDelta = std::min(minDelta, int(delta)); maxDelta = std::max(maxDelta, int(delta));
        }
    }
private:
    Geometry geometry;
    uint32_t columns = 0, rows = 0;
    std::vector<float> protection, backgroundScale, weights;
    std::vector<Region> footprints;
};
}
