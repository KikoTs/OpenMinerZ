// GunFX (openminerz/modding/gunfx.hpp): weapon parts cut out of retail
// models, part tracks and cues in pack clips, reload clips stretched to the
// game's reload window, clip variants, and the procedural first-person motion.
// No device and no game content: synthetic meshes, a synthetic character and
// clip files written here.

#include "modding_test_harness.hpp"

#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/gunfx.hpp"
#include "openminerz/retail_rig.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
using namespace openminerz::modding;
namespace graphics = openminerz::modding::graphics;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(float a, float b, float tolerance = 1.0e-4F)
{
    return std::abs(a - b) <= tolerance;
}

template <typename Function> std::string error_of(Function &&function)
{
    try
    {
        function();
    }
    catch (const ScriptError &error)
    {
        return error.what();
    }
    return {};
}

Value vec(double x, double y, double z)
{
    return Value{modding::Vec3{x, y, z}};
}

Value box(double x0, double y0, double z0, double x1, double y1, double z1)
{
    return Value{Map{{"min", vec(x0, y0, z0)}, {"max", vec(x1, y1, z1)}}};
}

// --- Definitions ---------------------------------------------------------------

void parts_parse_and_refuse()
{
    const Value parts{Array{
        Map{{"name", "mag"}, {"box", box(-0.3, 0.0, -0.2, -0.1, 0.2, 0.0)}, {"pivot", vec(-0.2, 0.1, 0.0)}},
        Map{{"name", "mag_new"}, {"copy", "mag"}, {"hidden", true}},
        Map{{"name", "bolt"}, {"boxes", Array{box(0, 0, 0, 1, 1, 1), box(2, 2, 2, 3, 3, 3)}}, {"islands", true}},
        Map{{"name", "rocket"}, {"mesh", 1}},
        Map{{"name", "shell"}, {"model", "models/shell"}, {"rotation", vec(0, 90, 0)}, {"scale", 0.5}, {"hidden", true}},
    }};
    const std::vector<gunfx::PartSpec> specs = gunfx::parts_from_value(parts);
    check(specs.size() == 5U && specs[0].name == "mag" && specs[1].copy == "mag" && specs[1].hidden,
          "parts keep their order and fields");
    check(specs[0].pivot.has_value() && near(specs[0].pivot->x, -0.2F), "pivot");
    check(specs[2].boxes.size() == 2U && specs[2].islands, "several boxes, islands");
    check(specs[3].mesh == 1 && specs[3].boxes.empty(), "a whole mesh");
    check(specs[4].model == "models/shell" && near(specs[4].rotation.y, 90.0F) && near(specs[4].scale, 0.5F),
          "a pack mesh part");
    // A map of parts works too.
    const auto mapped = gunfx::parts_from_value(Value{Map{{"slide", Map{{"box", box(0, 0, 0, 1, 1, 1)}}}}});
    check(mapped.size() == 1U && mapped[0].name == "slide", "map form");
    // Corners in either order.
    const auto flipped = gunfx::parts_from_value(Value{Array{Map{{"name", "a"}, {"box", box(1, 1, 1, 0, 0, 0)}}}});
    check(flipped[0].boxes[0].min.x == 0.0F && flipped[0].boxes[0].max.x == 1.0F, "corners are ordered");

    const auto fails = [](Value value, std::string_view expected) {
        const std::string message = error_of([&] { (void)gunfx::parts_from_value(value); });
        check(message.find(expected) != std::string::npos,
              std::format("expected an error naming \"{}\", got \"{}\"", expected, message));
    };
    fails(Value{Array{Map{{"name", "Mag"}, {"box", box(0, 0, 0, 1, 1, 1)}}}}, "lowercase");
    fails(Value{Array{Map{{"name", "mag"}, {"bx", box(0, 0, 0, 1, 1, 1)}}}}, "unknown key \"bx\"");
    fails(Value{Array{Map{{"name", "mag"}}}}, "give it box");
    fails(Value{Array{Map{{"name", "mag"}, {"copy", "nothing"}}}}, "copy names \"nothing\"");
    fails(Value{Array{Map{{"name", "a"}, {"box", box(0, 0, 0, 1, 1, 1)}}, Map{{"name", "a"}, {"mesh", 0}}}},
          "two parts are named");
    fails(Value{Array{Map{{"name", "a"}, {"box", Map{{"min", vec(0, 0, 0)}}}}}}, "min = vector");
    fails(Value{Array{Map{{"name", "a"}, {"model", "../x"}}}}, "asset name");
    fails(Value{42}, "list of parts");
}

void cues_parse_and_refuse()
{
    const Value cues{Array{
        Map{{"time", 1.2}, {"sound", "sounds/mag_in"}, {"volume", 0.8}, {"pitch_jitter", 0.05}},
        Map{{"time", 0.3}, {"attach", "mag"}, {"bone", "PropLeft"}},
        Map{{"time", 0.5}, {"drop", "mag"}, {"velocity", vec(0.5, -1, 0)}, {"spin", 6}, {"life", 3}},
        Map{{"at", 0.9}, {"kick", Map{{"pitch", 1.0}, {"roll", -2.0}, {"seconds", 0.2}}}},
        Map{{"time", 0.6}, {"show", "mag_new"}, {"bone", "PropLeft"}, {"who", "self"}},
        Map{{"time", 1.0}, {"detach", "mag_new"}, {"blend", 0.2}},
    }};
    const gunfx::ClipCues list = gunfx::cues_from_value(
        "GunReload", cues, Value{Map{{"model", "ak"}, {"mute", Array{"AssaultReload"}}, {"remote_volume", 0.5}}});
    check(list.clip == "GunReload" && list.model == "ak" && list.mute.size() == 1U && near(list.remote_volume, 0.5F),
          "options");
    check(list.cues.size() == 6U, "every cue");
    check(list.cues[0].action == gunfx::CueAction::attach && near(list.cues[0].time, 0.3F),
          "seconds cues are sorted by time");
    const auto drop = std::find_if(list.cues.begin(), list.cues.end(),
                                   [](const gunfx::Cue &cue) { return cue.action == gunfx::CueAction::drop; });
    check(drop != list.cues.end() && near(drop->velocity.x, 0.5F) && near(drop->spin.x, 6.0F) && near(drop->life, 3.0F),
          "drop fields");
    const auto kick = std::find_if(list.cues.begin(), list.cues.end(),
                                   [](const gunfx::Cue &cue) { return cue.action == gunfx::CueAction::kick; });
    check(kick != list.cues.end() && kick->fraction && near(kick->kick.z, -2.0F) && near(kick->kick_seconds, 0.2F),
          "kick fields");
    const auto show = std::find_if(list.cues.begin(), list.cues.end(),
                                   [](const gunfx::Cue &cue) { return cue.action == gunfx::CueAction::show; });
    check(show != list.cues.end() && show->who == gunfx::CueAudience::self && show->bone == "PropLeft",
          "show on a bone, self only");

    const auto fails = [](const Value &value, std::string_view expected) {
        const std::string message = error_of([&] { (void)gunfx::cues_from_value("GunReload", value, Value{}); });
        check(message.find(expected) != std::string::npos,
              std::format("expected an error naming \"{}\", got \"{}\"", expected, message));
    };
    fails(Value{Array{Map{{"sound", "sounds/x"}}}}, "time");
    fails(Value{Array{Map{{"time", 0.1}, {"at", 0.1}, {"sound", "sounds/x"}}}}, "time");
    fails(Value{Array{Map{{"time", 0.1}}}}, "exactly one");
    fails(Value{Array{Map{{"time", 0.1}, {"sound", "sounds/x"}, {"hide", "mag"}}}}, "exactly one");
    fails(Value{Array{Map{{"time", 0.1}, {"attach", "mag"}}}}, "name the bone");
    fails(Value{Array{Map{{"time", 0.1}, {"sound", "sounds/x"}, {"loud", true}}}}, "unknown key");
    fails(Value{Array{Map{{"time", 0.1}, {"sound", "sounds/x"}, {"who", "everyone"}}}}, "who");
    check(error_of([] { (void)gunfx::cues_from_value("Gun Reload", Value{Array{}}, Value{}); }).find("clip name") !=
              std::string::npos,
          "a bad clip name");
}

void motion_parse()
{
    check(!gunfx::motion_from_value(Value{}).has_value(), "nil turns it off");
    check(!gunfx::motion_from_value(Value{Map{{"enabled", false}}}).has_value(), "enabled = false turns it off");
    const auto motion = gunfx::motion_from_value(Value{Map{
        {"sway", 1.5}, {"recoil", 0.5},
        {"families", Map{{"LMG", Map{{"weight", 1.8}}}, {"Pistol", Map{{"recoil", 0.3}, {"sway", 0.6}}}}}}});
    check(motion.has_value(), "settings");
    using gunfx::Family;
    check(near(motion->tuning(Family::gun).sway, 1.5F) && near(motion->tuning(Family::gun).recoil, 0.5F),
          "a family without overrides takes the top level");
    check(near(motion->tuning(Family::lmg).weight, 1.8F) && near(motion->tuning(Family::lmg).sway, 1.5F),
          "a family override keeps the rest of the top level");
    check(near(motion->tuning(Family::pistol).recoil, 0.3F) && near(motion->tuning(Family::pistol).sway, 0.6F),
          "pistol overrides");
    check(error_of([] { (void)gunfx::motion_from_value(Value{Map{{"families", Map{{"Bazooka", Map{}}}}}}); })
                  .find("unknown family") != std::string::npos,
          "an unknown family");
    check(error_of([] { (void)gunfx::motion_from_value(Value{Map{{"wobble", 1}}}); }).find("unknown key") !=
              std::string::npos,
          "an unknown setting");
}

void names_and_variants()
{
    const gunfx::ClipName name = gunfx::split_clip_name("PistolReloadEmpty@rpg");
    check(name.base == "PistolReload" && name.empty && name.model == "rpg", "a full variant name");
    check(gunfx::split_clip_name("GunReload").base == "GunReload" && !gunfx::split_clip_name("GunReload").empty,
          "a plain name");
    check(gunfx::split_clip_name("Empty").base == "Empty", "a name that is only the suffix stays itself");
    const auto variants = gunfx::variant_names("PumpShotgunReload", "rpg", true);
    check(variants == std::vector<std::string>{"PumpShotgunReloadEmpty@rpg", "PumpShotgunReloadEmpty",
                                               "PumpShotgunReload@rpg", "PumpShotgunReload"},
          "variant order");
    check(gunfx::variant_names("GunShoulderIdle", "ak", true, "modern") ==
              std::vector<std::string>{"GunShoulderIdleEmpty@modern/ak", "GunShoulderIdleEmpty@ak",
                                       "GunShoulderIdleEmpty", "GunShoulderIdle@modern/ak", "GunShoulderIdle@ak",
                                       "GunShoulderIdle"},
          "a generation's own variant comes first");
    check(gunfx::valid_variant_model("classic/ak") && gunfx::valid_variant_model("rpg") &&
              !gunfx::valid_variant_model("future/ak") && !gunfx::valid_variant_model("modern/"),
          "variant models");
    check(gunfx::cues_from_value("GunShoulderIdle@modern/ak", Value{Array{}}, Value{}).clip ==
              "GunShoulderIdle@modern/ak",
          "a generation's variant takes cues");
    check(gunfx::variant_names("GunIdle", "", false) == std::vector<std::string>{"GunIdle"}, "no variant");
    check(gunfx::reload_clip("LaserSMGReload") && gunfx::reload_clip("PistolReloadEmpty@space_pistol") &&
              !gunfx::reload_clip("GunShoot"),
          "reload clips");
    check(gunfx::held_model_name(50U) == "ak" && gunfx::held_model_name(119U) == "lmg" &&
              gunfx::held_model_name(113U) == "rpg" && gunfx::held_model_name(86U) == "space_smg" &&
              gunfx::held_model_name(0U).empty(),
          "held model names");
    check(gunfx::family_of(PlayerAnimationMode::space_pump_shotgun) == gunfx::Family::laser_shotgun &&
              gunfx::family_name(gunfx::Family::pump_shotgun) == "PumpShotgun" &&
              gunfx::family_from_name("LaserSMG") == gunfx::Family::laser_smg,
          "families");
}

void library_layers_mods()
{
    gunfx::Library library;
    check(library.empty(), "empty at first");
    const auto parts = [](std::string_view name) {
        return gunfx::parts_from_value(Value{Array{Map{{"name", std::string{name}}, {"mesh", 0}}}});
    };
    library.define_parts("first", "ak", parts("mag"));
    library.define_parts("second", "ak", parts("bolt"));
    library.define_parts("first", "modern/ak", parts("drum"));
    check(library.parts("classic", "ak")->front().name == "bolt", "the later mod wins");
    check(library.parts("modern", "ak")->front().name == "drum", "a generation's own parts win over the plain ones");
    library.release_mod("second");
    check(library.parts("classic", "ak")->front().name == "mag", "the earlier mod's come back when the later goes");
    check(library.parts("classic", "pistol") == nullptr, "nothing for another model");

    const auto cues = [](std::string_view clip, std::string_view model, std::string_view mute) {
        Map options{{"mute", Array{std::string{mute}}}};
        if (!model.empty())
            options.emplace_back("model", std::string{model});
        gunfx::ClipCues list = gunfx::cues_from_value(clip, Value{Array{Map{{"at", 0.5}, {"sound", "sounds/x"}}}},
                                                      Value{std::move(options)});
        list.mod_id = "pack";
        return list;
    };
    library.define_cues(cues("PumpShotgunReload", "", "ShotGunReload"));
    library.define_cues(cues("PumpShotgunReload", "rpg", "RPGReload"));
    library.define_cues(cues("PistolReloadEmpty", "", "EmptyReload"));
    check(library.cues("PumpShotgunReload", "rpg")->model == "rpg", "a model's own cues win");
    check(library.cues("PumpShotgunReload", "shotgun")->model.empty(), "others take the general list");
    check(library.mutes("PumpShotgunReload", "shotgun", false, "ShotGunReload"), "mute");
    check(!library.mutes("PumpShotgunReload", "rpg", false, "ShotGunReload") &&
              library.mutes("PumpShotgunReload", "rpg", false, "RPGReload"),
          "the rpg's own list decides for the rpg");
    check(library.mutes("PistolReload", "pistol", true, "EmptyReload") &&
              !library.mutes("PistolReload", "pistol", false, "EmptyReload"),
          "an empty reload uses the Empty variant's list");
    check(!library.mutes("GunReload", "ak", false, "AssaultReload"), "no list, no mute");

    const std::uint64_t before = library.revision();
    library.define_motion("pack", gunfx::motion_from_value(Value{Map{{"sway", 2.0}}}));
    check(library.revision() != before && library.motion() != nullptr && near(library.motion()->base.sway, 2.0F),
          "motion and revision");
    library.define_motion("pack", std::nullopt);
    check(library.motion() == nullptr, "motion off");
    library.clear();
    check(library.empty(), "clear");
}

// --- Cutting ------------------------------------------------------------------------

// Two unit cubes as triangle soups (12 triangles each), the second at x = 3.
struct TestMesh final
{
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> indices;
};

void add_cube(TestMesh &mesh, float x, float y, float z, float size)
{
    const auto base = static_cast<std::uint32_t>(mesh.positions.size());
    for (int corner = 0; corner < 8; ++corner)
        mesh.positions.push_back({x + ((corner & 1) != 0 ? size : 0.0F), y + ((corner & 2) != 0 ? size : 0.0F),
                                  z + ((corner & 4) != 0 ? size : 0.0F)});
    static constexpr std::array<std::uint32_t, 36> faces{0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                                         2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    for (const std::uint32_t index : faces)
        mesh.indices.push_back(base + index);
}

void cut_parts_by_boxes_islands_meshes()
{
    TestMesh body;
    add_cube(body, 0.0F, 0.0F, 0.0F, 1.0F);
    add_cube(body, 3.0F, 0.0F, 0.0F, 1.0F);
    TestMesh rocket;
    add_cube(rocket, -2.0F, 0.0F, 0.0F, 0.5F);
    const std::array<gunfx::MeshTriangles, 2> meshes{
        gunfx::MeshTriangles{body.positions, body.indices}, gunfx::MeshTriangles{rocket.positions, rocket.indices}};

    // A box around the second cube's centroids takes all its triangles.
    const auto specs = gunfx::parts_from_value(Value{Array{
        Map{{"name", "mag"}, {"box", box(2.5, -1, -1, 4.5, 2, 2)}},
        Map{{"name", "mag_new"}, {"copy", "mag"}, {"hidden", true}},
        Map{{"name", "rocket"}, {"mesh", 1}},
        // Overlaps mag: mag claimed first.
        Map{{"name", "late"}, {"box", box(-10, -10, -10, 10, 10, 10)}, {"exclude", Array{box(-0.5, -0.5, -0.5, 1.5, 1.5, 1.5)}}},
    }});
    const gunfx::CutResult cut = gunfx::cut_parts(specs, meshes);
    check(cut.parts[0].count == 12U && cut.parts[0].triangles[0].size() == 12U, "the box takes the second cube");
    check(near(cut.parts[0].pivot.x, 3.5F) && near(cut.parts[0].pivot.y, 0.5F), "the pivot is the bounds' centre");
    check(cut.parts[1].count == 12U && cut.parts[1].triangles == cut.parts[0].triangles, "a copy has the same triangles");
    check(cut.parts[2].count == 12U && cut.parts[2].triangles[1].size() == 12U && cut.parts[2].triangles[0].empty(),
          "a whole mesh");
    check(cut.parts[3].count == 0U, "an earlier part's triangles and excluded ones are not taken");
    std::size_t body_left{};
    for (const int owner : cut.owner[0])
        body_left += owner < 0 ? 1U : 0U;
    check(body_left == 12U, "the first cube stays on the body");
    check(cut.owner[0][12] == 0 && cut.owner[1][0] == 2, "owners");

    // Islands: a box that reaches only the island's middle takes the whole of it.
    TestMesh joined;
    add_cube(joined, 0.0F, 0.0F, 0.0F, 1.0F);
    add_cube(joined, 0.0F, 2.0F, 0.0F, 1.0F);
    const std::array<gunfx::MeshTriangles, 1> one{gunfx::MeshTriangles{joined.positions, joined.indices}};
    const auto by_triangle =
        gunfx::cut_parts(gunfx::parts_from_value(Value{Array{Map{{"name", "a"}, {"box", box(0.4, 0.4, -1, 0.6, 0.6, 2)}}}}), one);
    const auto by_island = gunfx::cut_parts(
        gunfx::parts_from_value(Value{Array{Map{{"name", "a"}, {"box", box(0.4, 0.4, -1, 0.6, 0.6, 2)}, {"islands", true}}}}),
        one);
    check(by_triangle.parts[0].count < 12U, "by centroid only some triangles of the cube are inside");
    check(by_island.parts[0].count == 12U, "with islands the whole first cube, and not the second");
}

// --- Part poses --------------------------------------------------------------------------

std::array<float, 3> apply(const rig::Matrix &matrix, std::array<float, 3> point)
{
    return rig::transform_point(point.data(), matrix);
}

void part_motion_turns_about_the_pivot()
{
    const gunfx::Vec3 pivot{1.0F, 2.0F, 3.0F};
    const rig::Matrix rest = gunfx::part_motion(nullptr, 0, pivot);
    const auto at_rest = apply(rest, {5.0F, 6.0F, 7.0F});
    check(near(at_rest[0], 5.0F) && near(at_rest[1], 6.0F) && near(at_rest[2], 7.0F), "no track: at rest");

    gunfx::PartTrack track;
    track.part = "bolt";
    // Half a turn about +Z over two keys (1/30 s), and 0.1 down.
    track.rotation = {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F, 0.0F}};
    track.translation = {{0.0F, 0.0F, 0.0F}, {0.0F, -0.1F, 0.0F}};
    const std::int64_t key = 10'000'000 / 30;
    const rig::Matrix end = gunfx::part_motion(&track, key, pivot);
    const auto pivot_after = apply(end, {pivot.x, pivot.y, pivot.z});
    check(near(pivot_after[0], 1.0F) && near(pivot_after[1], 1.9F) && near(pivot_after[2], 3.0F),
          "the pivot only moves by the translation");
    const auto beside = apply(end, {2.0F, 2.0F, 3.0F});
    check(near(beside[0], 0.0F) && near(beside[1], 1.9F), "a point beside it turns half round about it");
    const rig::Matrix half = gunfx::part_motion(&track, key / 2, pivot);
    const auto quarter = apply(half, {2.0F, 2.0F, 3.0F});
    check(near(quarter[0], 1.0F, 2.0e-3F) && near(quarter[1], 2.95F, 2.0e-3F), "halfway: a quarter turn");
    const rig::Matrix later = gunfx::part_motion(&track, key * 9, pivot);
    const rig::Matrix last = gunfx::part_motion(&track, key + 1, pivot);
    check(later == last, "past the last key it holds");
}

void part_states_are_a_function_of_time()
{
    const auto specs = gunfx::parts_from_value(Value{Array{
        Map{{"name", "mag"}, {"mesh", 0}}, Map{{"name", "mag_new"}, {"copy", "mag"}, {"hidden", true}}}});
    const gunfx::ClipCues list = gunfx::cues_from_value(
        "GunReload",
        Value{Array{Map{{"at", 0.2}, {"attach", "mag"}, {"bone", "PropLeft"}}, Map{{"at", 0.4}, {"drop", "mag"}},
                    Map{{"at", 0.5}, {"show", "mag_new"}, {"bone", "PropLeft"}},
                    Map{{"at", 0.8}, {"detach", "mag_new"}, {"blend", 0.2}},
                    Map{{"at", 0.3}, {"sound", "sounds/x"}}}},
        Value{});
    const std::int64_t duration = 20'000'000; // 2 s
    const auto bone = [](std::string_view name) { return name == "PropLeft" ? 41 : -1; };
    std::vector<gunfx::PartState> states;
    const auto at = [&](double share) {
        gunfx::part_states(specs, &list, nullptr, duration,
                           static_cast<std::int64_t>(share * static_cast<double>(duration)), bone, states);
        return states;
    };
    auto s = at(0.1);
    check(s[0].visible && s[0].bone < 0 && !s[1].visible, "at the start: on the gun, the spare hidden");
    s = at(0.2125);
    check(s[0].bone == 41 && near(s[0].attach_weight, 0.25F, 1.0e-3F), "easing into the hand");
    s = at(0.25);
    check(s[0].bone == 41 && s[0].attach_cue >= 0 && s[0].attach_weight == 1.0F, "attached to the hand");
    s = at(0.45);
    check(!s[0].visible && s[0].dropped && s[0].bone == 41, "dropped from the hand");
    s = at(0.6);
    check(s[1].visible && s[1].bone == 41, "the spare shows in the hand");
    s = at(0.85);
    check(s[1].bone == 41 && s[1].detach_cue >= 0 && near(s[1].detach_weight, 0.5F, 1.0e-3F),
          "halfway through easing back onto the gun");
    s = at(0.95);
    check(s[1].visible && s[1].bone < 0 && s[1].detach_weight == 1.0F, "back on the gun");
    // The same time gives the same state whatever came before.
    const auto again = at(0.6);
    check(again[1].visible && again[1].bone == 41 && !again[0].visible, "a pure function of the time");
    gunfx::part_states(specs, nullptr, nullptr, duration, duration / 2, bone, states);
    check(states[0].visible && !states[1].visible, "without cues every part is at rest");
}

void cue_ticks_follow_the_clip()
{
    gunfx::ClipCues list;
    list.clip = "GunReload";
    gunfx::Cue cue;
    cue.time = 1.5F;
    const std::int64_t game = 29'830'000; // 2.983 s
    check(gunfx::cue_ticks(cue, list, nullptr, game) == 15'000'000, "no file, no length: the game clip's seconds");
    gunfx::ClipTracks tracks;
    tracks.authored_seconds = 3.0F;
    tracks.stretched = true;
    check(gunfx::cue_ticks(cue, list, &tracks, game) == game / 2, "a stretched clip: half its authored length");
    tracks.stretched = false;
    check(gunfx::cue_ticks(cue, list, &tracks, game) == 15'000'000, "not stretched: seconds");
    list.length = 6.0F;
    check(gunfx::cue_ticks(cue, list, nullptr, game) == game / 4, "length stretches a reload with no file");
    cue.fraction = true;
    cue.time = 0.25F;
    check(gunfx::cue_ticks(cue, list, nullptr, game) == game / 4, "a share");
}

// One clip played at a frame rate; every cue fired with its frame time.
std::vector<std::pair<int, double>> play(const gunfx::ClipCues &list, std::int64_t duration, double fps,
                                         double speed = 1.0)
{
    gunfx::CueCursor cursor;
    std::vector<std::pair<int, double>> fired;
    const int clip_tag = 1;
    double seconds = 0.0;
    const double frame = 1.0 / fps;
    while (true)
    {
        const auto position = std::min<std::int64_t>(
            duration, static_cast<std::int64_t>(seconds * speed * 10'000'000.0));
        const auto step = cursor.advance(&clip_tag, position, duration, false, &list, nullptr);
        for (const int index : step.fired)
            fired.emplace_back(index, seconds);
        if (position >= duration)
            break;
        seconds += frame;
    }
    return fired;
}

void cues_fire_once_at_any_frame_rate()
{
    const gunfx::ClipCues list = gunfx::cues_from_value(
        "GunReload",
        Value{Array{Map{{"at", 0.0}, {"sound", "sounds/a"}}, Map{{"at", 0.25}, {"sound", "sounds/b"}},
                    Map{{"at", 0.5}, {"sound", "sounds/c"}}, Map{{"at", 1.0}, {"sound", "sounds/d"}}}},
        Value{});
    const std::int64_t duration = 30'000'000;
    for (const double fps : {30.0, 60.0, 144.0, 360.0, 23.7})
    {
        const auto fired = play(list, duration, fps, 1.2);
        check(fired.size() == 4U, std::format("{} fps: every cue fires once ({} fired)", fps, fired.size()));
        for (std::size_t index = 0U; index < fired.size(); ++index)
        {
            check(fired[index].first == static_cast<int>(index), "in order");
            const double expected = std::array<double, 4>{0.0, 0.75, 1.5, 3.0}[index] / 1.2;
            check(fired[index].second >= expected - 1.0e-9 && fired[index].second < expected + 1.0 / fps + 1.0e-6,
                  std::format("{} fps: cue {} within a frame of {:.3f} s (at {:.3f})", fps, index, expected,
                              fired[index].second));
        }
    }
}

void cursor_knows_ends_cuts_restarts()
{
    const gunfx::ClipCues list = gunfx::cues_from_value(
        "GunReload", Value{Array{Map{{"at", 0.1}, {"sound", "sounds/a"}}, Map{{"at", 0.9}, {"sound", "sounds/b"}}}},
        Value{});
    const int reload = 1;
    const int idle = 2;
    const std::int64_t duration = 20'000'000;
    {
        gunfx::CueCursor cursor;
        (void)cursor.advance(&reload, 0, duration, false, &list, nullptr);
        auto step = cursor.advance(&reload, 6'000'000, duration, false, &list, nullptr);
        check(step.fired.size() == 1U, "the first cue");
        step = cursor.advance(&idle, 1'000, 5'000'000, true, nullptr, nullptr);
        check(step.cut && !step.ended, "switching away at 30 % cuts it short");
        step = cursor.advance(&idle, 2'000, 5'000'000, true, nullptr, nullptr);
        check(step.fired.empty() && !step.cut, "nothing more fires");
    }
    {
        gunfx::CueCursor cursor;
        for (std::int64_t at = 0; at < duration; at += 166'667)
            (void)cursor.advance(&reload, at, duration, false, &list, nullptr);
        const auto step = cursor.advance(&idle, 0, 5'000'000, true, nullptr, nullptr);
        check(step.ended && !step.cut, "reaching the end is not a cut");
    }
    {
        // A shot clip played again from the start: a new instance, cues again.
        gunfx::CueCursor cursor;
        (void)cursor.advance(&reload, 0, duration, false, &list, nullptr);
        (void)cursor.advance(&reload, 19'000'000, duration, false, &list, nullptr);
        const auto step = cursor.advance(&reload, 2'500'000, duration, false, &list, nullptr);
        check(step.restarted && step.ended && step.fired.size() == 1U && step.fired[0] == 0, "a restart fires again");
    }
    {
        // Seen first halfway through (joined late, a capture): nothing it passed.
        gunfx::CueCursor cursor;
        auto step = cursor.advance(&reload, 15'000'000, duration, false, &list, nullptr);
        check(step.fired.empty(), "a late start fires nothing it skipped");
        step = cursor.advance(&reload, 19'000'000, duration, false, &list, nullptr);
        check(step.fired.size() == 1U && step.fired[0] == 1, "but what comes after");
    }
    {
        // Played backwards (the sights lowered): nothing.
        gunfx::CueCursor cursor;
        auto step = cursor.advance(&reload, duration, duration, false, &list, nullptr);
        std::size_t fired = step.fired.size();
        for (std::int64_t at = duration; at >= 0; at -= 500'000)
            fired += cursor.advance(&reload, at, duration, false, &list, nullptr).fired.size();
        check(fired == 0U, "a reversed clip fires nothing");
    }
    {
        // A looping clip fires each loop.
        gunfx::CueCursor cursor;
        std::size_t fired{};
        for (std::int64_t t = 0; t < duration * 3; t += 333'333)
            fired += cursor.advance(&idle, t % duration, duration, true, &list, nullptr).fired.size();
        check(fired == 6U, std::format("three loops fire both cues three times ({})", fired));
    }
}

// --- Motion --------------------------------------------------------------------------------

void springs_are_exact()
{
    for (const float damping : {0.3F, 1.0F, 1.7F})
    {
        gunfx::Spring once{1.0F, 2.0F};
        once.step(0.0F, 3.0F, damping, 0.12F);
        gunfx::Spring many{1.0F, 2.0F};
        for (int step = 0; step < 12; ++step)
            many.step(0.0F, 3.0F, damping, 0.01F);
        check(near(once.position, many.position, 1.0e-5F) && near(once.velocity, many.velocity, 1.0e-4F),
              std::format("damping {}: one step equals twelve", damping));
    }
    gunfx::Spring settle{1.0F, 0.0F};
    for (int step = 0; step < 300; ++step)
        settle.step(0.25F, 4.0F, 0.6F, 1.0F / 60.0F);
    check(near(settle.position, 0.25F, 1.0e-4F) && near(settle.velocity, 0.0F, 1.0e-3F), "it settles on the target");
}

// A scripted second and a half of play: walking while turning, a hop, two
// shots, an item switch. Returns where a point of the weapon ends up every
// sixth of a second. Samples and events fall on multiples of 1/6 s, a frame boundary at
// every rate tested, so only the motion itself can differ between rates.
std::vector<std::array<float, 3>> run_motion(int fps, bool reduced = false)
{
    gunfx::MotionSettings settings;
    settings.families.fill(settings.base);
    gunfx::Motion motion;
    std::vector<std::array<float, 3>> samples;
    const double frame = 1.0 / fps;
    const auto at = [&](int frame_index, double seconds) {
        return std::abs(frame_index * frame - seconds) < 0.5 * frame;
    };
    for (int index = 0; index * frame < 1.5 + 1.0e-9; ++index)
    {
        const double t = index * frame;
        gunfx::MotionInput input;
        input.seconds = static_cast<float>(frame);
        input.yaw = static_cast<float>(0.8 * std::sin(t * 2.0));
        input.pitch = static_cast<float>(0.2 * t);
        const bool airborne = t > 2.0 / 3.0 + 1.0e-9 && t < 5.0 / 6.0 - 1.0e-9;
        input.velocity = {static_cast<float>(3.0 * std::sin(input.yaw)), airborne ? 4.0F : 0.0F,
                          static_cast<float>(-3.0 * std::cos(input.yaw))};
        input.grounded = !airborne;
        input.family = gunfx::Family::gun;
        input.reduced_motion = reduced;
        input.seed = 7U;
        input.shot = at(index, 1.0 / 3.0) || at(index, 0.5);
        input.switched = at(index, 7.0 / 6.0);
        const gunfx::MotionOutput output = motion.step(settings, input);
        for (const float value : output.offset)
            check(std::isfinite(value), "finite");
        // A frame's output is the pose at its end.
        if (const double end = (index + 1) * frame; at(index + 1, std::round(end * 6.0) / 6.0))
            samples.push_back(apply(output.offset, {0.1F, -0.15F, -0.5F}));
    }
    return samples;
}

void motion_is_frame_rate_independent()
{
    const auto reference = run_motion(360);
    for (const int fps : {30, 60, 144, 240})
    {
        const auto samples = run_motion(fps);
        check(samples.size() == reference.size(), "same sample count");
        float worst{};
        for (std::size_t index = 0U; index < samples.size(); ++index)
            for (std::size_t axis = 0U; axis < 3U; ++axis)
                worst = std::max(worst, std::abs(samples[index][axis] - reference[index][axis]));
        // Within a few millimetres of the 360 fps path at the same moments.
        check(worst < 0.004F, std::format("{} fps: {:.4f} m from the 360 fps path", fps, worst));
        std::cout << std::format("  motion at {:>3} fps: {:.4f} m from 360 fps\n", fps, worst);
    }
    // Something actually moved.
    float moved{};
    for (const auto &sample : reference)
        moved = std::max(moved, std::abs(sample[1] + 0.15F));
    check(moved > 0.005F, "the weapon moves");
    // Reduced motion keeps it still (the eased sights alone remain).
    float still{};
    for (const auto &sample : run_motion(60, true))
        still = std::max({still, std::abs(sample[0] - 0.1F), std::abs(sample[1] + 0.15F), std::abs(sample[2] + 0.5F)});
    check(still < 0.004F, std::format("reduced motion: {:.4f} m", still));
    check(near(gunfx::Motion::ease_aim(0.5F, 1.0F), 0.5F) && gunfx::Motion::ease_aim(0.2F, 1.0F) < 0.2F &&
              near(gunfx::Motion::ease_aim(0.2F, 0.0F), 0.2F) && near(gunfx::Motion::ease_aim(1.0F, 1.0F), 1.0F),
          "aim easing");
}

// --- The pack loader --------------------------------------------------------------------------

// A minimal glTF with data URIs: node 0 a bone ("BackUpper"), node 1 a part
// ("mag"); each animation turns the bone about Y and moves the part down over
// `seconds`.
std::string base64(const std::vector<std::uint8_t> &bytes)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t index = 0U; index < bytes.size(); index += 3U)
    {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
                                    (index + 1U < bytes.size() ? static_cast<std::uint32_t>(bytes[index + 1U]) << 8U : 0U) |
                                    (index + 2U < bytes.size() ? static_cast<std::uint32_t>(bytes[index + 2U]) : 0U);
        out += table[(chunk >> 18U) & 63U];
        out += table[(chunk >> 12U) & 63U];
        out += index + 1U < bytes.size() ? table[(chunk >> 6U) & 63U] : '=';
        out += index + 2U < bytes.size() ? table[chunk & 63U] : '=';
    }
    return out;
}

std::string clip_file(const std::vector<std::pair<std::string, float>> &animations)
{
    std::vector<std::uint8_t> buffer;
    const auto push = [&](float value) {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &value, 4U);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    };
    std::string accessors;
    std::string views;
    std::string json_animations;
    int accessor = 0;
    for (const auto &[name, seconds] : animations)
    {
        const auto view = [&](std::size_t floats, std::string_view type, bool time) {
            const std::size_t offset = buffer.size() - floats * 4U;
            views += std::format("{}{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{}}}", views.empty() ? "" : ",", offset,
                                 floats * 4U);
            accessors += std::format("{}{{\"bufferView\":{},\"componentType\":5126,\"count\":2,\"type\":\"{}\"{}}}",
                                     accessors.empty() ? "" : ",", accessor, type,
                                     time ? std::format(",\"min\":[0],\"max\":[{}]", seconds) : "");
            return accessor++;
        };
        push(0.0F);
        push(seconds);
        const int times = view(2U, "SCALAR", true);
        // Bone: 0 to 90 degrees about Y.
        const float s = std::sin(std::numbers::pi_v<float> / 4.0F);
        for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, s, 0.0F, s})
            push(value);
        const int rotation = view(8U, "VEC4", false);
        // Part: 0 to -0.2 in y.
        for (const float value : {0.0F, 0.0F, 0.0F, 0.0F, -0.2F, 0.0F})
            push(value);
        const int translation = view(6U, "VEC3", false);
        json_animations += std::format(
            "{}{{\"name\":\"{}\",\"samplers\":[{{\"input\":{},\"output\":{}}},{{\"input\":{},\"output\":{}}}],"
            "\"channels\":[{{\"sampler\":0,\"target\":{{\"node\":0,\"path\":\"rotation\"}}}},"
            "{{\"sampler\":1,\"target\":{{\"node\":1,\"path\":\"translation\"}}}}]}}",
            json_animations.empty() ? "" : ",", name, times, rotation, times, translation);
    }
    return std::format(
        "{{\"asset\":{{\"version\":\"2.0\"}},\"scene\":0,\"scenes\":[{{\"nodes\":[0,1]}}],"
        "\"nodes\":[{{\"name\":\"BackUpper\"}},{{\"name\":\"mag\"}}],"
        "\"buffers\":[{{\"byteLength\":{},\"uri\":\"data:application/octet-stream;base64,{}\"}}],"
        "\"bufferViews\":[{}],\"accessors\":[{}],\"animations\":[{}]}}",
        buffer.size(), base64(buffer), views, accessors, json_animations);
}

rig::RetailRig gun_rig()
{
    rig::RetailRig rig;
    rig.generation = "synthetic";
    const auto names = graphics::avatar_bone_names();
    for (std::size_t bone = 0U; bone < names.size(); ++bone)
    {
        rig.attachment[bone].name = std::string{names[bone]};
        rig.attachment[bone].parent = bone == 0U ? -1 : 0;
        rig.attachment[bone].bind = rig::identity();
    }
    rig::Clip reload;
    reload.asset = "GunReload";
    reload.duration_ticks = 20'000'000; // 2 s: the game's clip
    for (std::size_t bone = 0U; bone < names.size(); ++bone)
    {
        reload.rotations[bone] = {{0.0F, 0.0F, 0.0F, 1.0F}};
        reload.positions[bone] = {{}};
        reload.scales[bone] = {{1.0F, 1.0F, 1.0F}};
    }
    rig::Clip idle = reload;
    idle.asset = "GunIdle";
    rig.clips = {reload, idle};
    rig::BoneMask upper{};
    upper.fill(true);
    upper[0] = false;
    rig.registrations.push_back({"GunReload", 0, false, upper});
    rig.registrations.push_back({"GunIdle", 1, true, upper});
    return rig;
}

class TemporaryFolder final
{
  public:
    TemporaryFolder()
        : path_(std::filesystem::temp_directory_path() /
                std::format("openminerz-mod-gunfx-{}", std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        std::filesystem::create_directories(path_ / "assets" / "animations");
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

float bone_angle(const rig::RetailRig &rig, std::string_view clip, std::int64_t position)
{
    const rig::Registration *registration = rig.find(clip);
    check(registration != nullptr, std::string{clip} + " is registered");
    rig::BonePose pose{};
    rig.clips[static_cast<std::size_t>(registration->clip)].copy_transforms(pose, position, nullptr);
    const rig::Quat q = pose[5].rotation; // BackUpper
    return 2.0F * std::atan2(q.y, q.w) * 180.0F / std::numbers::pi_v<float>;
}

void reload_clips_stretch_and_carry_parts()
{
    TemporaryFolder folder;
    {
        std::ofstream file{folder.path() / "assets" / "animations" / "pack.gltf"};
        file << clip_file({{"GunReload", 1.0F}, {"GunIdle", 1.0F}, {"GunReloadEmpty", 4.0F}, {"GunIdle@rpg", 0.5F}});
    }
    graphics::AssetStack stack;
    stack.set_sources({{"pack", folder.path() / "assets"}});
    rig::RetailRig rig = gun_rig();
    std::vector<std::string> warnings;
    gunfx::TrackTable tracks;
    const std::vector<std::string> replaced = graphics::apply_animation_pack(rig, stack, &warnings, &tracks);
    for (const std::string &warning : warnings)
        std::cout << "  warning: " << warning << '\n';
    check(replaced.size() == 4U, std::format("four clips ({})", replaced.size()));

    // GunReload authored at 1 s plays over the game's 2 s: 45 degrees at 1 s.
    check(rig.clips[static_cast<std::size_t>(rig.find("GunReload")->clip)].duration_ticks == 20'000'000,
          "the game's length");
    check(near(bone_angle(rig, "GunReload", 10'000'000), 45.0F, 0.5F), "a reload is stretched: halfway at 1 s");
    check(near(bone_angle(rig, "GunReload", 20'000'000), 90.0F, 0.5F), "and ends at the game's end");
    // GunIdle is not a reload: its keys play at their own speed and hold.
    check(near(bone_angle(rig, "GunIdle", 5'000'000), 45.0F, 0.5F) &&
              near(bone_angle(rig, "GunIdle", 15'000'000), 90.0F, 0.5F),
          "other clips keep their own clock");
    // The Empty variant is a registration of its own, stretched onto GunReload's 2 s.
    const rig::Registration *empty = rig.find("GunReloadEmpty");
    check(empty != nullptr && empty->influence == rig.find("GunReload")->influence && !empty->looping,
          "the variant takes its base's influence and looping");
    check(near(bone_angle(rig, "GunReloadEmpty", 10'000'000), 45.0F, 0.5F), "a 4 s variant is stretched to 2 s too");
    check(rig.find("GunIdle@rpg") != nullptr && rig.find("GunIdle@rpg")->looping, "a model variant");

    // The part tracks.
    check(tracks.contains("GunReload") && tracks.contains("GunReloadEmpty"), "tracks per clip");
    const gunfx::ClipTracks &reload = tracks.at("GunReload");
    check(reload.stretched && near(reload.authored_seconds, 1.0F) && reload.duration_ticks == 20'000'000,
          "the reload's file length and stretch");
    const gunfx::PartTrack *mag = reload.find("mag");
    check(mag != nullptr && !mag->translation.empty() && mag->rotation.empty(), "mag moves, does not turn");
    const rig::Matrix halfway = gunfx::part_motion(mag, 10'000'000, {});
    check(near(halfway[13], -0.1F, 2.0e-3F), "the part track is stretched with the bones");
    check(!tracks.at("GunIdle").stretched, "an idle's tracks are not stretched");
    // A cue at 0.5 s of the 1 s file fires at 1 s of the game's clip.
    gunfx::ClipCues list;
    list.clip = "GunReload";
    gunfx::Cue cue;
    cue.time = 0.5F;
    check(gunfx::cue_ticks(cue, list, &reload, 20'000'000) == 10'000'000, "cue times stretch with the clip");

    // check-anim accepts part tracks and variants.
    std::string error;
    const auto model = graphics::load_model(folder.path() / "assets" / "animations" / "pack.gltf", &error,
                                            graphics::animation_file_limits());
    check(model != nullptr, "the file loads: " + error);
    const graphics::AnimationCheck checked = graphics::check_animation(*model, "pack");
    for (const auto &message : checked.messages)
        if (message.level == graphics::AnimationCheck::Level::error)
            throw std::runtime_error("check-anim error: " + message.text);
    const bool part_noted =
        std::any_of(checked.messages.begin(), checked.messages.end(),
                    [](const auto &message) { return message.text.find("part track") != std::string::npos; });
    check(part_noted, "check-anim names the part track");
    const bool variant_used = std::any_of(checked.clips.begin(), checked.clips.end(), [](const auto &clip) {
        return clip.animation == "GunReloadEmpty" && clip.registration == "GunReloadEmpty";
    });
    check(variant_used, "check-anim takes a variant as replacing something");
}

// --- The script library ---------------------------------------------------------------------

class SilentBackend final : public graphics::GraphicsBackend
{
  public:
    graphics::Mixer mixer_instance{48000};
    [[nodiscard]] bool locate(Handle, graphics::Vec3 &, float &) const override
    {
        return false;
    }
    [[nodiscard]] bool effects_available() const override
    {
        return false;
    }
    vfx::EffectHandle spawn_effect(const vfx::EffectDesc &, const vfx::SpawnParams &) override
    {
        return {};
    }
    [[nodiscard]] bool effect_alive(vfx::EffectHandle) const override
    {
        return false;
    }
    void move_effect(vfx::EffectHandle, graphics::Vec3, graphics::Vec3) override
    {
    }
    void kill_effect(vfx::EffectHandle, bool) override
    {
    }
    [[nodiscard]] graphics::Mixer *mixer(bool) override
    {
        return &mixer_instance;
    }
};

void scripts_declare_gunfx()
{
    SilentBackend backend;
    graphics::GraphicsService service{backend};
    testing::Harness harness;
    graphics::register_graphics_api(harness.registry(), harness.host(), service);
    const CapabilityMask client = testing::client_mod_capabilities();
    const auto result = harness.run(R"(
        gunfx.parts("ak", {
            {name = "mag", box = {min = vector.create(-0.32, 0.07, -0.15), max = vector.create(-0.2, 0.18, 0.03)}},
            {name = "mag_new", copy = "mag", hidden = true},
        })
        gunfx.parts("modern/rpg", {{name = "rocket", mesh = 1}})
        gunfx.events("GunReload", {
            {time = 0.4, sound = "sounds/mag_out", volume = 0.9, pitch_jitter = 0.04},
            {time = 0.5, attach = "mag", bone = "PropLeft"},
            {time = 0.8, drop = "mag", velocity = vector.create(0.2, -1, 0)},
        }, {mute = {"AssaultReload"}})
        gunfx.motion({sway = 1.2, families = {LMG = {weight = 1.6}}})
    )",
                                    Side::client, client);
    check(result.ok, "the script runs: " + result.error);
    const gunfx::Library &library = service.gunfx();
    check(library.parts("modern", "ak") != nullptr && library.parts("modern", "ak")->size() == 2U, "parts");
    check(library.parts("modern", "rpg") != nullptr && library.parts("classic", "rpg") == nullptr,
          "a generation's parts");
    const gunfx::ClipCues *cues = library.cues("GunReload", "ak");
    check(cues != nullptr && cues->mod_id == "test" && cues->cues.size() == 3U, "cues belong to the mod");
    check(library.mutes("GunReload", "ak", false, "AssaultReload"), "mute");
    check(library.motion() != nullptr &&
              near(library.motion()->tuning(gunfx::Family::lmg).weight, 1.6F),
          "motion");
    const auto refused = harness.run(R"(gunfx.parts("AK-47", {}))", Side::client, client);
    check(!refused.ok && refused.error.find("model name") != std::string::npos, "a bad model name: " + refused.error);
    const auto bad_cue = harness.run(R"(gunfx.events("GunReload", {{time = 1}}))", Side::client, client);
    check(!bad_cue.ok && bad_cue.error.find("exactly one") != std::string::npos, "a cue without an action");
    const auto *spec = harness.registry().find_function("gunfx.events");
    check(spec != nullptr && spec->spec.sides == side_client && spec->spec.requires_caps == capability::presentation,
          "client side, presentation capability");
    // The VM going away takes the definitions with it.
    service.release_mod("test");
    check(service.gunfx().empty(), "released with the mod");
    // Camera kicks are presentation too: none with reduced motion.
    service.camera().kick(2.0F, 1.0F, 0.2F);
    service.camera().step(0.05F);
    const graphics::CameraOutput kicked = service.camera().output({false, 0.0F});
    const graphics::CameraOutput reduced = service.camera().output({true, 1.0F});
    check(kicked.pitch > 0.01F && kicked.yaw < 0.0F, "a kick up and to the left, not scaled by explosion shake");
    check(reduced.pitch == 0.0F && reduced.yaw == 0.0F, "no kick with reduced motion");
    service.camera().step(0.2F);
    check(!service.camera().active(), "the kick is over");
}

struct TestCase final
{
    std::string_view name;
    void (*run)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"parts_parse_and_refuse", parts_parse_and_refuse},
        {"cues_parse_and_refuse", cues_parse_and_refuse},
        {"motion_parse", motion_parse},
        {"names_and_variants", names_and_variants},
        {"library_layers_mods", library_layers_mods},
        {"cut_parts_by_boxes_islands_meshes", cut_parts_by_boxes_islands_meshes},
        {"part_motion_turns_about_the_pivot", part_motion_turns_about_the_pivot},
        {"part_states_are_a_function_of_time", part_states_are_a_function_of_time},
        {"cue_ticks_follow_the_clip", cue_ticks_follow_the_clip},
        {"cues_fire_once_at_any_frame_rate", cues_fire_once_at_any_frame_rate},
        {"cursor_knows_ends_cuts_restarts", cursor_knows_ends_cuts_restarts},
        {"springs_are_exact", springs_are_exact},
        {"motion_is_frame_rate_independent", motion_is_frame_rate_independent},
        {"reload_clips_stretch_and_carry_parts", reload_clips_stretch_and_carry_parts},
        {"scripts_declare_gunfx", scripts_declare_gunfx},
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
