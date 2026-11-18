#pragma once
#include "AttentionMath.h"

namespace gvl::streaming::scene
{
class Motion
{
    Q previous{};
    double time = 0;
    bool ready = false;

  public:
    void Reset()
    {
        ready = false;
    }
    bool Sample(Q rotation, double now, float horizon, float maxDegrees, float &speed, Q &lead)
    {
        speed = 0;
        lead = {0, 0, 0, 1};
        const float norm = Norm(rotation);
        if (!std::isfinite(now) || !std::isfinite(norm) || norm < .5f || norm > 1.5f)
        {
            Reset();
            return false;
        }
        rotation = Normalize(rotation);
        if (ready && now <= time)
            return true;
        const double dt = now - time;
        const Q old = previous;
        const bool continuous = ready && dt >= .001 && dt <= .1;
        previous = rotation;
        time = now;
        ready = true;
        if (!continuous)
            return true;
        const float inverse = 1 / Norm(old);
        Q delta = Multiply({-old.x * inverse, -old.y * inverse, -old.z * inverse, old.w * inverse}, rotation);
        if (delta.w < 0)
            delta = {-delta.x, -delta.y, -delta.z, -delta.w};
        const float sine = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (sine < 1e-6f)
            return true;
        constexpr float degrees = 57.29577951308232f;
        const float angle = 2 * std::atan2(sine, Clamp(delta.w, 0, 1)) * degrees, measured = angle / float(dt);
        if (angle > 45 || measured > 720)
            return true;
        speed = measured;
        if (!std::isfinite(horizon) || !std::isfinite(maxDegrees))
            return true;
        const float radians = std::min(speed * Clamp(horizon, 0, .1f), Clamp(maxDegrees, 0, 15)) / degrees * .5f;
        const float scale = std::sin(radians) / sine;
        lead = {delta.x * scale, delta.y * scale, delta.z * scale, std::cos(radians)};
        return true;
    }
};
} // namespace gvl::streaming::scene
