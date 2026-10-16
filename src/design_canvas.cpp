#include "openminerz/design_canvas.hpp"

#include <cmath>
#include <stdexcept>

namespace openminerz::ui
{

int UiMetrics::font_pixels(float design_size) const noexcept
{
    const float normalized = std::clamp(design_size * density, 12.0F, 256.0F);
    return static_cast<int>(std::lround(normalized));
}

UiMetrics DesignCanvas::compute(int output_width, int output_height, float ui_scale,
                                float safe_area)
{
    if (output_width <= 0 || output_height <= 0)
    {
        throw std::invalid_argument{"drawable dimensions must be positive"};
    }
    if (!std::isfinite(ui_scale) || !std::isfinite(safe_area))
    {
        throw std::invalid_argument{"UI scale and safe area must be finite"};
    }

    const float normalized_ui_scale = std::clamp(ui_scale, 0.75F, 1.50F);
    const float normalized_safe_area = std::clamp(safe_area, 0.80F, 1.00F);
    const float fit_scale = std::min(static_cast<float>(output_width) / reference_width,
                                     static_cast<float>(output_height) / reference_height);
    const float requested_density = std::clamp(fit_scale * normalized_ui_scale, 0.50F, 4.00F);
    const float horizontal_inset =
        static_cast<float>(output_width) * (1.0F - normalized_safe_area) * 0.5F;
    const float vertical_inset =
        static_cast<float>(output_height) * (1.0F - normalized_safe_area) * 0.5F;

    UiMetrics metrics;
    metrics.output_width = output_width;
    metrics.output_height = output_height;
    metrics.safe = {horizontal_inset, vertical_inset,
                    static_cast<float>(output_width) - horizontal_inset * 2.0F,
                    static_cast<float>(output_height) - vertical_inset * 2.0F};
    // Fit the complete interface, including its header and footer. Scaling only
    // an outer panel leaves its children spilling out at large UI sizes.
    metrics.scale =
        std::min({requested_density, metrics.safe.width / 1650.0F, metrics.safe.height / 920.0F});
    metrics.density = metrics.scale;
    return metrics;
}

} // namespace openminerz::ui
