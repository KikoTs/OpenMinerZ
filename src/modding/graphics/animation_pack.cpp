// Animation packs: glTF clips that replace the retail character's registered
// clips (openminerz/modding/graphics.hpp, openminerz/retail_rig.hpp). The
// frame a file is read in: openminerz/modding/animation_authoring.hpp.

#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/gunfx.hpp" // (GunFX)

#include "openminerz/retail_rig.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace openminerz::modding::graphics
{
namespace
{

constexpr std::array<AnimationSlot, 33> slots{{
    {"Stand", "standing still, lower body"},
    {"Walk", "walking, lower body"},
    {"Run", "running, lower body"},
    {"Swim", "swimming (the Lagoon water enhancement only)"},
    {"Die", "the death fall"},
    {"Wave", "the wave emote"},
    {"Tilt", "the torso pitch that follows the camera"},
    {"IdleHead", "the head's idle look-around"},
    {"GenericIdle", "upper body, holding nothing special"},
    {"GenericWalk", "upper body while walking"},
    {"GenericUse", "upper body using an item"},
    {"FistIdle", "bare hands, idle"},
    {"FistWalk", "bare hands, walking"},
    {"FistUse", "bare hands, punching"},
    {"PickIdle", "pickaxe / tool, idle"},
    {"PickWalk", "pickaxe / tool, walking"},
    {"PickUse", "pickaxe / tool, swinging"},
    {"BlockIdle", "holding a block, idle"},
    {"BlockWalk", "holding a block, walking"},
    {"BlockUse", "placing a block"},
    {"GrenadeIdle", "holding a grenade"},
    {"GrenadeWalk", "holding a grenade, walking"},
    {"GunIdle", "rifle-class gun, idle"},
    {"GunRun", "rifle-class gun, running"},
    {"GunShoot", "rifle-class gun, firing from the hip"},
    {"GunReload", "rifle-class gun, reloading"},
    {"GunShoulder", "rifle-class gun, raising to the eye"},
    {"GunShoulderIdle", "rifle-class gun, aimed"},
    {"GunShoulderShoot", "rifle-class gun, firing aimed"},
    {"PistolIdle", "pistol, idle"},
    {"PistolShoot", "pistol, firing"},
    {"PistolReload", "pistol, reloading"},
    {"RPGShoot", "rocket launcher, firing"},
}};

enum Channel : std::size_t
{
    translation,
    rotation,
    scale
};

// (GunFX) File seconds per second of the game's clip for a stretched clip:
// 1 when it is not stretched, or already as long as the game's to within a
// frame (keys then stay exactly where they were authored).
double stretch_pace(bool stretch, double length, double game_length) noexcept
{
    if (!stretch || !(length > 0.0) || !(game_length > 0.0) ||
        std::abs(length - game_length) <= 1.0 / clip_frame_rate + 1.0e-4)
        return 1.0;
    return length / game_length;
}

} // namespace

std::span<const AnimationSlot> animation_slots() noexcept
{
    return slots;
}

std::optional<rig::Clip> clip_from_animation(const Model &model, int index,
                                             const rig::RetailRig &rig, const rig::Clip *fallback,
                                             std::string *error, bool stretch)
{
    const auto refuse = [&](std::string message) -> std::optional<rig::Clip> {
        if (error != nullptr)
            *error = std::move(message);
        return std::nullopt;
    };
    if (index < 0 || static_cast<std::size_t>(index) >= model.animations.size())
        return refuse("the file has no animation");
    const ModelAnimation &animation = model.animations[static_cast<std::size_t>(index)];
    if (!std::isfinite(animation.duration) || animation.duration < 0.0F ||
        animation.duration > 60.0F)
        return refuse("the clip must last at most 60 seconds");

    const BoneNodes nodes = match_bones(model, rig);
    if (std::all_of(nodes.begin(), nodes.end(), [](int node) { return node < 0; }))
        return refuse("no node is named after a retail bone (Root, BackLower, Head, ...)");

    // What the animation writes, per node and channel, and until when.
    std::vector<std::array<bool, 3>> animated(model.nodes.size());
    std::vector<std::array<float, 3>> last_key(model.nodes.size());
    for (const AnimationChannel &channel : animation.channels)
        if (channel.node >= 0 && static_cast<std::size_t>(channel.node) < animated.size())
        {
            const Channel path = channel.path == AnimationChannel::Path::translation ? translation
                                 : channel.path == AnimationChannel::Path::rotation  ? rotation
                                                                                      : scale;
            const auto node = static_cast<std::size_t>(channel.node);
            animated[node][path] = true;
            last_key[node][path] = std::max(last_key[node][path], channel.times.back());
        }

    // The clip runs on the replaced clip's clock: the game decides when a
    // swing, a step or a reload starts and ends, a presentation pack never
    // does. The file's keys play from the start at their own speed, sampled
    // every 1/30 s as the engine stores them; a shorter clip holds its last
    // pose until the game ends it, a longer one is cut off (the game's own
    // clips do both). Each channel keeps its own key count, because the
    // engine's last-interval rule (AnimationClip's `Length - 2` test) applies
    // to each key array: so a clip exported from the game comes back key for
    // key.
    const double length = animation.duration;
    constexpr double frame = 1.0 / clip_frame_rate;
    // (GunFX) A stretched clip (a reload) spreads the file's keys over the
    // replaced clip's length: `pace` file seconds per second of the game's clip.
    const double game_length = fallback != nullptr ? rig::seconds_from_ticks(fallback->duration_ticks) : length;
    const double pace = stretch_pace(stretch && fallback != nullptr, length, game_length);
    const double span = pace != 1.0 ? game_length : length;
    const int frames = static_cast<int>(std::floor(span * clip_frame_rate + 1.0e-3)) + 1;
    const auto keys_of = [&](std::size_t node, Channel path) {
        return std::min(frames, static_cast<int>(std::floor(static_cast<double>(last_key[node][path]) / pace *
                                                                clip_frame_rate +
                                                            1.0e-3)) +
                                    1);
    };

    rig::Clip clip;
    clip.asset = animation.name;
    clip.frame_rate = clip_frame_rate;
    clip.duration_ticks =
        fallback != nullptr ? fallback->duration_ticks : rig::ticks_from_seconds(length);

    rig::BonePose pose{};
    rig::BonePose first{};
    for (int key = 0; key < frames; ++key)
    {
        const double seconds = std::min(length, static_cast<double>(key) * frame * pace);
        sample_clip_pose(model, index, static_cast<float>(seconds), rig, nodes, pose);
        if (key == 0)
            first = pose;
        for (std::size_t bone = 0U; bone < nodes.size(); ++bone)
        {
            const int node = nodes[bone];
            if (node < 0)
                continue;
            const auto at = static_cast<std::size_t>(node);
            const std::array<bool, 3> &writes = animated[at];
            if (writes[translation] && key < keys_of(at, translation))
                clip.positions[bone].push_back(pose[bone].translation);
            if (writes[rotation] && key < keys_of(at, rotation))
                clip.rotations[bone].push_back(pose[bone].rotation);
            if (writes[scale] && key < keys_of(at, scale))
                clip.scales[bone].push_back(pose[bone].scale);
        }
    }

    // Each channel the file does not animate keeps the replaced clip's keys
    // (or, with nothing replaced, holds the file's rest value).
    for (std::size_t bone = 0U; bone < nodes.size(); ++bone)
    {
        const int node = nodes[bone];
        const bool present = node >= 0;
        if (clip.positions[bone].empty())
        {
            if (fallback != nullptr)
                clip.positions[bone] = fallback->positions[bone];
            else if (present)
                clip.positions[bone] = {first[bone].translation};
        }
        if (clip.rotations[bone].empty())
        {
            if (fallback != nullptr)
                clip.rotations[bone] = fallback->rotations[bone];
            else if (present)
                clip.rotations[bone] = {first[bone].rotation};
        }
        if (clip.scales[bone].empty())
        {
            if (fallback != nullptr)
                clip.scales[bone] = fallback->scales[bone];
            else if (present)
                clip.scales[bone] = {first[bone].scale};
        }
    }
    return clip;
}

std::optional<rig::Clip> clip_from_model(const Model &model, const rig::RetailRig &rig,
                                         const rig::Clip *fallback, std::string *error)
{
    return clip_from_animation(model, 0, rig, fallback, error);
}

// (GunFX) The part tracks of one animation: every animated node that is not a
// bone and is named like a part, sampled at the clip's 30 Hz keys exactly as
// clip_from_animation samples the bones (stretched the same way).
static gunfx::ClipTracks part_tracks(const Model &model, int index, const rig::RetailRig &rig,
                              const rig::Clip &clip, bool stretched)
{
    gunfx::ClipTracks tracks;
    const ModelAnimation &animation = model.animations[static_cast<std::size_t>(index)];
    const double length = animation.duration;
    const double game_length = rig::seconds_from_ticks(clip.duration_ticks);
    const double pace = stretch_pace(stretched, length, game_length);
    tracks.authored_seconds = animation.duration;
    tracks.stretched = pace != 1.0;
    tracks.duration_ticks = clip.duration_ticks;
    const BoneNodes bones = match_bones(model, rig);
    std::vector<std::array<bool, 3>> animated(model.nodes.size());
    for (const AnimationChannel &channel : animation.channels)
        if (channel.node >= 0 && static_cast<std::size_t>(channel.node) < animated.size())
            animated[static_cast<std::size_t>(channel.node)]
                    [channel.path == AnimationChannel::Path::translation ? 0U
                     : channel.path == AnimationChannel::Path::rotation  ? 1U
                                                                          : 2U] = true;
    std::vector<int> parts;
    for (std::size_t node = 0U; node < model.nodes.size(); ++node)
        if ((animated[node][0] || animated[node][1] || animated[node][2]) &&
            std::find(bones.begin(), bones.end(), static_cast<int>(node)) == bones.end() &&
            gunfx::valid_part_name(model.nodes[node].name))
            parts.push_back(static_cast<int>(node));
    if (parts.empty())
        return tracks;
    const double span = pace != 1.0 ? game_length : length;
    const int frames = std::min(static_cast<int>(std::floor(span * clip_frame_rate + 1.0e-3)) + 1, 3600);
    for (const int node : parts)
        tracks.parts.push_back({model.nodes[static_cast<std::size_t>(node)].name, {}, {}, {}});
    std::vector<ModelNode> local;
    for (int key = 0; key < frames; ++key)
    {
        const double seconds = std::min(length, static_cast<double>(key) / clip_frame_rate * pace);
        sample_animation(model, index, static_cast<float>(seconds), local);
        for (std::size_t part = 0U; part < parts.size(); ++part)
        {
            const auto node = static_cast<std::size_t>(parts[part]);
            if (node >= local.size())
                continue;
            gunfx::PartTrack &track = tracks.parts[part];
            if (animated[node][0])
                track.translation.push_back(local[node].translation);
            if (animated[node][1])
                track.rotation.push_back(local[node].rotation);
            if (animated[node][2])
                track.scale.push_back(local[node].scale);
        }
    }
    return tracks;
}

std::vector<std::string> apply_animation_pack(rig::RetailRig &rig, const AssetStack &stack,
                                              std::vector<std::string> *warnings, gunfx::TrackTable *tracks)
{
    std::vector<std::string> replaced;
    if (stack.empty())
        return replaced;
    const auto warn = [&](std::string message) {
        if (warnings != nullptr)
            warnings->push_back(std::move(message));
    };
    // (GunFX) A variant of a registered clip ("PistolReloadEmpty",
    // "PumpShotgunReload@rpg") is a name a file may use too.
    const auto variant = [&](std::string_view name) {
        const gunfx::ClipName split = gunfx::split_clip_name(name);
        if (split.base == name || rig.find(split.base) == nullptr)
            return false;
        return split.model.empty() || gunfx::valid_variant_model(split.model);
    };
    const auto registered = [&](std::string_view name) { return rig.find(name) != nullptr || variant(name); };
    static constexpr std::array<std::string_view, 2> extensions{".glb", ".gltf"};
    struct Pending final
    {
        std::shared_ptr<Model> model;
        std::string file;
        PackTarget target;
    };
    std::vector<Pending> pending;
    for (const AssetStack::Resolved &file : stack.list("animations", extensions))
    {
        const std::string stem = std::filesystem::path{file.name}.stem().string();
        std::string error;
        const auto model = load_model(file.path, &error, animation_file_limits());
        if (model == nullptr)
        {
            warn(file.mod_id + "/" + file.name + ": " + error);
            continue;
        }
        for (const ModelAnimation &animation : model->animations)
            if (animation.name.starts_with(reference_clip_prefix))
                warn(file.mod_id + "/" + file.name + ": \"" + animation.name +
                     "\" is one of the game's own clips, exported for reference: not used (and "
                     "not to be shipped)");
        std::vector<std::string> notes;
        const std::vector<PackTarget> targets = pack_targets(*model, stem, registered, &notes);
        for (std::string &note : notes)
            warn(file.mod_id + "/" + file.name + ": " + note);
        for (const PackTarget &target : targets)
            pending.push_back({model, file.mod_id + "/" + file.name, target});
    }
    // Plain clips first, so a variant falls back on the pack's own base clip.
    std::stable_partition(pending.begin(), pending.end(),
                          [&](const Pending &entry) { return rig.find(entry.target.registration) != nullptr; });
    for (const Pending &entry : pending)
    {
        const PackTarget &target = entry.target;
        auto registration = std::find_if(rig.registrations.begin(), rig.registrations.end(),
                                         [&](const rig::Registration &value) { return value.name == target.registration; });
        if (registration == rig.registrations.end())
        {
            // (GunFX) A variant: a registration of its own, as its base is.
            const std::string_view base_name = gunfx::split_clip_name(target.registration).base;
            const auto base = std::find_if(rig.registrations.begin(), rig.registrations.end(),
                                           [&](const rig::Registration &value) { return value.name == base_name; });
            if (base == rig.registrations.end())
                continue;
            rig::Registration added = *base;
            added.name = target.registration;
            rig.registrations.push_back(std::move(added));
            registration = std::prev(rig.registrations.end());
        }
        const rig::Clip *previous =
            registration->clip >= 0 && static_cast<std::size_t>(registration->clip) < rig.clips.size()
                ? &rig.clips[static_cast<std::size_t>(registration->clip)]
                : nullptr;
        const bool stretch = gunfx::reload_clip(target.registration);
        std::string error;
        auto clip = clip_from_animation(*entry.model, target.animation, rig, previous, &error, stretch);
        if (!clip)
        {
            // (GunFX) A file that moves only weapon parts keeps the clip's
            // own bones (a variant: its base's).
            if (tracks != nullptr && previous != nullptr)
            {
                gunfx::ClipTracks found = part_tracks(*entry.model, target.animation, rig, *previous, stretch);
                if (!found.parts.empty())
                {
                    found.registration = target.registration;
                    (*tracks)[target.registration] = std::move(found);
                    replaced.push_back(target.registration);
                    continue;
                }
            }
            warn(entry.file + " (" + target.registration + "): " + error);
            continue;
        }
        if (tracks != nullptr)
        {
            gunfx::ClipTracks found =
                part_tracks(*entry.model, target.animation, rig, *clip, stretch && previous != nullptr);
            found.registration = target.registration;
            (*tracks)[target.registration] = std::move(found);
        }
        // Its own clip, so another registration sharing the old one is untouched.
        rig.clips.push_back(std::move(*clip));
        registration->clip = static_cast<int>(rig.clips.size()) - 1;
        replaced.push_back(target.registration);
    }
    return replaced;
}

void apply_installed_animation_pack(rig::RetailRig &rig)
{
    gunfx::install_tracks(nullptr); // (GunFX)
    const auto assets = presentation_assets();
    if (assets == nullptr || assets->stack.empty())
        return;
    std::vector<std::string> warnings;
    auto tracks = std::make_shared<gunfx::TrackTable>(); // (GunFX)
    const std::vector<std::string> replaced = apply_animation_pack(rig, assets->stack, &warnings, tracks.get());
    if (!tracks->empty())
        gunfx::install_tracks(std::move(tracks));
    for (const std::string &warning : warnings)
        std::cerr << "Animation pack: " << warning << '\n';
    if (!replaced.empty())
        std::cerr << "Animation pack: replaced " << replaced.size() << " retail clip(s)\n";
}

} // namespace openminerz::modding::graphics
