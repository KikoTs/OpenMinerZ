// Mod creatures and NPCs, and mod blocks, on the world renderer's side
// (openminerz/modding/graphics.hpp). Member functions of WorldPreviewRenderer,
// kept with the rest of the graphics area (Visuals, modding wave 2).
//
// A creature is a glTF model (models/<name>) posed by the clip of its
// replicated animation state, cross-faded between states, re-skinned from a
// mod texture when it names one, flashed red when hurt, and drawn with the
// other creatures (lit, shadowed and fogged alike). A model that cannot be
// loaded draws a box of the creature's size instead, and says why once.
//
// PRESENTATION ONLY. Without creatures nothing here runs.

#include "world_preview_renderer.hpp"

#include "depth_voxel_renderer.hpp"
#include "modding/graphics/mod_model_batch.hpp"
#include "modding/graphics/mod_surfaces.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <unordered_map>

namespace openminerz
{

namespace graphics = modding::graphics;

// --- glTF primitives into the creature pass ------------------------------------

std::array<float, 3> mod_model_sun_color(float ambient_red, float ambient_green,
                                         float ambient_blue, float direct_red, float direct_green,
                                         float direct_blue) noexcept
{
    return {std::min(1.0F, (ambient_red * 0.35F + direct_red * 0.65F) * 1.2F),
            std::min(1.0F, (ambient_green * 0.35F + direct_green * 0.65F) * 1.2F),
            std::min(1.0F, (ambient_blue * 0.35F + direct_blue * 0.65F) * 1.2F)};
}

std::array<float, 4> mod_quaternion_from_euler(float yaw_degrees, float pitch_degrees,
                                               float roll_degrees) noexcept
{
    constexpr float to_radians = std::numbers::pi_v<float> / 180.0F;
    const float yaw = yaw_degrees * to_radians * 0.5F;
    const float pitch = pitch_degrees * to_radians * 0.5F;
    const float roll = roll_degrees * to_radians * 0.5F;
    const std::array<float, 4> qy{0.0F, std::sin(yaw), 0.0F, std::cos(yaw)};
    const std::array<float, 4> qx{std::sin(pitch), 0.0F, 0.0F, std::cos(pitch)};
    const std::array<float, 4> qz{0.0F, 0.0F, std::sin(roll), std::cos(roll)};
    const auto multiply = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
        return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    return multiply(multiply(qy, qx), qz);
}

void append_mod_primitive(EntityBatch &batch, const graphics::Model &model,
                          const graphics::ModelPrimitive &primitive,
                          const std::vector<graphics::Matrix4> &pose,
                          std::vector<graphics::Matrix4> &skin, const graphics::Matrix4 &instance,
                          SDL_Texture *texture, const std::array<float, 4> &color,
                          EntityBlend blend, const ModModelLight &light)
{
    EntitySpan span;
    span.texture = texture;
    span.first = static_cast<std::uint32_t>(batch.vertices.size());
    span.blend = blend;
    span.repeat_texture = true;
    const bool skinned = primitive.skin >= 0;
    if (skinned)
        graphics::skin_matrices(model, primitive.skin, pose, skin);
    const graphics::Matrix4 rigid =
        !skinned && primitive.node >= 0 && static_cast<std::size_t>(primitive.node) < pose.size()
            ? graphics::matrix_multiply(instance, pose[static_cast<std::size_t>(primitive.node)])
            : instance;
    // Original: the light baked in, as the creatures have it.
    const float suppressed = light.torch * (1.0F - light.sun * light.daylight * 0.85F);
    const std::array<float, 3> torch_add{suppressed, suppressed * 0.92F, suppressed * 0.75F};
    const float sun_curve = 0.06F + 0.94F * light.sun * light.sun * (3.0F - 2.0F * light.sun);
    // glTF's front faces wind counter-clockwise, the pass's (XNA's) clockwise,
    // and the Enhanced shading turns a back face's normal around: every
    // triangle is emitted reversed, unless the placement mirrors it.
    const graphics::Matrix4 &placement = skinned ? instance : rigid;
    const float determinant =
        placement[0] * (placement[5] * placement[10] - placement[9] * placement[6]) -
        placement[4] * (placement[1] * placement[10] - placement[9] * placement[2]) +
        placement[8] * (placement[1] * placement[6] - placement[5] * placement[2]);
    const bool reverse = determinant >= 0.0F;
    batch.vertices.reserve(batch.vertices.size() + primitive.indices.size());
    for (std::size_t corner = 0U; corner < primitive.indices.size(); ++corner)
    {
        // Corners 1 and 2 of each triangle trade places.
        const std::size_t in_triangle = corner % 3U;
        const std::size_t picked = !reverse || in_triangle == 0U ? corner
                                   : in_triangle == 1U           ? corner + 1U
                                                                 : corner - 1U;
        if (picked >= primitive.indices.size())
            break;
        const graphics::ModelVertex &source = primitive.vertices[primitive.indices[picked]];
        graphics::Vec3 point = source.position;
        graphics::Vec3 normal = source.normal;
        if (skinned && !skin.empty())
        {
            graphics::Vec3 blended{};
            graphics::Vec3 blended_normal{};
            for (std::size_t slot = 0U; slot < 4U; ++slot)
            {
                const float weight = source.weights[slot];
                if (weight <= 0.0F || source.joints[slot] >= skin.size())
                    continue;
                const graphics::Matrix4 &joint = skin[source.joints[slot]];
                blended += graphics::transform_point(joint, source.position) * weight;
                blended_normal += graphics::transform_direction(joint, source.normal) * weight;
            }
            point = graphics::transform_point(instance, blended);
            normal = graphics::transform_direction(instance, blended_normal);
        }
        else
        {
            point = graphics::transform_point(rigid, point);
            normal = graphics::transform_direction(rigid, normal);
        }
        normal = vfx::normalized(normal);
        const float shade = std::clamp(0.58F + normal.y * 0.28F, 0.35F, 1.0F);
        const float sun_gain = sun_curve * shade;
        EntityVertex vertex;
        vertex.x = point.x;
        vertex.y = point.y;
        vertex.z = point.z;
        vertex.u = source.uv[0];
        vertex.v = source.uv[1];
        // Enhanced: albedo, which the lit shader lights (sun, sky, torches,
        // effect and mod lights, shadows).
        const std::array<float, 3> baked =
            light.enhanced
                ? std::array<float, 3>{1.0F, 1.0F, 1.0F}
                : std::array<float, 3>{
                      std::min(1.0F, light.sun_color[0] * sun_gain + torch_add[0]),
                      std::min(1.0F, light.sun_color[1] * sun_gain + torch_add[1]),
                      std::min(1.0F, light.sun_color[2] * sun_gain + torch_add[2])};
        vertex.red = baked[0] * color[0];
        vertex.green = baked[1] * color[1];
        vertex.blue = baked[2] * color[2];
        vertex.alpha = color[3];
        vertex.normal_x = normal.x;
        vertex.normal_y = normal.y;
        vertex.normal_z = normal.z;
        vertex.sky_light = std::clamp(light.sun, 0.0F, 1.0F);
        vertex.torch_light = std::clamp(light.torch, 0.0F, 1.0F);
        batch.vertices.push_back(vertex);
    }
    span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
    if (span.count > 0U)
        batch.spans.push_back(span);
}

void append_mod_emissive(EntityBatch &batch, std::uint32_t first, SDL_Texture *texture,
                         const graphics::Vec3 &emissive)
{
    if (texture == nullptr || (emissive.x <= 0.0F && emissive.y <= 0.0F && emissive.z <= 0.0F))
        return;
    const auto end = static_cast<std::uint32_t>(batch.vertices.size());
    if (first >= end)
        return;
    EntitySpan span;
    span.texture = texture;
    span.first = end;
    span.count = end - first;
    span.blend = EntityBlend::additive;
    span.repeat_texture = true;
    batch.vertices.reserve(batch.vertices.size() + span.count);
    for (std::uint32_t index = first; index < end; ++index)
    {
        EntityVertex vertex = batch.vertices[index];
        vertex.red = emissive.x;
        vertex.green = emissive.y;
        vertex.blue = emissive.z;
        vertex.alpha = 1.0F;
        // The particle shader's plain path: no volume, no soft edge.
        vertex.sky_light = 0.0F;
        vertex.torch_light = 0.0F;
        batch.vertices.push_back(vertex);
    }
    batch.spans.push_back(span);
}

// --- Creatures -------------------------------------------------------------------

namespace
{

// The most creatures drawn in one frame (the nearest), and from how far the
// pose is refreshed only every fourth frame.
constexpr std::size_t max_drawn_creatures = 96U;
constexpr float far_pose_distance = 40.0F;
// A hurt creature flashes red for this long.
constexpr float hit_flash_seconds = 0.25F;
// A dead creature without a death clip topples over in this long, then every
// corpse waits and sinks into the ground.
constexpr float topple_seconds = 0.45F;
constexpr float corpse_rest_seconds = 0.6F;
constexpr float corpse_sink_seconds = 0.8F;

[[nodiscard]] bool same_name(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// (mod-ai) A creature model ref "retail:<family>[:<variant>]": the retail
// enemy type it wears (zombie, archer, skeleton, skeleton_sword,
// skeleton_axes, felguard, alien; the variant picks among a family's skins).
struct ModRetailLook final
{
    int type{};
};

[[nodiscard]] std::optional<ModRetailLook> mod_retail_look(std::string_view ref) noexcept
{
    if (!ref.starts_with("retail:"))
        return std::nullopt;
    ref.remove_prefix(7U);
    int variant{};
    if (const auto colon = ref.find(':'); colon != std::string_view::npos)
    {
        for (const char digit : ref.substr(colon + 1U))
            if (digit >= '0' && digit <= '9')
                variant = std::min(variant * 10 + (digit - '0'), 99);
        ref = ref.substr(0U, colon);
    }
    world::EnemyClass wanted{};
    if (same_name(ref, "zombie"))
        wanted = world::EnemyClass::zombie;
    else if (same_name(ref, "archer"))
        wanted = world::EnemyClass::archer;
    else if (same_name(ref, "skeleton"))
        wanted = world::EnemyClass::skeleton;
    else if (same_name(ref, "skeleton_sword"))
        wanted = world::EnemyClass::skeleton_sword;
    else if (same_name(ref, "skeleton_axes"))
        wanted = world::EnemyClass::skeleton_axes;
    else if (same_name(ref, "felguard"))
        wanted = world::EnemyClass::felguard;
    else if (same_name(ref, "alien"))
        wanted = world::EnemyClass::alien;
    else
        return std::nullopt;
    int seen{};
    std::optional<ModRetailLook> first;
    for (int type = 0; type < world::enemy_type_count; ++type)
        if (world::enemy_stats(type).enemy_class == wanted)
        {
            if (!first.has_value())
                first = ModRetailLook{type};
            if (seen++ == variant)
                return ModRetailLook{type};
        }
    return first;
}

// A model ref's alternatives, in order ("retail:zombie|models/ghoul").
[[nodiscard]] std::vector<std::string_view> mod_model_choices(std::string_view ref)
{
    std::vector<std::string_view> choices;
    while (!ref.empty() && choices.size() < 4U)
    {
        const auto bar = ref.find('|');
        if (bar != 0U)
            choices.push_back(ref.substr(0U, bar));
        if (bar == std::string_view::npos)
            break;
        ref.remove_prefix(bar + 1U);
    }
    return choices;
}

// The first retail look among a ref's alternatives that comes before any
// other (only asked when no glTF of an earlier alternative loaded).
[[nodiscard]] std::optional<ModRetailLook> mod_retail_choice(std::string_view ref)
{
    for (const std::string_view choice : mod_model_choices(ref))
        if (const auto look = mod_retail_look(choice))
            return look;
    return std::nullopt;
}

// The retail clip closest to a replicated state; a state that names a clip
// of the model itself ("enraged", "eat_start") plays that clip.
[[nodiscard]] std::string_view mod_retail_clip(world::EnemyModelKind model, std::string_view state) noexcept
{
    const bool idle = same_name(state, "idle");
    const bool walk = same_name(state, "walk");
    const bool run = same_name(state, "run");
    const bool attack = same_name(state, "attack");
    const bool hurt = same_name(state, "hurt");
    const bool death = same_name(state, "death");
    switch (model)
    {
    case world::EnemyModelKind::zombie:
        return idle ? "walk" : walk ? "walk" : run ? "run" : attack ? "attack1" : hurt ? "hit_reaction1"
               : death ? "death1" : state;
    case world::EnemyModelKind::skeleton_archer:
        return idle ? "idle_archer1" : walk ? "walk_archer1" : run ? "run" : attack ? "atack_archer1"
               : hurt ? "gethit1" : death ? "death1" : state;
    case world::EnemyModelKind::skeleton_axes:
        return idle ? "walk" : walk ? "walk" : run ? "run" : attack ? "axes_atack1" : hurt ? "gethit1"
               : death ? "death1" : state;
    case world::EnemyModelKind::skeleton_zombie:
    case world::EnemyModelKind::skeleton_sword:
        return idle ? "walk" : walk ? "walk" : run ? "run" : attack ? "attack1" : hurt ? "gethit1"
               : death ? "death1" : state;
    case world::EnemyModelKind::felguard:
        return idle ? "Idle" : walk ? "walk" : run ? "run" : attack ? "Attack1" : hurt ? "Idle"
               : death ? "death1" : state;
    case world::EnemyModelKind::alien:
        return idle || walk || run ? "MoveLoop" : attack ? "Attack1" : hurt ? "Damage1" : death ? "Death" : state;
    case world::EnemyModelKind::count:
        break;
    }
    return state;
}

// The stand-in for a model that cannot be loaded: a unit box on the ground
// (x and z -0.5..0.5, y 0..1), scaled to the creature's size.
const graphics::Model &stand_in_box()
{
    static const graphics::Model box = [] {
        graphics::Model model;
        graphics::ModelPrimitive primitive;
        static constexpr std::array<std::array<float, 3>, 6> normals{
            {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        for (const auto &n : normals)
        {
            // Two axes across the face, chosen so the winding faces out.
            const graphics::Vec3 normal{n[0], n[1], n[2]};
            const graphics::Vec3 across = std::abs(n[1]) > 0.5F
                                              ? graphics::Vec3{1.0F, 0.0F, 0.0F}
                                              : graphics::Vec3{-n[2], 0.0F, n[0]};
            const graphics::Vec3 up = vfx::cross(normal, across);
            const auto first = static_cast<std::uint32_t>(primitive.vertices.size());
            for (const auto &corner : std::array<std::array<float, 2>, 4>{
                     {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}}})
            {
                graphics::ModelVertex vertex;
                vertex.position = graphics::Vec3{0.0F, 0.5F, 0.0F} + normal * 0.5F +
                                  across * (corner[0] * 0.5F) + up * (corner[1] * 0.5F);
                vertex.normal = normal;
                vertex.uv = {corner[0] * 0.5F + 0.5F, 0.5F - corner[1] * 0.5F};
                primitive.vertices.push_back(vertex);
            }
            for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U})
                primitive.indices.push_back(first + index);
        }
        model.primitives.push_back(std::move(primitive));
        model.radius = 0.9F;
        return model;
    }();
    return box;
}

} // namespace

struct WorldPreviewRenderer::ModCreatures final
{
    struct Creature final
    {
        graphics::CreatureAnimator animator;
        // Held so the animator's model stays the same object.
        std::shared_ptr<const graphics::Model> model;
        std::vector<graphics::Matrix4> pose;
        float sun{1.0F};
        float torch{};
        bool sampled{};
        float last_health{1.0F};
        float flash{};
        std::string state;
        float state_seconds{};
        std::uint64_t seen{};
    };
    std::unordered_map<std::uint64_t, Creature> creatures;
    // Creature skins (CreatureView::texture) by asset name; null: failed.
    std::map<std::string, SDL_Texture *, std::less<>> skins;
    std::uint64_t skins_generation{~0ULL};
    // What has been reported once ("model:<name>", "skin:<name>").
    std::set<std::string, std::less<>> reported;
    std::vector<graphics::Matrix4> skin;
    std::uint64_t frame{};
    std::chrono::steady_clock::time_point last{};
};

void WorldPreviewRenderer::ModCreaturesDeleter::operator()(ModCreatures *state) const noexcept
{
    // The world renderer's members go first (mod_creatures_ is declared before
    // them), so nothing still holds a view of these skins.
    for (auto &[name, texture] : state->skins)
    {
        (void)name;
        SDL_DestroyTexture(texture);
    }
    delete state;
}

void WorldPreviewRenderer::mod_append_creatures(
    EntityBatch &batch, const world::PlayerState &camera, const world::SkyState &sky,
    float vertical_fov_degrees, float aspect, bool enhanced,
    const std::function<std::array<float, 2>(world::Vec3)> &sample_light)
{
    if (mod_graphics_ == nullptr)
        return;
    graphics::GraphicsService &service = *mod_graphics_;
    const auto release_skins = [this](ModCreatures &state) {
        for (auto &[name, texture] : state.skins)
        {
            (void)name;
            if (texture != nullptr && depth_renderer_ != nullptr)
                depth_renderer_->forget_entity_texture(texture);
            SDL_DestroyTexture(texture);
        }
        state.skins.clear();
    };
    if (service.creatures().empty())
    {
        // The last creature went: so does everything kept for them.
        if (mod_creatures_ != nullptr)
        {
            release_skins(*mod_creatures_);
            mod_creatures_.reset();
        }
        return;
    }
    if (mod_creatures_ == nullptr)
        mod_creatures_.reset(new ModCreatures{});
    ModCreatures &state = *mod_creatures_;
    ++state.frame;
    const auto now = std::chrono::steady_clock::now();
    const float delta =
        state.last.time_since_epoch().count() == 0
            ? 0.0F
            : std::clamp(std::chrono::duration<float>(now - state.last).count(), 0.0F, 0.25F);
    state.last = now;
    if (state.skins_generation != service.assets().generation())
    {
        release_skins(state);
        state.skins_generation = service.assets().generation();
    }
    const auto report = [&state](const std::string &key, const std::string &message) {
        if (state.reported.insert(key).second)
            std::cerr << "Mod creature " << message << '\n';
    };
    const auto skin_texture = [&](const graphics::CreatureView &view) -> SDL_Texture * {
        if (view.texture.empty())
            return nullptr;
        if (const auto found = state.skins.find(view.texture); found != state.skins.end())
            return found->second;
        static constexpr std::array<std::string_view, 4> extensions{".png", ".jpg", ".bmp",
                                                                    ".tga"};
        SDL_Texture *texture = nullptr;
        std::string error = "not found in any enabled mod";
        if (const auto resolved = service.assets().resolve(view.texture, extensions))
            if (auto image = graphics::load_image(resolved->path, &error, {2048, 16U << 20U}))
                if (SDL_Surface *surface = surface_from_image(*image))
                {
                    texture = SDL_CreateTextureFromSurface(renderer_, surface);
                    SDL_DestroySurface(surface);
                    if (texture != nullptr)
                    {
                        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
                    }
                }
        if (texture == nullptr)
            report("skin:" + view.texture, "'" + view.kind + "': texture '" + view.texture +
                                               "': " + error + " (the model's own is used)");
        state.skins.emplace(view.texture, texture);
        return texture;
    };

    const graphics::Vec3 eye{camera.position.x,
                             camera.position.y + world::PlayerSimulation::eye_height,
                             camera.position.z};
    const float sine_yaw = std::sin(camera.yaw);
    const float cosine_yaw = std::cos(camera.yaw);
    const float sine_pitch = std::sin(camera.pitch);
    const float cosine_pitch = std::cos(camera.pitch);
    const graphics::ViewFrustum frustum = graphics::make_frustum(
        eye, {sine_yaw * cosine_pitch, sine_pitch, -cosine_yaw * cosine_pitch},
        {cosine_yaw, 0.0F, sine_yaw}, {-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch},
        vertical_fov_degrees * std::numbers::pi_v<float> / 180.0F, aspect, 0.05F,
        visibility_distance_ + 16.0F);
    const float reach = visibility_distance_ + 8.0F;

    struct Candidate final
    {
        const graphics::CreatureView *view{};
        ModCreatures::Creature *record{};
        float distance{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(service.creatures().size());
    for (const graphics::CreatureView &view : service.creatures())
    {
        ModCreatures::Creature &record = state.creatures[view.id];
        record.seen = state.frame;
        // A hit: the health dropped, or the state turned to "hurt".
        record.flash = std::max(0.0F, record.flash - delta);
        if (view.state != record.state)
        {
            if (same_name(view.state, "hurt"))
                record.flash = hit_flash_seconds;
            record.state = view.state;
            record.state_seconds = 0.0F;
        }
        else
            record.state_seconds += delta;
        if (view.health < record.last_health - 0.001F && view.health > 0.0F)
            record.flash = hit_flash_seconds;
        record.last_health = view.health;

        std::shared_ptr<const graphics::Model> model;
        // (mod-ai) "retail:zombie|models/ghoul": the first look that can be
        // drawn here; a retail look is drawn from the enemy models, below.
        const bool retail_ready = enemy_models_ != nullptr && enemy_models_->loaded();
        for (const std::string_view choice : mod_model_choices(view.model))
        {
            if (choice.starts_with("retail:"))
            {
                if (retail_ready && mod_retail_look(choice).has_value())
                    break;
                continue;
            }
            std::string error;
            model = service.model_cache().get(service.assets(), std::string{choice}, &error);
            if (model != nullptr)
                break;
            report("model:" + std::string{choice},
                   "'" + view.kind + "': " + error + " (drawing a box of its size instead)");
        }
        if (model != record.model)
        {
            record.model = model;
            record.pose.clear();
        }
        const float distance = vfx::length(view.position - eye);
        if (distance > reach)
            continue;
        const float radius =
            model != nullptr ? model->radius * 1.5F + 0.25F
                             : std::max(view.height, view.width) * 0.75F + 0.25F;
        if (!graphics::sphere_visible(
                frustum, view.position + graphics::Vec3{0.0F, view.height * 0.5F, 0.0F}, radius))
            continue;
        candidates.push_back({&view, &record, distance});
    }
    std::erase_if(state.creatures, [&state](const auto &entry) {
        return entry.second.seen != state.frame;
    });
    if (candidates.size() > max_drawn_creatures)
    {
        std::nth_element(candidates.begin(),
                         candidates.begin() + static_cast<std::ptrdiff_t>(max_drawn_creatures),
                         candidates.end(),
                         [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
        candidates.resize(max_drawn_creatures);
    }

    const std::array<float, 3> sun_color =
        mod_model_sun_color(sky.ambient.red, sky.ambient.green, sky.ambient.blue, sky.direct.red,
                            sky.direct.green, sky.direct.blue);
    for (const Candidate &candidate : candidates)
    {
        const graphics::CreatureView &view = *candidate.view;
        ModCreatures::Creature &record = *candidate.record;
        // Light at the head, refreshed every eighth frame (staggered).
        if (!record.sampled || (state.frame + view.id) % 8U == 0U)
        {
            const std::array<float, 2> light =
                sample_light({view.position.x, view.position.y, view.position.z});
            record.sun = light[0];
            record.torch = light[1];
            record.sampled = true;
        }
        const ModModelLight light{enhanced, record.sun, record.torch, sky.daylight, sun_color};
        // Hurt: towards red for a moment.
        const float flash = std::clamp(record.flash / hit_flash_seconds, 0.0F, 1.0F);
        const std::array<float, 3> tint{1.0F, 1.0F - 0.7F * flash, 1.0F - 0.75F * flash};

        // A death: the clip, or a topple without one; then the corpse rests
        // and sinks into the ground (the app keeps it a few seconds).
        float roll{};
        float sink{};
        if (same_name(view.state, "death") && view.health <= 0.0F)
        {
            const int clip = record.model != nullptr
                                 ? graphics::find_state_clip(*record.model, view.state)
                                 : -1;
            const float played =
                clip >= 0 ? record.model->animations[static_cast<std::size_t>(clip)].duration
                          : topple_seconds;
            if (clip < 0)
                roll = std::min(1.0F, record.state_seconds / topple_seconds) * 90.0F;
            const float resting = played + corpse_rest_seconds;
            if (record.state_seconds > resting)
                sink = std::min(1.0F, (record.state_seconds - resting) / corpse_sink_seconds) *
                       (view.height * 0.6F + 0.2F);
        }
        // The game's yaw faces -Z at 0; a glTF model faces +Z.
        const float yaw_degrees = 180.0F - view.yaw * 180.0F / std::numbers::pi_v<float>;
        // (mod-ai) Nose up is a negative turn about X for a model facing +Z.
        const float pitch_degrees = -view.pitch * 180.0F / std::numbers::pi_v<float>;
        const graphics::Vec3 feet = view.position - graphics::Vec3{0.0F, sink, 0.0F};

        // (mod-ai) "retail:zombie" and friends: the retail enemy from the
        // player's own game, posed by the closest retail clip. Nothing is
        // shipped: without the enemy models it is the box below.
        if (record.model == nullptr && enemy_models_ != nullptr && enemy_models_->loaded())
            if (const auto look = mod_retail_choice(view.model))
            {
                world::Enemy body;
                body.type = look->type;
                body.position = {feet.x, feet.y, feet.z};
                body.yaw = view.yaw;
                const std::string_view clip = mod_retail_clip(world::enemy_stats(look->type).model, view.state);
                body.clip = clip;
                body.clip_loops = graphics::state_loops(view.state);
                body.clip_time = record.state_seconds;
                if (body.clip_loops && same_name(view.state, "idle"))
                    body.clip_time *= 0.25F; // a slow sway, not a walk
                if (!body.clip_loops)
                    body.clip_time = std::min(body.clip_time,
                                              std::max(0.0F, enemy_models_->clip_duration(
                                                                 world::enemy_stats(look->type).model, clip) -
                                                                 0.02F));
                // Lit the way the horde's own enemies are: in the Original
                // presentation full sun under an open sky, a fifth of it under
                // cover; in the Enhanced one (fix) the sky light sampled at the
                // head, which its shader lights the skin from. The torchlight
                // sampled.
                float sun = record.sun;
                if (!enhanced)
                {
                    const int cell_x = static_cast<int>(std::floor(feet.x));
                    const int cell_z = static_cast<int>(std::floor(feet.z));
                    bool sky_clear = true;
                    for (int y = static_cast<int>(std::floor(feet.y + 1.5F)) + 1;
                         y < world::world_min_y + world::chunk_height && sky_clear; ++y)
                        sky_clear = !(region_.contains(cell_x, y, cell_z) && world::occludes(region_.block(cell_x, y, cell_z)));
                    sun = sky_clear ? 1.0F : 0.2F;
                }
                const std::size_t first = batch.vertices.size();
                enemy_models_->append_enemy(batch, body, sun, record.torch, sun_color, sky.daylight, enhanced);
                if (batch.vertices.size() > first)
                {
                    if (flash > 0.0F)
                        for (std::size_t vertex = first; vertex < batch.vertices.size(); ++vertex)
                        {
                            batch.vertices[vertex].green *= tint[1];
                            batch.vertices[vertex].blue *= tint[2];
                        }
                    continue;
                }
            }
        SDL_Texture *skin = skin_texture(view);

        if (record.model == nullptr)
        {
            const graphics::Model &box = stand_in_box();
            const graphics::Matrix4 instance = graphics::matrix_compose(
                feet, mod_quaternion_from_euler(yaw_degrees, pitch_degrees, roll),
                {std::max(view.width, 0.1F), std::max(view.height, 0.1F),
                 std::max(view.width, 0.1F)});
            static const std::vector<graphics::Matrix4> no_pose;
            const std::array<float, 4> color =
                skin != nullptr ? std::array<float, 4>{tint[0], tint[1], tint[2], 1.0F}
                                : std::array<float, 4>{0.78F * tint[0], 0.55F * tint[1],
                                                       0.36F * tint[2], 1.0F};
            append_mod_primitive(batch, box, box.primitives.front(), no_pose, state.skin, instance,
                                 skin != nullptr ? skin : white_texture_, color,
                                 EntityBlend::opaque, light);
            continue;
        }
        const graphics::Model &model = *record.model;
        record.animator.vary(view.id); // (fix) herds out of lockstep
        record.animator.play(model, view.state);
        record.animator.step(delta);
        // Far away the pose is refreshed every fourth frame (staggered).
        if (record.pose.empty() || candidate.distance < far_pose_distance ||
            (state.frame + view.id) % 4U == 0U)
            graphics::evaluate_blended_pose(model, record.animator.layers(), record.pose);
        const graphics::Matrix4 instance = graphics::matrix_compose(
            feet, mod_quaternion_from_euler(yaw_degrees, pitch_degrees, roll),
            {view.scale, view.scale, view.scale}); // (mod-ai) pitch and scale
        for (const graphics::ModelPrimitive &primitive : model.primitives)
        {
            const graphics::ModelMaterial *material =
                primitive.material >= 0 &&
                        static_cast<std::size_t>(primitive.material) < model.materials.size()
                    ? &model.materials[static_cast<std::size_t>(primitive.material)]
                    : nullptr;
            SDL_Texture *texture = skin;
            if (texture == nullptr && material != nullptr)
                texture = mod_material_texture(record.model, *material);
            if (texture == nullptr)
                texture = white_texture_;
            if (texture == nullptr)
                continue;
            // A skin replaces the texture, not the material's colour.
            const std::array<float, 4> factor =
                material != nullptr ? material->color_factor : std::array<float, 4>{1, 1, 1, 1};
            const auto first = static_cast<std::uint32_t>(batch.vertices.size());
            append_mod_primitive(batch, model, primitive, record.pose, state.skin, instance,
                                 texture,
                                 {factor[0] * tint[0], factor[1] * tint[1], factor[2] * tint[2],
                                  factor[3]},
                                 material != nullptr && (material->blend || material->mask)
                                     ? EntityBlend::alpha_lit
                                     : EntityBlend::opaque,
                                 light);
            // (Polish) and the light it gives off.
            if (material != nullptr)
                append_mod_emissive(batch, first, mod_emissive_texture(record.model, *material),
                                    material->emissive);
        }
    }
}

// --- Mod blocks ------------------------------------------------------------------

void WorldPreviewRenderer::set_mod_block_overlay(
    std::shared_ptr<const graphics::ModBlockOverlay> overlay)
{
    if (overlay != nullptr && overlay->empty())
        overlay.reset();
    if (overlay == mod_block_overlay_)
        return;
    std::vector<world::ChunkCoordinate> changed =
        graphics::ModBlockOverlay::changed_sections(mod_block_overlay_.get(), overlay.get());
    mod_block_overlay_ = std::move(overlay);
    // The faces of the sections a mod block entered or left, at the front of
    // the queue like an edit's (sections outside the view are skipped there).
    if (!changed.empty())
        queue_chunk_rebuilds(std::move(changed), true);
}

std::optional<std::array<float, 3>> WorldPreviewRenderer::mod_block_color(int x, int y, int z,
                                                                          world::BlockType block) const
{
    if (mod_block_overlay_ == nullptr)
        return std::nullopt;
    const graphics::ModBlockLook *look = mod_block_overlay_->find(x, y, z, block);
    if (look == nullptr)
        return std::nullopt;
    return std::array<float, 3>{look->color.r, look->color.g, look->color.b};
}

} // namespace openminerz
