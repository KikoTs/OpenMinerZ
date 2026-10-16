// Animation timelines recorded from the shipped Castle Miner Z 1.6.3.8 and
// 1.9.9.8 clients by tools/OriginalClientInterop/AnimationParityProbe. The two
// .inc files are generated (--pin); this wrapper only gives them a shape.
#pragma once

#include <span>

namespace animation_parity_pinned
{

struct Item final
{
    int id;
    int mode;
    bool gun;
    bool grenade;
    long long reload_ticks;
    int capacity;
};

struct Scenario final
{
    const char *name;
    long long ticks;
    int frames;
    int item;
    const char *local;
    const char *remote;
    unsigned long long hash;
};

struct Generation final
{
    const char *version;
    bool modern;
    std::span<const Item> items;
    std::span<const Scenario> scenarios;
};

#define ANIMATION_PINNED_VERSION(version, modern)

#define ANIMATION_PINNED_SCENARIO(name, ticks, frames, item, local, remote, hash)
#define ANIMATION_PINNED_ITEM(id, mode, gun, grenade, reload, capacity)                           \
    {id, mode, gun, grenade, reload, capacity},
inline constexpr Item classic_items[] = {
#include "animation_parity_pinned_classic.inc"
};
inline constexpr Item modern_items[] = {
#include "animation_parity_pinned_modern.inc"
};
#undef ANIMATION_PINNED_ITEM
#undef ANIMATION_PINNED_SCENARIO

#define ANIMATION_PINNED_ITEM(id, mode, gun, grenade, reload, capacity)
#define ANIMATION_PINNED_SCENARIO(name, ticks, frames, item, local, remote, hash)                 \
    {name, ticks, frames, item, local, remote, hash},
inline constexpr Scenario classic_scenarios[] = {
#include "animation_parity_pinned_classic.inc"
};
inline constexpr Scenario modern_scenarios[] = {
#include "animation_parity_pinned_modern.inc"
};
#undef ANIMATION_PINNED_ITEM
#undef ANIMATION_PINNED_SCENARIO
#undef ANIMATION_PINNED_VERSION

inline constexpr Generation generations[] = {
    {"1.6.3.8", false, classic_items, classic_scenarios},
    {"1.9.9.8", true, modern_items, modern_scenarios},
};

} // namespace animation_parity_pinned
