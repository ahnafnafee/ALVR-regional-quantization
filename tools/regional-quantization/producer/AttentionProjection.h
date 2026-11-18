#pragma once
#include "AttentionMath.h"
#include <array>
#include <limits>

namespace gvl::streaming::scene
{
inline bool Bounds(const Shape &shape, const Matrix &m, float nearPlane, float guard, Rect &result)
{
    if (shape.count < 1 || shape.count > 8)
        return false;
    std::array<Clip, 8> clips{};
    const float inf = std::numeric_limits<float>::infinity();
    Rect r{inf, inf, -inf, -inf};
    bool front = false;
    auto include = [&](Clip c) {
        const float x = .5f + .5f * c.x / c.w, y = .5f - .5f * c.y / c.w;
        r = {std::min(r.left, x), std::min(r.top, y), std::max(r.right, x), std::max(r.bottom, y)};
    };
    for (uint32_t i = 0; i < shape.count; ++i)
    {
        clips[i] = Project(m, shape.points[i]);
        if (!Finite(clips[i]))
            return false;
        if (clips[i].w >= nearPlane)
        {
            front = true;
            include(clips[i]);
        }
    }
    if (!front)
        return false;
    for (uint32_t i = 0; i < shape.count; ++i)
        for (uint32_t j = i + 1; j < shape.count; ++j)
            if ((clips[i].w < nearPlane) != (clips[j].w < nearPlane))
                include(Cut(clips[i], clips[j], nearPlane));
    result = Guard(r, guard);
    return Positive(result);
}
inline uint32_t Road(const Shape &shape, const Matrix &m, float nearPlane, float guard, std::array<Rect, 3> &out)
{
    if (shape.count != 4)
        return 0;
    std::array<Clip, 4> clips{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        clips[i] = Project(m, shape.points[i]);
        if (!Finite(clips[i]))
            return 0;
    }
    struct V2
    {
        float x, y;
    };
    std::array<V2, 8> polygon{};
    uint32_t vertices = 0;
    auto append = [&](Clip c) { polygon[vertices++] = {.5f + .5f * c.x / c.w, .5f - .5f * c.y / c.w}; };
    for (uint32_t i = 0; i < 4; ++i)
    {
        Clip a = clips[(i + 3) % 4], b = clips[i];
        if ((a.w < nearPlane) != (b.w < nearPlane))
            append(Cut(a, b, nearPlane));
        if (b.w >= nearPlane)
            append(b);
    }
    if (vertices < 3)
        return 0;
    float minY = 1, maxY = 0;
    for (uint32_t i = 0; i < vertices; ++i)
    {
        minY = std::min(minY, polygon[i].y);
        maxY = std::max(maxY, polygon[i].y);
    }
    minY = Clamp(minY, 0, 1);
    maxY = Clamp(maxY, 0, 1);
    if (maxY <= minY)
        return 0;
    const uint32_t bands = uint32_t(Clamp(std::ceil((maxY - minY) / .18f), 1, 3));
    uint32_t count = 0;
    for (uint32_t band = 0; band < bands; ++band)
    {
        const float lo = minY + (maxY - minY) * (float(band) / bands),
                    hi = minY + (maxY - minY) * (float(band + 1) / bands);
        float minX = std::numeric_limits<float>::infinity(), maxX = -minX;
        auto include = [&](float x) {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        };
        for (uint32_t i = 0; i < vertices; ++i)
        {
            const V2 a = polygon[i], b = polygon[(i + 1) % vertices];
            if (a.y >= lo && a.y <= hi)
                include(a.x);
            if (std::abs(b.y - a.y) < 1e-8f)
                continue;
            for (float y : {lo, hi})
            {
                const float t = (y - a.y) / (b.y - a.y);
                if (t >= 0 && t <= 1)
                    include(a.x + (b.x - a.x) * t);
            }
        }
        if (!std::isfinite(minX))
            continue;
        Rect r = Guard({minX, lo, maxX, hi}, guard);
        if (Positive(r))
            out[count++] = r;
    }
    return count;
}
} // namespace gvl::streaming::scene
