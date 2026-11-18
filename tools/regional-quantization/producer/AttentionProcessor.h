#pragma once
#include "AttentionPrediction.h"
#include "AttentionProjection.h"

namespace gvl::streaming::scene
{
inline float Importance(float distance, float full, float range, float intermediate)
{
    const float t = Clamp((distance - full) / (std::max(full + 1, range) - full), 0, 1);
    return 1 + (intermediate - 1) * (t * t * (3 - 2 * t));
}
class Processor
{
    Motion motion;
    std::array<Region, MaxRegions> deferred{};
    uint32_t deferredCount = 0;
    bool fullView = false;
    std::array<Rect, 2> focus{};
    void Add(Packet &p, Rect r, uint32_t eye, uint32_t kind, float priority)
    {
        if (fullView || !Positive(r))
            return;
        Region region{r.left, r.top, r.right, r.bottom, eye, kind};
        if (priority < .99999f || kind == 1 || kind == 3)
        {
            // Unity Mathf.RoundToInt uses ties-to-even, as nearbyint does in default mode.
            const uint32_t rank = uint32_t(std::nearbyint(Clamp(priority, 0, 1) * 65535));
            auto rankOf = [](const Region& r) { return (r.kind & 0x100u) ? r.kind >> 16 : 65535u; };
            if (deferredCount == MaxRegions && rank <= rankOf(deferred.back()))
                return;
            uint32_t insert = std::min(deferredCount, MaxRegions - 1);
            while (insert > 0 && rankOf(deferred[insert - 1]) < rank)
            {
                deferred[insert] = deferred[insert - 1];
                --insert;
            }
            if (priority < .99999f) region.kind = (kind & 0xffu) | 0x100u | (rank << 16);
            deferred[insert] = region;
            deferredCount = std::min(deferredCount + 1, MaxRegions);
            return;
        }
        if (p.count >= MaxRegions)
        {
            Full(p);
            return;
        }
        p.regions[p.count++] = region;
    }
    void Full(Packet &p)
    {
        fullView = true;
        p.count = 2;
        p.regions[0] = {0, 0, 1, 1, 0, 4};
        p.regions[1] = {0, 0, 1, 1, 1, 4};
    }
    void Focused(Packet &p, Rect r, uint32_t eye, uint32_t kind, float priority, float intermediate)
    {
        Add(p, r, eye, kind, std::min(priority, intermediate));
        Add(p, Intersection(r, focus[eye]), eye, kind, priority);
    }

  public:
    void Reset()
    {
        motion.Reset();
    }
    void Process(const Frame &f, const Shape *shapes, uint32_t pid, uint64_t sequence, Packet &p)
    {
        p = {};
        p.magic = Magic;
        p.version = Version;
        p.bytes = sizeof(p);
        p.producerPid = pid;
        p.sequence = sequence;
        p.qpc = f.qpc;
        p.frequency = f.frequency;
        p.frame = f.frame;
        const Settings &s = f.settings;
        p.importantWeight = Clamp(s.important, 1, 100);
        p.backgroundWeight = Clamp(s.background, 1, p.importantWeight);
        p.padding = s.feather;
        p.maxAgeMs = Clamp(s.lifetime, 8, 100);
        deferredCount = 0;
        fullView = false;
        if (!(f.flags & Active) || f.count > MaxShapes || !shapes || !std::isfinite(f.nearPlane) || f.nearPlane <= 0)
        {
            Reset();
            return;
        }
        for (float setting : {s.important, s.background, s.lifetime, s.guard, s.feather, s.width, s.height, s.horizon,
                              s.maxDegrees, s.rapidTurn, s.intermediate})
            if (!std::isfinite(setting))
            {
                Reset();
                return;
            }
        if (!std::isfinite(Norm(f.orientation)) || std::abs(Norm(f.orientation) - 1) > .01f)
        {
            Reset();
            return;
        }
        float speed = 0;
        Q lead{};
        if (!motion.Sample(f.tracked, f.time, s.horizon, s.maxDegrees, speed, lead))
            return;
        if (speed >= s.rapidTurn)
            p.maxAgeMs = 8;
        if (f.flags & Overflow)
        {
            Full(p);
            return;
        }
        const float guard = s.guard + std::min(.05f, speed / 4000);
        for (uint32_t eye = 0; eye < 2; ++eye)
            focus[eye] = (f.flags & Predictive) ? Sweep(f, eye, lead)
                                                : Focus(f.matrices[eye], f.position + Rotate(f.orientation, {0, 0, 10}),
                                                        s.width, s.height, guard);
        for (uint32_t i = 0; i < f.count; ++i)
        {
            const Shape &shape = shapes[i];
            if (shape.kind > 255 || !std::isfinite(shape.distance) || !std::isfinite(shape.fullUntil) ||
                !std::isfinite(shape.fadeUntil) || !std::isfinite(shape.minimum))
                continue;
            const bool selective = (f.flags & Selective) != 0;
            const float priority =
                !selective ? 1
                           : Importance(shape.distance, shape.fullUntil, shape.fadeUntil, Clamp(shape.minimum, 0, 1));
            for (uint32_t eye = 0; eye < 2; ++eye)
            {
                std::array<Rect, 3> regions{};
                uint32_t count = 0;
                if (shape.projection == 1)
                    count = Road(shape, f.matrices[eye], f.nearPlane, guard, regions);
                else if (Bounds(shape, f.matrices[eye], f.nearPlane, guard, regions[0]))
                    count = 1;
                for (uint32_t j = 0; j < count; ++j)
                {
                    if (selective && shape.clipToFocus)
                        Focused(p, regions[j], eye, shape.kind, priority, s.intermediate);
                    else
                        Add(p, regions[j], eye, shape.kind, priority);
                }
            }
        }
        if (!fullView)
            for (uint32_t i = 0; i < deferredCount && p.count < MaxRegions; ++i)
                p.regions[p.count++] = deferred[i];
    }
};
} // namespace gvl::streaming::scene
