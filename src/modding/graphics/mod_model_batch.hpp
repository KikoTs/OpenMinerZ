#pragma once

// glTF models (props and creatures) into the creature pass's batch: one posed
// primitive at a time, skinned or rigid, with the light the pass expects
// (openminerz/modding/graphics.hpp). Shared by world_preview_mod.cpp (props)
// and world_preview_creatures.cpp (mod creatures and NPCs).

#include "enemy_renderer.hpp"
#include "openminerz/modding/graphics.hpp"

#include <array>
#include <vector>

namespace openminerz
{

// How a model is lit in the pass: Enhanced lights albedo in the shader (the
// vertex colour is the tint); the Original has the light baked into the
// vertex colour, as the creatures have it.
struct ModModelLight final
{
    bool enhanced{};
    // Sun and torch light at the model (0..1), and the sun's colour.
    float sun{1.0F};
    float torch{};
    float daylight{1.0F};
    std::array<float, 3> sun_color{1.0F, 1.0F, 1.0F};
};

// The sun and sky colour the creature pass bakes into the Original look.
[[nodiscard]] std::array<float, 3> mod_model_sun_color(float ambient_red, float ambient_green,
                                                       float ambient_blue, float direct_red,
                                                       float direct_green, float direct_blue) noexcept;

// Appends `primitive` of `model`, posed by `pose` (evaluate_pose/evaluate_blended_pose)
// and placed by `instance`, as one span drawn with `texture`. `color` is the
// material factor times the tint (alpha too). `skin` is scratch storage.
void append_mod_primitive(EntityBatch &batch, const modding::graphics::Model &model,
                          const modding::graphics::ModelPrimitive &primitive,
                          const std::vector<modding::graphics::Matrix4> &pose,
                          std::vector<modding::graphics::Matrix4> &skin,
                          const modding::graphics::Matrix4 &instance, SDL_Texture *texture,
                          const std::array<float, 4> &color, EntityBlend blend,
                          const ModModelLight &light);

// (Polish) The light a material gives off (glTF emissive): the vertices
// appended since `first` once more, as an additive span of `emissive` times
// `texture` over the lit surface. Unlit, so it glows the same by day, by night
// and in either presentation; fog still covers it. Nothing for a black
// `emissive`. Same vertices, same depth: the opaque surface below never hides it.
void append_mod_emissive(EntityBatch &batch, std::uint32_t first, SDL_Texture *texture,
                         const modding::graphics::Vec3 &emissive);

// q = yaw (about Y) * pitch (about X) * roll (about Z), all in degrees.
[[nodiscard]] std::array<float, 4> mod_quaternion_from_euler(float yaw_degrees, float pitch_degrees,
                                                             float roll_degrees) noexcept;

} // namespace openminerz
