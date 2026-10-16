#pragma once

#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/view_projection.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/player_animation.hpp"
#include "openminerz/world/sky.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace openminerz
{

namespace xbox_avatar
{
struct Skin;
}
namespace modding::graphics
{
struct IconModel; // Modding, visuals area (openminerz/modding/icon_model.hpp)
// (Items) openminerz/modding/held_item.hpp, openminerz/modding/graphics.hpp
struct HeldItemPose;
struct Model;
struct ModelMaterial;
class GraphicsService; // (GunFX)
}

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
    void append_world_player(EntityBatch &batch, std::uint16_t gid, InventoryItemId carried_item,
                             world::Vec3 feet, float yaw, float torso_pitch,
                             float animation_seconds, float movement_x, float movement_y,
                             PlayerAnimationMode animation_mode, bool using_tool, bool dead,
                             bool shouldering, bool reloading, bool throwing_grenade,
                             bool ready_to_throw_grenade, float sky_light, float torch_light,
                             SDL_Texture *block_atlas, const world::SkyState &sky,
                             bool rebirth_presentation, bool underwater = false,
                             bool hide_head = false, world::Vec3 *eye_position = nullptr,
                             world::Vec3 *eye_forward = nullptr,
                             const modding::graphics::HeldItemPose *mod_item = nullptr);
    void forget_world_player(std::uint16_t gid) noexcept;
    void clear_world_players() noexcept;
    // True when the character is the retail SWATMale rig (the default).
    [[nodiscard]] bool retail_character() const noexcept;
    // (Animation packs) The installed packs changed: put the imported clips
    // back and apply the packs now installed. Every character's animation
    // restarts. Costs only the packs' own small files.
    void reload_animation_pack();
    // (GunFX, openminerz/modding/gunfx.hpp) The client mods' weapon parts,
    // clip cues and viewmodel motion: the graphics service whose gunfx()
    // library they declared into, whose sounds and camera kicks the cues use
    // (null, or an empty library: nothing changes). Presentation only.
    void set_gunfx(modding::graphics::GraphicsService *service) noexcept;
    // Before each draw(): whether your current reload started with an empty
    // magazine (the "<Clip>Empty" variants), and the world camera's vertical
    // field of view (where a part dropped from the viewmodel lands).
    void set_gunfx_frame(bool reload_empty, float world_fov_degrees) noexcept;
    // Once a frame with the entities: parts dropped by any character fall,
    // settle on solid blocks and fade. `light` gives (sky, torch) light.
    using GunfxSolid = std::function<bool(int x, int y, int z)>;
    using GunfxLight = std::function<std::pair<float, float>(world::Vec3)>;
    void append_gunfx_props(EntityBatch &batch, float elapsed_seconds, const GunfxSolid &solid,
                            const GunfxLight &light, const world::SkyState &sky, bool rebirth_presentation);
    // Whether a dropped part is still in the world (the entity pass must run).
    [[nodiscard]] bool gunfx_props_pending() const noexcept;
    // The animation seam for the retail character. world::PlayerAnimator (the
    // port of Player.UpdateAnimation) owns WHICH clip plays WHEN; hand its
    // pose() for this frame to the view before draw() / append_world_player()
    // and the rig samples, blends, skins and attaches from exactly that. A frame
    // without one falls back to the view's built-in driver
    // (openminerz/retail_character_driver.hpp). Each pose is consumed once.
    void set_first_person_animation(const world::PlayerAnimationPose &pose);
    void set_world_player_animation(std::uint16_t gid, const world::PlayerAnimationPose &pose);
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
    // THE VIEWMODEL PASS (first-person body + held item).
    //
    // Projection: `viewmodel_fov_degrees` is this pass's OWN vertical angle -
    // retail's Avatar.EyePointCamera, 90 degrees and 45 shouldered
    // at the default setting (view::presented_viewmodel_fov_degrees). Its
    // perspective scale follows the world FOV setting for hands and weapon
    // together. Near plane 0.01 m, far plane 32 m: its own depth range.
    //
    // HOW VIEWMODEL PIXELS ARE IDENTIFIED - one convention for every renderer,
    // the same one post_processing.hpp states as `viewmodel_is_separate_layer`
    // ("THE VIEWMODEL HOOK", pass 13 of its pass order):
    // they are NEVER written to the world's colour or depth targets
    // (DepthVoxelRenderer). The pass rasterises into its own colour + depth
    // target (2x supersampled, cleared to transparent) and that target is
    // alpha-composited over the FINISHED world image, after all of the world's
    // post-processing. A viewmodel pixel is therefore "alpha > 0 in the
    // viewmodel target", and:
    //   * world depth never contains the hands, so depth of field, ambient
    //     occlusion, contact shadows, volumetrics and motion blur cannot
    //     blur, darken or smear them, and need no mask of their own;
    //   * the world can never draw over the hands, and the weapon can never
    //     clip into a wall or be cut by the world's near plane;
    //   * a whole-frame effect that runs AFTER this composite (the lens
    //     overlay's vignette and grain) covers world and hands alike by
    //     design; anything that must treat them differently belongs before
    //     this pass, or must sample this target's alpha.
    // The pass lights itself from the scene (set_environment_light) and applies
    // the world's exposure, tone curve and grade in its own shader so it sits
    // in the same picture.
    void draw(const ui::UiMetrics &metrics, const world::PlayerState &player, float elapsed_seconds,
              bool reduced_motion, float viewmodel_fov_degrees, SDL_Texture *block_atlas);
    // Modding, visuals area (openminerz/modding/icon_model.hpp): the held
    // item is a mod item drawn as its icon in solid form instead of its
    // fallback's model, in the first-person hand of the retail character (and
    // an Xbox-avatar skin on it). nullptr - the default, and every unmodded
    // frame - draws the held item exactly as before. Set before each draw().
    // (Items) Now a whole held mod item (openminerz/modding/held_item.hpp):
    // its glTF model posed by its own clips or its icon, gripped for its hold
    // style; for eat and drink the use clip follows the item's use. The same
    // goes for append_world_player's `mod_item`.
    void set_held_mod_item(std::shared_ptr<const modding::graphics::HeldItemPose> pose) noexcept;
    // (Items) The texture of a mod model's material (the world renderer's,
    // which lives as long as the model); null draws it untextured.
    using ModMaterialTextures = std::function<SDL_Texture *(
        const std::shared_ptr<const modding::graphics::Model> &, const modding::graphics::ModelMaterial &)>;
    void set_mod_material_textures(ModMaterialTextures textures);

    // The held gun's muzzle as draw() last put it on screen: a point in the
    // viewmodel camera's space and the angle of that pass. Empty when the last
    // draw() held no gun. A projectile that should be SEEN leaving the weapon
    // starts at view::viewmodel_to_world_view(point, fov_degrees, world fov).
    // Presentation only.
    using Muzzle = view::ViewmodelPoint;
    [[nodiscard]] std::optional<Muzzle> muzzle() const noexcept;

    // XBOX AVATAR SKINS (openminerz/xbox_avatar.hpp). A skin replaces the stock
    // body's mesh for one player - `gid` is a remote player's gamer id, or 256
    // for the local player in both views - on the unchanged retail pose, held
    // item and lighting. nullptr returns that player to the stock body.
    // Presentation only.
    //
    // The switch is deferred, never abrupt: the skin's textures are created a
    // band at a time by pump_skin_uploads() (call it once per frame, before
    // drawing), and the player keeps the current character until the new skin is
    // complete. Setting the skin a player already wears, or already has on the
    // way, does nothing, so callers may simply restate what they want each frame.
    void set_player_skin(std::uint16_t gid, std::shared_ptr<const xbox_avatar::Skin> skin);
    [[nodiscard]] bool player_skin_applied(std::uint16_t gid) const noexcept;
    // `everything` completes every pending upload at once, for a capture taken
    // on a known frame; in play the per-frame budget applies.
    void pump_skin_uploads(bool everything = false);
    // Called just before a skin texture is destroyed, so a renderer that caches
    // per-texture state (the entity pass's shader views) can drop it.
    void set_skin_texture_release_hook(std::function<void(SDL_Texture *)> hook);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The Xbox-avatar export behind the skin seam (see avatar_viewmodel.cpp); the
// default character does not read it.
[[nodiscard]] std::filesystem::path default_avatar_path();
// DerivedData/character.omrc: the retail character of the active content pack.
// Classic HD, which only restyles textures, falls back to the Classic pack's.
[[nodiscard]] std::filesystem::path default_character_rig_path();

} // namespace openminerz
