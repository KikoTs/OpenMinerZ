// Atmosphere and post layers for mods, and colour-grading LUTs
// (openminerz/modding/graphics.hpp).

#include "openminerz/modding/graphics.hpp"

#include "openminerz/graphics_settings.hpp"
#include "openminerz/world/sky.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::modding::graphics
{
namespace
{

float finite_or(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

void blend(world::SkyColor &target, const std::optional<Color> &value, float weight) noexcept
{
    if (!value || weight <= 0.0F)
        return;
    target.red += (std::clamp(finite_or(value->r, 0.0F), 0.0F, 4.0F) - target.red) * weight;
    target.green += (std::clamp(finite_or(value->g, 0.0F), 0.0F, 4.0F) - target.green) * weight;
    target.blue += (std::clamp(finite_or(value->b, 0.0F), 0.0F, 4.0F) - target.blue) * weight;
}

void scale(world::SkyColor &target, const std::optional<float> &value, float weight) noexcept
{
    if (!value || weight <= 0.0F)
        return;
    const float factor = 1.0F + (std::clamp(finite_or(*value, 1.0F), 0.0F, 8.0F) - 1.0F) * weight;
    target.red *= factor;
    target.green *= factor;
    target.blue *= factor;
}

void blend(float &target, const std::optional<float> &value, float weight, float low,
           float high) noexcept
{
    if (!value || weight <= 0.0F)
        return;
    target += (std::clamp(finite_or(*value, target), low, high) - target) * weight;
}

float box_distance(const AtmosphereRegion &region, Vec3 eye) noexcept
{
    const float dx = std::max({region.minimum.x - eye.x, 0.0F, eye.x - region.maximum.x});
    const float dy = std::max({region.minimum.y - eye.y, 0.0F, eye.y - region.maximum.y});
    const float dz = std::max({region.minimum.z - eye.z, 0.0F, eye.z - region.maximum.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

float region_weight(const AtmosphereRegion &region, Vec3 eye) noexcept
{
    float distance{};
    switch (region.kind)
    {
    case AtmosphereRegion::Kind::everywhere:
        return 1.0F;
    case AtmosphereRegion::Kind::box:
        distance = box_distance(region, eye);
        break;
    case AtmosphereRegion::Kind::sphere:
        distance = std::max(0.0F, vfx::length(eye - region.center) - std::max(region.radius, 0.0F));
        break;
    }
    const float feather = std::max(finite_or(region.feather, 0.0F), 0.0F);
    if (feather <= 1.0e-4F)
        return distance <= 0.0F ? 1.0F : 0.0F;
    const float t = std::clamp(1.0F - distance / feather, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

std::optional<std::uint64_t> AtmosphereStack::create(std::string_view mod_id,
                                                     const AtmosphereLayerDesc &desc)
{
    if (layers_.size() >= max_layers)
        return std::nullopt;
    Layer layer;
    layer.id = next_id_++;
    layer.mod_id = mod_id;
    layer.desc = desc;
    layer.order = next_order_++;
    layer.fade = desc.fade_in <= 0.0F ? 1.0F : 0.0F;
    layers_.push_back(std::move(layer));
    return layers_.back().id;
}

bool AtmosphereStack::update(std::uint64_t id, const AtmosphereLayerDesc &desc)
{
    for (Layer &layer : layers_)
        if (layer.id == id)
        {
            layer.desc = desc;
            return true;
        }
    return false;
}

bool AtmosphereStack::remove(std::uint64_t id, bool immediate)
{
    for (auto layer = layers_.begin(); layer != layers_.end(); ++layer)
        if (layer->id == id)
        {
            if (immediate || layer->desc.fade_out <= 0.0F)
                layers_.erase(layer);
            else
                layer->removing = true;
            return true;
        }
    return false;
}

void AtmosphereStack::remove_mod(std::string_view mod_id) noexcept
{
    std::erase_if(layers_, [&](const Layer &layer) { return layer.mod_id == mod_id; });
}

void AtmosphereStack::clear() noexcept
{
    layers_.clear();
}

void AtmosphereStack::step(float seconds, Vec3 eye)
{
    seconds = std::clamp(finite_or(seconds, 0.0F), 0.0F, 1.0F);
    for (Layer &layer : layers_)
    {
        if (layer.removing)
        {
            const float rate = 1.0F / std::max(layer.desc.fade_out, 1.0e-3F);
            layer.fade = std::max(0.0F, layer.fade - rate * seconds);
        }
        else
        {
            const float rate = 1.0F / std::max(layer.desc.fade_in, 1.0e-3F);
            layer.fade = std::min(1.0F, layer.fade + rate * seconds);
        }
        layer.region = region_weight(layer.desc.region, eye);
    }
    std::erase_if(layers_, [](const Layer &layer) { return layer.removing && layer.fade <= 0.0F; });
}

bool AtmosphereStack::active() const noexcept
{
    return std::any_of(layers_.begin(), layers_.end(),
                       [](const Layer &layer) { return layer.weight() > 1.0e-4F; });
}

float AtmosphereStack::weight(std::uint64_t id) const noexcept
{
    for (const Layer &layer : layers_)
        if (layer.id == id)
            return std::clamp(layer.weight(), 0.0F, 1.0F);
    return 0.0F;
}

std::vector<const AtmosphereStack::Layer *> AtmosphereStack::ordered() const
{
    std::vector<const Layer *> result;
    result.reserve(layers_.size());
    for (const Layer &layer : layers_)
        if (layer.weight() > 1.0e-4F)
            result.push_back(&layer);
    std::sort(result.begin(), result.end(), [](const Layer *a, const Layer *b) {
        return a->desc.priority != b->desc.priority ? a->desc.priority < b->desc.priority
                                                    : a->order < b->order;
    });
    return result;
}

void AtmosphereStack::apply(world::SkyState &sky) const
{
    for (const Layer *layer : ordered())
    {
        const float weight = std::clamp(layer->weight(), 0.0F, 1.0F);
        const AtmosphereValues &values = layer->desc.values;
        blend(sky.zenith, values.sky_zenith, weight);
        blend(sky.horizon, values.sky_horizon, weight);
        blend(sky.fog, values.fog_color, weight);
        blend(sky.ambient, values.ambient, weight);
        blend(sky.direct, values.sun_color, weight);
        scale(sky.ambient, values.ambient_intensity, weight);
        scale(sky.direct, values.sun_intensity, weight);
        scale(sky.specular, values.sun_intensity, weight);
    }
}

void AtmosphereStack::apply(PostEffects &effects, float &exposure_ev) const
{
    for (const Layer *layer : ordered())
    {
        const float weight = std::clamp(layer->weight(), 0.0F, 1.0F);
        const AtmosphereValues &values = layer->desc.values;
        if (values.exposure)
            exposure_ev += std::clamp(finite_or(*values.exposure, 0.0F), -4.0F, 4.0F) * weight;
        if (values.bloom)
            effects.bloom_strength *=
                1.0F + (std::clamp(finite_or(*values.bloom, 1.0F), 0.0F, 4.0F) - 1.0F) * weight;
        blend(effects.saturation, values.saturation, weight, 0.0F, 2.0F);
        blend(effects.contrast, values.contrast, weight, 0.5F, 1.5F);
        blend(effects.temperature, values.temperature, weight, -1.0F, 1.0F);
        blend(effects.vignette, values.vignette, weight, 0.0F, 1.0F);
    }
}

bool AtmosphereStack::fog_band(float &start, float &end) const
{
    bool any{};
    for (const Layer *layer : ordered())
    {
        const float weight = std::clamp(layer->weight(), 0.0F, 1.0F);
        const AtmosphereValues &values = layer->desc.values;
        if (values.fog_start)
        {
            blend(start, values.fog_start, weight, 0.0F, 4096.0F);
            any = true;
        }
        if (values.fog_end)
        {
            blend(end, values.fog_end, weight, 1.0F, 4096.0F);
            any = true;
        }
    }
    if (any)
        end = std::max(end, start + 1.0F);
    return any;
}

ResolvedLut AtmosphereStack::lut() const
{
    ResolvedLut result;
    // The top-most layer with a LUT grades; layers do not mix two LUTs.
    for (const Layer *layer : ordered())
        if (!layer->desc.values.lut.empty())
        {
            result.name = layer->desc.values.lut;
            result.strength = std::clamp(
                layer->weight() *
                    std::clamp(finite_or(layer->desc.values.lut_strength, 1.0F), 0.0F, 1.0F),
                0.0F, 1.0F);
        }
    return result;
}

bool valid_lut(const Image &lut) noexcept
{
    if (lut.empty())
        return false;
    const int size = lut.height;
    return size >= 2 && size <= 64 && lut.width == size * size;
}

Color sample_lut(const Image &lut, Color color) noexcept
{
    if (!valid_lut(lut))
        return color;
    const int size = lut.height;
    const auto fetch = [&](int r, int g, int b) {
        const std::size_t at = (static_cast<std::size_t>(g) * static_cast<std::size_t>(lut.width) +
                                static_cast<std::size_t>(b * size + r)) *
                               4U;
        return Color{lut.rgba[at] / 255.0F, lut.rgba[at + 1U] / 255.0F, lut.rgba[at + 2U] / 255.0F};
    };
    const float scale = static_cast<float>(size - 1);
    const float r = std::clamp(color.r, 0.0F, 1.0F) * scale;
    const float g = std::clamp(color.g, 0.0F, 1.0F) * scale;
    const float b = std::clamp(color.b, 0.0F, 1.0F) * scale;
    const int r0 = static_cast<int>(r), g0 = static_cast<int>(g), b0 = static_cast<int>(b);
    const int r1 = std::min(r0 + 1, size - 1), g1 = std::min(g0 + 1, size - 1),
              b1 = std::min(b0 + 1, size - 1);
    const float tr = r - static_cast<float>(r0), tg = g - static_cast<float>(g0),
                tb = b - static_cast<float>(b0);
    const auto mix = [](Color a, Color c, float t) {
        return Color{a.r + (c.r - a.r) * t, a.g + (c.g - a.g) * t, a.b + (c.b - a.b) * t};
    };
    const Color c00 = mix(fetch(r0, g0, b0), fetch(r1, g0, b0), tr);
    const Color c10 = mix(fetch(r0, g1, b0), fetch(r1, g1, b0), tr);
    const Color c01 = mix(fetch(r0, g0, b1), fetch(r1, g0, b1), tr);
    const Color c11 = mix(fetch(r0, g1, b1), fetch(r1, g1, b1), tr);
    return mix(mix(c00, c10, tg), mix(c01, c11, tg), tb);
}

} // namespace openminerz::modding::graphics
