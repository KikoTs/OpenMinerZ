// Effect recipes defined by mods from Luau tables, and the script-value
// helpers shared by the graphics API (openminerz/modding/graphics.hpp).

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace openminerz::modding::graphics
{
namespace
{

[[noreturn]] void refuse(const std::string &path, const std::string &message)
{
    throw ScriptError{path.empty() ? message : path + ": " + message};
}

std::string join(const std::string &path, std::string_view field)
{
    return path.empty() ? std::string{field} : path + "." + std::string{field};
}

float number(const Value &value, const std::string &path, float low, float high)
{
    if (!value.is_number())
        refuse(path, std::string{"expected a number, got "} + value_type_name(value.type()));
    const double raw = value.as_number();
    if (!std::isfinite(raw))
        refuse(path, "expected a finite number");
    return static_cast<float>(std::clamp(raw, static_cast<double>(low), static_cast<double>(high)));
}

// Reads `table[field]` into `target` when present.
void read(const Value &table, std::string_view field, const std::string &path, float &target,
          float low, float high)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
        target = number(*value, join(path, field), low, high);
}

void read(const Value &table, std::string_view field, const std::string &path, bool &target)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
    {
        if (value->type() != ValueType::boolean)
            refuse(join(path, field), "expected true or false");
        target = value->as_bool();
    }
}

void read(const Value &table, std::string_view field, const std::string &path, int &target, int low,
          int high)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
        target = static_cast<int>(std::lround(
            number(*value, join(path, field), static_cast<float>(low), static_cast<float>(high))));
}

const Array *array_of(const Value &value)
{
    if (value.type() == ValueType::array)
        return &value.as_array();
    return nullptr;
}

bool empty_table(const Value &value)
{
    return value.type() == ValueType::map && value.as_map().empty();
}

Vec3 vec3_from_value(const Value &value, const std::string &path)
{
    if (value.type() == ValueType::vec3)
    {
        const modding::Vec3 v = value.as_vec3();
        const Vec3 result{static_cast<float>(v.x), static_cast<float>(v.y),
                          static_cast<float>(v.z)};
        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
            refuse(path, "expected a finite vector");
        return result;
    }
    if (const Array *items = array_of(value); items != nullptr && items->size() == 3U)
        return {number((*items)[0], path + "[1]", -1.0e6F, 1.0e6F),
                number((*items)[1], path + "[2]", -1.0e6F, 1.0e6F),
                number((*items)[2], path + "[3]", -1.0e6F, 1.0e6F)};
    if (value.type() == ValueType::map)
    {
        const Value *x = value.field("x");
        const Value *y = value.field("y");
        const Value *z = value.field("z");
        if (x != nullptr && y != nullptr && z != nullptr)
            return {number(*x, path + ".x", -1.0e6F, 1.0e6F),
                    number(*y, path + ".y", -1.0e6F, 1.0e6F),
                    number(*z, path + ".z", -1.0e6F, 1.0e6F)};
    }
    refuse(path, "expected a vector (vector.create(x, y, z) or {x, y, z})");
}

void read(const Value &table, std::string_view field, const std::string &path, Vec3 &target)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
        target = vec3_from_value(*value, join(path, field));
}

Color color_at(const Value &value, const std::string &path)
{
    try
    {
        return color_from_value(value);
    }
    catch (const ScriptError &error)
    {
        refuse(path, error.what());
    }
}

vfx::Range range(const Value &value, const std::string &path, float low, float high)
{
    if (value.is_number())
        return {number(value, path, low, high)};
    if (const Array *items = array_of(value); items != nullptr && items->size() == 2U)
    {
        const float a = number((*items)[0], path + "[1]", low, high);
        const float b = number((*items)[1], path + "[2]", low, high);
        return {std::min(a, b), std::max(a, b)};
    }
    if (value.type() == ValueType::map && value.field("min") != nullptr &&
        value.field("max") != nullptr)
    {
        const float a = number(*value.field("min"), path + ".min", low, high);
        const float b = number(*value.field("max"), path + ".max", low, high);
        return {std::min(a, b), std::max(a, b)};
    }
    refuse(path, "expected a number or {min, max}");
}

void read(const Value &table, std::string_view field, const std::string &path, vfx::Range &target,
          float low, float high)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
        target = range(*value, join(path, field), low, high);
}

// A number (constant), {from, to} as a map, or keys {{t, v}, ...}.
vfx::Curve curve(const Value &value, const std::string &path, float low, float high)
{
    if (value.is_number())
        return vfx::Curve::constant(number(value, path, low, high));
    if (value.type() == ValueType::map && value.field("from") != nullptr &&
        value.field("to") != nullptr)
        return vfx::Curve::line(number(*value.field("from"), path + ".from", low, high),
                                number(*value.field("to"), path + ".to", low, high));
    const Array *keys = array_of(value);
    if (keys == nullptr || keys->empty() || keys->size() > vfx::Curve::capacity)
        refuse(path, "expected a number, {from=, to=} or 1 to 6 keys {{t, value}, ...}");
    vfx::Curve result;
    result.count = 0U;
    float previous = -1.0F;
    for (std::size_t index = 0U; index < keys->size(); ++index)
    {
        const std::string key_path = path + "[" + std::to_string(index + 1U) + "]";
        const Array *pair = array_of((*keys)[index]);
        if (pair == nullptr || pair->size() != 2U)
            refuse(key_path, "expected {t, value}");
        const float time = number((*pair)[0], key_path + "[1]", 0.0F, 1.0F);
        if (time < previous)
            refuse(key_path, "keys must be in rising time order");
        previous = time;
        result.time[result.count] = time;
        result.value[result.count] = number((*pair)[1], key_path + "[2]", low, high);
        ++result.count;
    }
    return result;
}

void read(const Value &table, std::string_view field, const std::string &path, vfx::Curve &target,
          float low, float high)
{
    if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
        target = curve(*value, join(path, field), low, high);
}

vfx::ColorCurve color_curve(const Value &value, const std::string &path)
{
    const Array *keys = array_of(value);
    // A colour of three numbers is a constant; keys are pairs.
    const bool is_keys = keys != nullptr && !keys->empty() &&
                         (*keys)[0].type() == ValueType::array &&
                         (*keys)[0].as_array().size() == 2U;
    if (!is_keys)
        return vfx::ColorCurve::constant(color_at(value, path));
    if (keys->size() > vfx::ColorCurve::capacity)
        refuse(path, "at most 4 colour keys");
    vfx::ColorCurve result;
    result.count = 0U;
    float previous = -1.0F;
    for (std::size_t index = 0U; index < keys->size(); ++index)
    {
        const std::string key_path = path + "[" + std::to_string(index + 1U) + "]";
        const Array *pair = array_of((*keys)[index]);
        if (pair == nullptr || pair->size() != 2U)
            refuse(key_path, "expected {t, colour}");
        const float time = number((*pair)[0], key_path + "[1]", 0.0F, 1.0F);
        if (time < previous)
            refuse(key_path, "keys must be in rising time order");
        previous = time;
        result.time[result.count] = time;
        result.value[result.count] = color_at((*pair)[1], key_path + "[2]");
        ++result.count;
    }
    return result;
}

template <typename Enum, std::size_t Count>
Enum choice(const Value &table, std::string_view field, const std::string &path, Enum fallback,
            const std::array<std::pair<std::string_view, Enum>, Count> &options)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return fallback;
    if (value->type() != ValueType::string)
        refuse(join(path, field), "expected a name");
    for (const auto &[name, option] : options)
        if (value->as_string() == name)
            return option;
    std::string names;
    for (const auto &[name, option] : options)
    {
        (void)option;
        names += names.empty() ? std::string{name} : ", " + std::string{name};
    }
    refuse(join(path, field), "expected one of " + names);
}

vfx::EmitterDesc emitter(const Value &table, const std::string &path, std::size_t emitter_count)
{
    if (table.type() != ValueType::map)
        refuse(path, "expected a table");
    vfx::EmitterDesc out;
    static constexpr std::array<std::pair<std::string_view, vfx::Material>, 7> materials{{
        {"smoke", vfx::Material::smoke},
        {"flame", vfx::Material::flame},
        {"spark", vfx::Material::spark},
        {"glow", vfx::Material::glow},
        {"shockwave", vfx::Material::shockwave},
        {"heat", vfx::Material::heat},
        {"debris", vfx::Material::debris},
    }};
    static constexpr std::array<std::pair<std::string_view, vfx::Shape>, 3> shapes{{
        {"point", vfx::Shape::point},
        {"sphere", vfx::Shape::sphere},
        {"ring", vfx::Shape::ring},
    }};
    out.material = choice(table, "material", path, out.material, materials);
    out.shape = choice(table, "shape", path, out.shape, shapes);
    read(table, "burst", path, out.burst, 0, 512);
    read(table, "rate", path, out.rate, 0.0F, 2000.0F);
    read(table, "delay", path, out.delay, 0.0F, 30.0F);
    read(table, "duration", path, out.duration, 0.0F, 60.0F);
    read(table, "loop", path, out.loop);
    read(table, "sub_only", path, out.sub_only);
    read(table, "shape_radius", path, out.shape_radius, 0.0F, 16.0F);
    read(table, "offset", path, out.offset);
    read(table, "direction", path, out.direction);
    out.direction = vfx::normalized(out.direction);
    read(table, "spread", path, out.spread, 0.0F, 1.0F);
    read(table, "speed", path, out.speed, 0.0F, 200.0F);
    read(table, "velocity_bias", path, out.velocity_bias);
    read(table, "inherit_velocity", path, out.inherit_velocity, 0.0F, 2.0F);
    read(table, "life", path, out.life, 0.01F, 30.0F);
    read(table, "drag", path, out.drag, 0.0F, 50.0F);
    read(table, "gravity", path, out.gravity, -50.0F, 50.0F);
    read(table, "buoyancy", path, out.buoyancy, 0.0F, 50.0F);
    read(table, "turbulence", path, out.turbulence, 0.0F, 50.0F);
    read(table, "turbulence_scale", path, out.turbulence_scale, 0.1F, 64.0F);
    read(table, "collide", path, out.collide);
    read(table, "bounce", path, out.bounce, 0.0F, 1.0F);
    read(table, "friction", path, out.friction, 0.0F, 1.0F);
    read(table, "size", path, out.size, 0.001F, 16.0F);
    read(table, "size_over_life", path, out.size_over_life, 0.0F, 64.0F);
    read(table, "rotation", path, out.rotation, -100.0F, 100.0F);
    read(table, "spin", path, out.spin, -100.0F, 100.0F);
    if (const Value *value = table.field("color"); value != nullptr && !value->is_nil())
        out.color = color_curve(*value, join(path, "color"));
    read(table, "alpha", path, out.alpha, 0.0F, 1.0F);
    read(table, "emissive", path, out.emissive, 0.0F, 64.0F);
    read(table, "temperature", path, out.temperature, 0.0F, 4.0F);
    read(table, "erosion", path, out.erosion, 0.0F, 1.0F);
    read(table, "cold", path, out.cold);
    read(table, "stretch", path, out.stretch, 0.0F, 1.0F);
    read(table, "distortion", path, out.distortion, 0.0F, 0.2F);
    read(table, "frame_rate", path, out.frame_rate, 0.0F, 60.0F);
    // Script indices are 1-based, like every Luau array.
    if (const Value *value = table.field("sub_emitter"); value != nullptr && !value->is_nil())
    {
        const int index = static_cast<int>(std::lround(
            number(*value, join(path, "sub_emitter"), 1.0F, static_cast<float>(emitter_count))));
        out.sub_emitter = index - 1;
    }
    read(table, "sub_rate", path, out.sub_rate, 0.0F, 200.0F);
    read(table, "sub_on_death", path, out.sub_on_death, 0, 32);
    return out;
}

vfx::LightDesc effect_light(const Value &table, const std::string &path)
{
    if (table.type() != ValueType::map)
        refuse(path, "expected a table");
    vfx::LightDesc out;
    if (const Value *value = table.field("color"); value != nullptr && !value->is_nil())
        out.color = color_at(*value, join(path, "color"));
    read(table, "radius", path, out.radius, 0.5F, 64.0F);
    read(table, "intensity", path, out.intensity, 0.0F, 64.0F);
    read(table, "duration", path, out.duration, 0.0F, 60.0F);
    read(table, "flicker", path, out.flicker, 0.0F, 1.0F);
    read(table, "offset", path, out.offset);
    return out;
}

} // namespace

Color color_from_value(const Value &value)
{
    if (value.type() == ValueType::string)
    {
        const std::string &text = value.as_string();
        if (text.size() == 7U && text[0] == '#')
        {
            char *end{};
            const unsigned long rgb = std::strtoul(text.c_str() + 1, &end, 16);
            if (end == text.c_str() + 7)
                return {static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F,
                        static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
                        static_cast<float>(rgb & 0xFFU) / 255.0F};
        }
        throw ScriptError{"expected a colour \"#rrggbb\""};
    }
    if (value.type() == ValueType::vec3)
    {
        const modding::Vec3 v = value.as_vec3();
        return {std::clamp(static_cast<float>(v.x), 0.0F, 16.0F),
                std::clamp(static_cast<float>(v.y), 0.0F, 16.0F),
                std::clamp(static_cast<float>(v.z), 0.0F, 16.0F)};
    }
    if (const Array *items = array_of(value); items != nullptr && items->size() == 3U)
        return {number((*items)[0], "[1]", 0.0F, 16.0F), number((*items)[1], "[2]", 0.0F, 16.0F),
                number((*items)[2], "[3]", 0.0F, 16.0F)};
    if (value.type() == ValueType::map)
    {
        const Value *r = value.field("r");
        const Value *g = value.field("g");
        const Value *b = value.field("b");
        if (r != nullptr && g != nullptr && b != nullptr)
            return {number(*r, "r", 0.0F, 16.0F), number(*g, "g", 0.0F, 16.0F),
                    number(*b, "b", 0.0F, 16.0F)};
    }
    throw ScriptError{"expected a colour ({r, g, b}, {r=, g=, b=}, a vector or \"#rrggbb\")"};
}

vfx::EffectDesc EffectLibrary::parse(const Value &table)
{
    if (table.type() != ValueType::map && !empty_table(table))
        refuse("", "an effect is a table");
    vfx::EffectDesc out;
    if (const Value *emitters = table.field("emitters"); emitters != nullptr && !emitters->is_nil())
    {
        const Array *items = array_of(*emitters);
        if (items == nullptr && !empty_table(*emitters))
            refuse("emitters", "expected an array of emitter tables");
        if (items != nullptr)
        {
            if (items->size() > vfx::System::max_emitters)
                refuse("emitters",
                       "at most " + std::to_string(vfx::System::max_emitters) + " emitters");
            for (std::size_t index = 0U; index < items->size(); ++index)
                out.emitters.push_back(emitter((*items)[index],
                                               "emitters[" + std::to_string(index + 1U) + "]",
                                               items->size()));
        }
    }
    if (const Value *lights = table.field("lights"); lights != nullptr && !lights->is_nil())
    {
        const Array *items = array_of(*lights);
        if (items == nullptr && !empty_table(*lights))
            refuse("lights", "expected an array of light tables");
        if (items != nullptr)
        {
            if (items->size() > 4U)
                refuse("lights", "at most 4 lights");
            for (std::size_t index = 0U; index < items->size(); ++index)
                out.lights.push_back(
                    effect_light((*items)[index], "lights[" + std::to_string(index + 1U) + "]"));
        }
    }
    if (out.emitters.empty() && out.lights.empty())
        refuse("", "an effect needs at least one emitter or light");
    read(table, "shake", "", out.shake, 0.0F, 1.0F);
    read(table, "shake_duration", "", out.shake_duration, 0.0F, 5.0F);
    read(table, "shake_radius", "", out.shake_radius, 0.0F, 200.0F);
    read(table, "flash", "", out.flash, 0.0F, 1.0F);
    read(table, "flash_duration", "", out.flash_duration, 0.0F, 2.0F);
    read(table, "flash_radius", "", out.flash_radius, 0.0F, 200.0F);
    if (const Value *value = table.field("flash_color"); value != nullptr && !value->is_nil())
        out.flash_color = color_at(*value, "flash_color");
    read(table, "lod_start", "", out.lod_start, 0.0F, 512.0F);
    read(table, "lod_end", "", out.lod_end, 0.0F, 512.0F);
    read(table, "lod_floor", "", out.lod_floor, 0.0F, 1.0F);
    read(table, "cull_distance", "", out.cull_distance, 1.0F, 512.0F);
    read(table, "max_particles", "", out.max_particles, 1, 2048);
    read(table, "priority", "", out.priority, -10, 10);
    out.lod_end = std::max(out.lod_end, out.lod_start + 1.0F);
    return out;
}

const vfx::EffectDesc &EffectLibrary::define(std::string_view mod_id, std::string_view name,
                                             const Value &table)
{
    if (name.empty() || name.size() > 64U || name.find(':') != std::string_view::npos)
        throw ScriptError{"effect names are 1 to 64 characters without ':'"};
    const std::string key = std::string{mod_id} + ":" + std::string{name};
    const bool replacing = live_.contains(key);
    if (!replacing)
    {
        if (live_.size() >= max_definitions)
            throw ScriptError{"too many effect definitions"};
        const std::string prefix = std::string{mod_id} + ":";
        const auto own = static_cast<std::size_t>(
            std::count_if(live_.begin(), live_.end(),
                          [&](const auto &entry) { return entry.first.starts_with(prefix); }));
        if (own >= max_definitions_per_mod)
            throw ScriptError{"too many effect definitions in this mod"};
    }
    if (storage_.size() >= max_definitions * 4U)
        throw ScriptError{"too many effect redefinitions this session"};
    auto stored = std::make_unique<Stored>();
    stored->key = key;
    stored->desc = parse(table);
    stored->desc.name = stored->key;
    Stored *pointer = stored.get();
    // The old recipe stays in storage: effects already playing it keep a
    // pointer to it until they end.
    storage_.push_back(std::move(stored));
    live_.insert_or_assign(key, pointer);
    return pointer->desc;
}

const vfx::EffectDesc *EffectLibrary::find(std::string_view mod_id, std::string_view name) const
{
    if (name.find(':') != std::string_view::npos)
    {
        const auto found = live_.find(name);
        return found != live_.end() ? &found->second->desc : nullptr;
    }
    const std::string own = std::string{mod_id} + ":" + std::string{name};
    if (const auto found = live_.find(own); found != live_.end())
        return &found->second->desc;
    return vfx::find_effect(name);
}

LightDesc light_from_value(const Value &table, const LightDesc &base)
{
    if (table.type() != ValueType::map && !empty_table(table))
        throw ScriptError{"a light is a table"};
    LightDesc out = base;
    read(table, "position", "", out.position);
    if (const Value *value = table.field("color"); value != nullptr && !value->is_nil())
        out.color = color_at(*value, "color");
    read(table, "intensity", "", out.intensity, 0.0F, 64.0F);
    read(table, "radius", "", out.radius, 0.5F, 64.0F);
    read(table, "flicker", "", out.flicker, 0.0F, 1.0F);
    read(table, "flicker_speed", "", out.flicker_speed, 0.0F, 60.0F);
    read(table, "pulse", "", out.pulse, 0.0F, 1.0F);
    read(table, "pulse_speed", "", out.pulse_speed, 0.0F, 30.0F);
    read(table, "pulse_phase", "", out.pulse_phase, 0.0F, 1.0F);
    static constexpr std::array<std::pair<std::string_view, LightShape>, 2> shapes{{
        {"point", LightShape::point},
        {"spot", LightShape::spot},
    }};
    out.shape = choice(table, "shape", "", out.shape, shapes);
    read(table, "direction", "", out.direction);
    read(table, "inner_angle", "", out.inner_angle, 0.0F, 89.0F);
    read(table, "outer_angle", "", out.outer_angle, 1.0F, 89.0F);
    read(table, "enabled", "", out.enabled);
    read(table, "shadow", "", out.shadow);
    return out;
}

AtmosphereLayerDesc atmosphere_from_value(const Value &table)
{
    if (table.type() != ValueType::map && !empty_table(table))
        throw ScriptError{"an atmosphere layer is a table"};
    AtmosphereLayerDesc out;
    AtmosphereValues &values = out.values;
    const auto color = [&](std::string_view field, std::optional<Color> &target) {
        if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
            target = color_at(*value, std::string{field});
    };
    const auto scalar = [&](std::string_view field, std::optional<float> &target, float low,
                            float high) {
        if (const Value *value = table.field(field); value != nullptr && !value->is_nil())
            target = number(*value, std::string{field}, low, high);
    };
    color("sky_zenith", values.sky_zenith);
    color("sky_horizon", values.sky_horizon);
    color("fog_color", values.fog_color);
    color("ambient", values.ambient);
    color("sun_color", values.sun_color);
    scalar("ambient_intensity", values.ambient_intensity, 0.0F, 8.0F);
    scalar("sun_intensity", values.sun_intensity, 0.0F, 8.0F);
    scalar("fog_start", values.fog_start, 0.0F, 4096.0F);
    scalar("fog_end", values.fog_end, 1.0F, 4096.0F);
    scalar("exposure", values.exposure, -4.0F, 4.0F);
    scalar("bloom", values.bloom, 0.0F, 4.0F);
    scalar("saturation", values.saturation, 0.0F, 2.0F);
    scalar("contrast", values.contrast, 0.5F, 1.5F);
    scalar("temperature", values.temperature, -1.0F, 1.0F);
    scalar("vignette", values.vignette, 0.0F, 1.0F);
    if (const Value *value = table.field("lut"); value != nullptr && !value->is_nil())
    {
        if (value->type() != ValueType::string || !valid_asset_name(value->as_string()))
            throw ScriptError{"lut: expected an asset name such as \"luts/dungeon\""};
        values.lut = value->as_string();
    }
    read(table, "lut_strength", "", values.lut_strength, 0.0F, 1.0F);
    read(table, "priority", "", out.priority, -1000, 1000);
    read(table, "fade_in", "", out.fade_in, 0.0F, 60.0F);
    read(table, "fade_out", "", out.fade_out, 0.0F, 60.0F);
    read(table, "strength", "", out.strength, 0.0F, 1.0F);
    if (const Value *region = table.field("region"); region != nullptr && !region->is_nil())
    {
        if (region->type() != ValueType::map)
            throw ScriptError{"region: expected {min=, max=} or {center=, radius=}"};
        AtmosphereRegion &out_region = out.region;
        if (region->field("min") != nullptr && region->field("max") != nullptr)
        {
            out_region.kind = AtmosphereRegion::Kind::box;
            const Vec3 a = vec3_from_value(*region->field("min"), "region.min");
            const Vec3 b = vec3_from_value(*region->field("max"), "region.max");
            out_region.minimum = {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
            out_region.maximum = {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        }
        else if (region->field("center") != nullptr && region->field("radius") != nullptr)
        {
            out_region.kind = AtmosphereRegion::Kind::sphere;
            out_region.center = vec3_from_value(*region->field("center"), "region.center");
            out_region.radius = number(*region->field("radius"), "region.radius", 0.0F, 4096.0F);
        }
        else
            throw ScriptError{"region: expected {min=, max=} or {center=, radius=}"};
        read(*region, "feather", "region", out_region.feather, 0.0F, 256.0F);
    }
    return out;
}

ModelInstanceDesc model_instance_from_value(const Value &table, const ModelInstanceDesc &base)
{
    if (table.type() != ValueType::map && !empty_table(table))
        throw ScriptError{"model options are a table"};
    ModelInstanceDesc out = base;
    if (const Value *value = table.field("model"); value != nullptr && !value->is_nil())
    {
        if (value->type() != ValueType::string || !valid_asset_name(value->as_string()))
            throw ScriptError{"model: expected an asset name such as \"models/lantern\""};
        out.model = value->as_string();
    }
    read(table, "position", "", out.position);
    read(table, "yaw", "", out.yaw, -100000.0F, 100000.0F);
    read(table, "pitch", "", out.pitch, -100000.0F, 100000.0F);
    read(table, "roll", "", out.roll, -100000.0F, 100000.0F);
    read(table, "scale", "", out.scale, 0.01F, 64.0F);
    if (const Value *value = table.field("tint"); value != nullptr && !value->is_nil())
        out.tint = color_at(*value, "tint");
    if (const Value *value = table.field("animation"); value != nullptr && !value->is_nil())
    {
        if (value->type() != ValueType::string || value->as_string().size() > 128U)
            throw ScriptError{"animation: expected a clip name"};
        out.animation = value->as_string();
    }
    read(table, "loop", "", out.loop);
    read(table, "speed", "", out.speed, 0.0F, 16.0F);
    read(table, "visible", "", out.visible);
    return out;
}

} // namespace openminerz::modding::graphics
