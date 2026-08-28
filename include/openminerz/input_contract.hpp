#pragma once

namespace openminerz
{

// Keep mouse routing independent from SDL so the shipped control contract can
// be covered by the core regression executable. In particular, secondary click
// is Activate + Shoulder/secondary-use; it is never a jump source.
enum class PointerButton
{
    other,
    primary,
    secondary,
};

struct WorldPointerPress final
{
    bool primary_action{};
    bool activate{};
    bool secondary_action{};
    bool jump{};
};

[[nodiscard]] constexpr WorldPointerPress world_pointer_press(PointerButton button) noexcept
{
    switch (button)
    {
    case PointerButton::primary:
        return {.primary_action = true};
    case PointerButton::secondary:
        return {.activate = true, .secondary_action = true};
    case PointerButton::other:
        return {};
    }
    return {};
}

enum class InventoryPointerRoute
{
    ignore,
    slot,
};

// Dropping is deliberately absent from pointer routing. Q is the sole explicit
// cursor/focused-slot drop command, so an imprecise click outside the inventory
// can never turn into an irreversible world action.
[[nodiscard]] constexpr InventoryPointerRoute inventory_pointer_route(bool has_slot) noexcept
{
    return has_slot ? InventoryPointerRoute::slot : InventoryPointerRoute::ignore;
}

// A world preview is also used as the loading presentation. Keep that visual
// state separate from an interactive world so queued mouse, keyboard, and pad
// events cannot turn the camera, mine, fire, or mutate the inventory while the
// terrain is still being installed.
[[nodiscard]] constexpr bool world_controls_enabled(bool world_preview_screen,
                                                    bool waiting_for_world) noexcept
{
    return world_preview_screen && !waiting_for_world;
}

} // namespace openminerz
