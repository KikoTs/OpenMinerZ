#pragma once

// (Items) How a mod item looks in a hand and on the ground: its glTF model
// (items.define `model`) or, without one, its icon as a thin solid sprite
// (icon_model.hpp); where the hand grips it for its hold style; the retail
// arm clips that style plays; and the item's own clips - `idle`, `equip` and
// `use` - played on the item. Shared by the first- and third-person body
// (src/retail_character_view.inc), the pickups (world_preview_renderer.cpp)
// and the app (src/app_mod_items.inc), which resolves the assets and drives
// the clips.
//
// Model conventions (docs/modding/authoring.md, "Held items"): one
// unit is one block (a metre); the origin is where the fist closes around the
// grip; +Y runs from the fist along the handle or blade; +Z is the edge, the
// head of a tool or the face of a thing held out (the side that strikes or
// faces away from the player). An icon is held by its lower-left corner like
// a sword sprite for the swing and tool styles, and upright otherwise.
//
// PRESENTATION ONLY: nothing here reaches the simulation, the wire or a save.

#include "openminerz/inventory.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/icon_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace openminerz::modding::graphics
{

using gameplay::HoldStyle;

// The style a hold is drawn with: `fallback` becomes the style that matches
// the fallback item's own arm clips (a pick: tool, a block: block, bare
// hands: fist, anything else: item); every other style is itself.
[[nodiscard]] HoldStyle resolved_hold(HoldStyle hold, InventoryItemId fallback) noexcept;
// The retail arm clips (Player._playerMode) a body plays holding it:
// swing and tool pick, item generic, block, eat and drink block, fist fist;
// `fallback` the fallback's own.
[[nodiscard]] PlayerAnimationMode hold_animation_mode(HoldStyle hold, InventoryItemId fallback) noexcept;
// Held out in front of the body like a block (block, eat, drink) rather than
// closed in the fist.
[[nodiscard]] bool held_in_front(HoldStyle resolved) noexcept;
// The retail item whose hand transform (CreateEntity(ItemUse.Hand)) the grip
// is placed on: the knife for the fist, a block for the front.
[[nodiscard]] InventoryItemId grip_reference_item(HoldStyle resolved) noexcept;

// How a mod item looks in 3D: a model, or else its icon.
struct ItemLook final
{
    std::shared_ptr<const Model> model;
    std::shared_ptr<const IconModel> icon;
    // Resolved (never `fallback`).
    HoldStyle hold{HoldStyle::item};
    float scale{1.0F};

    [[nodiscard]] bool empty() const noexcept
    {
        return model == nullptr && (icon == nullptr || icon->empty());
    }
};

// One hand holding a mod item this frame.
struct HeldItemPose final
{
    ItemLook look;
    // The item's own clips (blended; count 0: the bind pose).
    std::array<PoseLayer, 2> layers{};
    std::size_t layer_count{};
    // Eat and drink: how far through a use the hand is, 0..1 (0 at rest).
    // The body's use clip and the item's motion at the mouth follow it.
    float use_progress{};
};

// The item's own clips for one holder: `equip` once when an item comes into
// the hand, `use` once at every use, `idle` (looping) the rest of the time,
// cross-faded; the bind pose for clips the model lacks. Clip names match as
// creature states do (any case; "Armature|use" works).
class HeldItemAnimator final
{
  public:
    static constexpr float blend_seconds = 0.12F;

    // Every frame: what the hand holds (0: nothing of a mod) and whether a
    // use began since the last call; `use_seconds` is how long the eat and
    // drink motion lasts.
    void update(std::uint32_t item, const ItemLook &look, bool use_started, float use_seconds,
                float seconds);
    [[nodiscard]] HeldItemPose pose() const;
    [[nodiscard]] std::uint32_t item() const noexcept
    {
        return item_;
    }

  private:
    void play(int animation, bool loop);

    std::uint32_t item_{};
    ItemLook look_;
    const Model *model_{};
    int idle_{-1};
    int equip_{-1};
    int use_{-1};
    // [0] playing, [1] fading out.
    std::array<PoseLayer, 2> layers_{};
    std::array<float, 2> durations_{};
    std::size_t count_{};
    float fade_{1.0F};
    float use_elapsed_{-1.0F};
    float use_seconds_{1.0F};
};

// The eat/drink use as the body plays it: how far into the retail BlockUse
// clip the arm is at `progress` (0..1) - up to the mouth, a few small bites
// there, back down.
[[nodiscard]] float mouth_clip_fraction(float progress) noexcept;

// A model posed by `layers` (CPU skinning), as triangles in the item's own
// space, wound like the retail (XNA) models: clockwise from the front.
struct PosedItemVertex final
{
    Vec3 position{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    std::array<float, 2> uv{};
};
struct PosedItemPart final
{
    int material{-1};
    std::vector<PosedItemVertex> vertices;
};
struct ItemPoseScratch final
{
    std::vector<Matrix4> world;
    std::vector<Matrix4> skin;
};
void pose_item_model(const Model &model, std::span<const PoseLayer> layers,
                     std::vector<PosedItemPart> &parts, ItemPoseScratch &scratch);

// Item space -> the grip reference item's model space (grip_reference_item),
// for one look in one hand: the hold's grip, the item's scale, the motion of
// an eat/drink use at `use_progress`, and the icon's own placement (an icon
// spans one unit). `reference_scale` is the uniform scale of the reference's
// hand transform, which the grip undoes so items keep their authored size.
[[nodiscard]] Matrix4 held_item_transform(const ItemLook &look, float use_progress,
                                          float reference_scale, bool icon) noexcept;

// A dropped item's model: the transform that stands it upright on the
// pickup's centre at its spin, no larger than a pickup is.
[[nodiscard]] Matrix4 dropped_item_transform(const ItemLook &look, Vec3 center, float spin) noexcept;

// Material colour and texture of a posed part (glTF base colour factor).
[[nodiscard]] std::array<float, 4> item_material_color(const Model &model, int material) noexcept;

} // namespace openminerz::modding::graphics
