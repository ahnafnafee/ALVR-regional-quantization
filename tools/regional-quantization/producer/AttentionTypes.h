#pragma once
#include "AttentionPacket.h"

namespace gvl::streaming::scene
{
constexpr uint32_t Abi = 1, MaxShapes = 128;
enum Flags : uint32_t
{
    Active = 1,
    Predictive = 2,
    Selective = 4,
    Overflow = 8
};
#pragma pack(push, 4)
struct V3
{
    float x, y, z;
};
struct Q
{
    float x, y, z, w;
};
struct Matrix
{
    float v[16];
}; // Unity column-major Matrix4x4 layout.
struct Settings
{
    float important, background, lifetime, guard, feather, width, height;
    float horizon, maxDegrees, rapidTurn, intermediate;
};
struct Frame
{
    uint32_t bytes, abi, frame, count, flags, reserved;
    uint64_t qpc, frequency;
    double time;
    Q tracked;
    V3 position;
    Q orientation;
    float nearPlane;
    Matrix matrices[2];
    Settings settings;
};
struct Shape
{
    uint32_t kind, count;
    float distance;
    uint32_t clipToFocus;
    V3 points[8];
    float fullUntil, fadeUntil, minimum;
    uint32_t projection; // 0 convex bounds, 1 horizontal bands for a convex quad.
};
#pragma pack(pop)
static_assert(sizeof(Frame) == 268 && sizeof(Shape) == 128, "Scene ABI layout changed");
static_assert(offsetof(Frame, qpc) == 24 && offsetof(Frame, matrices) == 96, "Scene ABI offsets changed");
} // namespace gvl::streaming::scene
