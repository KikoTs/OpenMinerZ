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
// entity pass. Soft effects leave normal/light data at zero and retain their
// authored colour; skinned geometry carries the data needed by Rebirth's
// directional and shadowed lighting path.
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

// The dragon's fireball: the original FireBall model tumbling about its local
// X axis at 3 rad/s, rendered fullbright like FireballModelEntity. The id is
// stable across frames so the renderer can keep trail-emitter accumulators;
// velocity feeds the LocalSpace trail particles (glow/ball ride along).
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
    void append_enemy(EntityBatch &batch, const world::Enemy &enemy, float sun_light,
                      float torch_light, const std::array<float, 3> &sun_color,
                      float daylight);
    void append_dragon(EntityBatch &batch, const world::Dragon &dragon,
                       const std::array<float, 3> &sun_color);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
