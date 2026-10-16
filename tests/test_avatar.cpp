#include "avatar_viewmodel.hpp"
#include "xbox_avatar_fixture.hpp"
#include "openminerz/asset_paths.hpp"
#include "openminerz/retail_character_driver.hpp"
#include "openminerz/retail_rig.hpp"
#include "openminerz/world/player_animation.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void expect(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error{message};
}

float maximum_y(const openminerz::EntityBatch &batch)
{
    float result = -1000.0F;
    for (const auto &vertex : batch.vertices)
        result = std::max(result, vertex.y);
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    using namespace openminerz;
    set_active_asset_style(argc > 1 && std::string_view{argv[1]} == "classic" ? AssetStyle::classic
                                                                              : AssetStyle::modern);
    if (!std::filesystem::exists(default_character_rig_path()))
    {
        std::cout << "SKIP: the retail character has not been imported into this content pack\n";
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 1;
    SDL_Window *window = SDL_CreateWindow("Avatar parity tests", 64, 64, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = window == nullptr ? nullptr : SDL_CreateRenderer(window, "software");
    int result = 0;
    try
    {
        expect(renderer != nullptr, "software renderer unavailable");
        AvatarViewmodel avatar{renderer, default_avatar_path()};
        expect(avatar.retail_character(), "the default character must be the retail rig");
        expect(avatar.world_animation_ready(), "owned locomotion/death clips unavailable");
        // The retail body, exactly as imported: one SWATMale mesh on its own
        // 71-bone skeleton, every clip the game registers, no retargeting.
        const rig::RetailRig retail{default_character_rig_path()};
        expect(retail.body.size() == 71U && retail.parts.size() == 1U,
               "the retail body is one mesh on 71 bones");
        expect(retail.body_asset.find("SWATMale") != std::string::npos,
               "the retail body is Character/SWATMale");
        expect(avatar.animation_clip_count() == retail.registrations.size() &&
                   retail.registrations.size() >= 106U,
               "every registered clip must be available");
        expect(avatar.triangle_count() == retail.triangle_count(), "the whole body is drawn");
        world::SkyState sky;
        sky.ambient = {1.0F, 1.0F, 1.0F};
        const auto draw = [&](float time, bool dead,
                              InventoryItemId item = InventoryItemId::bare_hands, float sun = 1.0F,
                              bool hdr = false, float yaw = 0.0F, bool shouldering = false) {
            EntityBatch batch;
            avatar.append_world_player(batch, 1U, item, {}, yaw, 0.0F, time, 0.0F, 0.0F,
                                       item_player_animation_mode(item), false, dead, shouldering,
                                       false, false, false, sun, 0.0F, nullptr, sky, hdr);
            expect(!batch.vertices.empty(), "avatar geometry is empty");
            for (const auto &v : batch.vertices)
                expect(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                       "animation emitted non-finite geometry");
            return batch;
        };
        const auto standing = draw(0.0F, false);
        // The body span comes first: its vertices are the mesh's index list, with
        // texture coordinates untouched.
        const rig::MeshPart &mesh = retail.parts.front();
        expect(standing.spans.front().count == mesh.indices.size(), "the body span is the whole mesh");
        for (std::size_t i = 0; i < mesh.indices.size(); i += 97U)
            expect(standing.vertices[i].u == mesh.vertices[mesh.indices[i]].uv[0] &&
                       standing.vertices[i].v == mesh.vertices[mesh.indices[i]].uv[1],
                   "mesh texture coordinates must reach the rasterizer unchanged");
        // Scale and placement: feet on the ground, a 1.4 m avatar-sized figure,
        // centred on the player, facing -Z at heading zero.
        float bottom = 1000.0F, left = 1000.0F, right = -1000.0F;
        for (const auto &v : standing.vertices)
        {
            bottom = std::min(bottom, v.y);
            left = std::min(left, v.x);
            right = std::max(right, v.x);
        }
        std::cout << "stand.bottom=" << bottom << " stand.top=" << maximum_y(standing) << " stand.x=" << left
                  << ".." << right << std::endl;
        expect(std::abs(bottom) < 0.03F, "the feet must stand on the ground, not under it");
        expect(maximum_y(standing) > 1.30F && maximum_y(standing) < 1.60F,
               "the body must keep the retail scale");
        expect(std::abs(left + right) < 0.12F, "the body must be centred on the player");
        expect(standing.spans.front().repeat_texture, "avatar meshes must use a wrapping sampler");
        draw(0.0F, true);
        EntityBatch fallen;
        for (int frame = 1; frame <= 120; ++frame)
            fallen = draw(static_cast<float>(frame) / 60.0F, true);
        std::cout << "stand.top=" << maximum_y(standing) << " faint.top=" << maximum_y(fallen)
                  << '\n';
        expect(maximum_y(fallen) < maximum_y(standing) - 0.7F,
               "Faint must lower the body through authored root motion");
        const auto switched = draw(2.0F, true, InventoryItemId::iron_pickaxe);
        expect(switched.vertices.size() >= fallen.vertices.size(),
               "held item must retain body geometry");
        for (std::size_t i = 0; i < fallen.vertices.size(); ++i)
            expect(std::abs(switched.vertices[i].y - fallen.vertices[i].y) < 0.0001F,
                   "equipping an item must not restart the death layer");
        draw(2.1F, false);
        for (int frame = 1; frame <= 20; ++frame)
            fallen = draw(2.1F + static_cast<float>(frame) / 60.0F, false);
        expect(maximum_y(fallen) > maximum_y(standing) - 0.1F,
               "respawn must clear the death layer");
        avatar.clear_world_players();
        const auto local_eye = [&](float seconds, world::Vec3 feet = {}, float yaw = 0.0F) {
            EntityBatch body;
            world::Vec3 eye{};
            world::Vec3 facing{};
            avatar.append_world_player(body, 256U, InventoryItemId::bare_hands, feet, yaw, 0.0F,
                                       seconds, 0.0F, 0.0F, PlayerAnimationMode::generic, false,
                                       true, false, false, false, false, 1.0F, 0.0F, nullptr, sky,
                                       false, false, true, &eye, &facing);
            expect(!body.vertices.empty(), "local death body must be present");
            expect(std::isfinite(eye.x) && std::isfinite(eye.y) && std::isfinite(eye.z),
                   "death camera must remain finite");
            const float facing_length = std::sqrt(facing.x*facing.x + facing.y*facing.y + facing.z*facing.z);
            expect(std::isfinite(facing_length) && std::abs(facing_length-1.0F) < 0.002F,
                   "death camera must receive the animated head's viewing direction");
            return eye;
        };
        const auto upright_eye = local_eye(0.0F);
        auto previous_eye = upright_eye;
        for (int frame = 1; frame <= 180; ++frame)
        {
            const auto eye = local_eye(static_cast<float>(frame) / 60.0F);
            expect(std::abs(eye.y - previous_eye.y) < 0.2F,
                   "death camera must follow the fall without discontinuities");
            previous_eye = eye;
        }
        const auto settled_eye = local_eye(30.0F);
        std::cout << "death.eye=" << upright_eye.x << ',' << upright_eye.y << ',' << upright_eye.z
                  << " -> " << settled_eye.x << ',' << settled_eye.y << ',' << settled_eye.z
                  << '\n';
        expect(settled_eye.y < upright_eye.y - 0.7F,
               "death camera must descend with the animated head, not stay at standing height");
        expect(std::abs(settled_eye.y - previous_eye.y) < 0.001F &&
                   std::abs(settled_eye.z - previous_eye.z) < 0.001F,
               "death camera must hold the final pose until respawn");
        const auto moved_eye = local_eye(30.0F, {10.0F, 20.0F, 30.0F}, 1.570796327F);
        expect(std::abs(moved_eye.y - 20.0F - settled_eye.y) < 0.001F &&
                   std::abs(moved_eye.x - 10.0F + settled_eye.z) < 0.001F &&
                   std::abs(moved_eye.z - 30.0F - settled_eye.x) < 0.001F,
               "death camera and body must share world translation and yaw");
        avatar.forget_world_player(256U);
        const auto next_life = local_eye(0.0F);
        expect(std::abs(next_life.y - upright_eye.y) < 0.001F,
               "a new death must not reuse the previous life's fallen camera");
        avatar.clear_world_players();
        const auto darkness = draw(0.0F, false, InventoryItemId::bare_hands, 0.0F);
        expect(std::ranges::all_of(darkness.vertices,
                                   [](const auto &v) {
                                       return v.red == 0.0F && v.green == 0.0F && v.blue == 0.0F;
                                   }),
               "Classic avatars must not be fullbright in an unlit room");
        avatar.clear_world_players();
        const auto hdr = draw(0.0F, false, InventoryItemId::bare_hands, 1.0F, true);
        for (std::size_t i = 0; i < standing.vertices.size(); ++i)
            expect(std::abs(hdr.vertices[i].red -
                            standing.vertices[i].red * standing.vertices[i].red) < 0.0001F,
                   "HDR palette albedo must be linear before tone mapping");
        avatar.clear_world_players();
        const auto turned =
            draw(0.0F, false, InventoryItemId::bare_hands, 1.0F, false, 1.570796327F);
        for (std::size_t i = 0; i < standing.vertices.size(); ++i)
            expect(std::abs(turned.vertices[i].x + standing.vertices[i].z) < 0.0001F &&
                       std::abs(turned.vertices[i].z - standing.vertices[i].x) < 0.0001F,
                   "avatar model yaw must match the native world/camera basis");
        avatar.clear_world_players();
        EntityBatch aimed;
        for (int frame = 0; frame <= 120; ++frame)
            aimed = draw(static_cast<float>(frame) / 60.0F, false, InventoryItemId::assault_rifle,
                         1.0F, false, 0.0F, true);
        expect(aimed.vertices.size() > standing.vertices.size(), "rifle model must be attached");
        float min_x = 100.0F, max_x = -100.0F, min_y = 100.0F, max_y = -100.0F;
        float min_z = 100.0F, max_z = -100.0F;
        for (std::size_t i = standing.vertices.size(); i < aimed.vertices.size(); ++i)
        {
            const auto &v = aimed.vertices[i];
            min_x = std::min(min_x, v.x);
            max_x = std::max(max_x, v.x);
            min_y = std::min(min_y, v.y);
            max_y = std::max(max_y, v.y);
            min_z = std::min(min_z, v.z);
            max_z = std::max(max_z, v.z);
        }
        std::cout << "aimed.rifle.extent=" << max_x - min_x << ',' << max_y - min_y << ','
                  << max_z - min_z << '\n';
        std::cout << "aimed.rifle.z=" << min_z << ',' << max_z << '\n';
        expect(max_z - min_z > 1.5F * (max_y - min_y) && max_z - min_z > 1.5F * (max_x - min_x),
               "a shouldered rifle must align with the player's forward axis");
        expect(min_z < -0.4F && max_z < 0.25F, "the shouldered rifle must extend toward the native "
                                               "forward direction, not behind the player");
        // Every clip the animation state machine can name in this generation is a
        // clip imported from the game, with the game's own length: none
        // substituted, none invented.
        const world::AnimationGeneration generation = retail.generation == "modern"
                                                          ? world::AnimationGeneration::modern
                                                          : world::AnimationGeneration::classic;
        std::size_t resolved = 0;
        for (int value = 1; value < static_cast<int>(world::AnimationClip::count); ++value)
        {
            const auto clip = static_cast<world::AnimationClip>(value);
            if (!world::animation_clip_registered(clip, generation))
                continue;
            const std::string name{world::animation_clip_info(clip).name};
            const rig::Registration *found = retail.find(name);
            if (found == nullptr)
                throw std::runtime_error{"the imported character lacks the registered clip " + name};
            if (retail.clips[static_cast<std::size_t>(found->clip)].duration_ticks !=
                world::animation_clip_ticks(clip, generation))
                throw std::runtime_error{"imported clip length differs from the game's for " + name};
            ++resolved;
        }
        expect(resolved == retail.registrations.size(),
               "the state machine and the imported character must list the same clips");

        // First person. The game hides exactly one thing - Avatar.HideHead scales
        // the head bone to a point - and draws the whole body from the head; the
        // legs stay out of the picture only because the body stands with a level
        // torso under the camera (FPSMode: Stand, no head sweep, Tilt at 0.5).
        const auto descends = [&](int bone, int ancestor) {
            for (; bone >= 0; bone = retail.attachment[static_cast<std::size_t>(bone)].parent)
                if (bone == ancestor)
                    return true;
            return false;
        };
        const rig::MeshPart &body_mesh = retail.parts.front();
        const auto skinned = [&](const rig::CharacterPose &pose, const rig::MeshVertex &vertex) {
            std::array<float, 3> position{};
            for (std::size_t influence = 0; influence < 4U; ++influence)
            {
                const auto moved =
                    rig::transform_point(vertex.position.data(), pose.skin()[vertex.bones[influence]]);
                for (std::size_t axis = 0; axis < 3U; ++axis)
                    position[axis] += moved[axis] * vertex.weights[influence];
            }
            return position;
        };
        const auto weight_under = [&](const rig::MeshVertex &vertex, int first, int second) {
            float weight = 0.0F;
            for (std::size_t influence = 0; influence < 4U; ++influence)
            {
                const int source = retail.body[vertex.bones[influence]].source;
                if (descends(source, first) || (second >= 0 && descends(source, second)))
                    weight += vertex.weights[influence];
            }
            return weight;
        };
        // Vertices of the legs inside the 90-degree first-person frustum (near
        // plane 0.01), tested at a 21:9 aspect so every display is covered.
        const auto legs_in_view = [&](const rig::CharacterPose &pose) {
            const rig::Matrix view = rig::invert(pose.eye());
            std::size_t count = 0;
            for (const rig::MeshVertex &vertex : body_mesh.vertices)
            {
                if (weight_under(vertex, 2, 3) < 0.5F) // HipLeft, HipRight
                    continue;
                const auto world_position = skinned(pose, vertex);
                const auto camera = rig::transform_point(world_position.data(), view);
                const float depth = -camera[2];
                if (depth > 0.01F && std::abs(camera[1]) <= depth && std::abs(camera[0]) <= depth * (21.0F / 9.0F))
                    ++count;
            }
            return count;
        };
        for (int mode = 0; mode <= static_cast<int>(PlayerAnimationMode::rpg); ++mode)
        {
            for (const float movement : {0.0F, 1.0F})
            {
                rig::CharacterPose first_person{retail};
                first_person.hide_head = true;
                rig::DriveState drive;
                rig::FirstPersonInput input;
                input.mode = static_cast<PlayerAnimationMode>(mode);
                input.movement = movement;
                for (int frame = 0; frame < 40; ++frame)
                {
                    rig::drive_first_person(first_person, drive, input);
                    first_person.update(166'667);
                    expect(legs_in_view(first_person) == 0U,
                           "a standing first-person body must keep its legs out of view");
                }
                // The head, and only the head, is gone: its vertices sit on the joint.
                const rig::Matrix &head = first_person.attachment()[rig::bone_head];
                float head_radius = 0.0F;
                float hand_radius = 0.0F;
                const rig::Matrix &hand = first_person.right_hand();
                for (const rig::MeshVertex &vertex : body_mesh.vertices)
                {
                    const auto p = skinned(first_person, vertex);
                    if (weight_under(vertex, rig::bone_head, -1) > 0.99F)
                        head_radius = std::max(head_radius, std::hypot(p[0] - head[12], p[1] - head[13],
                                                                       p[2] - head[14]));
                    if (weight_under(vertex, 36, -1) > 0.99F) // WristRight
                        hand_radius = std::max(hand_radius, std::hypot(p[0] - hand[12], p[1] - hand[13],
                                                                       p[2] - hand[14]));
                }
                expect(head_radius < 0.01F, "first person must collapse the head onto its joint");
                expect(hand_radius > 0.04F && hand_radius < 0.30F,
                       "first person must keep the hands, full size, around the held item");
            }
        }
        {
            // What a first-person frame looks like when the paused Tilt clip is
            // left where SetupAnimation put it (progress 0, a torso bent to the
            // ground): the legs come into view. The state machine must never be
            // sampled in that state.
            rig::CharacterPose bent{retail};
            bent.hide_head = true;
            bent.animations.play("Stand", 0, 0.0);
            if (rig::ClipPlayer *tilt = bent.animations.play("Tilt", 1, 0.0))
                tilt->pause();
            bent.update(0);
            bent.update(0);
            expect(legs_in_view(bent) > 0U, "a torso bent to the ground does show the legs");
        }
        {
            // An Xbox avatar worn as a skin: a different mesh on the same pose,
            // switched without a frame in between, and gone without a trace.
            // The avatar is synthetic (tests/xbox_avatar_fixture.hpp).
            std::string error;
            const auto skin = xbox_avatar::load(test::build_synthetic_avatar(), test::decode_stored_png, error);
            expect(skin != nullptr, "the synthetic avatar must load");
            test::SyntheticAvatar taller;
            taller.scales[0] = {1.2F, 1.2F, 1.2F};
            const auto second = xbox_avatar::load(test::build_synthetic_avatar(taller), test::decode_stored_png, error);
            expect(second != nullptr, "the second synthetic avatar must load");
            avatar.clear_world_players();
            const auto item = InventoryItemId::iron_pickaxe;
            const auto stock = draw(0.0F, false, item);
            const std::size_t body = mesh.indices.size();
            const std::size_t held = stock.vertices.size() - body;
            expect(stock.spans.front().count == body && held > 0U, "the stock body and its item");
            std::size_t drawn{};
            for (const xbox_avatar::Layer &layer : skin->layers)
                drawn += layer.indices.size();
            int released = 0;
            avatar.set_skin_texture_release_hook([&](SDL_Texture *) { ++released; });

            avatar.set_player_skin(1U, skin);
            expect(!avatar.player_skin_applied(1U), "a skin is not worn before its textures exist");
            expect(draw(0.0F, false, item).spans.front().count == body,
                   "the stock body stays until the skin is complete");
            avatar.pump_skin_uploads(true);
            expect(avatar.player_skin_applied(1U), "the skin must be worn once uploaded");
            const auto worn = draw(0.0F, false, item);
            expect(worn.vertices.size() == drawn + held && worn.spans.size() == skin->layers.size() + stock.spans.size() - 1U,
                   "the skin replaces the body mesh and nothing else");
            for (std::size_t span = 0; span < skin->layers.size(); ++span)
                expect((worn.spans[span].blend != EntityBlend::opaque) == skin->layers[span].blended,
                       "a painted layer blends over its surface; a solid one writes depth");
            // The item is the stock one, re-hung from the avatar's own hand.
            float lowest = 1000.0F;
            for (std::size_t vertex = 0; vertex < drawn; ++vertex)
                lowest = std::min(lowest, worn.vertices[vertex].y);
            expect(std::isfinite(lowest) && lowest > -0.5F && maximum_y(worn) < 2.5F, "the skin stands where the body stood");
            // Everybody else keeps the stock body.
            EntityBatch other;
            avatar.append_world_player(other, 2U, item, {}, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                                       item_player_animation_mode(item), false, false, false, false, false,
                                       false, 1.0F, 0.0F, nullptr, sky, false);
            expect(other.spans.front().count == body && other.vertices.size() == stock.vertices.size(),
                   "a skin belongs to one player");
            // Asking again for what is worn costs nothing; a new avatar replaces
            // it in place, the old one staying on screen until the new is ready.
            avatar.set_player_skin(1U, skin);
            expect(avatar.player_skin_applied(1U) && released == 0, "restating the skin must not rebuild it");
            avatar.set_player_skin(1U, second);
            expect(draw(0.0F, false, item).vertices.size() == worn.vertices.size(),
                   "the old skin stays until its replacement is complete");
            avatar.pump_skin_uploads(true);
            const auto replaced = draw(0.0F, false, item);
            std::cout << "skin.top=" << maximum_y(worn) << " skin.bottom=" << lowest << " taller.top=" << maximum_y(replaced)
                      << '\n';
            expect(maximum_y(replaced) > maximum_y(worn) + 0.02F, "the replacement (a taller build) must be the one drawn");
            // Back to stock, immediately; the textures go a few frames later,
            // after every batch that named them has been drawn.
            avatar.set_player_skin(1U, nullptr);
            expect(!avatar.player_skin_applied(1U), "taking the skin off is immediate");
            const auto bare = draw(0.0F, false, item);
            expect(bare.spans.front().count == body && bare.vertices.size() == stock.vertices.size(),
                   "without a skin the stock body is drawn");
            for (std::size_t vertex = 0; vertex < body; vertex += 53U)
                expect(bare.vertices[vertex].x == stock.vertices[vertex].x && bare.vertices[vertex].y == stock.vertices[vertex].y &&
                           bare.vertices[vertex].z == stock.vertices[vertex].z && bare.vertices[vertex].u == stock.vertices[vertex].u,
                       "the stock body is exactly what it was before any skin");
            for (int frame = 0; frame < 8; ++frame)
                avatar.pump_skin_uploads();
            expect(released == static_cast<int>(skin->textures.size() + second->textures.size()) && released > 0,
                   "every texture of a skin that is no longer worn is released, once");
            avatar.set_skin_texture_release_hook({});
            avatar.clear_world_players();
        }
        std::cout
            << "PASS: retail rig, clips, first-person visibility, Faint, item-change, respawn, "
               "lighting and yaw regressions\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
