#pragma once

#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/world/player.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>

namespace openminerz
{

class AvatarViewmodel final
{
  public:
    AvatarViewmodel(SDL_Renderer *renderer, std::filesystem::path asset_path);
    ~AvatarViewmodel();

    AvatarViewmodel(const AvatarViewmodel &) = delete;
    AvatarViewmodel &operator=(const AvatarViewmodel &) = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool world_animation_ready() const noexcept;
    [[nodiscard]] std::size_t animation_clip_count() const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
    // CPU-skin the complete Xbox avatar into the shared world/entity depth pass.
    // This is the native counterpart of OpenClassicAvatarMod's third-person
    // ImportedAvatarModelEntity path; it intentionally reuses the same imported
    // .omavatar asset as the first-person hands.
    void append_world_player(EntityBatch &batch, std::uint8_t gid,
                             InventoryItemId carried_item, world::Vec3 feet, float yaw,
                             float torso_pitch, float animation_seconds,
                             float movement_x, float movement_y,
                             PlayerAnimationMode animation_mode, bool using_tool,
                             bool dead, bool shouldering, bool reloading,
                             bool throwing_grenade, bool ready_to_throw_grenade,
                             float sky_light, float torch_light, SDL_Texture *block_atlas);
    // GetAvatarColor's environment: an ambient term, a directional sun term,
    // and the sun direction expressed in the viewmodel's camera space. The
    // hands finally darken at night, in caves, and in shadow.
    void set_environment_light(const std::array<float, 3> &ambient,
                               const std::array<float, 3> &directional,
                               const std::array<float, 3> &direction_camera,
                               bool rebirth_presentation, float local_torch, float exposure,
                               int tone_curve,
                               float color_grading,
                               const std::array<float, 3> &fog_color) noexcept;
    void draw(const ui::UiMetrics &metrics, const world::PlayerState &player, float elapsed_seconds,
              bool reduced_motion, float viewmodel_fov_degrees, SDL_Texture *block_atlas);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::filesystem::path default_avatar_path();

} // namespace openminerz
