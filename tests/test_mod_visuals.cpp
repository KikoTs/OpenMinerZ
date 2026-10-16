// Modding visuals (wave 2): what makes server-mod content visible on clients.
// Creature animation (state -> clip, cross-fades, blended poses), mod blocks on
// the terrain (the overlay pass over the retail mesher, with a mesh digest that
// proves retail faces are untouched), and effects spawned before the first
// frame. Everything runs in memory; no device is needed.

#include "openminerz/modding/graphics.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
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

void check_near(float actual, float expected, float tolerance, const std::string &message)
{
    if (!(std::fabs(actual - expected) <= tolerance))
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " +
                                 std::to_string(actual));
}

struct TestCase final
{
    const char *name;
    std::function<void()> body;
};

// A model of one node with a clip per name; each clip holds the node at its
// own x (clip i at x = 2 * i) for one second, and turns it by `turn` degrees
// about Y when given.
graphics::Model clip_model(const std::vector<std::string> &names,
                           const std::vector<float> &turns = {})
{
    graphics::Model model;
    model.nodes.push_back({"body", -1, {}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}});
    model.node_order = {0};
    for (std::size_t index = 0U; index < names.size(); ++index)
    {
        graphics::ModelAnimation clip;
        clip.name = names[index];
        clip.duration = 1.0F;
        graphics::AnimationChannel move;
        move.node = 0;
        move.path = graphics::AnimationChannel::Path::translation;
        move.times = {0.0F, 1.0F};
        const float x = 2.0F * static_cast<float>(index);
        move.values = {x, 0.0F, 0.0F, x, 0.0F, 0.0F};
        clip.channels.push_back(move);
        if (index < turns.size())
        {
            graphics::AnimationChannel turn;
            turn.node = 0;
            turn.path = graphics::AnimationChannel::Path::rotation;
            turn.times = {0.0F, 1.0F};
            const float half = turns[index] * 3.14159265F / 360.0F;
            turn.values = {0.0F, std::sin(half), 0.0F, std::cos(half),
                           0.0F, std::sin(half), 0.0F, std::cos(half)};
            clip.channels.push_back(turn);
        }
        model.animations.push_back(clip);
    }
    return model;
}

float node_x(const graphics::Model &model, std::span<const graphics::PoseLayer> layers)
{
    std::vector<graphics::Matrix4> world;
    graphics::evaluate_blended_pose(model, layers, world);
    return world.at(0)[12];
}

// --- Creatures -----------------------------------------------------------------

void state_clips_follow_names_and_stand_ins()
{
    const graphics::Model model =
        clip_model({"Armature|Idle", "walk_cycle", "Attack01", "Die", "Run"});
    check(graphics::find_state_clip(model, "idle") == 0, "idle by the exporter's name");
    check(graphics::find_state_clip(model, "walk") == 1, "walk inside walk_cycle");
    check(graphics::find_state_clip(model, "run") == 4, "run by its own clip first");
    check(graphics::find_state_clip(model, "attack") == 2, "attack inside Attack01");
    check(graphics::find_state_clip(model, "death") == 3, "death stands in as die");
    check(graphics::find_state_clip(model, "hurt") == -1, "no hurt clip: the bind pose");
    check(graphics::find_state_clip(model, "WALK") == 1, "any case");
    check(graphics::find_state_clip(model, "dance") == -1, "an unknown state has no clip");

    const graphics::Model walker = clip_model({"Walk"});
    check(graphics::find_state_clip(walker, "run") == 0, "run stands in as walk");
    check(graphics::find_state_clip(walker, "idle") == -1, "idle does not borrow a walk");
    const graphics::Model none;
    check(graphics::find_state_clip(none, "idle") == -1, "a model without clips");

    check(graphics::state_loops("idle") && graphics::state_loops("walk") &&
              graphics::state_loops("my_dance"),
          "idle, walk and mod states loop");
    check(!graphics::state_loops("attack") && !graphics::state_loops("hurt") &&
              !graphics::state_loops("death"),
          "attack, hurt and death play once");
}

void animator_cross_fades_between_states()
{
    const graphics::Model model = clip_model({"idle", "walk", "attack"});
    graphics::CreatureAnimator animator;
    check(animator.finished(), "nothing playing counts as ended");
    animator.play(model, "idle");
    animator.step(0.5F);
    check(animator.layers().size() == 1U, "one clip");
    check_near(node_x(model, animator.layers()), 0.0F, 1e-5F, "idle holds x = 0");

    animator.play(model, "walk");
    check(animator.layers().size() == 2U, "a fade has two layers");
    animator.step(graphics::CreatureAnimator::blend_seconds * 0.5F);
    check_near(node_x(model, animator.layers()), 1.0F, 1e-4F, "halfway through the fade");
    animator.play(model, "walk"); // the same state again changes nothing
    animator.step(graphics::CreatureAnimator::blend_seconds);
    check(animator.layers().size() == 1U, "the fade ends");
    check_near(node_x(model, animator.layers()), 2.0F, 1e-5F, "walk holds x = 2");

    // run has no clip of its own here and stands in as walk: no restart.
    animator.play(model, "run");
    check(animator.layers().size() == 1U, "the same looping clip carries on");

    animator.play(model, "attack");
    animator.step(0.3F);
    check(!animator.finished(), "attack still playing");
    animator.step(2.0F);
    check(animator.finished(), "attack ended and holds");
    check_near(animator.layers()[0].seconds, 1.0F, 1e-5F, "held at its last frame");

    // A different model starts over without a fade.
    const graphics::Model other = clip_model({"idle"});
    animator.play(other, "attack");
    check(animator.layers().size() == 1U, "a new model does not fade");
}

// (fix, wave 5) Creatures spawned together do not idle in lockstep.
void each_creature_keeps_its_own_rhythm()
{
    const graphics::Model model = clip_model({"idle", "walk", "attack"});
    // Without vary() a clip starts at 0 and plays at its own speed, as before.
    graphics::CreatureAnimator plain;
    plain.play(model, "idle");
    plain.step(0.25F);
    check_near(plain.layers()[0].seconds, 0.25F, 1e-6F, "an animator nobody varied keeps time");
    std::vector<float> phases;
    for (std::uint64_t id = 1U; id <= 8U; ++id)
    {
        graphics::CreatureAnimator animator;
        animator.vary(id);
        animator.play(model, "idle");
        phases.push_back(animator.layers()[0].seconds);
        const float start = phases.back();
        animator.step(1.0F / 8.0F);
        const float advanced = std::fmod(animator.layers()[0].seconds - start + 1.0F, 1.0F);
        check(advanced > 0.92F / 8.0F - 1e-4F && advanced < 1.08F / 8.0F + 1e-4F,
              "a creature's pace stays within 8 % of the clip's");
        // Clips that play once keep their timing.
        animator.play(model, "attack");
        check_near(animator.layers()[0].seconds, 0.0F, 1e-6F, "an attack starts at its start");
        animator.step(0.5F);
        check_near(animator.layers()[0].seconds, 0.5F, 1e-6F, "and plays at its own speed");
    }
    std::sort(phases.begin(), phases.end());
    check(phases.back() - phases.front() > 0.4F, "eight creatures start their idle at different phases");
    graphics::CreatureAnimator again;
    again.vary(3U);
    again.play(model, "idle");
    graphics::CreatureAnimator same;
    same.vary(3U);
    same.play(model, "idle");
    check_near(again.layers()[0].seconds, same.layers()[0].seconds, 0.0F, "one id, one phase");
}

void blended_poses_weigh_each_node()
{
    const graphics::Model model = clip_model({"a", "b"}, {0.0F, 90.0F});
    std::vector<graphics::Matrix4> single;
    std::vector<graphics::Matrix4> blended;
    graphics::evaluate_pose(model, 1, 0.25F, true, single);
    const std::array<graphics::PoseLayer, 2> only_b{{{1, 0.25F, true, 1.0F}, {0, 0.5F, true, 0.0F}}};
    graphics::evaluate_blended_pose(model, only_b, blended);
    for (std::size_t element = 0U; element < 16U; ++element)
        check_near(blended[0][element], single[0][element], 1e-5F, "a zero weight changes nothing");

    const std::array<graphics::PoseLayer, 2> half{{{0, 0.0F, true, 0.5F}, {1, 0.0F, true, 0.5F}}};
    graphics::evaluate_blended_pose(model, half, blended);
    check_near(blended[0][12], 1.0F, 1e-5F, "translations average");
    // Halfway between no turn and 90 degrees is 45: +X turns towards -Z.
    const graphics::Vec3 turned = graphics::transform_direction(blended[0], {1.0F, 0.0F, 0.0F});
    check_near(turned.x, std::sqrt(0.5F), 1e-4F, "rotations blend along the arc (x)");
    check_near(turned.z, -std::sqrt(0.5F), 1e-4F, "rotations blend along the arc (z)");

    const std::array<graphics::PoseLayer, 1> none{{{-1, 0.0F, true, 1.0F}}};
    graphics::evaluate_blended_pose(model, none, blended);
    check_near(blended[0][12], 0.0F, 1e-6F, "no clip is the bind pose");
}

// --- Mod blocks ----------------------------------------------------------------

// FNV-1a over every field of every face, in order: the mesh a section draws.
std::uint64_t digest(const std::vector<world::ChunkFaceBatch> &batches)
{
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t index = 0U; index < size; ++index)
            hash = (hash ^ bytes[index]) * 0x100000001B3ULL;
    };
    for (const world::ChunkFaceBatch &batch : batches)
    {
        mix(&batch.coordinate, sizeof(batch.coordinate));
        for (const world::WorldFace &face : batch.faces)
        {
            const std::array<int, 5> header{face.x, face.y, face.z, static_cast<int>(face.direction),
                                            static_cast<int>(face.block)};
            mix(header.data(), sizeof(header));
            mix(&face.tile, 1U);
            mix(face.ambient_occlusion.data(), 4U);
            mix(face.sky_light.data(), 4U);
            mix(face.block_light.data(), 4U);
            mix(face.indirect_light.data(), 4U);
            mix(face.block_light_lobe.data(), 4U);
            mix(&face.emission, 1U);
            mix(face.lava_light.data(), 4U);
        }
    }
    return hash;
}

std::uint64_t digest_except_tiles(std::vector<world::ChunkFaceBatch> batches)
{
    for (world::ChunkFaceBatch &batch : batches)
        for (world::WorldFace &face : batch.faces)
            face.tile = 0U;
    return digest(batches);
}

void mod_blocks_change_only_their_own_faces()
{
    const world::WorldGenerator generator{1234, world::TerrainProfile::classic};
    const world::WorldRegion region = generator.generate_region(0, 0, 2, 2);
    const std::vector<world::ChunkFaceBatch> retail = region.visible_faces_by_chunk();
    check(!retail.empty(), "the region has faces");
    const std::uint64_t retail_digest = digest(retail);

    // An empty overlay, one far away, and one whose fallbacks do not match
    // what the cells hold: every retail face is exactly as the mesher made it.
    graphics::ModBlockOverlay far;
    far.set(1000, 10, 1000, {5U, {70U, 71U, 72U}, {}});
    graphics::ModBlockOverlay mismatched;
    // Six cells of the commonest block with faces stand in for mod blocks.
    std::map<world::BlockType, int> counts;
    for (const world::ChunkFaceBatch &batch : retail)
        for (const world::WorldFace &face : batch.faces)
            ++counts[face.block];
    const world::BlockType host =
        std::max_element(counts.begin(), counts.end(), [](const auto &a, const auto &b) {
            return a.second < b.second;
        })->first;
    const world::BlockType other =
        host == world::BlockType::wood ? world::BlockType::sand : world::BlockType::wood;
    std::set<std::tuple<int, int, int>> chosen;
    for (const world::ChunkFaceBatch &batch : retail)
        for (const world::WorldFace &face : batch.faces)
            if (face.block == host && chosen.size() < 6U)
                chosen.insert({face.x, face.y, face.z});
    check(chosen.size() == 6U, "faces found");
    for (const auto &[x, y, z] : chosen)
        mismatched.set(x, y, z, {static_cast<std::uint8_t>(other), {70U, 71U, 72U}, {}});
    const std::array<const graphics::ModBlockOverlay *, 3> unchanged{nullptr, &far, &mismatched};
    for (const graphics::ModBlockOverlay *overlay : unchanged)
    {
        std::vector<world::ChunkFaceBatch> batches = retail;
        std::size_t changed{};
        if (overlay != nullptr)
            for (world::ChunkFaceBatch &batch : batches)
                changed += overlay->apply(batch);
        check(changed == 0U, "no face changed");
        check(digest(batches) == retail_digest, "the retail mesh is byte-identical");
    }
    graphics::ModBlockOverlay empty;
    {
        std::vector<world::ChunkFaceBatch> batches = retail;
        for (world::ChunkFaceBatch &batch : batches)
            check(empty.apply(batch) == 0U, "an empty overlay changes nothing");
        check(digest(batches) == retail_digest, "empty overlay: byte-identical");
    }

    // Real mod blocks over rock: their faces, and only their tiles, change.
    graphics::ModBlockOverlay mod;
    for (const auto &[x, y, z] : chosen)
        mod.set(x, y, z, {static_cast<std::uint8_t>(host), {70U, 71U, 72U}, {}});
    std::vector<world::ChunkFaceBatch> batches = retail;
    std::size_t changed{};
    for (world::ChunkFaceBatch &batch : batches)
        changed += mod.apply(batch);
    check(changed > 0U, "mod block faces changed");
    check(digest(batches) != retail_digest, "the mod blocks show");
    check(digest_except_tiles(batches) == digest_except_tiles(retail),
          "nothing but the tile of a face changes");
    std::size_t counted{};
    for (std::size_t batch = 0U; batch < batches.size(); ++batch)
        for (std::size_t face = 0U; face < batches[batch].faces.size(); ++face)
        {
            const world::WorldFace &after = batches[batch].faces[face];
            const world::WorldFace &before = retail[batch].faces[face];
            const bool modded = chosen.contains({after.x, after.y, after.z});
            if (!modded)
            {
                check(after.tile == before.tile, "a retail face keeps its tile");
                continue;
            }
            ++counted;
            const std::uint8_t expected =
                after.direction == world::FaceDirection::positive_y   ? 70U
                : after.direction == world::FaceDirection::negative_y ? 72U
                                                                      : 71U;
            check(after.tile == expected, "top, side and bottom tiles");
        }
    check(counted == changed, "every changed face belongs to a mod block");

    // The sections to mesh again are exactly those holding a changed cell.
    const std::vector<world::ChunkCoordinate> sections =
        graphics::ModBlockOverlay::changed_sections(nullptr, &mod);
    std::set<std::tuple<int, int, int>> expected_sections;
    for (const auto &[x, y, z] : chosen)
        expected_sections.insert({x >= 0 ? x / 16 : -((-x + 15) / 16), z >= 0 ? z / 16 : -((-z + 15) / 16),
                                  (y - world::world_min_y) / world::chunk_section_height});
    check(sections.size() == expected_sections.size(), "one entry per section");
    for (const world::ChunkCoordinate &section : sections)
        check(expected_sections.contains({section.x, section.z, section.y}), "a section holding a mod block");
    check(graphics::ModBlockOverlay::changed_sections(&mod, &mod).empty(), "no change, no remesh");
    graphics::ModBlockOverlay retextured = mod;
    const auto &[x0, y0, z0] = *chosen.begin();
    retextured.set(x0, y0, z0, {static_cast<std::uint8_t>(host), {80U, 81U, 82U}, {}});
    check(graphics::ModBlockOverlay::changed_sections(&mod, &retextured).size() == 1U,
          "a retextured cell remeshes its own section");
    check(graphics::ModBlockOverlay::changed_sections(&mod, nullptr).size() == sections.size(),
          "removing the overlay remeshes the same sections");
    check(mod.find(x0, y0, z0, host) != nullptr &&
              mod.find(x0, y0, z0, other) == nullptr,
          "a cell shows its mod block only over its fallback");
    check(graphics::mod_block_face_slot(world::FaceDirection::positive_y) == 0U &&
              graphics::mod_block_face_slot(world::FaceDirection::negative_z) == 1U &&
              graphics::mod_block_face_slot(world::FaceDirection::negative_y) == 2U,
          "face slots");
}

// --- Effects before the first frame ------------------------------------------------

class PendingBackend final : public graphics::GraphicsBackend
{
  public:
    bool available{};
    bool pending{true};
    int spawned{};
    std::vector<graphics::Vec3> positions;

    [[nodiscard]] bool locate(Handle target, graphics::Vec3 &position, float &yaw) const override
    {
        if (target.kind != HandleKind::entity || target.id != 7U)
            return false;
        position = {10.0F, 2.0F, 3.0F};
        yaw = 0.0F;
        return true;
    }
    [[nodiscard]] bool effects_available() const override
    {
        return available;
    }
    [[nodiscard]] bool effects_pending() const override
    {
        return pending;
    }
    vfx::EffectHandle spawn_effect(const vfx::EffectDesc &, const vfx::SpawnParams &params) override
    {
        ++spawned;
        positions.push_back(params.position);
        return {static_cast<std::uint16_t>(spawned), 1U};
    }
    [[nodiscard]] bool effect_alive(vfx::EffectHandle) const override
    {
        return true;
    }
    void move_effect(vfx::EffectHandle, graphics::Vec3, graphics::Vec3) override
    {
    }
    void kill_effect(vfx::EffectHandle, bool) override
    {
    }
    [[nodiscard]] graphics::Mixer *mixer(bool) override
    {
        return nullptr;
    }
};

// (Polish) Chips of a mod block wear its own tiles, once the debris can draw
// them; everything else keeps the retail tile.
void chips_wear_mod_block_tiles()
{
    graphics::ModBlockOverlay overlay;
    overlay.set(3, 20, -5, {static_cast<std::uint8_t>(world::BlockType::rock), {70U, 71U, 72U}, {}});
    const auto tile = [&](world::BlockType block, world::FaceDirection face, bool mod_tiles,
                          const graphics::ModBlockOverlay *over, int x = 3) {
        return graphics::debris_tile(over, x, 20, -5, block, face, mod_tiles);
    };
    using world::BlockType;
    using world::FaceDirection;
    check(tile(BlockType::rock, FaceDirection::positive_y, true, &overlay) == 70U, "top");
    check(tile(BlockType::rock, FaceDirection::negative_x, true, &overlay) == 71U, "side");
    check(tile(BlockType::rock, FaceDirection::negative_y, true, &overlay) == 72U, "bottom");
    for (const FaceDirection face : {FaceDirection::positive_y, FaceDirection::positive_x,
                                     FaceDirection::negative_y})
    {
        const std::uint8_t retail = world::atlas_tile(BlockType::rock, face);
        check(tile(BlockType::rock, face, false, &overlay) == retail,
              "without a debris atlas the retail tile");
        check(tile(BlockType::rock, face, true, nullptr) == retail, "without an overlay too");
        check(tile(BlockType::rock, face, true, &overlay, 4) == retail, "a cell without one");
        check(tile(BlockType::sand, face, true, &overlay) == world::atlas_tile(BlockType::sand, face),
              "a cell that no longer holds the fallback");
    }
}

void effects_wait_for_the_first_frame()
{
    PendingBackend backend;
    graphics::GraphicsService service{backend};
    vfx::SpawnParams params;
    params.position = {1.0F, 2.0F, 3.0F};
    const Handle early = service.spawn_effect("mod", "torch_flame", params, std::nullopt);
    const Handle attached =
        service.spawn_effect("mod", "torch_flame", {}, graphics::Attachment{{HandleKind::entity, 7U}, {0.0F, 1.0F, 0.0F}});
    const Handle stopped = service.spawn_effect("mod", "torch_flame", params, std::nullopt);
    check(early.valid() && attached.valid() && stopped.valid(), "handles before the first frame");
    check(backend.spawned == 0 && service.pending_effects() == 3U, "they wait");
    check(service.stop_effect(stopped, true), "a waiting effect can be stopped");
    check(!service.idle(), "waiting effects keep the service busy");
    service.step(0.016F, {});
    check(backend.spawned == 0, "still no world");
    backend.available = true;
    backend.pending = false;
    service.step(0.016F, {});
    check(backend.spawned == 2 && service.pending_effects() == 0U, "spawned with the first frame");
    check_near(backend.positions[1].y, 3.0F, 1e-6F, "the attachment is followed");
    check(service.stop_effect(early, false), "the handle stays good once spawned");

    // A presentation without effects refuses at once, as before, and drops
    // whatever was waiting when it turns out effects never come.
    backend.available = false;
    check(!service.spawn_effect("mod", "torch_flame", params, std::nullopt).valid(),
          "no effects layer: nil");
    backend.pending = true;
    (void)service.spawn_effect("mod", "torch_flame", params, std::nullopt);
    backend.pending = false;
    service.step(0.016F, {});
    check(service.pending_effects() == 0U && backend.spawned == 2, "dropped, never spawned");
    backend.pending = true;
    (void)service.spawn_effect("mod", "torch_flame", params, std::nullopt);
    for (int frame = 0; frame < 700; ++frame)
        service.step(0.016F, {});
    check(service.pending_effects() == 0U, "an effect waits ten seconds at most");
    service.release_mod("mod");
}

void creatures_are_found_by_id()
{
    PendingBackend backend;
    graphics::GraphicsService service{backend};
    check(service.idle(), "idle without creatures");
    graphics::CreatureView view;
    view.id = 42U;
    view.position = {1.0F, 2.0F, 3.0F};
    service.creatures().push_back(view);
    check(!service.idle(), "creatures keep the service busy");
    check(service.creature(42U) != nullptr && service.creature(42U)->position.z == 3.0F, "by id");
    check(service.creature(43U) == nullptr, "absent");
    service.reset();
    check(service.creatures().empty() && service.idle(), "reset drops them");
}

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"state_clips_follow_names_and_stand_ins", state_clips_follow_names_and_stand_ins},
        {"animator_cross_fades_between_states", animator_cross_fades_between_states},
        {"each_creature_keeps_its_own_rhythm", each_creature_keeps_its_own_rhythm},
        {"blended_poses_weigh_each_node", blended_poses_weigh_each_node},
        {"mod_blocks_change_only_their_own_faces", mod_blocks_change_only_their_own_faces},
        {"chips_wear_mod_block_tiles", chips_wear_mod_block_tiles},
        {"effects_wait_for_the_first_frame", effects_wait_for_the_first_frame},
        {"creatures_are_found_by_id", creatures_are_found_by_id},
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " passed\n";
    return failures == 0U ? 0 : 1;
}
