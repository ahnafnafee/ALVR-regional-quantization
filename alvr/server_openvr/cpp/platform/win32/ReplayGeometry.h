#pragma once
#include "AttentionMap.h"

namespace gvl::streaming::alvr {
struct ReplayGeometry {
    uint32_t magic = 0x31525047, version = 1, width = 0, height = 0, block = 32, foveated = 0;
    float xCenter = 1, xShift = 0, xRatio = 1, xSizeRatio = 1;
    float yCenter = 1, yShift = 0, yRatio = 1, ySizeRatio = 1;
    explicit ReplayGeometry(const Geometry& g) : width(g.width), height(g.height), block(g.block), foveated(g.foveated),
        xCenter(g.x.center), xShift(g.x.shift), xRatio(g.x.ratio), xSizeRatio(g.x.sizeRatio),
        yCenter(g.y.center), yShift(g.y.shift), yRatio(g.y.ratio), ySizeRatio(g.y.sizeRatio) {}
    ReplayGeometry() = default;
    Geometry GeometryValue() const {
        Geometry g; g.width = width; g.height = height; g.block = block; g.foveated = foveated != 0;
        g.x = {xCenter, xShift, xRatio, xSizeRatio}; g.y = {yCenter, yShift, yRatio, ySizeRatio}; return g;
    }
    bool Valid() const {
        auto axis = [](float c, float s, float r, float size) {
            return std::isfinite(c) && std::isfinite(s) && std::isfinite(r) && std::isfinite(size)
                && c >= 0 && c <= 1 && s >= -1 && s <= 1 && r >= 1 && r <= 16 && size > 0 && size <= 1;
        };
        return magic == 0x31525047 && version == 1 && width > 0 && width <= 8192 && width % 64 == 0
            && height > 0 && height <= 8192 && height % 32 == 0 && block == 32 && foveated <= 1
            && axis(xCenter, xShift, xRatio, xSizeRatio) && axis(yCenter, yShift, yRatio, ySizeRatio);
    }
};
static_assert(sizeof(ReplayGeometry) == 56, "Replay geometry layout changed");
}
