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

// ---------------------------------------------------------------------------
// Pause and "the game stops" contract
// ---------------------------------------------------------------------------

// What is presented in front of a world session.
enum class WorldOverlay
{
    // The first-person view. The HUD, inventory, crate, chat and name dialogs
    // draw on top of it and never stop anything.
    none,
    // InGameMenu, or Options/Achievements pushed on top of it.
    pause_menu,
    // An in-world picker such as the teleport-station list. The original pushes
    // these as dialogs over the HUD; only InGameMenu ever touches DoUpdate.
    dialog,
    // Main menu, browsers, world list: no session is being played.
    frontend,
};

// world_view: the first-person screen is current. pause_overlay: the pause menu
// or a screen opened from it. world_dialog: an in-world picker over a world
// that has actually started.
[[nodiscard]] constexpr WorldOverlay classify_world_overlay(bool world_view, bool pause_overlay,
                                                            bool world_dialog) noexcept
{
    if (world_view)
        return WorldOverlay::none;
    if (pause_overlay)
        return WorldOverlay::pause_menu;
    if (world_dialog)
        return WorldOverlay::dialog;
    return WorldOverlay::frontend;
}

// InGameMenu.OnPushed/OnPoped set mainScene.DoUpdate and GameScreen.DoUpdate to
// false only when !IsOnlineGame. IsOnlineGame is "the session is a PlayerMatch",
// which a host is from the moment it starts hosting, with or without guests, so
// a hosting or joined player's world keeps running behind the menu and behind
// everything opened from it. A private game freezes until the menu is popped.
[[nodiscard]] constexpr bool world_simulation_active(WorldOverlay overlay, bool online_session,
                                                     bool waiting_for_world) noexcept
{
    if (waiting_for_world)
        return false;
    switch (overlay)
    {
    case WorldOverlay::none:
    case WorldOverlay::dialog:
        return true;
    case WorldOverlay::pause_menu:
        return online_session;
    case WorldOverlay::frontend:
        return false;
    }
    return false;
}

// The detached F1 inspector camera parks the fixed step (physics, enemies,
// pickups, replication) in a private world only. Online it is one more view
// with the controls released: a host must keep simulating for its guests and
// every peer must keep replicating itself.
[[nodiscard]] constexpr bool world_fixed_step_active(bool simulation_active, bool inspector_view,
                                                     bool online_session) noexcept
{
    return simulation_active && (!inspector_view || online_session);
}

// InGameHUD.OnLostFocus clears every control the moment another screen is
// pushed over the HUD, and a screen above it swallows input, so nothing reaches
// Player.ProcessInput until the HUD is on top again. The player still obeys
// physics and can be hurt meanwhile.
[[nodiscard]] constexpr bool local_player_controls_enabled(WorldOverlay overlay,
                                                           bool waiting_for_world,
                                                           bool inspector_view,
                                                           bool inventory_open,
                                                           bool text_entry) noexcept
{
    return overlay == WorldOverlay::none && !waiting_for_world && !inspector_view &&
           !inventory_open && !text_entry;
}

// Player.ProcessInput feeds zero movement to the rig while Dead and skips the
// held item entirely; only the respawn buttons are read.
[[nodiscard]] constexpr bool local_player_movement_enabled(bool controls_enabled,
                                                           bool player_alive) noexcept
{
    return controls_enabled && player_alive;
}

// The frame on which the controls are taken away is the frame every held action
// (fire, mine, shoulder, grenade cook, reload) must end.
[[nodiscard]] constexpr bool local_controls_released(bool were_enabled, bool enabled) noexcept
{
    return were_enabled && !enabled;
}

// Only the first-person view may capture the pointer; every menu needs a cursor.
[[nodiscard]] constexpr bool pointer_capture_allowed(WorldOverlay overlay) noexcept
{
    return overlay == WorldOverlay::none;
}

// A level-triggered input that was held while the controls were away must be
// let go before it counts again. The original reads Jump as Trigger.Pressed (an
// edge), so the Space/A press that picks "Resume" never becomes a jump.
class HeldInputLatch final
{
  public:
    constexpr void arm() noexcept
    {
        armed_ = true;
    }
    [[nodiscard]] constexpr bool filter(bool held) noexcept
    {
        if (!armed_)
            return held;
        if (!held)
            armed_ = false;
        return false;
    }
    [[nodiscard]] constexpr bool armed() const noexcept
    {
        return armed_;
    }

  private:
    bool armed_{};
};

// Longest stretch of wall-clock time one update may simulate. A stall of any
// length (a blocked window drag, a debugger, a suspended laptop) therefore
// costs at most this much game time; nothing fast-forwards to catch up.
inline constexpr float maximum_update_delta_seconds = 0.05F;

[[nodiscard]] constexpr float bounded_update_delta(double elapsed_seconds) noexcept
{
    // A NaN or negative interval (clock trouble) simulates nothing.
    if (!(elapsed_seconds > 0.0))
        return 0.0F;
    return elapsed_seconds >= static_cast<double>(maximum_update_delta_seconds)
               ? maximum_update_delta_seconds
               : static_cast<float>(elapsed_seconds);
}

// Frames ticked from inside a blocked event poll: Windows runs a modal loop
// while a window is dragged, resized or showing its system menu, and the frame
// loop does not get the thread back until the user lets go.
enum class StalledLoopSignal
{
    // The live-resize timer SDL keeps running inside the modal loop. It exists
    // nowhere else, so every tick is a frame.
    modal_timer,
    // The window moved or changed size. Each step of a drag reports one, but so
    // does ordinary polling, where the frame loop is alive and must not be
    // doubled: it only counts once the loop has visibly stalled.
    window_changed,
};

inline constexpr double modal_timer_minimum_interval_seconds = 0.004;
inline constexpr double stalled_loop_threshold_seconds = 0.05;
// Game time one such frame may make up, paid in bounded updates. A host keeps
// real time through a drag as long as a tick arrives four times a second.
inline constexpr double maximum_stalled_frame_debt_seconds = 0.25;

[[nodiscard]] constexpr bool stalled_loop_frame_due(StalledLoopSignal signal,
                                                    double seconds_since_update) noexcept
{
    return seconds_since_update >= (signal == StalledLoopSignal::modal_timer
                                        ? modal_timer_minimum_interval_seconds
                                        : stalled_loop_threshold_seconds);
}

} // namespace openminerz
