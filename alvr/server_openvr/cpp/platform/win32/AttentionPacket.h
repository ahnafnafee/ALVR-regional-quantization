#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gvl::streaming
{
constexpr uint32_t Magic = 0x31414246; // Stable region-packet ABI magic
constexpr uint32_t Version = 1;
constexpr uint32_t ControlVersion = 2; // Adds the producer's asynchronous-frame fence at byte 120.
constexpr uint32_t MaxRegions = 64;
constexpr uint32_t Slots = 4;
constexpr uint32_t ControlBytes = 128;
constexpr uint32_t MappingBytes = 8192;
constexpr wchar_t MappingName[] = L"Local\\GVL.StreamingAttention.v1";
constexpr wchar_t MutexName[] = L"Local\\GVL.StreamingAttention.v1.Lock";

// Coordinates are per-eye normalized view coordinates, with a TOP-LEFT origin.
// Separate eyes are required: a single cyclopean rectangle misses nearby HUD edges.
struct Region
{
    float left, top, right, bottom;
    uint32_t eye; // 0 left, 1 right
    // Low byte: caller-defined semantic tag. Bit 8 opts into UNORM16
    // importance in the high 16 bits; absent bit 8 means legacy/full priority.
    uint32_t kind;
};
inline float Importance(const Region &r)
{
    return (r.kind & 0x100u) ? float(r.kind >> 16) / 65535.0f : 1.0f;
}
struct Packet
{
    uint32_t magic, version, bytes, producerPid;
    uint64_t sequence, qpc, frequency;
    uint32_t frame, count;
    float importantWeight, backgroundWeight, maxAgeMs, padding;
    Region regions[MaxRegions];
};
struct Control
{
    uint32_t magic = Magic, version = ControlVersion, consumerPid = 0, enabled = 0;
    uint64_t heartbeat = 0, lastSequence = 0, applied = 0, rejected = 0;
    float protectedFraction = 0;
    int32_t minDelta = 0, maxDelta = 0;
    uint32_t width = 0, height = 0, block = 0, encoderError = 0, scenePid = 0;
    float mapBuildMs = 0;
    uint32_t regions = 0;
    float producerAgeMs = 0;
    uint32_t producerPid = 0;
    uint8_t producerDiagnostics[12]{}; // Unity-owned bytes 96..107; never overwritten.
    float highPriorityFraction = 0, intermediateFraction = 0, backgroundFraction = 0;
    uint64_t requestedQpc = 0; // Producer-owned, atomically updated; zero selects the legacy synchronous path.
};
static_assert(sizeof(Region) == 24 && sizeof(Packet) == 1600, "Wire layout changed");
static_assert(sizeof(Control) <= ControlBytes, "Control overlaps packets");
static_assert(offsetof(Control, producerDiagnostics) == 96 && offsetof(Control, highPriorityFraction) == 108,
              "Diagnostic layout changed");
static_assert(offsetof(Control, requestedQpc) == 120 && sizeof(Control) == ControlBytes, "Frame fence layout changed");
static_assert(ControlBytes + sizeof(Packet) * Slots <= MappingBytes, "Mapping too small");

inline bool MatchesRequest(const Packet &packet, uint64_t requestedQpc)
{
    return requestedQpc == 0 || packet.qpc == requestedQpc;
}

inline bool Valid(const Packet &p, uint64_t now, uint64_t frequency, uint32_t scenePid)
{
    if (p.magic != Magic || p.version != Version || p.bytes != sizeof(Packet) || p.producerPid == 0 ||
        p.producerPid != scenePid || p.frequency != frequency || p.sequence == 0 || p.count == 0 ||
        p.count > MaxRegions || p.qpc > now || !std::isfinite(p.maxAgeMs) || p.maxAgeMs < 1 || p.maxAgeMs > 100 ||
        !std::isfinite(p.importantWeight) || !std::isfinite(p.backgroundWeight) || p.backgroundWeight < 1 ||
        p.importantWeight < p.backgroundWeight || p.importantWeight > 100 || !std::isfinite(p.padding) ||
        p.padding < 0 || p.padding > 0.2f || frequency == 0)
        return false;
    if (1000.0 * double(now - p.qpc) / double(frequency) > p.maxAgeMs)
        return false;
    for (uint32_t i = 0; i < p.count; ++i)
    {
        const auto &r = p.regions[i];
        if (r.eye > 1 || !std::isfinite(r.left) || !std::isfinite(r.top) || !std::isfinite(r.right) ||
            !std::isfinite(r.bottom) || r.left < 0 || r.top < 0 || r.right > 1 || r.bottom > 1 || r.left >= r.right ||
            r.top >= r.bottom)
            return false;
    }
    return true;
}

} // namespace gvl::streaming
