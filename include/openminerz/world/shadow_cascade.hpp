#pragma once

#include <algorithm>
#include <cmath>

namespace openminerz::world
{
struct ShadowSlice final
{
    float near_depth{};
    float far_depth{};
    float center_depth{};
    float radius{};
};

// A bounding sphere keeps the shadow texel size independent of camera yaw and
// pitch. The next cascade also covers the previous cascade's blend interval.
[[nodiscard]] inline ShadowSlice shadow_slice(float previous_start, float split_start,
                                              float split_end, float tangent_x,
                                              float tangent_y) noexcept
{
    const float near_depth = split_start - (split_start - previous_start) * 0.14F;
    const float diagonal_squared = tangent_x * tangent_x + tangent_y * tangent_y;
    const float center =
        std::min(split_end, (near_depth + split_end) * (1.0F + diagonal_squared) * 0.5F);
    const float radius = std::sqrt((split_end - center) * (split_end - center) +
                                   split_end * split_end * diagonal_squared);
    return {near_depth, split_end, center, std::ceil(radius * 16.0F) / 16.0F};
}
} // namespace openminerz::world
