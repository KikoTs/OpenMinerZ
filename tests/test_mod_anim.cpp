// Animation pack authoring (openminerz/modding/animation_authoring.hpp):
// the rig export, the pack loader reading it back, and check-anim.
//
//   openminerz_mod_anim_tests            a synthetic character, no game needed
//   openminerz_mod_anim_tests classic    the character imported from the
//   openminerz_mod_anim_tests modern     user's own game (exit 77 without it)
//
// THE ROUND TRIP. A clip exported by export-rig and loaded back as a pack must
// pose the character exactly as the game's own clip does: every attachment
// bone (eye camera, held item), every skinning matrix of the body, at several
// moments of several clips, including the last interval, which the engine
// never interpolates. And the exported file, played by any glTF viewer, must
// show the game's own pose (turned into the authoring frame): that is what
// Blender displays, so what a modder sees is what the game draws.

#include "openminerz/asset_paths.hpp"
#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/retail_rig.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
namespace graphics = openminerz::modding::graphics;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// CTest runs this executable three times at once (no argument, classic and
// modern): each run's folder is its own, or one run's clean-up deletes the
// pack another is reading.
class TemporaryFolder final
{
  public:
    explicit TemporaryFolder(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                std::format("openminerz-mod-anim-{}-{}", name,
                            std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryFolder(const TemporaryFolder &) = delete;
    TemporaryFolder &operator=(const TemporaryFolder &) = delete;
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(file), "cannot write " + path.string());
}

std::shared_ptr<graphics::Model> parse(const std::vector<std::uint8_t> &bytes)
{
    std::string error;
    auto model = graphics::parse_model(bytes, {}, &error, graphics::animation_file_limits());
    check(model != nullptr, "the export must load as a glTF: " + error);
    return model;
}

std::vector<std::uint8_t> export_clips(const rig::RetailRig &rig, std::vector<std::string> clips,
                                       std::string prefix = {})
{
    graphics::RigExportOptions options;
    options.reference_clips = std::move(clips);
    options.reference_prefix = std::move(prefix);
    std::string error;
    std::vector<std::uint8_t> bytes = graphics::export_rig(rig, options, &error);
    check(!bytes.empty(), "export failed: " + error);
    return bytes;
}

// The pack loader, through a mod's assets/animations/ folder, onto a copy.
rig::RetailRig loaded_back(const rig::RetailRig &rig, const std::vector<std::uint8_t> &file,
                           std::string_view file_name, std::vector<std::string> &replaced,
                           std::vector<std::string> &warnings)
{
    TemporaryFolder folder{"pack"};
    write_bytes(folder.path() / "assets" / "animations" / file_name, file);
    graphics::AssetStack stack;
    stack.set_sources({{"anim", folder.path() / "assets"}});
    rig::RetailRig copy = rig;
    replaced = graphics::apply_animation_pack(copy, stack, &warnings);
    return copy;
}

struct PoseDifference final
{
    float attachment{};
    float skin{};
    float eye{};
};

// One clip alone on channel 0 at `position`, posed by each rig.
PoseDifference pose_difference(const rig::RetailRig &a, const rig::RetailRig &b, std::string_view clip,
                               std::int64_t position)
{
    rig::CharacterPose first{a};
    rig::CharacterPose second{b};
    rig::ChannelStates states_a{};
    rig::ChannelStates states_b{};
    states_a[0].current = a.find(clip);
    states_b[0].current = b.find(clip);
    check(states_a[0].current != nullptr && states_b[0].current != nullptr, std::string{clip} + " registered");
    states_a[0].current_position = states_b[0].current_position = position;
    first.apply(states_a);
    second.apply(states_b);
    PoseDifference result;
    const auto worst = [](const rig::Matrix &x, const rig::Matrix &y) {
        float largest{};
        for (std::size_t index = 0U; index < x.size(); ++index)
            largest = std::max(largest, std::abs(x[index] - y[index]));
        return largest;
    };
    for (std::size_t bone = 0U; bone < first.attachment().size(); ++bone)
        result.attachment = std::max(result.attachment, worst(first.attachment()[bone], second.attachment()[bone]));
    if (a.parts.empty())
        for (std::size_t bone = 0U; bone < first.skin().size(); ++bone)
            result.skin = std::max(result.skin, worst(first.skin()[bone], second.skin()[bone]));
    else
        // The body as drawn: its vertices skinned by each palette, in metres.
        // (The mesh is modelled small and its inverse binds scale it up, so
        // the palette's own elements are a hundred times larger.)
        for (const rig::MeshPart &part : a.parts)
            for (std::size_t index = 0U; index < part.vertices.size(); index += 7U)
            {
                const rig::MeshVertex &vertex = part.vertices[index];
                std::array<float, 3> p{};
                std::array<float, 3> q{};
                for (std::size_t influence = 0U; influence < 4U; ++influence)
                {
                    const float weight = vertex.weights[influence];
                    if (weight == 0.0F)
                        continue;
                    const auto x = rig::transform_point(vertex.position.data(), first.skin()[vertex.bones[influence]]);
                    const auto y = rig::transform_point(vertex.position.data(), second.skin()[vertex.bones[influence]]);
                    for (std::size_t axis = 0U; axis < 3U; ++axis)
                    {
                        p[axis] += x[axis] * weight;
                        q[axis] += y[axis] * weight;
                    }
                }
                for (std::size_t axis = 0U; axis < 3U; ++axis)
                    result.skin = std::max(result.skin, std::abs(p[axis] - q[axis]));
            }
    result.eye = worst(first.eye_now(), second.eye_now());
    return result;
}

std::vector<std::int64_t> sample_positions(const rig::RetailRig &rig, std::string_view clip)
{
    const rig::Registration *registration = rig.find(clip);
    const std::int64_t duration = rig.clips[static_cast<std::size_t>(registration->clip)].duration_ticks;
    // The start, a millisecond in, inside, the last interval, the end.
    return {0, 10'000, duration * 37 / 100, duration * 81 / 100, std::max<std::int64_t>(0, duration - 10'000),
            duration};
}

// Exported clips loaded back pose every bone as the game's own clips do.
float round_trip(const rig::RetailRig &rig, const std::vector<std::string> &clips, std::string_view label)
{
    const std::vector<std::uint8_t> file = export_clips(rig, clips);
    std::vector<std::string> replaced;
    std::vector<std::string> warnings;
    const rig::RetailRig back = loaded_back(rig, file, "pack.glb", replaced, warnings);
    for (const std::string &warning : warnings)
        std::cout << "  warning: " << warning << '\n';
    check(warnings.empty(), "loading an export back raises no warning");
    check(replaced.size() == clips.size(), std::format("every exported clip is replaced ({} of {})", replaced.size(),
                                                       clips.size()));
    float worst{};
    for (const std::string &clip : clips)
    {
        const rig::Registration *before = rig.find(clip);
        const rig::Registration *after = back.find(clip);
        check(after->clip != before->clip, clip + " now plays the pack's clip");
        check(back.clips[static_cast<std::size_t>(after->clip)].duration_ticks ==
                  rig.clips[static_cast<std::size_t>(before->clip)].duration_ticks,
              clip + " keeps the game's length");
        for (const std::int64_t position : sample_positions(rig, clip))
        {
            const PoseDifference difference = pose_difference(rig, back, clip, position);
            const float largest = std::max({difference.attachment, difference.skin, difference.eye});
            worst = std::max(worst, largest);
            check(largest < 2.0e-5F,
                  std::format("{} {} at {:.4f} s: bones {:.2e}, body {:.2e}, eye {:.2e} away from the game's pose",
                              label, clip, rig::seconds_from_ticks(position), difference.attachment, difference.skin,
                              difference.eye));
        }
    }
    return worst;
}

// The file's own pose (a glTF viewer's, Blender's) is the game's, a half turn
// about Y. Only at whole keys: between keys glTF interpolates linearly where
// the engine holds its last interval.
float viewer_matches_game(const rig::RetailRig &rig, std::string_view clip)
{
    const std::vector<std::uint8_t> file = export_clips(rig, {std::string{clip}});
    const auto model = parse(file);
    const graphics::BoneNodes nodes = graphics::match_bones(*model, rig);
    const rig::Registration *registration = rig.find(clip);
    const rig::Clip &source = rig.clips[static_cast<std::size_t>(registration->clip)];
    const graphics::Matrix4 turn = graphics::authoring_from_avatar();
    float worst{};
    const auto frames = static_cast<int>(rig::seconds_from_ticks(source.duration_ticks) * 30.0);
    for (int frame = 0; frame <= frames; frame += 3) // whole milliseconds: 100 ms steps
    {
        std::vector<graphics::Matrix4> world;
        graphics::evaluate_pose(*model, 0, static_cast<float>(frame / 30.0), false, world);
        rig::CharacterPose pose{rig};
        rig::ChannelStates states{};
        states[0].current = registration;
        states[0].current_position = rig::ticks_from_seconds(frame / 30.0);
        pose.apply(states);
        for (std::size_t bone = 0U; bone < nodes.size(); ++bone)
        {
            if (!registration->influence[bone])
                continue;
            // XNA's row-vector matrix holds the same sixteen floats as glTF's
            // column-major column-vector one.
            const graphics::Matrix4 game = graphics::matrix_multiply(turn, pose.attachment()[bone]);
            const graphics::Matrix4 &shown = world[static_cast<std::size_t>(nodes[bone])];
            for (std::size_t index = 0U; index < 16U; ++index)
                worst = std::max(worst, std::abs(game[index] - shown[index]));
        }
    }
    check(worst < 1.0e-4F, std::format("{}: the exported file draws the pose {:.2e} away from the game's", clip, worst));
    return worst;
}

// --- A synthetic character ----------------------------------------------------

std::uint32_t seed = 12345U;
float random_unit()
{
    seed = seed * 1664525U + 1013904223U;
    return static_cast<float>(seed >> 8U) / 16777216.0F;
}

rig::Quat random_rotation(float largest)
{
    const graphics::Vec3 axis =
        vfx::normalized({random_unit() - 0.5F, random_unit() - 0.5F, random_unit() - 0.5F});
    const float angle = (random_unit() - 0.5F) * 2.0F * largest;
    const float s = std::sin(angle * 0.5F);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5F)};
}

rig::Clip random_clip(std::string asset, double seconds, int keys)
{
    rig::Clip clip;
    clip.asset = std::move(asset);
    clip.duration_ticks = rig::ticks_from_seconds(seconds);
    for (std::size_t bone = 0U; bone < static_cast<std::size_t>(rig::attachment_bone_count); ++bone)
    {
        // Some bones constant, some with shorter key arrays than the rest.
        const int count = bone % 7U == 3U ? 1 : bone % 5U == 2U ? std::max(2, keys - 6) : keys;
        for (int key = 0; key < count; ++key)
            clip.rotations[bone].push_back(random_rotation(0.9F));
        if (bone == 0U)
            for (int key = 0; key < keys; ++key)
                clip.positions[0].push_back({(random_unit() - 0.5F) * 0.1F, (random_unit() - 0.5F) * 0.1F,
                                             (random_unit() - 0.5F) * 0.1F});
        else
            clip.positions[bone] = {{}};
        clip.scales[bone] = {{1.0F, 1.0F, 1.0F}};
    }
    return clip;
}

rig::RetailRig synthetic_rig()
{
    rig::RetailRig rig;
    rig.generation = "synthetic";
    rig.game_version = "0";
    const auto names = graphics::avatar_bone_names();
    std::vector<rig::Matrix> world(names.size());
    for (std::size_t bone = 0U; bone < names.size(); ++bone)
    {
        rig::AttachmentBone &out = rig.attachment[bone];
        out.name = std::string{names[bone]};
        out.native_name = std::format("B{}__Skeleton", bone);
        out.parent = bone == 0U ? -1 : static_cast<int>((bone - 1U) / 2U);
        out.bind = rig::identity();
        if (bone == 0U)
        {
            // A half turn about Y lifted to the hips, as the game's.
            out.bind[0] = out.bind[10] = -1.0F;
            out.bind[13] = 0.8F;
            out.bind[14] = -0.01F;
            out.initial.rotation = {0.0F, 1.0F, 0.0F, 0.0F};
        }
        else
        {
            out.bind[12] = 0.03F * static_cast<float>(static_cast<int>(bone % 5U) - 2);
            out.bind[13] = 0.05F + 0.01F * static_cast<float>(bone % 3U);
            out.bind[14] = 0.02F * (static_cast<float>(bone % 4U) - 1.5F);
        }
        out.initial.translation = {out.bind[12], out.bind[13], out.bind[14]};
        rig::BodyBone body;
        body.name = out.native_name;
        body.parent = out.parent;
        body.source = static_cast<int>(bone);
        body.bind_local = rig::identity();
        body.bind_local[12] = out.bind[12];
        body.bind_local[13] = out.bind[13];
        body.bind_local[14] = out.bind[14];
        world[bone] = out.parent < 0 ? body.bind_local
                                     : rig::multiply(body.bind_local, world[static_cast<std::size_t>(out.parent)]);
        body.inverse_bind = rig::invert(world[bone]);
        rig.body.push_back(body);
        rig.body_order.push_back(static_cast<int>(bone));
    }
    rig.root_adjust = rig.attachment[0].bind;
    rig.eye_to_head = rig::identity();
    rig.clips.push_back(random_clip("Stand0", 160.0 / 30.0, 162)); // one key past the end, as Stand0
    rig.clips.push_back(random_clip("Walk", 32.0 / 30.0, 33));
    rig.clips.push_back(random_clip("Use", 0.317, 10));
    rig.clips.push_back(random_clip("Faint", 1.3166666, 40));
    rig::BoneMask all{};
    all.fill(true);
    rig::BoneMask arm{};
    for (std::size_t bone = 5U; bone < arm.size(); ++bone)
        arm[bone] = bone == 5U || (rig.attachment[bone].parent >= 0 && arm[static_cast<std::size_t>(rig.attachment[bone].parent)]);
    rig.registrations.push_back({"Stand", 0, true, all});
    rig.registrations.push_back({"Walk", 1, true, all});
    rig.registrations.push_back({"PickUse", 2, true, arm});
    rig.registrations.push_back({"Die", 3, false, all});
    rig.registrations.push_back({"FistUse", 2, true, arm});
    return rig;
}

void synthetic_round_trip()
{
    const rig::RetailRig rig = synthetic_rig();
    const float worst = round_trip(rig, {"Stand", "Walk", "PickUse", "Die"}, "synthetic");
    std::cout << std::format("  synthetic round trip: worst difference {:.2e}\n", worst);
    for (const std::string_view clip : {"Stand", "Walk", "Die"})
        viewer_matches_game(rig, clip);

    // The registration sharing PickUse's clip keeps the game's.
    std::vector<std::string> replaced;
    std::vector<std::string> warnings;
    const rig::RetailRig back = loaded_back(rig, export_clips(rig, {"PickUse"}), "pack.glb", replaced, warnings);
    check(back.find("FistUse")->clip == rig.find("FistUse")->clip, "FistUse, which shared PickUse's clip, is untouched");
}

void the_export_is_a_rig_for_blender()
{
    const rig::RetailRig rig = synthetic_rig();
    const auto model = parse(export_clips(rig, {"Stand"}, std::string{graphics::reference_clip_prefix}));
    check(model->nodes.size() == 72U, "71 bones and the proxy body");
    check(model->skins.size() == 1U && model->skins[0].joints.size() == 71U, "one skin of 71 joints");
    check(model->primitives.size() == 3U && model->triangle_count > 500U, "a box body in three colours");
    check(model->animations.size() == 1U && model->animations[0].name == "REF_Stand", "reference clips are marked");
    check(model->nodes[0].name == "Root" && model->nodes[0].parent < 0, "Root is the scene's root bone");
    // The Root rests at the hips, unrotated: the authoring frame.
    check(std::abs(model->nodes[0].rotation[3]) > 0.99999F, "the Root rests unrotated");
    check(std::abs(model->nodes[0].translation.y - 0.8F) < 1.0e-6F &&
              std::abs(model->nodes[0].translation.z - 0.01F) < 1.0e-6F,
          "the Root rests at the hips, turned to face +Z");
    // Every joint's inverse bind undoes its rest pose.
    std::vector<graphics::Matrix4> world;
    graphics::evaluate_pose(*model, -1, 0.0F, false, world);
    std::vector<graphics::Matrix4> skin;
    graphics::skin_matrices(*model, 0, world, skin);
    for (const graphics::Matrix4 &matrix : skin)
        for (std::size_t index = 0U; index < 16U; ++index)
            check(std::abs(matrix[index] - graphics::matrix_identity()[index]) < 1.0e-5F,
                  "the proxy body is bound at the rest pose");
    // An export without a mesh or clips still loads as a pack file.
    graphics::RigExportOptions bare;
    bare.proxy_mesh = false;
    const std::vector<std::uint8_t> armature = graphics::export_rig(rig, bare);
    std::string error;
    check(graphics::parse_model(armature, {}, &error, graphics::animation_file_limits()) != nullptr,
          "an armature on its own loads as an animation file: " + error);
    check(graphics::parse_model(armature, {}, &error) == nullptr, "but not as a model, which needs a mesh");
    check(graphics::export_rig(rig, {{"Dance"}}, &error).empty() && error.find("Dance") != std::string::npos,
          "an unregistered clip is refused by name");
}

void reference_clips_are_never_used()
{
    const rig::RetailRig rig = synthetic_rig();
    std::vector<std::string> replaced;
    std::vector<std::string> warnings;
    const rig::RetailRig back =
        loaded_back(rig, export_clips(rig, {"Stand", "Walk"}, std::string{graphics::reference_clip_prefix}),
                    "pack.glb", replaced, warnings);
    check(replaced.empty(), "REF_ animations replace nothing");
    check(std::count_if(warnings.begin(), warnings.end(),
                        [](const std::string &w) { return w.find("game's own") != std::string::npos; }) == 2,
          "and each is reported");
    // A file named after a clip whose only animation is a reference.
    loaded_back(rig, export_clips(rig, {"Walk"}, std::string{graphics::reference_clip_prefix}), "Walk.glb", replaced,
                warnings);
    check(replaced.empty(), "Walk.glb holding only REF_Walk replaces nothing");
}

// Hand edits on the exported model, the way Blender files differ from ours.
void channels_fall_back_and_clocks_stay_the_games()
{
    const rig::RetailRig rig = synthetic_rig();
    const auto model = parse(export_clips(rig, {"Stand"}));
    const rig::Clip &retail = rig.clips[static_cast<std::size_t>(rig.find("Stand")->clip)];

    // Only one bone's rotation: every other key array is the game's.
    graphics::Model one = *model;
    const int node = graphics::match_bones(one, rig)[9];
    std::erase_if(one.animations[0].channels, [&](const graphics::AnimationChannel &channel) {
        return channel.node != node || channel.path != graphics::AnimationChannel::Path::rotation;
    });
    std::string error;
    const auto clip = graphics::clip_from_animation(one, 0, rig, &retail, &error);
    check(clip.has_value(), error);
    for (std::size_t bone = 0U; bone < static_cast<std::size_t>(rig::attachment_bone_count); ++bone)
    {
        check(clip->positions[bone].size() == retail.positions[bone].size(), "positions fall back");
        if (bone != 9U)
            check(clip->rotations[bone].size() == retail.rotations[bone].size() &&
                      std::equal(clip->rotations[bone].begin(), clip->rotations[bone].end(),
                                 retail.rotations[bone].begin(),
                                 [](const rig::Quat &a, const rig::Quat &b) {
                                     return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
                                 }),
                  std::format("bone {} keeps the game's rotation keys", bone));
    }

    // Twice as long: played at its own speed on the game's clock (cut off
    // where the game ends the clip), never stretched.
    graphics::Model slow = *model;
    for (graphics::AnimationChannel &channel : slow.animations[0].channels)
        for (float &time : channel.times)
            time *= 2.0F;
    slow.animations[0].duration *= 2.0F;
    const auto slowed = graphics::clip_from_animation(slow, 0, rig, &retail, &error);
    check(slowed.has_value() && slowed->duration_ticks == retail.duration_ticks,
          "a longer clip still lasts as long as the game's");
    float worst{};
    for (std::size_t key = 0U; key < 80U; ++key)
        for (const std::size_t bone : {1U, 9U, 40U})
        {
            const rig::Quat &a = slowed->rotations[bone][key * 2U];
            const rig::Quat &b = retail.rotations[bone][key];
            worst = std::max(worst, 1.0F - std::min(1.0F, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w)));
        }
    check(worst < 1.0e-5F, "its keys play at their own speed");

    // An "Armature" node above the Root is seen through: unturned, nothing
    // changes; turned, the game turns the body the same way (what Blender shows).
    for (const float degrees : {0.0F, 90.0F})
    {
        graphics::Model wrapped = *model;
        graphics::ModelNode armature;
        armature.name = "Armature";
        const float half = degrees * 3.14159265F / 360.0F;
        armature.rotation = {0.0F, std::sin(half), 0.0F, std::cos(half)};
        wrapped.nodes.push_back(armature);
        wrapped.nodes[0].parent = static_cast<int>(wrapped.nodes.size()) - 1;
        wrapped.node_order.insert(wrapped.node_order.begin(), static_cast<int>(wrapped.nodes.size()) - 1);
        const auto again = graphics::clip_from_animation(wrapped, 0, rig, &retail, &error);
        check(again.has_value(), error);
        const auto plain = graphics::clip_from_animation(*model, 0, rig, &retail, &error);
        const rig::Quat &a = again->rotations[0][5];
        const rig::Quat &b = plain->rotations[0][5];
        const float alignment = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        const float turned = 2.0F * std::acos(std::min(1.0F, alignment)) * 180.0F / 3.14159265F;
        check(std::abs(turned - degrees) < 0.01F,
              std::format("an Armature turned {} degrees turns the Root {:.3f} degrees", degrees, turned));
    }
}

void files_pick_their_clips()
{
    graphics::Model model;
    for (const std::string_view name : {"Action", "Walk", "REF_Run"})
        model.animations.push_back({std::string{name}, 1.0F, {}});
    const auto registered = [](std::string_view name) { return name == "Walk" || name == "Run" || name == "Stand"; };
    std::vector<std::string> warnings;
    auto targets = graphics::pack_targets(model, "Stand", registered, &warnings);
    check(targets.size() == 2U && targets[0].registration == "Walk" && targets[1].registration == "Stand" &&
              targets[1].animation == 0,
          "named animations replace their clips; the only other one takes the file's name");
    targets = graphics::pack_targets(model, "anything", registered, &warnings);
    check(targets.size() == 1U && targets[0].registration == "Walk", "a file of any name replaces by animation name");
    model.animations.push_back({"Action.001", 1.0F, {}});
    warnings.clear();
    targets = graphics::pack_targets(model, "Stand", registered, &warnings);
    check(targets.size() == 1U && warnings.size() == 1U && warnings[0].find("none is named") != std::string::npos,
          "two unnamed animations for one file name are refused");
}

bool reports(const graphics::AnimationCheck &check, graphics::AnimationCheck::Level level, std::string_view text)
{
    return std::any_of(check.messages.begin(), check.messages.end(), [&](const auto &message) {
        return message.level == level && message.text.find(text) != std::string::npos;
    });
}

void check_anim_explains_problems()
{
    using Level = graphics::AnimationCheck::Level;
    const rig::RetailRig rig = synthetic_rig();
    const std::vector<graphics::AnimationCheckRig> rigs{{"Synthetic", &rig}};
    const auto exported = parse(export_clips(rig, {"PickUse"}));

    // The game's own clip under its own name.
    auto result = graphics::check_animation(*exported, "pack", rigs);
    check(reports(result, Level::error, "the game's own \"Use\" animation"), "a copy of the game's clip is refused");
    check(result.clips.size() == 1U && result.clips[0].registration == "PickUse" && !result.clips[0].rotated.empty(),
          "the clip it replaces and the bones it rotates are listed");
    const auto marked = parse(export_clips(rig, {"PickUse"}, "REF_"));
    check(reports(graphics::check_animation(*marked, "pack", rigs), Level::error, "one of the game's own animations"),
          "a REF_ clip is refused");

    // An original clip: nudge every key, then break it in various ways.
    graphics::Model own = *exported;
    for (graphics::AnimationChannel &channel : own.animations[0].channels)
        if (channel.path == graphics::AnimationChannel::Path::rotation)
            for (std::size_t at = 0U; at < channel.values.size(); at += 4U)
                channel.values[at] += 0.05F;
    result = graphics::check_animation(own, "pack", rigs);
    check(result.count(Level::error) == 0U, "an original clip passes: " +
                                                (result.messages.empty() ? std::string{} : result.messages[0].text));

    graphics::Model renamed = own;
    const int elbow = graphics::match_bones(renamed, rig)[25]; // ElbowLeft, inside PickUse here
    renamed.nodes[static_cast<std::size_t>(elbow)].name = "elbow_left";
    check(reports(graphics::check_animation(renamed, "pack", rigs), Level::error, "did you mean ElbowLeft"),
          "a misspelt bone gets a suggestion");
    renamed.nodes[static_cast<std::size_t>(elbow)].name = "mixamorig:LeftForeArm";
    check(reports(graphics::check_animation(renamed, "pack", rigs), Level::error, "another rig's name for ElbowLeft"),
          "another rig's bone name is explained");

    graphics::Model rootless = own;
    rootless.nodes[0].name = "Hips";
    check(reports(graphics::check_animation(rootless, "pack", rigs), Level::warning, "no node is named Root"),
          "a missing Root is reported");

    graphics::Model huge = own;
    for (graphics::ModelNode &node : huge.nodes)
        node.translation = node.translation * 100.0F;
    check(reports(graphics::check_animation(huge, "pack", rigs), Level::error, "bad scale"),
          "bones a hundred times too long are reported");
    graphics::Model scaled = own;
    scaled.nodes[0].scale = {0.01F, 0.01F, 0.01F};
    check(reports(graphics::check_animation(scaled, "pack", rigs), Level::error, "bad scale"),
          "a scaled Root is reported");

    graphics::Model lying = own;
    lying.nodes[0].rotation = {-0.70710678F, 0.0F, 0.0F, 0.70710678F}; // Z-up data
    check(reports(graphics::check_animation(lying, "pack", rigs), Level::error, "not upright"),
          "a Z-up export is reported");

    graphics::Model misnamed = own;
    misnamed.animations[0].name = "PikUse";
    check(reports(graphics::check_animation(misnamed, "pack", rigs), Level::warning, "did you mean PickUse"),
          "a misspelt clip name gets a suggestion");

    // Without a skeleton: names only, against the static bone list and the
    // clip names both generations register.
    const auto loose = graphics::check_animation(own, "pack");
    check(loose.count(Level::error) == 0U, "names alone pass");
    check(!graphics::registered_clip_names().empty() &&
              std::ranges::binary_search(graphics::registered_clip_names(), std::string{"GunReload"}),
          "the clip names come from the registration table");
}

// --- The user's own game ----------------------------------------------------------

// examples/mods/animation_pack on the real character: it loads cleanly, the
// sway keeps both feet where they stand, and the wave raises the right hand
// (the character's right: -X in the authoring frame) above the shoulder.
void example_pack_on(const rig::RetailRig &rig)
{
    graphics::AssetStack stack;
    stack.set_sources({{"animation_pack", std::filesystem::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods" /
                                              "animation_pack" / "assets"}});
    rig::RetailRig packed = rig;
    std::vector<std::string> warnings;
    const std::vector<std::string> replaced = graphics::apply_animation_pack(packed, stack, &warnings);
    check(replaced.size() == 2U && warnings.empty(), "the example replaces Stand and PickUse without a warning");

    const auto pose_at = [&](const rig::RetailRig &source, std::string_view clip, double seconds) {
        rig::CharacterPose pose{source};
        rig::ChannelStates states{};
        states[0].current = source.find(clip);
        states[0].current_position = rig::ticks_from_seconds(seconds);
        pose.apply(states);
        return pose.attachment();
    };
    const auto position = [](const rig::Matrix &m) { return std::array<float, 3>{m[12], m[13], m[14]}; };
    const int ankle_left = graphics::find_rig_bone(rig, "AnkleLeft");
    const int ankle_right = graphics::find_rig_bone(rig, "AnkleRight");
    const int root = 0;
    const double length = rig::seconds_from_ticks(
        packed.clips[static_cast<std::size_t>(packed.find("Stand")->clip)].duration_ticks);
    float slide{};
    float sway{};
    const auto start = pose_at(packed, "Stand", 0.0);
    for (double seconds = 0.0; seconds <= length; seconds += 0.05)
    {
        const auto now = pose_at(packed, "Stand", seconds);
        for (const int ankle : {ankle_left, ankle_right})
            for (std::size_t axis = 0U; axis < 3U; ++axis)
                slide = std::max(slide, std::abs(position(now[static_cast<std::size_t>(ankle)])[axis] -
                                                 position(start[static_cast<std::size_t>(ankle)])[axis]));
        sway = std::max(sway, std::abs(position(now[root])[0] - position(start[root])[0]));
    }
    std::cout << std::format("  example Stand: the hips sway {:.1f} cm, the ankles move {:.2f} mm\n", sway * 100.0F,
                             slide * 1000.0F);
    check(sway > 0.06F, "the hips sway from side to side");
    check(slide < 0.002F, "the feet stay where they stand");

    const int hand = graphics::find_rig_bone(rig, "WristRight");
    const int shoulder = graphics::find_rig_bone(rig, "ShoulderRight");
    const auto wave = pose_at(packed, "PickUse", 0.1);
    const auto swing = pose_at(rig, "PickUse", 0.1);
    std::cout << std::format("  example PickUse: right wrist {:.2f} m above the shoulder (the game's swing: {:.2f})\n",
                             position(wave[static_cast<std::size_t>(hand)])[1] -
                                 position(wave[static_cast<std::size_t>(shoulder)])[1],
                             position(swing[static_cast<std::size_t>(hand)])[1] -
                                 position(swing[static_cast<std::size_t>(shoulder)])[1]);
    check(position(wave[static_cast<std::size_t>(hand)])[1] > position(wave[static_cast<std::size_t>(shoulder)])[1] + 0.1F,
          "the wave raises the right hand above the shoulder");
}

int content_tests(std::string_view style)
{
    set_active_asset_style(style == "classic" ? AssetStyle::classic : AssetStyle::modern);
    const std::filesystem::path path = content_pack_path(active_asset_style()) / "DerivedData" / "character.omrc";
    if (!std::filesystem::exists(path))
    {
        std::cout << "SKIP: the player character has not been imported (" << path.string() << ")\n";
        return 77;
    }
    const rig::RetailRig rig{path};
    std::cout << "character: " << rig.generation << ' ' << rig.game_version << ", " << rig.registrations.size()
              << " clips\n";

    // The bone names a pack uses are the game's.
    const auto names = graphics::avatar_bone_names();
    for (std::size_t bone = 0U; bone < names.size(); ++bone)
        check(rig.attachment[bone].name == names[bone],
              std::format("bone {} is {}, not {}", bone, rig.attachment[bone].name, names[bone]));

    // Axes, units and handedness of the authoring frame.
    const std::vector<graphics::AuthoringBone> skeleton = graphics::authoring_skeleton(rig);
    const auto at = [&](std::string_view name) {
        const int bone = graphics::find_rig_bone(rig, name);
        const graphics::Matrix4 &m = skeleton[static_cast<std::size_t>(bone)].world;
        return graphics::Vec3{m[12], m[13], m[14]};
    };
    check(std::abs(skeleton[0].rotation[3]) > 0.9999F, "the Root rests unrotated");
    check(at("HipLeft").x > 0.05F && at("HipRight").x < -0.05F && at("ShoulderLeft").x > 0.05F &&
              at("WristLeft").x > at("ElbowLeft").x,
          "the left side is on +X");
    check(at("ToeLeft").z > at("AnkleLeft").z + 0.05F && at("ToeRight").z > at("AnkleRight").z + 0.05F,
          "the character faces +Z");
    check(at("Head").y > 1.05F && at("Head").y < 1.4F && at("Root").y > 0.6F && at("Root").y < 0.9F &&
              std::abs(at("ToeLeft").y) < 0.1F,
          "+Y is up and one unit is one block (a 1.4 m figure standing on y = 0)");

    std::vector<std::string> clips;
    for (const std::string_view name : {"Stand", "Walk", "Run", "Die", "PickUse", "FistUse", "GenericUse", "BlockUse",
                                        "GunReload", "GunShoulder", "IdleHead", "Tilt", "Wave", "Swim"})
        if (rig.find(name) != nullptr)
            clips.emplace_back(name);
    float shown{};
    for (const std::string_view clip : {"Stand", "Walk", "Die"})
        shown = std::max(shown, viewer_matches_game(rig, clip));
    const float worst = round_trip(rig, clips, rig.generation);
    std::cout << std::format("  {} clips exported and loaded back: {:.2e} from the game's pose at worst (bone "
                             "matrices; the skinned body in metres); the file itself draws the game's pose to {:.2e}\n",
                             clips.size(), worst, shown);

    example_pack_on(rig);

    // check-anim on the export: the game's own clips, renamed or not.
    const std::vector<graphics::AnimationCheckRig> rigs{{rig.generation, &rig}};
    const auto plain = parse(export_clips(rig, {"Walk", "PickUse"}));
    const auto result = graphics::check_animation(*plain, "pack", rigs);
    check(result.count(graphics::AnimationCheck::Level::error) == 2U,
          "both exported clips are recognised as the game's own");
    return 0;
}

// `openminerz_mod_anim_tests compare <classic|modern> <file.glb>`: loads a
// pack file (one that went through Blender, say) onto the imported character
// and reports how far each clip it replaces poses from the game's own clip.
int compare_file(std::string_view style, const std::filesystem::path &file)
{
    set_active_asset_style(style == "classic" ? AssetStyle::classic : AssetStyle::modern);
    const std::filesystem::path path = content_pack_path(active_asset_style()) / "DerivedData" / "character.omrc";
    if (!std::filesystem::exists(path))
    {
        std::cout << "SKIP: the player character has not been imported (" << path.string() << ")\n";
        return 77;
    }
    const rig::RetailRig rig{path};
    std::ifstream input{file, std::ios::binary};
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::string> replaced;
    std::vector<std::string> warnings;
    const rig::RetailRig back = loaded_back(rig, bytes, file.filename().string(), replaced, warnings);
    for (const std::string &warning : warnings)
        std::cout << "warning: " << warning << '\n';
    float worst{};
    for (const std::string &clip : replaced)
    {
        float largest{};
        const std::int64_t duration =
            rig.clips[static_cast<std::size_t>(rig.find(clip)->clip)].duration_ticks;
        for (std::int64_t position = 0; position <= duration; position += 10'000 * 7) // every 7 ms
        {
            const PoseDifference difference = pose_difference(rig, back, clip, position);
            largest = std::max({largest, difference.attachment, difference.skin, difference.eye});
        }
        std::cout << std::format("{}: {:.2e} from the game's pose at worst\n", clip, largest);
        worst = std::max(worst, largest);
    }
    return replaced.empty() ? 1 : (worst < 1.0e-4F ? 0 : 1);
}

struct TestCase final
{
    const char *name;
    void (*run)();
};

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        try
        {
            if (argc == 4 && std::string_view{argv[1]} == "compare")
                return compare_file(argv[2], argv[3]);
            return content_tests(argv[1]);
        }
        catch (const std::exception &error)
        {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    const std::vector<TestCase> tests{
        {"synthetic_round_trip", synthetic_round_trip},
        {"the_export_is_a_rig_for_blender", the_export_is_a_rig_for_blender},
        {"reference_clips_are_never_used", reference_clips_are_never_used},
        {"channels_fall_back_and_clocks_stay_the_games", channels_fall_back_and_clocks_stay_the_games},
        {"files_pick_their_clips", files_pick_their_clips},
        {"check_anim_explains_problems", check_anim_explains_problems},
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << "/" << tests.size() << " passed\n";
    return failures == 0U ? 0 : 1;
}
