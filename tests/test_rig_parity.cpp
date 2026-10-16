// Character rig parity.
//
// With no arguments: pinned expectations for the pieces of the port that do not
// need the game (the millisecond clip clock, XNA's slerp, the last-key hold,
// ping-pong and the blender), so the suite covers them in every build.
//
//   openminerz_rig_parity_tests <character.omrc> <rig-reference.bin>
//
// replays what tools/OriginalClientInterop/RigParityProbe recorded from the
// ORIGINAL engine: every Play/Clear/Update the probe issued to a real
// AvatarAnimationCollection is issued to rig::Animator, and at every snapshot
// each body bone's world transform, the skinning palette, the attachment
// skeleton, the eye camera and the held-item parent are compared with what the
// original computed. The held-item transforms in the pack are compared with
// the original's CreateEntity(ItemUse.Hand) for every item.

#include "openminerz/retail_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace openminerz::rig;

void expect(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error{message};
}

class Input final
{
  public:
    explicit Input(const std::filesystem::path &path) : stream_(path, std::ios::binary)
    {
        if (!stream_)
            throw std::runtime_error{"could not open " + path.string()};
    }

    template <typename Value> Value read()
    {
        Value value{};
        stream_.read(reinterpret_cast<char *>(&value), sizeof(value));
        if (!stream_)
            throw std::runtime_error{"truncated rig reference"};
        return value;
    }

    std::string string()
    {
        std::uint32_t length{};
        for (int shift = 0; shift < 35; shift += 7)
        {
            const auto byte = read<std::uint8_t>();
            length |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U)
                break;
        }
        std::string result(length, '\0');
        stream_.read(result.data(), static_cast<std::streamsize>(result.size()));
        if (!stream_)
            throw std::runtime_error{"truncated rig reference string"};
        return result;
    }

    Matrix matrix()
    {
        Matrix m{};
        stream_.read(reinterpret_cast<char *>(m.data()), sizeof(float) * m.size());
        if (!stream_)
            throw std::runtime_error{"truncated rig reference matrix"};
        return m;
    }

  private:
    std::ifstream stream_;
};

struct Error final
{
    double position{};    // metres between the two bone origins
    double orientation{}; // degrees between the two bone frames (worst axis)
    double element{};     // largest absolute matrix element difference
    std::string where;

    void merge(const Error &other)
    {
        if (other.position > position || (where.empty() && !other.where.empty()))
            where = other.where;
        position = std::max(position, other.position);
        orientation = std::max(orientation, other.orientation);
        element = std::max(element, other.element);
    }
};

Error compare(const Matrix &ours, const Matrix &theirs, const std::string &where)
{
    Error error;
    error.where = where;
    const double dx = ours[12] - theirs[12], dy = ours[13] - theirs[13], dz = ours[14] - theirs[14];
    error.position = std::sqrt(dx * dx + dy * dy + dz * dz);
    for (std::size_t index = 0; index < ours.size(); ++index)
        error.element = std::max(error.element, static_cast<double>(std::abs(ours[index] - theirs[index])));
    for (std::size_t row = 0; row < 3U; ++row)
    {
        double dot{}, a{}, b{};
        for (std::size_t column = 0; column < 3U; ++column)
        {
            dot += static_cast<double>(ours[row * 4U + column]) * theirs[row * 4U + column];
            a += static_cast<double>(ours[row * 4U + column]) * ours[row * 4U + column];
            b += static_cast<double>(theirs[row * 4U + column]) * theirs[row * 4U + column];
        }
        if (a <= 0.0 || b <= 0.0)
            continue;
        const double cosine = std::clamp(dot / std::sqrt(a * b), -1.0, 1.0);
        error.orientation = std::max(error.orientation, std::acos(cosine) * 180.0 / 3.14159265358979323846);
        // A collapsed (hidden) head keeps its direction; compare its length too.
        error.element = std::max(error.element, std::abs(std::sqrt(a) - std::sqrt(b)));
    }
    return error;
}

int replay(const std::filesystem::path &rig_path, const std::filesystem::path &reference_path)
{
    const RetailRig rig{rig_path};
    Input input{reference_path};
    expect(input.read<std::uint32_t>() == 0x52524D4FU && input.read<std::int32_t>() == 1,
           "not a rig reference file");
    const std::string generation = input.string();
    const std::string version = input.string();
    const std::string profile = input.string();
    const int body_bones = input.read<std::int32_t>();
    const int scenario_count = input.read<std::int32_t>();
    if (generation != rig.generation || version != rig.game_version)
        throw std::runtime_error{"the reference (" + generation + " " + version + ") and the pack (" + rig.generation +
                                 " " + rig.game_version + ") come from different games"};
    expect(body_bones == static_cast<int>(rig.body.size()), "body bone count differs from the original");

    Error body, skin, attachment, eye, hand, held;
    std::map<std::string, Error> families;
    std::size_t snapshots{};
    std::size_t item_snapshots{};
    std::size_t clip_scenarios{};
    Error worst_clip;
    std::string worst_clip_name;
    for (int scenario = 0; scenario < scenario_count; ++scenario)
    {
        const std::string name = input.string();
        const int ops = input.read<std::int32_t>();
        CharacterPose pose{rig};
        Error scenario_error;
        for (int op = 0; op < ops; ++op)
        {
            switch (input.read<std::uint8_t>())
            {
            case 1:
            {
                const int channel = input.read<std::uint8_t>();
                const std::string clip = input.string();
                const auto blend = input.read<std::int64_t>();
                if (pose.animations.play_ticks(clip, channel, blend) == nullptr)
                    throw std::runtime_error{"the pack does not register " + clip};
                break;
            }
            case 2:
            {
                const int channel = input.read<std::uint8_t>();
                pose.animations.replay_ticks(channel, nullptr, input.read<std::int64_t>());
                break;
            }
            case 3:
            {
                const int channel = input.read<std::uint8_t>();
                const int property = input.read<std::uint8_t>();
                const double value = input.read<double>();
                ClipPlayer *player = pose.animations.active(channel);
                expect(player != nullptr, "a scenario configures an empty channel");
                switch (property)
                {
                case 1: player->speed = static_cast<float>(value); break;
                case 2: player->looping = value != 0.0; break;
                case 3: player->reversed = value != 0.0; break;
                case 4: player->ping_pong = value != 0.0; break;
                case 5: player->set_progress(static_cast<float>(value)); break;
                case 6: player->pause(); break;
                case 7: player->play(); break;
                default: throw std::runtime_error{"unknown player property in the reference"};
                }
                break;
            }
            case 4:
                pose.update(input.read<std::int64_t>());
                break;
            case 5:
            {
                ++snapshots;
                const std::string at = name + " snapshot " + std::to_string(snapshots);
                Error here;
                for (std::size_t bone = 0; bone < rig.body.size(); ++bone)
                {
                    const Error e = compare(pose.body_world()[bone], input.matrix(), at + " body " + rig.body[bone].name);
                    body.merge(e);
                    here.merge(e);
                }
                for (std::size_t bone = 0; bone < rig.body.size(); ++bone)
                    skin.merge(compare(pose.skin()[bone], input.matrix(), at + " skin " + rig.body[bone].name));
                for (std::size_t bone = 0; bone < static_cast<std::size_t>(attachment_bone_count); ++bone)
                {
                    const Error e = compare(pose.attachment()[bone], input.matrix(),
                                            at + " attachment " + rig.attachment[bone].name);
                    attachment.merge(e);
                    here.merge(e);
                }
                eye.merge(compare(pose.eye(), input.matrix(), at + " eye camera"));
                hand.merge(compare(pose.right_hand(), input.matrix(), at + " held-item parent"));
                for (int channel = 0; channel < 6; ++channel)
                {
                    const bool active = input.read<std::uint8_t>() != 0U;
                    const ClipPlayer *player = pose.animations.active(channel);
                    if ((player != nullptr) != active)
                        throw std::runtime_error{at + ": channel " + std::to_string(channel) + " occupancy differs"};
                    if (!active)
                        continue;
                    const std::string clip = input.string();
                    const auto ticks = input.read<std::int64_t>();
                    const float progress = input.read<float>();
                    const bool finished = input.read<std::uint8_t>() != 0U;
                    if (player->name() != clip || player->current_ticks() != ticks || player->finished() != finished ||
                        std::abs(player->progress() - progress) > 1.0e-6F)
                        throw std::runtime_error{at + ": channel " + std::to_string(channel) + " clock differs: ours " +
                                                 std::string{player->name()} + "@" +
                                                 std::to_string(player->current_ticks()) + ", original " + clip + "@" +
                                                 std::to_string(ticks)};
                }
                scenario_error.merge(here);
                break;
            }
            case 6:
                pose.hide_head = input.read<std::uint8_t>() != 0U;
                break;
            case 7:
            {
                const int channel = input.read<std::uint8_t>();
                pose.animations.replay_ticks(channel, pose.animations.active(channel), input.read<std::int64_t>());
                break;
            }
            case 8:
            {
                // The original's item entity, parented to its PropRight part.
                const int id = input.read<std::int32_t>();
                const bool local = input.read<std::uint8_t>() != 0U;
                const Matrix original_world = input.matrix();
                const HeldItem *item = rig.item(id);
                if (item == nullptr)
                    throw std::runtime_error{name + ": the pack has no held-item transform for item " +
                                             std::to_string(id)};
                ++item_snapshots;
                const Error e = compare(multiply(local ? item->hand : item->hand_remote, pose.right_hand()),
                                        original_world, name + " " + item->name);
                held.merge(e);
                scenario_error.merge(e);
                break;
            }
            default:
                throw std::runtime_error{"unknown op in the rig reference"};
            }
        }
        if (name.rfind("clip:", 0) == 0)
        {
            ++clip_scenarios;
            if (scenario_error.position >= worst_clip.position)
            {
                worst_clip = scenario_error;
                worst_clip_name = name.substr(5);
            }
        }
        else
            families[name] = scenario_error;
    }

    // Held items: the pack's table against the original's CreateEntity results.
    const int item_count = input.read<std::int32_t>();
    Error items;
    std::map<std::string, std::pair<int, double>> by_mode;
    for (int index = 0; index < item_count; ++index)
    {
        const int id = input.read<std::int32_t>();
        const std::string item_name = input.string();
        const std::string mode = input.string();
        const Matrix local = input.matrix();
        const Matrix remote = input.matrix();
        const HeldItem *item = rig.item(id);
        if (item == nullptr)
            throw std::runtime_error{"the pack has no held-item transform for " + item_name};
        Error e = compare(item->hand, local, "item " + item_name);
        e.merge(compare(item->hand_remote, remote, "item " + item_name + " (remote)"));
        items.merge(e);
        auto &family = by_mode[mode];
        ++family.first;
        family.second = std::max(family.second, e.element);
    }

    std::cout.precision(3);
    std::cout << std::scientific;
    std::cout << "rig parity vs original " << version << " (" << generation << ", " << profile << "): "
              << scenario_count << " scenarios, " << clip_scenarios << " clips, " << snapshots << " snapshots, "
              << rig.body.size() << " body bones\n";
    const auto print = [](const char *label, const Error &e) {
        std::cout << "  " << label << ": position " << e.position << " m, orientation " << e.orientation
                  << " deg, element " << e.element << "  [" << e.where << "]\n";
    };
    print("body bones (world)      ", body);
    print("skinning palette        ", skin);
    print("attachment skeleton     ", attachment);
    print("eye camera              ", eye);
    print("held-item parent        ", hand);
    print("held item (in the hand) ", held);
    std::cout << "  held-item attachment: " << item_snapshots
              << " samples of the original's item entity on its PropRight part (every item, local and remote,"
                 " in its own idle, second stance and use clips)\n";
    std::cout << "  worst single clip: " << worst_clip_name << " position " << worst_clip.position
              << " m, orientation " << worst_clip.orientation << " deg\n";
    for (const auto &[name, e] : families)
        std::cout << "  " << name << ": position " << e.position << " m, orientation " << e.orientation << " deg\n";
    std::cout << "  held items: " << item_count << " item transforms, worst element difference " << items.element
              << "\n";
    for (const auto &[mode, family] : by_mode)
        std::cout << "    " << (mode.empty() ? "(none)" : mode) << ": " << family.first << " items, worst "
                  << family.second << "\n";

    // Float noise from a different evaluation width is ~1e-6; anything real
    // (a wrong key, a wrong channel, a wrong axis) is centimetres or degrees.
    const double position_limit = 2.0e-5;
    const double orientation_limit = 2.0e-2;
    if (item_snapshots == 0U)
        throw std::runtime_error{"the reference holds no held-item attachment samples"};
    for (const Error *e : {&body, &attachment, &eye, &hand, &held})
        if (e->position > position_limit || e->orientation > orientation_limit)
            throw std::runtime_error{"pose differs from the original at " + e->where};
    if (skin.element > 2.0e-3)
        throw std::runtime_error{"skinning palette differs from the original at " + skin.where};
    if (items.element > 1.0e-6)
        throw std::runtime_error{"held-item transform differs from the original at " + items.where};
    std::cout << "PASS: every bone of every registered clip matches the original engine\n";
    return 0;
}

// A two-bone-of-seventy-one synthetic rig: bone 0 and bone 5 carry keys.
RetailRig synthetic()
{
    RetailRig rig;
    for (int bone = 0; bone < attachment_bone_count; ++bone)
    {
        AttachmentBone &attachment = rig.attachment[static_cast<std::size_t>(bone)];
        attachment.parent = bone == 0 ? -1 : 0;
        attachment.bind = identity();
    }
    rig.root_adjust = identity();
    rig.eye_to_head = identity();
    Clip clip;
    clip.frame_rate = 30;
    clip.duration_ticks = 1'000'000; // 0.1 s: four keys at 30 Hz
    for (std::size_t bone = 0; bone < static_cast<std::size_t>(attachment_bone_count); ++bone)
    {
        clip.positions[bone] = {{0.0F, 0.0F, 0.0F}};
        clip.rotations[bone] = {{}};
        clip.scales[bone] = {{1.0F, 1.0F, 1.0F}};
    }
    clip.positions[0] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {3.0F, 0.0F, 0.0F}};
    rig.clips.push_back(clip);
    Clip other = clip;
    other.positions[0] = {{10.0F, 0.0F, 0.0F}};
    rig.clips.push_back(other);
    Registration loop;
    loop.name = "Loop";
    loop.clip = 0;
    loop.influence.fill(true);
    rig.registrations.push_back(loop);
    Registration once = loop;
    once.name = "Once";
    once.looping = false;
    rig.registrations.push_back(once);
    Registration far = loop;
    far.name = "Far";
    far.clip = 1;
    rig.registrations.push_back(far);
    Registration upper = far;
    upper.name = "UpperOnly";
    upper.influence.fill(false);
    upper.influence[5] = true;
    rig.registrations.push_back(upper);
    return rig;
}

int pinned()
{
    // TimeSpan.FromSeconds rounds to whole milliseconds: a 60 Hz frame is 17 ms
    // on the clip clock (2% fast), and 144 Hz is 7 ms.
    expect(ticks_from_seconds(seconds_from_ticks(166667)) == 170'000, "60 Hz step must round to 17 ms");
    expect(ticks_from_seconds(seconds_from_ticks(69444)) == 70'000, "144 Hz step must round to 7 ms");
    expect(ticks_from_seconds(seconds_from_ticks(166667) * 0.6) == 100'000, "speed scales before rounding");
    expect(ticks_from_seconds(0.0004) == 0 && ticks_from_seconds(0.0005) == 10'000, "half a millisecond rounds up");
    expect(ticks_from_seconds(0.25) == 2'500'000, "blend times are exact");

    // Quaternion.Slerp takes the short way round and does not renormalise.
    const Quat a{0.0F, 0.0F, 0.0F, 1.0F};
    const Quat b{0.0F, 0.0F, -0.70710678F, -0.70710678F};
    const Quat half = slerp(a, b, 0.5F);
    expect(std::abs(half.z - 0.38268343F) < 1.0e-6F && std::abs(half.w - 0.92387953F) < 1.0e-6F,
           "slerp must follow the shortest arc");
    const Quat near = slerp(a, {0.0F, 0.0F, 0.0005F, 1.0F}, 0.5F);
    expect(std::abs(near.z - 0.00025F) < 1.0e-9F && near.w == 1.0F, "near-parallel slerp is a plain lerp");

    const RetailRig rig = synthetic();
    // The last key interval holds: with four keys, only [0,1) and [1,2) blend.
    BonePose pose{};
    const Clip &clip = rig.clips[0];
    clip.copy_transforms(pose, 166'667, nullptr); // frame 0.5
    expect(std::abs(pose[0].translation.x - 0.5F) < 1.0e-5F, "keys interpolate inside the clip");
    clip.copy_transforms(pose, 833'333, nullptr); // frame 2.5: index 2 == Length - 2
    expect(pose[0].translation.x == 2.0F, "the last key interval must hold, not interpolate");
    clip.copy_transforms(pose, 1'000'000, nullptr); // frame 3
    expect(pose[0].translation.x == 3.0F, "the clip ends on its last key");
    clip.copy_transforms(pose, 9'000'000, nullptr);
    expect(pose[0].translation.x == 3.0F, "past the end holds the last key");

    {
        // Looping wraps only when the clock passes the duration (not at it).
        CharacterPose character{rig};
        ClipPlayer *player = character.animations.play("Loop", 0, 0.0);
        expect(player != nullptr && player->looping && !player->finished(), "registered loop plays");
        for (int frame = 0; frame < 5; ++frame)
            character.update(166'667); // 5 x 17 ms = 85 ms
        expect(player->current_ticks() == 850'000, "five 60 Hz frames advance 85 ms");
        character.update(166'667); // 102 ms -> wraps to 2 ms
        expect(player->current_ticks() == 20'000, "a loop wraps by subtracting the duration");
    }
    {
        // A non-looping clip clamps and reports finished; reversed mirrors time.
        CharacterPose character{rig};
        ClipPlayer *player = character.animations.play("Once", 0, 0.0);
        player->reversed = true;
        character.update(0);
        expect(character.skeleton()[0].translation.x == 3.0F, "a reversed clip starts on its last key");
        for (int frame = 0; frame < 10; ++frame)
            character.update(166'667);
        expect(player->finished() && player->current_ticks() == 1'000'000, "a one-shot clamps at its duration");
        expect(character.skeleton()[0].translation.x == 0.0F, "a reversed clip ends on its first key");
    }
    {
        // Ping-pong reflects at both ends; set_progress leaves the pong.
        CharacterPose character{rig};
        ClipPlayer *player = character.animations.play("Loop", 4, 0.0);
        player->ping_pong = true;
        for (int frame = 0; frame < 7; ++frame)
            character.update(166'667); // 119 ms -> reflects to 81 ms
        expect(player->current_ticks() == 810'000, "ping-pong reflects at the end");
        for (int frame = 0; frame < 5; ++frame)
            character.update(166'667); // 81 - 85 = -4 ms -> reflects to 4 ms
        expect(player->current_ticks() == 40'000, "ping-pong reflects at the start");
    }
    {
        // The blender: both players advance, the fade is linear in the unrounded
        // frame time, and a channel that only owns some bones fades against
        // whatever the lower channels wrote.
        CharacterPose character{rig};
        character.animations.play("Loop", 0, 0.0);
        character.update(0);
        character.animations.play("Far", 0, 0.25);
        character.update(1'250'000); // half the blend; Loop is at 125 ms -> wrapped to 25 ms (frame 0.75)
        expect(std::abs(character.skeleton()[0].translation.x - (0.75F + (10.0F - 0.75F) * 0.5F)) < 1.0e-4F,
               "a cross-fade mixes the previous and the current clip");
        character.update(1'250'000);
        expect(character.skeleton()[0].translation.x == 10.0F, "a finished fade is the current clip alone");
        character.animations.play("UpperOnly", 2, 0.0);
        character.update(0);
        expect(character.skeleton()[0].translation.x == 10.0F, "an upper-body clip leaves the root to channel 0");
        character.animations.clear(0, 0.0);
        character.update(0);
        expect(character.skeleton()[0].translation.x == 10.0F, "a cleared channel leaves the last pose in the skeleton");
    }
    {
        // Three recycled players per channel; a recycled player keeps its paused state.
        CharacterPose character{rig};
        ClipPlayer *first = character.animations.play("Loop", 1, 0.0);
        first->pause();
        character.animations.replay(1, first, 0.0);
        character.update(166'667);
        expect(first->current_ticks() == 0, "a paused player does not advance, even replayed onto itself");
        ClipPlayer *second = character.animations.play("Loop", 1, 0.0);
        ClipPlayer *third = character.animations.play("Loop", 1, 0.0);
        ClipPlayer *fourth = character.animations.play("Far", 1, 0.0);
        expect(second != first && third != second && fourth == first, "a channel cycles through three players");
        expect(!fourth->playing() && fourth->name() == "Far" && fourth->speed == 1.0F,
               "SetClip resets the clip state but not the pause");
        expect(character.animations.play("Missing", 1, 0.0) == nullptr, "an unregistered clip is refused");
    }
    {
        // The eye camera is built from the head of the previous update.
        RetailRig moving = synthetic();
        moving.clips[0].positions[static_cast<std::size_t>(bone_head)] = {{0.0F, 0.0F, 0.0F}};
        moving.attachment[static_cast<std::size_t>(bone_head)].bind[13] = 1.0F;
        CharacterPose character{moving};
        expect(character.eye()[13] == 1.0F, "the first eye camera comes from the bind pose");
        expect(character.eye_now()[13] == 1.0F, "the unlagged eye agrees at rest");
    }
    std::cout << "PASS: clip clock, slerp, key hold, loop/clamp/ping-pong, blender and player recycling\n";
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc < 3)
            return pinned();
        if (!std::filesystem::exists(argv[1]) || !std::filesystem::exists(argv[2]))
        {
            std::cout << "SKIP: the retail character or the original's reference has not been produced\n";
            return 77;
        }
        return replay(argv[1], argv[2]);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
