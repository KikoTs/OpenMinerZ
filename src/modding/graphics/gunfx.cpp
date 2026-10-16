// GunFX: weapon parts, animation cues and procedural first-person motion
// (openminerz/modding/gunfx.hpp). Pure CPU: the renderer
// (src/gunfx_view.inc) and the script library (register_graphics_api) use it.

#include "openminerz/modding/gunfx.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <numbers>

namespace openminerz::modding::gunfx
{
namespace
{

constexpr double ticks_per_second = 10'000'000.0;
constexpr float pi = std::numbers::pi_v<float>;
constexpr float degrees = pi / 180.0F;

// --- Script values ----------------------------------------------------------

Vec3 vector_of(const Value &value, std::string_view what)
{
    if (value.type() == ValueType::vec3)
    {
        const modding::Vec3 v = value.as_vec3();
        const Vec3 result{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        if (std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z) &&
            std::abs(result.x) < 1.0e4F && std::abs(result.y) < 1.0e4F && std::abs(result.z) < 1.0e4F)
            return result;
    }
    throw ScriptError{std::format("{}: expected a vector (vector.create(x, y, z))", what)};
}

float number_of(const Value &table, std::string_view field, float fallback, float low, float high)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return fallback;
    if (!value->is_number() || !std::isfinite(value->as_number()))
        throw ScriptError{std::format("{}: expected a number", field)};
    return static_cast<float>(std::clamp(value->as_number(), static_cast<double>(low), static_cast<double>(high)));
}

bool flag_of(const Value &table, std::string_view field, bool fallback)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return fallback;
    if (value->type() != ValueType::boolean)
        throw ScriptError{std::format("{}: expected true or false", field)};
    return value->as_bool();
}

std::string text_of(const Value &table, std::string_view field)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return {};
    if (value->type() != ValueType::string)
        throw ScriptError{std::format("{}: expected a string", field)};
    return value->as_string();
}

bool asset_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 200U || name.front() == '/' || name.find("..") != std::string_view::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-' || c == '.' || c == '/' || c == '@';
    });
}

void only_keys(const Value &table, std::span<const std::string_view> allowed, std::string_view what)
{
    if (table.type() != ValueType::map)
        return;
    for (const auto &[key, value] : table.as_map())
    {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            std::string list;
            for (const std::string_view name : allowed)
                list += (list.empty() ? "" : ", ") + std::string{name};
            throw ScriptError{std::format("{}: unknown key \"{}\" (expected {})", what, key, list)};
        }
    }
}

Box box_of(const Value &value, std::string_view what)
{
    Box box;
    if (value.type() == ValueType::array && value.as_array().size() == 2U)
    {
        box.min = vector_of(value.as_array()[0], what);
        box.max = vector_of(value.as_array()[1], what);
    }
    else if (value.type() == ValueType::map)
    {
        const Value *low = value.field("min");
        const Value *high = value.field("max");
        if (low == nullptr || high == nullptr)
            throw ScriptError{std::format("{}: expected {{min = vector, max = vector}}", what)};
        box.min = vector_of(*low, what);
        box.max = vector_of(*high, what);
    }
    else
        throw ScriptError{std::format("{}: expected {{min = vector, max = vector}}", what)};
    // Either corner order works.
    const Box ordered{{std::min(box.min.x, box.max.x), std::min(box.min.y, box.max.y), std::min(box.min.z, box.max.z)},
                      {std::max(box.min.x, box.max.x), std::max(box.min.y, box.max.y), std::max(box.min.z, box.max.z)}};
    return ordered;
}

std::vector<Box> boxes_of(const Value &value, std::string_view what)
{
    std::vector<Box> boxes;
    if (value.is_nil())
        return boxes;
    if (value.type() != ValueType::array)
        throw ScriptError{std::format("{}: expected a list of boxes", what)};
    for (const Value &entry : value.as_array())
        boxes.push_back(box_of(entry, what));
    if (boxes.size() > max_boxes_per_part)
        throw ScriptError{std::format("{}: at most {} boxes", what, max_boxes_per_part)};
    return boxes;
}

PartSpec part_of(const Value &table, std::string name)
{
    static constexpr std::array<std::string_view, 12> keys{"name",   "box",   "boxes",  "exclude",
                                                           "mesh",   "islands", "copy", "model",
                                                           "pivot",  "rotation", "scale", "hidden"};
    if (table.type() != ValueType::map)
        throw ScriptError{"gunfx.parts: every part is a table"};
    only_keys(table, keys, "part \"" + name + "\"");
    if (name.empty())
        name = text_of(table, "name");
    if (!valid_part_name(name))
        throw ScriptError{std::format("part name \"{}\": use lowercase letters, digits and _ (mag, slide, "
                                      "mag_new), starting with a letter",
                                      name)};
    PartSpec part;
    part.name = std::move(name);
    const std::string where = "part \"" + part.name + "\"";
    if (const Value *box = table.field("box"); box != nullptr && !box->is_nil())
        part.boxes.push_back(box_of(*box, where + " box"));
    if (const Value *boxes = table.field("boxes"); boxes != nullptr)
        for (const Box &box : boxes_of(*boxes, where + " boxes"))
            part.boxes.push_back(box);
    if (const Value *exclude = table.field("exclude"); exclude != nullptr)
        part.exclude = boxes_of(*exclude, where + " exclude");
    if (part.boxes.size() > max_boxes_per_part)
        throw ScriptError{std::format("{}: at most {} boxes", where, max_boxes_per_part)};
    part.mesh = static_cast<int>(number_of(table, "mesh", -1.0F, -1.0F, 127.0F));
    part.islands = flag_of(table, "islands", false);
    part.copy = text_of(table, "copy");
    part.model = text_of(table, "model");
    if (!part.model.empty() && !asset_name(part.model))
        throw ScriptError{std::format("{} model: expected an asset name such as \"models/shell\"", where)};
    if (const Value *rotation = table.field("rotation"); rotation != nullptr && !rotation->is_nil())
        part.rotation = vector_of(*rotation, where + " rotation");
    part.scale = number_of(table, "scale", 1.0F, 0.001F, 100.0F);
    if (const Value *pivot = table.field("pivot"); pivot != nullptr && !pivot->is_nil())
        part.pivot = vector_of(*pivot, where + " pivot");
    part.hidden = flag_of(table, "hidden", false);
    const int sources = (part.boxes.empty() && part.mesh < 0 ? 0 : 1) + (part.copy.empty() ? 0 : 1) +
                        (part.model.empty() ? 0 : 1);
    if (sources == 0)
        throw ScriptError{std::format("{}: give it box/boxes, mesh, copy or model", where)};
    if (!part.copy.empty() && (!part.boxes.empty() || part.mesh >= 0 || !part.model.empty()))
        throw ScriptError{std::format("{}: copy takes the other part's triangles; no boxes, mesh or model", where)};
    if (!part.model.empty() && (!part.boxes.empty() || part.mesh >= 0))
        throw ScriptError{std::format("{}: a model part draws the pack's mesh; no boxes or mesh", where)};
    return part;
}

// --- Spring --------------------------------------------------------------------

std::uint32_t next_random(std::uint32_t &state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

float signed_random(std::uint32_t &state) noexcept
{
    return static_cast<float>(next_random(state) & 0xFFFFU) / 32767.5F - 1.0F;
}

rig::Matrix translation(Vec3 offset) noexcept
{
    rig::Matrix m = rig::identity();
    m[12] = offset.x;
    m[13] = offset.y;
    m[14] = offset.z;
    return m;
}

// XNA CreateFromYawPitchRoll: roll about Z, then pitch about X, then yaw about Y.
rig::Matrix rotation_ypr(float yaw, float pitch, float roll) noexcept
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    rig::Matrix z = rig::identity();
    z[0] = cr;
    z[1] = sr;
    z[4] = -sr;
    z[5] = cr;
    rig::Matrix x = rig::identity();
    x[5] = cp;
    x[6] = sp;
    x[9] = -sp;
    x[10] = cp;
    rig::Matrix y = rig::identity();
    y[0] = cy;
    y[2] = -sy;
    y[8] = sy;
    y[10] = cy;
    return rig::multiply(rig::multiply(z, x), y);
}

} // namespace

// ===========================================================================
// Names
// ===========================================================================

std::string_view family_name(Family family) noexcept
{
    static constexpr std::array<std::string_view, static_cast<std::size_t>(Family::count)> names{
        "Generic", "Fist", "Pick", "Block", "Grenade", "Pistol", "SMG", "Gun", "Rifle", "PumpShotgun",
        "LMG", "RPG", "LaserGun", "LaserPistol", "LaserRifle", "LaserSMG", "LaserShotgun"};
    const auto index = static_cast<std::size_t>(family);
    return index < names.size() ? names[index] : std::string_view{};
}

std::optional<Family> family_from_name(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < static_cast<std::size_t>(Family::count); ++index)
        if (family_name(static_cast<Family>(index)) == name)
            return static_cast<Family>(index);
    return std::nullopt;
}

Family family_of(PlayerAnimationMode mode) noexcept
{
    switch (mode)
    {
    case PlayerAnimationMode::pick: return Family::pick;
    case PlayerAnimationMode::block: return Family::block;
    case PlayerAnimationMode::fist: return Family::fist;
    case PlayerAnimationMode::assault: return Family::gun;
    case PlayerAnimationMode::bolt_rifle: return Family::rifle;
    case PlayerAnimationMode::pistol: return Family::pistol;
    case PlayerAnimationMode::pump_shotgun: return Family::pump_shotgun;
    case PlayerAnimationMode::smg: return Family::smg;
    case PlayerAnimationMode::lmg: return Family::lmg;
    case PlayerAnimationMode::space_assault: return Family::laser_gun;
    case PlayerAnimationMode::space_bolt_rifle: return Family::laser_rifle;
    case PlayerAnimationMode::space_pistol: return Family::laser_pistol;
    case PlayerAnimationMode::space_pump_shotgun: return Family::laser_shotgun;
    case PlayerAnimationMode::space_smg: return Family::laser_smg;
    case PlayerAnimationMode::grenade: return Family::grenade;
    case PlayerAnimationMode::rpg: return Family::rpg;
    case PlayerAnimationMode::generic:
    default: return Family::generic;
    }
}

bool gun_family(Family family) noexcept
{
    return family >= Family::pistol && family < Family::count;
}

bool valid_part_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 32U || name.front() < 'a' || name.front() > 'z')
        return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

ClipName split_clip_name(std::string_view name) noexcept
{
    ClipName result;
    std::string_view base = name;
    if (const std::size_t at = base.find('@'); at != std::string_view::npos)
    {
        result.model = base.substr(at + 1U);
        base = base.substr(0U, at);
    }
    constexpr std::string_view empty_suffix = "Empty";
    if (base.size() > empty_suffix.size() && base.ends_with(empty_suffix))
    {
        result.empty = true;
        base.remove_suffix(empty_suffix.size());
    }
    result.base = base;
    return result;
}

std::vector<std::string> variant_names(std::string_view base, std::string_view model, bool empty,
                                       std::string_view generation)
{
    std::vector<std::string> names;
    const std::string at = model.empty() ? std::string{} : "@" + std::string{model};
    const std::string at_generation =
        model.empty() || generation.empty() ? std::string{} : "@" + std::string{generation} + "/" + std::string{model};
    const auto add = [&](std::string_view suffix) {
        if (!at_generation.empty())
            names.push_back(std::string{base} + std::string{suffix} + at_generation);
        if (!at.empty())
            names.push_back(std::string{base} + std::string{suffix} + at);
    };
    if (empty)
    {
        add("Empty");
        names.push_back(std::string{base} + "Empty");
    }
    add({});
    names.emplace_back(base);
    return names;
}

bool valid_variant_model(std::string_view model) noexcept
{
    if (model.starts_with("classic/"))
        model.remove_prefix(8U);
    else if (model.starts_with("modern/"))
        model.remove_prefix(7U);
    return !model.empty() && model.size() <= 40U && std::all_of(model.begin(), model.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool reload_clip(std::string_view name) noexcept
{
    return split_clip_name(name).base.ends_with("Reload");
}

std::string_view held_model_name(std::uint16_t item) noexcept
{
    // The same table as the renderer's retail_held_model (InventoryItemClass
    // models), by the content pack's file stems.
    switch (item)
    {
    case 13U: case 23U: case 24U: case 25U: case 26U: case 27U: case 28U: return "pickaxe";
    case 14U: return "torch";
    case 15U: case 16U: case 17U: case 18U: return "ore";
    case 19U: return "gems";
    case 20U: case 21U: case 22U: return "bars";
    case 29U: case 30U: case 31U: case 32U: case 33U: return "spade";
    case 34U: case 35U: case 36U: case 37U: case 38U: return "axe";
    case 39U: return "compass";
    case 40U: return "clock";
    case 41U: case 42U: case 43U: case 44U: case 45U: case 46U: case 47U: case 48U:
    case 105U: case 109U: case 111U: return "ammo";
    case 49U: case 55U: case 61U: case 67U: return "knife";
    case 50U: case 56U: case 62U: case 68U: return "ak";
    case 51U: case 57U: case 63U: case 69U: return "pistol";
    case 52U: case 58U: case 64U: case 70U: return "shotgun";
    case 53U: case 59U: case 65U: case 71U: return "rifle";
    case 54U: case 60U: case 66U: case 72U: return "smg";
    case 77U: return "locator";
    case 78U: return "teleporter";
    case 81U: case 114U: case 115U: case 116U: case 117U: case 118U: return "saber";
    case 82U: case 87U: case 92U: case 97U: return "space_ak";
    case 83U: case 88U: case 93U: case 98U: return "space_pistol";
    case 84U: case 89U: case 94U: case 99U: return "space_shotgun";
    case 85U: case 90U: case 95U: case 100U: return "space_rifle";
    case 86U: case 91U: case 96U: case 101U: return "space_smg";
    case 103U: case 104U: case 112U: case 113U: return "rpg";
    case 106U: case 108U: return "gunpowder";
    case 110U: return "grenade";
    case 119U: case 120U: case 121U: case 122U: return "lmg";
    default: return {};
    }
}

// ===========================================================================
// Script values
// ===========================================================================

std::vector<PartSpec> parts_from_value(const Value &parts)
{
    std::vector<PartSpec> result;
    if (parts.type() == ValueType::array)
    {
        for (const Value &entry : parts.as_array())
            result.push_back(part_of(entry, {}));
    }
    else if (parts.type() == ValueType::map)
    {
        for (const auto &[name, entry] : parts.as_map())
        {
            if (const Value *named = entry.field("name"); named != nullptr && !named->is_nil() &&
                                                          (named->type() != ValueType::string || named->as_string() != name))
                throw ScriptError{std::format("part \"{}\": its name field says otherwise", name)};
            result.push_back(part_of(entry, name));
        }
    }
    else
        throw ScriptError{"gunfx.parts: expected a list of parts ({ {name = \"mag\", box = ...}, ... })"};
    if (result.size() > max_parts_per_model)
        throw ScriptError{std::format("gunfx.parts: at most {} parts per model", max_parts_per_model)};
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        for (std::size_t other = 0U; other < index; ++other)
            if (result[other].name == result[index].name)
                throw ScriptError{std::format("gunfx.parts: two parts are named \"{}\"", result[index].name)};
        const std::string &copy = result[index].copy;
        if (copy.empty())
            continue;
        const auto source = std::find_if(result.begin(), result.end(), [&](const PartSpec &part) { return part.name == copy; });
        if (source == result.end() || !source->copy.empty() || !source->model.empty())
            throw ScriptError{std::format("part \"{}\": copy names \"{}\", which is not a part cut from the model",
                                          result[index].name, copy)};
    }
    return result;
}

ClipCues cues_from_value(std::string_view clip, const Value &cues, const Value &options)
{
    if (clip.empty() || clip.size() > 96U ||
        !std::all_of(clip.begin(), clip.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                   c == '@' || c == '/';
        }) ||
        (split_clip_name(clip).base != clip && !split_clip_name(clip).model.empty() &&
         !valid_variant_model(split_clip_name(clip).model)))
        throw ScriptError{"gunfx.events: expected a clip name such as \"GunReload\", \"PumpShotgunReload@rpg\" "
                          "or \"GunShoulderIdle@modern/ak\""};
    ClipCues list;
    list.clip = std::string{clip};
    if (!options.is_nil())
    {
        static constexpr std::array<std::string_view, 5> keys{"model", "mute", "length", "remote_volume",
                                                              "remote_distance"};
        if (options.type() != ValueType::map)
            throw ScriptError{"gunfx.events: options must be a table"};
        only_keys(options, keys, "gunfx.events options");
        list.model = text_of(options, "model");
        if (const Value *mute = options.field("mute"); mute != nullptr && !mute->is_nil())
        {
            if (mute->type() == ValueType::string)
                list.mute.push_back(mute->as_string());
            else if (mute->type() == ValueType::array)
                for (const Value &cue : mute->as_array())
                    list.mute.push_back(cue.as_string());
            else
                throw ScriptError{"mute: expected a cue name or a list of them"};
        }
        if (const Value *length = options.field("length"); length != nullptr && !length->is_nil())
            list.length = number_of(options, "length", 1.0F, 0.01F, 60.0F);
        list.remote_volume = number_of(options, "remote_volume", 0.6F, 0.0F, 4.0F);
        list.remote_distance = number_of(options, "remote_distance", 24.0F, 1.0F, 128.0F);
    }
    if (cues.type() != ValueType::array)
        throw ScriptError{"gunfx.events: expected a list of cues ({ {time = 0.2, sound = \"sounds/x\"}, ... })"};
    if (cues.as_array().size() > max_cues_per_clip)
        throw ScriptError{std::format("gunfx.events: at most {} cues per clip", max_cues_per_clip)};
    static constexpr std::array<std::string_view, 21> keys{
        "time", "at", "who", "sound", "volume", "pitch", "volume_jitter", "pitch_jitter", "show", "hide", "attach",
        "detach", "drop", "kick", "bone", "blend", "velocity", "spin", "life", "seconds", "note"};
    for (const Value &entry : cues.as_array())
    {
        if (entry.type() != ValueType::map)
            throw ScriptError{"gunfx.events: every cue is a table"};
        only_keys(entry, keys, "cue");
        Cue cue;
        const Value *time = entry.field("time");
        const Value *at = entry.field("at");
        if ((time == nullptr || time->is_nil()) == (at == nullptr || at->is_nil()))
            throw ScriptError{"cue: give it time (seconds) or at (0..1)"};
        if (time != nullptr && !time->is_nil())
            cue.time = number_of(entry, "time", 0.0F, 0.0F, 60.0F);
        else
        {
            cue.time = number_of(entry, "at", 0.0F, 0.0F, 1.0F);
            cue.fraction = true;
        }
        if (const std::string who = text_of(entry, "who"); !who.empty())
        {
            if (who == "self")
                cue.who = CueAudience::self;
            else if (who == "others")
                cue.who = CueAudience::others;
            else if (who != "all")
                throw ScriptError{"who: expected \"self\", \"others\" or \"all\""};
        }
        int actions = 0;
        const auto part_action = [&](std::string_view key, CueAction action) {
            const std::string name = text_of(entry, key);
            if (name.empty())
                return;
            if (!valid_part_name(name))
                throw ScriptError{std::format("{}: \"{}\" is not a part name", key, name)};
            cue.action = action;
            cue.part = name;
            ++actions;
        };
        if (const std::string sound = text_of(entry, "sound"); !sound.empty())
        {
            if (!asset_name(sound))
                throw ScriptError{"sound: expected an asset name such as \"sounds/mag_out\""};
            cue.action = CueAction::sound;
            cue.sound = sound;
            ++actions;
        }
        part_action("show", CueAction::show);
        part_action("hide", CueAction::hide);
        part_action("attach", CueAction::attach);
        part_action("detach", CueAction::detach);
        part_action("drop", CueAction::drop);
        if (const Value *kick = entry.field("kick"); kick != nullptr && !kick->is_nil())
        {
            if (kick->type() != ValueType::map)
                throw ScriptError{"kick: expected {pitch =, yaw =, roll =, seconds =} in degrees"};
            static constexpr std::array<std::string_view, 4> kick_keys{"pitch", "yaw", "roll", "seconds"};
            only_keys(*kick, kick_keys, "kick");
            cue.action = CueAction::kick;
            cue.kick = {number_of(*kick, "pitch", 0.0F, -10.0F, 10.0F), number_of(*kick, "yaw", 0.0F, -10.0F, 10.0F),
                        number_of(*kick, "roll", 0.0F, -10.0F, 10.0F)};
            cue.kick_seconds = number_of(*kick, "seconds", 0.15F, 0.02F, 2.0F);
            ++actions;
        }
        if (actions != 1)
            throw ScriptError{"cue: give it exactly one of sound, show, hide, attach, detach, drop, kick"};
        cue.volume = number_of(entry, "volume", 1.0F, 0.0F, 4.0F);
        cue.pitch = number_of(entry, "pitch", 1.0F, 0.25F, 4.0F);
        cue.volume_jitter = number_of(entry, "volume_jitter", 0.0F, 0.0F, 1.0F);
        cue.pitch_jitter = number_of(entry, "pitch_jitter", 0.0F, 0.0F, 0.5F);
        cue.bone = text_of(entry, "bone");
        if (cue.action == CueAction::attach && cue.bone.empty())
            throw ScriptError{"attach: name the bone (bone = \"PropLeft\")"};
        cue.blend = number_of(entry, "blend", 0.1F, 0.0F, 2.0F);
        if (const Value *velocity = entry.field("velocity"); velocity != nullptr && !velocity->is_nil())
            cue.velocity = vector_of(*velocity, "velocity");
        if (const Value *spin = entry.field("spin"); spin != nullptr && !spin->is_nil())
        {
            if (spin->is_number())
            {
                const float amount = number_of(entry, "spin", 0.0F, -100.0F, 100.0F);
                cue.spin = {amount, amount * 0.37F, amount * 0.21F};
            }
            else
                cue.spin = vector_of(*spin, "spin");
        }
        cue.life = number_of(entry, "life", 5.0F, 0.1F, 30.0F);
        list.cues.push_back(std::move(cue));
    }
    std::stable_sort(list.cues.begin(), list.cues.end(), [](const Cue &a, const Cue &b) {
        // Shares and seconds are not comparable before the clip is known;
        // each kind keeps its order, which is all firing needs (it sorts again
        // by tick).
        return !a.fraction && !b.fraction ? a.time < b.time : (a.fraction && b.fraction ? a.time < b.time : false);
    });
    return list;
}

std::optional<MotionSettings> motion_from_value(const Value &settings)
{
    if (settings.is_nil())
        return std::nullopt;
    if (settings.type() != ValueType::map)
        throw ScriptError{"gunfx.motion: expected a table of settings (or nil to turn it off)"};
    static constexpr std::array<std::string_view, 13> keys{"enabled", "sway",     "bob",      "roll",   "sprint",
                                                           "jump",    "breathe",  "recoil",   "switch", "aim_ease",
                                                           "aim_damping", "weight", "families"};
    only_keys(settings, keys, "gunfx.motion");
    if (!flag_of(settings, "enabled", true))
        return std::nullopt;
    const auto tuning = [](const Value &table, const Tuning &base, std::string_view what) {
        static constexpr std::array<std::string_view, 11> family_keys{
            "sway", "bob", "roll", "sprint", "jump", "breathe", "recoil", "switch", "aim_ease", "aim_damping", "weight"};
        only_keys(table, family_keys, what);
        Tuning result = base;
        result.sway = number_of(table, "sway", base.sway, 0.0F, 4.0F);
        result.bob = number_of(table, "bob", base.bob, 0.0F, 4.0F);
        result.roll = number_of(table, "roll", base.roll, 0.0F, 4.0F);
        result.sprint = number_of(table, "sprint", base.sprint, 0.0F, 4.0F);
        result.jump = number_of(table, "jump", base.jump, 0.0F, 4.0F);
        result.breathe = number_of(table, "breathe", base.breathe, 0.0F, 4.0F);
        result.recoil = number_of(table, "recoil", base.recoil, 0.0F, 4.0F);
        result.switch_ease = number_of(table, "switch", base.switch_ease, 0.0F, 4.0F);
        result.aim_ease = number_of(table, "aim_ease", base.aim_ease, 0.0F, 1.0F);
        result.aim_damping = number_of(table, "aim_damping", base.aim_damping, 0.0F, 1.0F);
        result.weight = number_of(table, "weight", base.weight, 0.25F, 4.0F);
        return result;
    };
    MotionSettings motion;
    {
        // The top level without its families table.
        Map top;
        for (const auto &[key, value] : settings.as_map())
            if (key != "families" && key != "enabled")
                top.emplace_back(key, value);
        motion.base = tuning(Value{std::move(top)}, Tuning{}, "gunfx.motion");
    }
    motion.families.fill(motion.base);
    if (const Value *families = settings.field("families"); families != nullptr && !families->is_nil())
    {
        if (families->type() != ValueType::map)
            throw ScriptError{"families: expected { Pistol = {...}, LMG = {...} }"};
        for (const auto &[name, table] : families->as_map())
        {
            const std::optional<Family> family = family_from_name(name);
            if (!family)
                throw ScriptError{std::format("families: unknown family \"{}\" (Pistol, SMG, Gun, Rifle, PumpShotgun, "
                                              "LMG, RPG, LaserGun, LaserPistol, LaserRifle, LaserSMG, LaserShotgun, "
                                              "Fist, Pick, Block, Grenade, Generic)",
                                              name)};
            if (table.type() != ValueType::map)
                throw ScriptError{std::format("families.{}: expected a table", name)};
            motion.families[static_cast<std::size_t>(*family)] = tuning(table, motion.base, "families." + name);
        }
    }
    return motion;
}

// ===========================================================================
// Library
// ===========================================================================

void Library::define_parts(std::string_view mod_id, std::string_view model, std::vector<PartSpec> parts)
{
    auto &entries = parts_[std::string{model}];
    std::erase_if(entries, [&](const auto &entry) { return entry.mod_id == mod_id; });
    entries.push_back({std::string{mod_id}, std::move(parts)});
    ++revision_;
}

void Library::define_cues(ClipCues cues)
{
    auto &entries = cues_[cues.clip + "\n" + cues.model];
    std::erase_if(entries, [&](const auto &entry) { return entry.mod_id == cues.mod_id; });
    std::string mod_id = cues.mod_id;
    entries.push_back({std::move(mod_id), std::move(cues)});
    ++revision_;
}

void Library::define_motion(std::string_view mod_id, std::optional<MotionSettings> motion)
{
    std::erase_if(motions_, [&](const MotionSettings &entry) { return entry.mod_id == mod_id; });
    if (motion)
    {
        motion->mod_id = std::string{mod_id};
        motions_.push_back(std::move(*motion));
    }
    ++revision_;
}

void Library::release_mod(std::string_view mod_id)
{
    const auto drop = [&](auto &map) {
        for (auto entry = map.begin(); entry != map.end();)
        {
            std::erase_if(entry->second, [&](const auto &value) { return value.mod_id == mod_id; });
            entry = entry->second.empty() ? map.erase(entry) : std::next(entry);
        }
    };
    drop(parts_);
    drop(cues_);
    std::erase_if(motions_, [&](const MotionSettings &entry) { return entry.mod_id == mod_id; });
    ++revision_;
}

void Library::clear() noexcept
{
    parts_.clear();
    cues_.clear();
    motions_.clear();
    ++revision_;
}

const std::vector<PartSpec> *Library::parts(std::string_view generation, std::string_view model) const
{
    if (model.empty())
        return nullptr;
    if (!generation.empty())
        if (const auto found = parts_.find(std::string{generation} + "/" + std::string{model});
            found != parts_.end() && !found->second.empty())
            return &found->second.back().value;
    const auto found = parts_.find(model);
    return found != parts_.end() && !found->second.empty() ? &found->second.back().value : nullptr;
}

const ClipCues *Library::cues(std::string_view clip, std::string_view model) const
{
    if (cues_.empty())
        return nullptr;
    std::string key{clip};
    key += '\n';
    if (!model.empty())
        if (const auto found = cues_.find(key + std::string{model}); found != cues_.end() && !found->second.empty())
            return &found->second.back().value;
    const auto found = cues_.find(key);
    return found != cues_.end() && !found->second.empty() ? &found->second.back().value : nullptr;
}

const MotionSettings *Library::motion() const noexcept
{
    return motions_.empty() ? nullptr : &motions_.back();
}

bool Library::mutes(std::string_view clip, std::string_view model, bool empty, std::string_view cue,
                    std::string_view generation) const
{
    if (cues_.empty())
        return false;
    for (const std::string &name : variant_names(split_clip_name(clip).base, model, empty, generation))
        if (const ClipCues *list = cues(name, model))
            return std::find(list->mute.begin(), list->mute.end(), cue) != list->mute.end();
    return false;
}

// ===========================================================================
// Part tracks
// ===========================================================================

const PartTrack *ClipTracks::find(std::string_view part) const noexcept
{
    const auto found = std::find_if(parts.begin(), parts.end(), [&](const PartTrack &track) { return track.part == part; });
    return found != parts.end() ? &*found : nullptr;
}

namespace
{
std::mutex &tracks_mutex()
{
    static std::mutex mutex;
    return mutex;
}
std::shared_ptr<const TrackTable> &tracks_slot()
{
    static std::shared_ptr<const TrackTable> table;
    return table;
}
} // namespace

std::shared_ptr<const TrackTable> installed_tracks() noexcept
{
    const std::lock_guard lock{tracks_mutex()};
    return tracks_slot();
}

void install_tracks(std::shared_ptr<const TrackTable> table) noexcept
{
    const std::lock_guard lock{tracks_mutex()};
    tracks_slot() = std::move(table);
}

std::int64_t cue_ticks(const Cue &cue, const ClipCues &list, const ClipTracks *tracks,
                       std::int64_t duration_ticks) noexcept
{
    const double duration = static_cast<double>(std::max<std::int64_t>(duration_ticks, 0));
    if (cue.fraction)
        return static_cast<std::int64_t>(std::llround(static_cast<double>(cue.time) * duration));
    // A stretched clip (a reload) maps its authored seconds onto the game's
    // clip; anything else plays on its own clock, second for second.
    float authored = 0.0F;
    bool stretched = false;
    if (tracks != nullptr && tracks->authored_seconds > 0.0F)
    {
        authored = tracks->authored_seconds;
        stretched = tracks->stretched;
    }
    else if (list.length.has_value())
    {
        authored = *list.length;
        stretched = reload_clip(list.clip);
    }
    if (stretched && authored > 0.0F)
        return static_cast<std::int64_t>(std::llround(static_cast<double>(cue.time) / authored * duration));
    return static_cast<std::int64_t>(std::llround(static_cast<double>(cue.time) * ticks_per_second));
}

// ===========================================================================
// Cutting
// ===========================================================================

CutResult cut_parts(std::span<const PartSpec> parts, std::span<const MeshTriangles> meshes)
{
    CutResult result;
    result.parts.resize(parts.size());
    result.owner.resize(meshes.size());
    for (std::size_t mesh = 0U; mesh < meshes.size(); ++mesh)
        result.owner[mesh].assign(meshes[mesh].indices.size() / 3U, -1);
    for (CutPart &part : result.parts)
        part.triangles.resize(meshes.size());

    const auto vertex = [&](std::size_t mesh, std::uint32_t index) -> Vec3 {
        const auto &positions = meshes[mesh].positions;
        if (index >= positions.size())
            return {};
        const auto &p = positions[index];
        return {p[0], p[1], p[2]};
    };
    // Connected islands per mesh, built only when a part asks for them:
    // triangles that share a welded corner (0.1 mm) are one island.
    std::vector<std::vector<int>> island_of(meshes.size());
    std::vector<std::vector<Vec3>> island_centre(meshes.size());
    const auto build_islands = [&](std::size_t mesh) {
        if (!island_of[mesh].empty() || meshes[mesh].indices.size() < 3U)
            return;
        const auto &indices = meshes[mesh].indices;
        const std::size_t triangles = indices.size() / 3U;
        // Weld by position.
        std::map<std::array<std::int64_t, 3>, std::uint32_t> welded;
        std::vector<std::uint32_t> weld(meshes[mesh].positions.size());
        for (std::size_t index = 0U; index < weld.size(); ++index)
        {
            const Vec3 p = vertex(mesh, static_cast<std::uint32_t>(index));
            const std::array<std::int64_t, 3> key{std::llround(p.x * 1.0e4F), std::llround(p.y * 1.0e4F),
                                                  std::llround(p.z * 1.0e4F)};
            weld[index] = welded.try_emplace(key, static_cast<std::uint32_t>(welded.size())).first->second;
        }
        std::vector<std::uint32_t> parent(welded.size());
        for (std::size_t index = 0U; index < parent.size(); ++index)
            parent[index] = static_cast<std::uint32_t>(index);
        const auto find = [&](std::uint32_t at) {
            while (parent[at] != at)
                at = parent[at] = parent[parent[at]];
            return at;
        };
        for (std::size_t triangle = 0U; triangle < triangles; ++triangle)
        {
            std::array<std::uint32_t, 3> corners{};
            for (std::size_t corner = 0U; corner < 3U; ++corner)
            {
                const std::uint32_t index = indices[triangle * 3U + corner];
                corners[corner] = find(index < weld.size() ? weld[index] : 0U);
            }
            parent[corners[1]] = corners[0];
            parent[find(corners[2])] = find(corners[0]);
        }
        std::map<std::uint32_t, int> numbered;
        island_of[mesh].resize(triangles);
        std::vector<Vec3> low;
        std::vector<Vec3> high;
        for (std::size_t triangle = 0U; triangle < triangles; ++triangle)
        {
            const std::uint32_t index = indices[triangle * 3U];
            const std::uint32_t root = find(index < weld.size() ? weld[index] : 0U);
            const auto [entry, fresh] = numbered.try_emplace(root, static_cast<int>(numbered.size()));
            if (fresh)
            {
                low.push_back({1.0e9F, 1.0e9F, 1.0e9F});
                high.push_back({-1.0e9F, -1.0e9F, -1.0e9F});
            }
            const int island = entry->second;
            island_of[mesh][triangle] = island;
            for (std::size_t corner = 0U; corner < 3U; ++corner)
            {
                const Vec3 p = vertex(mesh, indices[triangle * 3U + corner]);
                Vec3 &a = low[static_cast<std::size_t>(island)];
                Vec3 &b = high[static_cast<std::size_t>(island)];
                a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
                b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
            }
        }
        island_centre[mesh].resize(low.size());
        for (std::size_t island = 0U; island < low.size(); ++island)
            island_centre[mesh][island] = (low[island] + high[island]) * 0.5F;
    };

    const auto in_boxes = [](const PartSpec &spec, Vec3 point) {
        const bool inside = std::any_of(spec.boxes.begin(), spec.boxes.end(), [&](const Box &box) { return box.contains(point); });
        return inside && std::none_of(spec.exclude.begin(), spec.exclude.end(),
                                      [&](const Box &box) { return box.contains(point); });
    };

    for (std::size_t part_index = 0U; part_index < parts.size(); ++part_index)
    {
        const PartSpec &spec = parts[part_index];
        if (!spec.copy.empty() || !spec.model.empty())
            continue;
        CutPart &part = result.parts[part_index];
        for (std::size_t mesh = 0U; mesh < meshes.size(); ++mesh)
        {
            if (spec.mesh >= 0 && static_cast<std::size_t>(spec.mesh) != mesh)
                continue;
            if (spec.islands)
                build_islands(mesh);
            const auto &indices = meshes[mesh].indices;
            for (std::size_t triangle = 0U; triangle * 3U + 2U < indices.size(); ++triangle)
            {
                if (result.owner[mesh][triangle] >= 0)
                    continue;
                bool take = spec.boxes.empty(); // a whole mesh
                if (!take)
                {
                    Vec3 point{};
                    if (spec.islands && triangle < island_of[mesh].size())
                        point = island_centre[mesh][static_cast<std::size_t>(island_of[mesh][triangle])];
                    else
                        point = (vertex(mesh, indices[triangle * 3U]) + vertex(mesh, indices[triangle * 3U + 1U]) +
                                 vertex(mesh, indices[triangle * 3U + 2U])) *
                                (1.0F / 3.0F);
                    take = in_boxes(spec, point);
                }
                if (!take)
                    continue;
                result.owner[mesh][triangle] = static_cast<int>(part_index);
                part.triangles[mesh].push_back(static_cast<std::uint32_t>(triangle));
            }
        }
    }
    // Copies take their source's triangles; bounds and pivots for all.
    for (std::size_t part_index = 0U; part_index < parts.size(); ++part_index)
    {
        const PartSpec &spec = parts[part_index];
        CutPart &part = result.parts[part_index];
        if (!spec.copy.empty())
            for (std::size_t source = 0U; source < parts.size(); ++source)
                if (parts[source].name == spec.copy)
                    part.triangles = result.parts[source].triangles;
        Vec3 low{1.0e9F, 1.0e9F, 1.0e9F};
        Vec3 high{-1.0e9F, -1.0e9F, -1.0e9F};
        for (std::size_t mesh = 0U; mesh < part.triangles.size(); ++mesh)
            for (const std::uint32_t triangle : part.triangles[mesh])
                for (std::size_t corner = 0U; corner < 3U; ++corner)
                {
                    const Vec3 p = vertex(mesh, meshes[mesh].indices[triangle * 3U + corner]);
                    low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                    high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                }
        part.count = 0U;
        for (const auto &list : part.triangles)
            part.count += list.size();
        if (part.count > 0U)
        {
            part.bounds_min = low;
            part.bounds_max = high;
        }
        part.pivot = spec.pivot.value_or(part.count > 0U ? (low + high) * 0.5F : Vec3{});
    }
    return result;
}

// ===========================================================================
// Part poses
// ===========================================================================

rig::Matrix part_motion(const PartTrack *track, std::int64_t position_ticks, Vec3 pivot) noexcept
{
    rig::BoneTransform motion;
    if (track != nullptr)
    {
        const double frame = std::max(0.0, static_cast<double>(position_ticks) / ticks_per_second * 30.0);
        const auto sample3 = [&](const std::vector<Vec3> &keys, Vec3 fallback) {
            if (keys.empty())
                return fallback;
            const std::size_t last = keys.size() - 1U;
            const std::size_t index = std::min(static_cast<std::size_t>(frame), last);
            const std::size_t next = std::min(index + 1U, last);
            const float t = static_cast<float>(std::clamp(frame - static_cast<double>(index), 0.0, 1.0));
            return keys[index] + (keys[next] - keys[index]) * t;
        };
        const Vec3 t = sample3(track->translation, {});
        const Vec3 s = sample3(track->scale, {1.0F, 1.0F, 1.0F});
        motion.translation = {t.x, t.y, t.z};
        motion.scale = {s.x, s.y, s.z};
        if (!track->rotation.empty())
        {
            const std::size_t last = track->rotation.size() - 1U;
            const std::size_t index = std::min(static_cast<std::size_t>(frame), last);
            const std::size_t next = std::min(index + 1U, last);
            const float t2 = static_cast<float>(std::clamp(frame - static_cast<double>(index), 0.0, 1.0));
            const auto &a = track->rotation[index];
            const auto &b = track->rotation[next];
            rig::Quat q = rig::slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, t2);
            const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (length > 1.0e-6F)
                q = {q.x / length, q.y / length, q.z / length, q.w / length};
            motion.rotation = q;
        }
    }
    motion.translation.x += pivot.x;
    motion.translation.y += pivot.y;
    motion.translation.z += pivot.z;
    return rig::multiply(translation({-pivot.x, -pivot.y, -pivot.z}), rig::compose(motion));
}

void part_states(std::span<const PartSpec> parts, const ClipCues *list, const ClipTracks *tracks,
                 std::int64_t duration_ticks, std::int64_t position_ticks,
                 const std::function<int(std::string_view)> &bone_index, std::vector<PartState> &states)
{
    states.assign(parts.size(), PartState{});
    for (std::size_t index = 0U; index < parts.size(); ++index)
        states[index].visible = !parts[index].hidden;
    if (list == nullptr)
        return;
    const auto part_named = [&](std::string_view name) -> int {
        for (std::size_t index = 0U; index < parts.size(); ++index)
            if (parts[index].name == name)
                return static_cast<int>(index);
        return -1;
    };
    // The cues in firing order: by tick, then as written.
    std::vector<std::pair<std::int64_t, int>> order;
    order.reserve(list->cues.size());
    for (std::size_t index = 0U; index < list->cues.size(); ++index)
        if (list->cues[index].action != CueAction::sound && list->cues[index].action != CueAction::kick)
            order.emplace_back(cue_ticks(list->cues[index], *list, tracks, duration_ticks), static_cast<int>(index));
    std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[ticks, index] : order)
    {
        if (ticks > position_ticks)
            break;
        const Cue &cue = list->cues[static_cast<std::size_t>(index)];
        const int part = part_named(cue.part);
        // How far through the cue's blend this time is.
        const double blend_ticks = static_cast<double>(cue.blend) * ticks_per_second;
        const float eased = blend_ticks <= 0.0 ? 1.0F
                                                : static_cast<float>(std::clamp(
                                                      static_cast<double>(position_ticks - ticks) / blend_ticks, 0.0, 1.0));
        if (part < 0)
            continue;
        PartState &state = states[static_cast<std::size_t>(part)];
        switch (cue.action)
        {
        case CueAction::show:
            state.visible = true;
            state.dropped = false;
            if (!cue.bone.empty())
                if (const int bone = bone_index ? bone_index(cue.bone) : -1; bone >= 0)
                {
                    state.bone = bone;
                    state.attach_cue = index;
                    state.attach_weight = eased;
                    state.detach_cue = -1;
                    state.detach_weight = 1.0F;
                }
            break;
        case CueAction::hide:
            state.visible = false;
            break;
        case CueAction::attach:
            if (const int bone = bone_index ? bone_index(cue.bone) : -1; bone >= 0)
            {
                state.bone = bone;
                state.attach_cue = index;
                state.attach_weight = eased;
                state.detach_cue = -1;
                state.detach_weight = 1.0F;
            }
            break;
        case CueAction::detach:
            if (state.bone >= 0)
            {
                const float weight = eased;
                if (weight >= 1.0F)
                {
                    state.bone = -1;
                    state.attach_cue = -1;
                    state.attach_weight = 1.0F;
                    state.detach_cue = -1;
                    state.detach_weight = 1.0F;
                }
                else
                {
                    state.detach_cue = index;
                    state.detach_weight = weight;
                }
            }
            break;
        case CueAction::drop:
            state.dropped = true;
            state.visible = false;
            break;
        case CueAction::sound:
        case CueAction::kick:
            break;
        }
    }
}

// ===========================================================================
// Firing cues
// ===========================================================================

CueCursor::Step CueCursor::advance(const void *playing, std::int64_t at, std::int64_t duration, bool looping,
                                   const ClipCues *list, const ClipTracks *tracks, std::int64_t late_start)
{
    Step step;
    const auto fire_between = [&](std::int64_t after, std::int64_t until, bool inclusive_start) {
        if (list == nullptr)
            return;
        std::vector<std::pair<std::int64_t, int>> crossed;
        for (std::size_t index = 0U; index < list->cues.size(); ++index)
        {
            const std::int64_t ticks = cue_ticks(list->cues[index], *list, tracks, duration);
            if ((inclusive_start ? ticks >= after : ticks > after) && ticks <= until)
                crossed.emplace_back(ticks, static_cast<int>(index));
        }
        std::stable_sort(crossed.begin(), crossed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &entry : crossed)
            step.fired.push_back(entry.second);
    };
    if (playing != clip)
    {
        if (clip != nullptr && started)
        {
            // The clip before ran out, or something else took the channel
            // while it still had a way to go.
            const std::int64_t margin = std::max<std::int64_t>(2 * last_step_, 400'000);
            if (looping_ || reversed_ || position + margin >= duration_)
                step.ended = true;
            else
                step.cut = true;
        }
        clip = playing;
        started = playing != nullptr;
        position = at;
        duration_ = duration;
        looping_ = looping;
        last_step_ = 0;
        reversed_ = false;
        if (playing == nullptr)
            return step;
        step.restarted = true;
        // A clip that starts its second half is either joined late (a
        // capture, a player who comes into view) or played backwards (the
        // sights lowered): neither fires what it skipped.
        if (!looping && at > duration / 2)
            reversed_ = true;
        if (at <= late_start && !reversed_)
            fire_between(0, at, true);
        return step;
    }
    if (playing == nullptr)
        return step;
    duration_ = duration;
    if (at >= position)
    {
        if (reversed_ && at > position)
            reversed_ = false; // a late join that is playing forward after all
        if (!reversed_)
            fire_between(position, at, false);
        last_step_ = at - position;
    }
    else if (reversed_)
    {
        last_step_ = position - at;
    }
    else if (looping)
    {
        // Wrapped round: the end of this loop, then the start of the next.
        fire_between(position, duration, false);
        fire_between(0, at, true);
        step.restarted = true;
    }
    else
    {
        // Played again from the start (a shot, the next shell).
        step.ended = true;
        step.restarted = true;
        fire_between(0, at, true);
        last_step_ = 0;
    }
    position = at;
    return step;
}

// ===========================================================================
// Motion
// ===========================================================================

void Spring::step(float target, float frequency, float damping, float seconds) noexcept
{
    if (!(seconds > 0.0F) || !(frequency > 0.0F))
        return;
    // x'' = -w^2 (x - target) - 2 z w x', solved exactly over `seconds`.
    const double w = 2.0 * std::numbers::pi * static_cast<double>(frequency);
    const double z = std::max(0.0, static_cast<double>(damping));
    const double t = static_cast<double>(seconds);
    const double x0 = static_cast<double>(position) - static_cast<double>(target);
    const double v0 = static_cast<double>(velocity);
    double x = 0.0;
    double v = 0.0;
    if (z < 0.9999)
    {
        const double wd = w * std::sqrt(1.0 - z * z);
        const double decay = std::exp(-z * w * t);
        const double c = std::cos(wd * t);
        const double s = std::sin(wd * t);
        const double b = (v0 + z * w * x0) / wd;
        x = decay * (x0 * c + b * s);
        v = decay * ((-z * w) * (x0 * c + b * s) + (-x0 * wd * s + b * wd * c));
    }
    else if (z <= 1.0001)
    {
        const double decay = std::exp(-w * t);
        const double b = v0 + w * x0;
        x = decay * (x0 + b * t);
        v = decay * (b - w * (x0 + b * t));
    }
    else
    {
        const double root = std::sqrt(z * z - 1.0);
        const double r1 = -w * (z - root);
        const double r2 = -w * (z + root);
        const double c2 = (v0 - r1 * x0) / (r2 - r1);
        const double c1 = x0 - c2;
        x = c1 * std::exp(r1 * t) + c2 * std::exp(r2 * t);
        v = c1 * r1 * std::exp(r1 * t) + c2 * r2 * std::exp(r2 * t);
    }
    position = static_cast<float>(x + static_cast<double>(target));
    velocity = static_cast<float>(v);
    if (!std::isfinite(position) || !std::isfinite(velocity))
    {
        position = target;
        velocity = 0.0F;
    }
}

float Motion::ease_aim(float share, float amount) noexcept
{
    const float linear = std::clamp(share, 0.0F, 1.0F);
    // Smootherstep: no velocity at either end, the sights settle in.
    const float eased = linear * linear * linear * (linear * (linear * 6.0F - 15.0F) + 10.0F);
    return linear + (eased - linear) * std::clamp(amount, 0.0F, 1.0F);
}

void Motion::reset() noexcept
{
    *this = Motion{};
}

MotionOutput Motion::step(const MotionSettings &settings, const MotionInput &input) noexcept
{
    MotionOutput output;
    const Tuning &tuning = settings.tuning(input.family);
    output.aim_ease = tuning.aim_ease;
    const float dt = std::clamp(input.seconds, 0.0F, 0.1F);
    const float motion = input.reduced_motion ? 0.0F : 1.0F;
    const float weight = std::max(tuning.weight, 0.25F);
    const float slow = 1.0F / std::sqrt(weight); // spring frequencies
    if (!started_)
    {
        started_ = true;
        last_yaw_ = input.yaw;
        last_pitch_ = input.pitch;
        was_grounded_ = input.grounded;
    }
    clock_ += dt;
    if (input.seed != 0U && random_ == 0x9E3779B9U)
        random_ ^= input.seed;

    // How aimed: sway, bob, roll and breathing shrink towards aim_damping.
    const float aim = std::clamp(input.aim, 0.0F, 1.0F);
    const float steady = 1.0F + (tuning.aim_damping - 1.0F) * aim;

    // --- Look sway: the weapon lags the camera and springs back. The turn of
    // this frame goes into the spring's velocity, so how the turn is cut into
    // frames barely matters.
    float turn_yaw = input.yaw - last_yaw_;
    turn_yaw -= std::round(turn_yaw / (2.0F * pi)) * 2.0F * pi;
    const float turn_pitch = input.pitch - last_pitch_;
    last_yaw_ = input.yaw;
    last_pitch_ = input.pitch;
    const float sway_gain = 9.0F * tuning.sway * motion * std::sqrt(weight);
    // A snap of the view (a respawn, a teleport) is not a turn.
    if (std::abs(turn_yaw) < 1.2F && std::abs(turn_pitch) < 1.2F)
    {
        sway_yaw_.velocity += turn_yaw * sway_gain;
        sway_pitch_.velocity -= turn_pitch * sway_gain;
    }
    sway_yaw_.step(0.0F, 4.2F * slow, 0.62F, dt);
    sway_pitch_.step(0.0F, 4.2F * slow, 0.62F, dt);
    const float sway_limit = 7.0F * degrees * std::max(tuning.sway, 0.0F);
    sway_yaw_.position = std::clamp(sway_yaw_.position, -sway_limit, sway_limit);
    sway_pitch_.position = std::clamp(sway_pitch_.position, -sway_limit, sway_limit);

    // --- Movement: bob (a figure eight per stride), strafe roll, sprint.
    const float right_x = std::cos(input.yaw);
    const float right_z = std::sin(input.yaw);
    const float ahead_x = std::sin(input.yaw);
    const float ahead_z = -std::cos(input.yaw);
    const float lateral = input.velocity.x * right_x + input.velocity.z * right_z;
    const float forward = input.velocity.x * ahead_x + input.velocity.z * ahead_z;
    const float speed = std::sqrt(input.velocity.x * input.velocity.x + input.velocity.z * input.velocity.z);
    const bool walking = input.grounded && !input.flying && speed > 0.3F;
    const float bob_target = walking ? std::clamp(speed / 4.0F, 0.0F, 1.6F) : 0.0F;
    bob_amount_ += (bob_target - bob_amount_) * (1.0F - std::exp(-dt * 7.0F));
    bob_phase_ += dt * 2.0F * pi * (0.55F + 0.13F * std::min(speed, 10.0F)) * (walking ? 1.0F : 0.4F);
    if (bob_phase_ > 2.0F * pi * 64.0F)
        bob_phase_ -= 2.0F * pi * 64.0F;
    roll_.step(std::clamp(-lateral / 4.0F, -1.5F, 1.5F) * 3.5F * degrees * tuning.roll * motion, 3.2F * slow, 0.75F, dt);
    const bool sprinting = input.sprinting && walking && forward > 2.0F && aim < 0.5F;
    sprint_.step(sprinting ? tuning.sprint * motion : 0.0F, 2.6F * slow, 0.85F, dt);

    // --- Jumping and landing kick the weapon down; it springs back up
    // (metres).
    if (!input.grounded && !input.flying)
        fall_speed_ = std::max(fall_speed_, -input.velocity.y);
    if (was_grounded_ && !input.grounded && !input.flying && input.velocity.y > 1.0F)
        vertical_.velocity -= 0.5F * tuning.jump * motion;
    if (!was_grounded_ && input.grounded)
    {
        vertical_.velocity -= std::clamp(fall_speed_ / 12.0F, 0.25F, 1.5F) * 1.2F * tuning.jump * motion;
        fall_speed_ = 0.0F;
    }
    was_grounded_ = input.grounded;
    vertical_.step(0.0F, 4.8F * slow, 0.42F, dt);

    // --- A shot pushes the weapon back (metres) and up (radians); it springs
    // home. The camera and the aim are the game's and stay where they are.
    const float snap = 7.5F * slow;
    const float omega = 2.0F * pi * snap;
    if (input.kick_roll != 0.0F)
        recoil_roll_.velocity += input.kick_roll * degrees * omega * 1.8F * motion;
    if (input.shot)
    {
        const float kick = tuning.recoil / std::sqrt(weight) * motion;
        recoil_back_.velocity += 2.4F * kick;
        recoil_pitch_.velocity += 3.5F * kick;
        recoil_yaw_.velocity += signed_random(random_) * 1.0F * kick;
        recoil_roll_.velocity += signed_random(random_) * 2.0F * kick;
    }
    recoil_back_.step(0.0F, snap, 0.55F, dt);
    recoil_pitch_.step(0.0F, snap * 0.9F, 0.5F, dt);
    recoil_yaw_.step(0.0F, snap, 0.6F, dt);
    recoil_roll_.step(0.0F, snap * 0.8F, 0.55F, dt);

    // --- A new item comes up from below.
    if (input.switched && tuning.switch_ease > 0.0F)
    {
        lowered_.position = std::min(1.0F, tuning.switch_ease) * motion;
        lowered_.velocity = 0.0F;
    }
    lowered_.step(0.0F, 2.4F * slow, 1.0F, dt);

    // --- Breathing: a slow drift, mostly when standing still.
    const float still = 1.0F - std::clamp(bob_amount_, 0.0F, 1.0F) * 0.7F;
    const float breathe = tuning.breathe * motion * still;
    const float breath = std::sin(clock_ * 2.0F * pi * 0.23F);

    // --- The pose.
    const float bob = tuning.bob * motion * bob_amount_ * std::sqrt(weight);
    const float stride = std::sin(bob_phase_);
    // Two smooth dips per stride (a figure eight with the side-to-side `stride`).
    const float step_bounce = std::sin(bob_phase_ * 2.0F);
    const float dip = stride * stride;
    const float sprint = sprint_.position;
    const float lower = lowered_.position;

    Vec3 offset{};
    offset.x = (stride * 0.011F * bob + sway_yaw_.position * -0.09F + sprint * 0.035F) * steady +
               std::sin(clock_ * 2.0F * pi * 0.11F) * 0.0015F * breathe * steady;
    offset.y = (-dip * 0.010F * bob + breath * 0.0025F * breathe) * steady + vertical_.position -
               sprint * 0.03F - lower * 0.22F;
    offset.z = recoil_back_.position + sprint * 0.02F;
    const float yaw = (sway_yaw_.position + stride * 1.1F * degrees * bob) * steady + sprint * 22.0F * degrees +
                      recoil_yaw_.position +
                      std::sin(clock_ * 2.0F * pi * 0.09F + 1.3F) * 0.25F * degrees * breathe * steady;
    const float pitch =
        (sway_pitch_.position + step_bounce * 0.5F * degrees * bob + breath * 0.45F * degrees * breathe) * steady +
        vertical_.position * 0.6F + recoil_pitch_.position - sprint * 9.0F * degrees - lower * 32.0F * degrees;
    const float roll = (roll_.position + stride * 1.4F * degrees * bob + sway_yaw_.position * 0.45F) * steady +
                       sprint * 14.0F * degrees + recoil_roll_.position + lower * 8.0F * degrees;

    // Turned about a point at the grip, in front of and below the eye.
    const Vec3 pivot{0.06F, -0.16F, -0.32F};
    output.offset = rig::multiply(rig::multiply(translation({-pivot.x, -pivot.y, -pivot.z}), rotation_ypr(yaw, pitch, roll)),
                                  translation(pivot + offset));
    return output;
}

} // namespace openminerz::modding::gunfx
