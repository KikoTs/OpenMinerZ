// Mod creatures and NPCs as a client draws them (openminerz/modding/graphics.hpp):
// which clip a replicated animation state plays, and the cross-fade between
// states. The renderer side is in world_preview_mod.cpp.

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace openminerz::modding::graphics
{
namespace
{

[[nodiscard]] char lower(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool same_text(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

[[nodiscard]] bool contains_text(std::string_view text, std::string_view part) noexcept
{
    if (part.empty() || part.size() > text.size())
        return false;
    for (std::size_t start = 0U; start + part.size() <= text.size(); ++start)
        if (same_text(text.substr(start, part.size()), part))
            return true;
    return false;
}

// "Armature|Walk" -> "Walk": exporters put the armature's name in front.
[[nodiscard]] std::string_view clip_name(std::string_view name) noexcept
{
    const std::size_t bar = name.find_last_of('|');
    return bar == std::string_view::npos ? name : name.substr(bar + 1U);
}

} // namespace

int find_state_clip(const Model &model, std::string_view state) noexcept
{
    if (state.empty() || model.animations.empty())
        return -1;
    std::array<std::string_view, 3> names{state, {}, {}};
    if (same_text(state, "run"))
        names = {"run", "walk", {}};
    else if (same_text(state, "walk"))
        names = {"walk", "run", {}};
    else if (same_text(state, "hurt"))
        names = {"hurt", "hit", "damage"};
    else if (same_text(state, "death"))
        names = {"death", "die", "dead"};
    else if (same_text(state, "idle"))
        names = {"idle", "stand", {}};
    for (const std::string_view name : names)
    {
        if (name.empty())
            continue;
        // The clip of exactly that name first, then one that contains it
        // ("walk_cycle"), before the next stand-in is tried.
        for (std::size_t index = 0U; index < model.animations.size(); ++index)
            if (same_text(clip_name(model.animations[index].name), name))
                return static_cast<int>(index);
        for (std::size_t index = 0U; index < model.animations.size(); ++index)
            if (contains_text(clip_name(model.animations[index].name), name))
                return static_cast<int>(index);
    }
    return -1;
}

bool state_loops(std::string_view state) noexcept
{
    return !same_text(state, "attack") && !same_text(state, "hurt") && !same_text(state, "death");
}

void CreatureAnimator::play(const Model &model, std::string_view state)
{
    if (model_ != &model)
    {
        model_ = &model;
        count_ = 0U;
    }
    if (count_ > 0U && state == state_)
        return;
    const int clip = find_state_clip(model, state);
    const bool loop = state_loops(state);
    const float duration =
        clip >= 0 ? model.animations[static_cast<std::size_t>(clip)].duration : 0.0F;
    // (fix) A looping clip starts at this creature's own phase.
    const float start = loop ? phase_ * duration : 0.0F;
    state_ = state;
    if (count_ == 0U)
    {
        layers_[0] = {clip, start, loop, 1.0F};
        durations_[0] = duration;
        count_ = 1U;
        fade_ = 1.0F;
        return;
    }
    // Two states sharing one looping clip (walk and run without a run clip)
    // carry on where it was.
    if (clip == layers_[0].animation && loop && layers_[0].loop)
        return;
    layers_[1] = layers_[0];
    durations_[1] = durations_[0];
    layers_[0] = {clip, start, loop, 0.0F};
    durations_[0] = duration;
    layers_[1].weight = 1.0F;
    count_ = 2U;
    fade_ = 0.0F;
}

void CreatureAnimator::step(float seconds, float speed)
{
    if (!std::isfinite(seconds) || seconds < 0.0F)
        seconds = 0.0F;
    if (!std::isfinite(speed) || speed < 0.0F)
        speed = 1.0F;
    for (std::size_t index = 0U; index < count_; ++index)
    {
        PoseLayer &layer = layers_[index];
        layer.seconds += seconds * speed * (layer.loop ? pace_ : 1.0F);
        const float duration = durations_[index];
        // A looping clip wraps here, so its clock never loses precision; one
        // that plays once stops at its end and holds the last frame.
        if (duration > 1.0e-6F)
            layer.seconds = layer.loop ? std::fmod(layer.seconds, duration)
                                       : std::min(layer.seconds, duration);
    }
    if (count_ == 2U)
    {
        fade_ += seconds / blend_seconds;
        if (fade_ >= 1.0F)
        {
            fade_ = 1.0F;
            count_ = 1U;
        }
        layers_[0].weight = fade_;
        layers_[1].weight = 1.0F - fade_;
    }
    if (count_ == 1U)
        layers_[0].weight = 1.0F;
}

void CreatureAnimator::vary(std::uint64_t seed) noexcept
{
    // splitmix64: two well-spread fractions from one id.
    std::uint64_t bits = seed + 0x9E3779B97F4A7C15ULL;
    bits = (bits ^ (bits >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    bits = (bits ^ (bits >> 27U)) * 0x94D049BB133111EBULL;
    bits ^= bits >> 31U;
    phase_ = static_cast<float>(bits & 0xFFFFU) / 65536.0F;
    pace_ = 0.92F + 0.16F * static_cast<float>((bits >> 16U) & 0xFFFFU) / 65535.0F;
}

bool CreatureAnimator::finished() const noexcept
{
    if (count_ == 0U || layers_[0].animation < 0)
        return true;
    return !layers_[0].loop && layers_[0].seconds >= durations_[0];
}

} // namespace openminerz::modding::graphics
