#pragma once

#include "openminerz/world/enemy.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace openminerz
{

// One CPU-skinned world-space triangle vertex handed to the depth renderer's
// entity pass. Skinned geometry carries normals and baked lighting. Procedural
// volumes reuse the normal fields for their world centre, sky_light for radius,
// and negative torch_light values for the smoke/fire/ice shader selector.
struct EntityVertex final
{
    float x{};
    float y{};
    float z{};
    float u{};
    float v{};
    float red{1.0F};
    float green{1.0F};
    float blue{1.0F};
    float alpha{1.0F};
    float normal_x{};
    float normal_y{};
    float normal_z{};
    float sky_light{};
    float torch_light{};
};
static_assert(sizeof(EntityVertex) == 56U,
              "entity input layout offsets require the packed 56-byte vertex contract");

// How an entity span composites: opaque model spans write depth. Their original
// XNA textures often carry zero in the unused alpha byte, so alpha belongs only
// to the explicit soft spans. Those depth-test read-only so particles and face
// decals sit behind enemies and terrain instead of drawing over them.
enum class EntityBlend : std::uint8_t
{
    opaque,
    // Skinned/material surface using texture alpha while retaining the entity
    // normal, sun, torch and fog shader (avatar face/decal layers).
    alpha_lit,
    alpha,
    additive,
    multiply,
};

struct EntitySpan final
{
    SDL_Texture *texture{};
    std::uint32_t first{};
    std::uint32_t count{};
    EntityBlend blend{EntityBlend::opaque};
    // Mesh UVs remain continuous across triangle edges; the sampler wraps them.
    bool repeat_texture{};
};

struct EntityBatch final
{
    std::vector<EntityVertex> vertices;
    std::vector<EntitySpan> spans;

    void clear() noexcept
    {
        vertices.clear();
        spans.clear();
    }
};

// A tracer/arrow/fireball streak: the original TracerManager bipyramid spear
// (head point, three-vertex ring near the head, tail point).
struct ProjectileVisual final
{
    world::Vec3 head{};
    world::Vec3 tail{};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float width{0.025F};
};

struct RocketVisual final
{
    world::Vec3 position{};
    world::Vec3 direction{0.0F, 0.0F, -1.0F};
    float spin{};
    int id{};
};

struct ExplosiveFuseVisual final
{
    int x{};
    int y{};
    int z{};
    int id{};
    bool flash{};
};

// A dragon fireball's procedural volume and trail. Stable ids preserve emission
// history between frames; ice selects the blue core, trail and impact palette.
struct FireballVisual final
{
    world::Vec3 position{};
    world::Vec3 velocity{};
    float spin{};
    int id{};
    bool ice{};
};

// Loads the exported .omsk enemy models plus the shared skin textures and skins
// every live enemy into an EntityBatch each frame.
class EnemyModelLibrary final
{
  public:
    EnemyModelLibrary(SDL_Renderer *renderer, const std::filesystem::path &derived_directory);
    ~EnemyModelLibrary();

    EnemyModelLibrary(const EnemyModelLibrary &) = delete;
    EnemyModelLibrary &operator=(const EnemyModelLibrary &) = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] float clip_duration(world::EnemyModelKind model,
                                      std::string_view clip) const noexcept;
    // sun_light/torch_light are 0..1 factors sampled by the caller at each enemy.
    // `enhanced`: the Enhanced presentation's shader lights the vertices from
    // those factors itself, so they carry the skin's albedo (white) instead of
    // the Original presentation's baked light - lit once, not twice.
    void append_enemy(EntityBatch &batch, const world::Enemy &enemy, float sun_light,
                      float torch_light, const std::array<float, 3> &sun_color,
                      float daylight, bool enhanced = false);
    void append_dragon(EntityBatch &batch, const world::Dragon &dragon,
                       const std::array<float, 3> &sun_color, bool enhanced = false);
    // (Visuals) A texture pack changed: the skins are loaded again (through
    // the pack). Returns the textures they replaced, for the caller to forget
    // in the depth renderer and destroy.
    [[nodiscard]] std::vector<SDL_Texture *> reload_textures();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
