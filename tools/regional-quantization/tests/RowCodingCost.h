#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gvl::streaming::alvr {
// A rate-cost proxy learned from actual previous coded-row outputs, not a
// semantic bit allocation. It never uses a later frame or modifies current ROIs.
class RowCodingCost {
    uint32_t columns = 0, rows = 0;
    std::vector<float> learned;
    bool observed = false;
public:
    std::vector<float> blocks;
    void Configure(uint32_t width, uint32_t height, uint32_t block = 32) {
        columns = block ? (width + block - 1) / block : 0;
        rows = block ? (height + block - 1) / block : 0;
        learned.assign(rows, 1);
        blocks.assign(size_t(columns) * rows, 1);
        observed = false;
    }
    bool Observe(const uint32_t* bits, const uint8_t* qp, uint32_t count, uint32_t frameQp) {
        if (!bits || !qp || count != rows || !rows || !columns || frameQp > 51) return false;
        double sum = 0;
        for (uint32_t row = 0; row < rows; ++row) {
            if (qp[row] > 51) return false;
            sum += double(bits[row]) * std::exp2((int(qp[row]) - int(frameQp)) / 6.0);
        }
        if (!std::isfinite(sum) || sum <= 0) return false;
        const double mean = sum / rows;
        for (uint32_t row = 0; row < rows; ++row) {
            const float current = float(std::clamp(double(bits[row])
                * std::exp2((int(qp[row]) - int(frameQp)) / 6.0) / mean, .05, 20.0));
            learned[row] = observed ? .85f * learned[row] + .15f * current : current;
            for (uint32_t column = 0; column < columns; ++column)
                blocks[size_t(row) * columns + column] = learned[row];
        }
        observed = true;
        return true;
    }
};
}
