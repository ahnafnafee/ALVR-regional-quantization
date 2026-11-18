#pragma once
#include "AttentionTypes.h"
#include <algorithm>

namespace gvl::streaming::scene
{
inline float Clamp(float x, float lo, float hi)
{
    return std::clamp(x, lo, hi);
}
inline V3 operator+(V3 a, V3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator*(V3 a, float b)
{
    return {a.x * b, a.y * b, a.z * b};
}
inline Q Multiply(Q a, Q b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline V3 Rotate(Q q, V3 v)
{
    // Same expanded unit-quaternion rotation as Unity's Quaternion * Vector3.
    const float x = q.x * 2, y = q.y * 2, z = q.z * 2;
    return {(1 - q.y * y - q.z * z) * v.x + (q.x * y - q.w * z) * v.y + (q.x * z + q.w * y) * v.z,
            (q.x * y + q.w * z) * v.x + (1 - q.x * x - q.z * z) * v.y + (q.y * z - q.w * x) * v.z,
            (q.x * z - q.w * y) * v.x + (q.y * z + q.w * x) * v.y + (1 - q.x * x - q.y * y) * v.z};
}
inline float Norm(Q q)
{
    return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
}
inline Q Normalize(Q q)
{
    const float s = 1 / std::sqrt(Norm(q));
    return {q.x * s, q.y * s, q.z * s, q.w * s};
}
struct Clip
{
    float x, y, w;
};
inline Clip Project(const Matrix &m, V3 p)
{
    return {m.v[0] * p.x + m.v[4] * p.y + m.v[8] * p.z + m.v[12], m.v[1] * p.x + m.v[5] * p.y + m.v[9] * p.z + m.v[13],
            m.v[3] * p.x + m.v[7] * p.y + m.v[11] * p.z + m.v[15]};
}
inline bool Finite(Clip p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.w);
}
inline Clip Cut(Clip a, Clip b, float nearPlane)
{
    const float t = (nearPlane - a.w) / (b.w - a.w);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, nearPlane};
}
struct Rect
{
    float left, top, right, bottom;
};
inline bool Positive(Rect r)
{
    return r.right > r.left && r.bottom > r.top;
}
inline Rect Guard(Rect r, float g)
{
    return {Clamp(r.left - g, 0, 1), Clamp(r.top - g, 0, 1), Clamp(r.right + g, 0, 1), Clamp(r.bottom + g, 0, 1)};
}
inline Rect Intersection(Rect a, Rect b)
{
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
inline Rect Focus(const Matrix &m, V3 p, float w, float h, float guard)
{
    auto c = Project(m, p);
    if (!Finite(c) || c.w <= 0)
        return {};
    const float x = .5f + .5f * c.x / c.w, y = .5f - .5f * c.y / c.w;
    return Guard({x - w * .5f, y - h * .5f, x + w * .5f, y + h * .5f}, guard);
}
inline Rect Sweep(const Frame &f, uint32_t eye, Q lead)
{
    const auto &s = f.settings;
    Rect result = Focus(f.matrices[eye], f.position + Rotate(f.orientation, {0, 0, 10}), s.width, s.height, s.guard);
    if (!Positive(result))
        return result;
    const Q half = Normalize({lead.x, lead.y, lead.z, 1 + lead.w});
    for (Q turn : {half, lead})
    {
        Rect next = Focus(f.matrices[eye], f.position + Rotate(f.orientation, Rotate(turn, {0, 0, 10})), s.width,
                          s.height, s.guard);
        if (Positive(next))
            result = {std::min(result.left, next.left), std::min(result.top, next.top),
                      std::max(result.right, next.right), std::max(result.bottom, next.bottom)};
    }
    return result;
}
} // namespace gvl::streaming::scene
