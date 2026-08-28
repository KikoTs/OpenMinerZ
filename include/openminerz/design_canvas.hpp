#pragma once

#include <algorithm>

namespace openminerz::ui
{

struct Rect final
{
    float x{};
    float y{};
    float width{};
    float height{};

    [[nodiscard]] bool contains(float px, float py) const noexcept
    {
        return px >= x && py >= y && px <= x + width && py <= y + height;
    }
};

struct UiMetrics final
{
    int output_width{};
    int output_height{};
    float scale{1.0F};
    float density{1.0F};
    Rect safe{};

    [[nodiscard]] float px(float design_units) const noexcept
    {
        return design_units * scale;
    }
    [[nodiscard]] int font_pixels(float design_size) const noexcept;
};

class DesignCanvas final
{
  public:
    static constexpr float reference_width = 1920.0F;
    static constexpr float reference_height = 1080.0F;

    [[nodiscard]] static UiMetrics compute(int output_width, int output_height, float ui_scale,
                                           float safe_area);
};

} // namespace openminerz::ui
