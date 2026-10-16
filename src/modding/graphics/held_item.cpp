// (Items) Held and dropped mod items (openminerz/modding/held_item.hpp): hold
// styles, the grip in the hand, the item's own clips and its posed mesh.
// PRESENTATION ONLY.

#include "openminerz/modding/held_item.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openminerz::modding::graphics
{
namespace
{

constexpr float pi = std::numbers::pi_v<float>;

[[nodiscard]] float smoothstep(float edge0, float edge1, float value) noexcept
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] Matrix4 translation(Vec3 offset) noexcept
{
    return matrix_compose(offset, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F});
}

[[nodiscard]] Matrix4 uniform_scale(float scale) noexcept
{
    return matrix_compose({}, {0.0F, 0.0F, 0.0F, 1.0F}, {scale, scale, scale});
}

// A rotation of `radians` about a unit axis.
[[nodiscard]] Matrix4 rotation(Vec3 axis, float radians) noexcept
{
    const float half = radians * 0.5F;
    const float sine = std::sin(half);
    return matrix_compose({}, {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half)}, {1.0F, 1.0F, 1.0F});
}

// Columns: where the item's X, Y and Z axes go.
[[nodiscard]] Matrix4 basis(Vec3 x, Vec3 y, Vec3 z) noexcept
{
    return {x.x, x.y, x.z, 0.0F, y.x, y.y, y.z, 0.0F, z.x, z.y, z.z, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

// The fist grip, in the knife's model space (knife.omsm: the handle runs
// along +Z from the pommel at z -0.045 to the guard at z 0.05, its middle
// at x 0.015, y 0.11). The item's +Y (the blade) goes up the knife's handle
// and its face (+Z) turns to the holder; then the blade leans 35 degrees
// away from the face, so a long one points ahead past the crosshair the way
// the knife's does instead of standing up in front of the eye.
[[nodiscard]] Matrix4 fist_grip() noexcept
{
    return matrix_multiply(matrix_multiply(translation({0.015F, 0.11F, 0.0F}),
                                           basis({0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 0.0F})),
                           rotation({1.0F, 0.0F, 0.0F}, 35.0F * pi / 180.0F));
}

// The front grip, in a held block's space: the block's cube is centred on
// its origin; the item stands up where the block sits, its face (+Z) turned
// to the holder.
[[nodiscard]] Matrix4 front_grip() noexcept
{
    return matrix_multiply(rotation({1.0F, 0.0F, 0.0F}, pi * 0.5F), rotation({0.0F, 1.0F, 0.0F}, pi * 0.5F));
}

} // namespace

HoldStyle resolved_hold(HoldStyle hold, InventoryItemId fallback) noexcept
{
    if (hold != HoldStyle::fallback)
        return hold;
    switch (item_player_animation_mode(fallback))
    {
    case PlayerAnimationMode::pick:
        return HoldStyle::tool;
    case PlayerAnimationMode::block:
        return HoldStyle::block;
    case PlayerAnimationMode::fist:
        return HoldStyle::fist;
    default:
        return HoldStyle::item;
    }
}

PlayerAnimationMode hold_animation_mode(HoldStyle hold, InventoryItemId fallback) noexcept
{
    switch (hold)
    {
    case HoldStyle::item:
        return PlayerAnimationMode::generic;
    case HoldStyle::swing:
    case HoldStyle::tool:
        return PlayerAnimationMode::pick;
    case HoldStyle::block:
    case HoldStyle::eat:
    case HoldStyle::drink:
        return PlayerAnimationMode::block;
    case HoldStyle::fist:
        return PlayerAnimationMode::fist;
    case HoldStyle::fallback:
        break;
    }
    return item_player_animation_mode(fallback);
}

bool held_in_front(HoldStyle resolved) noexcept
{
    return resolved == HoldStyle::block || resolved == HoldStyle::eat || resolved == HoldStyle::drink;
}

InventoryItemId grip_reference_item(HoldStyle resolved) noexcept
{
    return held_in_front(resolved) ? InventoryItemId::dirt_block : InventoryItemId::knife;
}

// ------------------------------------------------------------- the clips

void HeldItemAnimator::play(int animation, bool loop)
{
    if (count_ > 0U)
    {
        layers_[1] = layers_[0];
        durations_[1] = durations_[0];
        count_ = 2U;
        fade_ = 0.0F;
    }
    else
    {
        count_ = 1U;
        fade_ = 1.0F;
    }
    layers_[0] = {animation, 0.0F, loop, 1.0F};
    durations_[0] = model_ != nullptr && animation >= 0 &&
                            static_cast<std::size_t>(animation) < model_->animations.size()
                        ? model_->animations[static_cast<std::size_t>(animation)].duration
                        : 0.0F;
}

void HeldItemAnimator::update(std::uint32_t item, const ItemLook &look, bool use_started,
                              float use_seconds, float seconds)
{
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0F, 0.25F) : 0.0F;
    if (item != item_ || look.model.get() != model_)
    {
        // A different item in the hand starts over, without a fade.
        item_ = item;
        model_ = look.model.get();
        idle_ = model_ != nullptr ? find_state_clip(*model_, "idle") : -1;
        equip_ = model_ != nullptr ? find_state_clip(*model_, "equip") : -1;
        use_ = model_ != nullptr ? find_state_clip(*model_, "use") : -1;
        count_ = 0U;
        use_elapsed_ = -1.0F;
        if (equip_ >= 0)
            play(equip_, false);
        else if (idle_ >= 0)
            play(idle_, true);
    }
    look_ = look;
    if (item_ == 0U)
        return;
    if (use_started)
    {
        use_elapsed_ = 0.0F;
        use_seconds_ = std::clamp(use_seconds, 0.1F, 10.0F);
        if (use_ >= 0)
            play(use_, false);
    }
    else if (use_elapsed_ >= 0.0F)
    {
        use_elapsed_ += seconds;
        if (use_elapsed_ >= use_seconds_)
            use_elapsed_ = -1.0F;
    }
    for (std::size_t index = 0U; index < count_; ++index)
        layers_[index].seconds += seconds;
    fade_ = std::min(1.0F, fade_ + seconds / blend_seconds);
    // A one-shot that has run out gives way to the idle (or the bind pose).
    if (count_ >= 1U && !layers_[0].loop && layers_[0].seconds >= durations_[0] &&
        layers_[0].animation != idle_)
        play(idle_, true);
    if (count_ == 2U && fade_ >= 1.0F)
        count_ = 1U;
    layers_[0].weight = count_ == 2U ? fade_ : 1.0F;
    layers_[1].weight = 1.0F - fade_;
}

HeldItemPose HeldItemAnimator::pose() const
{
    HeldItemPose pose;
    pose.look = look_;
    if (model_ != nullptr)
    {
        pose.layer_count = count_;
        pose.layers = layers_;
    }
    pose.use_progress = use_elapsed_ >= 0.0F ? std::clamp(use_elapsed_ / use_seconds_, 0.0F, 1.0F) : 0.0F;
    return pose;
}

float mouth_clip_fraction(float progress) noexcept
{
    progress = std::clamp(progress, 0.0F, 1.0F);
    // BlockUse lifts the hand to the face early in the clip and lowers it
    // after; the bites are a small dip and rise there.
    constexpr float mouth = 0.2F;
    if (progress < 0.18F)
        return mouth * smoothstep(0.0F, 0.18F, progress);
    if (progress > 0.82F)
        return mouth + (1.0F - mouth) * smoothstep(0.82F, 1.0F, progress);
    const float bites = (progress - 0.18F) / 0.64F;
    return mouth - 0.06F * std::abs(std::sin(bites * pi * 3.0F));
}

// ------------------------------------------------------------- the mesh

void pose_item_model(const Model &model, std::span<const PoseLayer> layers,
                     std::vector<PosedItemPart> &parts, ItemPoseScratch &scratch)
{
    if (layers.empty())
        evaluate_pose(model, -1, 0.0F, false, scratch.world);
    else
        evaluate_blended_pose(model, layers, scratch.world);
    parts.resize(model.primitives.size());
    for (std::size_t index = 0U; index < model.primitives.size(); ++index)
    {
        const ModelPrimitive &primitive = model.primitives[index];
        PosedItemPart &part = parts[index];
        part.material = primitive.material;
        part.vertices.clear();
        const bool skinned = primitive.skin >= 0;
        if (skinned)
            skin_matrices(model, primitive.skin, scratch.world, scratch.skin);
        const Matrix4 rigid = !skinned && primitive.node >= 0 &&
                                      static_cast<std::size_t>(primitive.node) < scratch.world.size()
                                  ? scratch.world[static_cast<std::size_t>(primitive.node)]
                                  : matrix_identity();
        part.vertices.reserve(primitive.indices.size());
        for (std::size_t corner = 0U; corner + 2U < primitive.indices.size(); corner += 3U)
            // glTF fronts wind counter-clockwise, the retail models clockwise.
            for (const std::size_t offset : {0U, 2U, 1U})
            {
                const std::uint32_t at = primitive.indices[corner + offset];
                if (at >= primitive.vertices.size())
                    continue;
                const ModelVertex &source = primitive.vertices[at];
                PosedItemVertex vertex;
                vertex.uv = source.uv;
                if (skinned && !scratch.skin.empty())
                {
                    Vec3 point{};
                    Vec3 normal{};
                    for (std::size_t slot = 0U; slot < 4U; ++slot)
                    {
                        const float weight = source.weights[slot];
                        if (weight <= 0.0F || source.joints[slot] >= scratch.skin.size())
                            continue;
                        const Matrix4 &joint = scratch.skin[source.joints[slot]];
                        point += transform_point(joint, source.position) * weight;
                        normal += transform_direction(joint, source.normal) * weight;
                    }
                    vertex.position = point;
                    vertex.normal = normal;
                }
                else
                {
                    vertex.position = transform_point(rigid, source.position);
                    vertex.normal = transform_direction(rigid, source.normal);
                }
                const float length = std::sqrt(vertex.normal.x * vertex.normal.x + vertex.normal.y * vertex.normal.y +
                                               vertex.normal.z * vertex.normal.z);
                vertex.normal = length > 1.0e-6F ? vertex.normal * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
                part.vertices.push_back(vertex);
            }
        // A triangle cut short by a bad index leaves no stray corners.
        part.vertices.resize(part.vertices.size() - part.vertices.size() % 3U);
    }
}

Matrix4 held_item_transform(const ItemLook &look, float use_progress, float reference_scale,
                            bool icon) noexcept
{
    const HoldStyle hold = look.hold;
    const bool front = held_in_front(hold);
    const float undo = reference_scale > 1.0e-4F && std::isfinite(reference_scale) ? 1.0F / reference_scale : 1.0F;
    Matrix4 result = matrix_multiply(front ? front_grip() : fist_grip(), uniform_scale(undo));
    // Eat and drink: the item's own motion at the mouth, about the grip.
    if (use_progress > 0.0F && (hold == HoldStyle::eat || hold == HoldStyle::drink))
    {
        const float there = smoothstep(0.1F, 0.25F, use_progress) * (1.0F - smoothstep(0.8F, 0.95F, use_progress));
        // The raised hand holds the block's face up to the eye; the item
        // turns back to face the holder.
        result = matrix_multiply(result, rotation({1.0F, 0.0F, 0.0F}, -0.25F * pi * there));
        if (hold == HoldStyle::eat)
        {
            const float bite = std::sin((use_progress - 0.1F) * pi * 7.0F) * there;
            result = matrix_multiply(result, translation({0.0F, 0.015F * bite, -0.01F * std::abs(bite)}));
            result = matrix_multiply(result, rotation({1.0F, 0.0F, 0.0F}, 0.12F * bite));
        }
        else
        {
            // Tipped towards the holder and over to the middle, as poured.
            result = matrix_multiply(result, rotation({0.0F, 0.0F, 1.0F}, 0.85F * there));
            result = matrix_multiply(result, rotation({1.0F, 0.0F, 0.0F}, 0.35F * there));
        }
    }
    result = matrix_multiply(result, uniform_scale(std::clamp(look.scale, 0.05F, 4.0F)));
    if (!icon)
        return result;
    // An icon (one unit square in the item's XY plane, its face +Z towards
    // the holder, as a model's flat is).
    if (hold == HoldStyle::swing || hold == HoldStyle::tool)
    {
        // A sword sprite: held a fifth of the way up its diagonal, the
        // diagonal along the blade.
        constexpr float size = 0.5F;
        return matrix_multiply(result,
                               matrix_multiply(uniform_scale(size),
                                               matrix_multiply(translation({0.0F, 0.4243F, 0.0F}),
                                                               rotation({0.0F, 0.0F, 1.0F}, pi * 0.25F))));
    }
    constexpr float size = 0.17F;
    if (front)
        return matrix_multiply(result, uniform_scale(size));
    // In the palm or the fist: upright, standing on the grip.
    return matrix_multiply(result, matrix_multiply(uniform_scale(size), translation({0.0F, 0.5F, 0.0F})));
}

Matrix4 dropped_item_transform(const ItemLook &look, Vec3 center, float spin) noexcept
{
    float extent = 0.3F;
    Vec3 middle{};
    if (look.model != nullptr)
    {
        const Vec3 size = look.model->bounds_max - look.model->bounds_min;
        extent = std::max({size.x, size.y, size.z, 0.01F});
        middle = (look.model->bounds_min + look.model->bounds_max) * 0.5F;
    }
    // At most 0.6 of a block across, whatever the model's size.
    const float scale = std::min(std::clamp(look.scale, 0.05F, 4.0F), 0.6F / extent);
    return matrix_multiply(translation(center),
                           matrix_multiply(rotation({0.0F, 1.0F, 0.0F}, -spin),
                                           matrix_multiply(uniform_scale(scale), translation(middle * -1.0F))));
}

std::array<float, 4> item_material_color(const Model &model, int material) noexcept
{
    if (material < 0 || static_cast<std::size_t>(material) >= model.materials.size())
        return {1.0F, 1.0F, 1.0F, 1.0F};
    return model.materials[static_cast<std::size_t>(material)].color_factor;
}

} // namespace openminerz::modding::graphics
