#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz
{

struct AudioOutputDevice final
{
    std::string id;
    std::string name;
};

// The first entry is always the Windows system-default route (empty id).
[[nodiscard]] std::vector<AudioOutputDevice> enumerate_audio_output_devices();

// Plays cues from the selected Classic/Modern content pack's original XACT banks.
class OriginalSoundBank final
{
  public:
    explicit OriginalSoundBank(std::string_view output_id = {});
    ~OriginalSoundBank();

    OriginalSoundBank(const OriginalSoundBank &) = delete;
    OriginalSoundBank &operator=(const OriginalSoundBank &) = delete;

    // (Graphics area) A sound pack's replacement for a one-shot cue: called
    // with the cue and, for a 3D cue, where it plays; true when the pack
    // played it and the original must not. Unset (no pack): never called.
    using CueOverride = std::function<bool(std::string_view cue, const world::Vec3 *position)>;
    static void set_cue_override(CueOverride hook);

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::string_view output_name() const noexcept;
    [[nodiscard]] bool output_muted() const noexcept;
    // Selected Windows endpoint scalar, or -1 when it cannot be queried.
    [[nodiscard]] float output_volume() const noexcept;
    void set_levels(float master, float music, float effects, float ambience, bool muted);
    void play_menu_music();
    void stop_music();
    // One-shot in-game song (Song1..Song6, SpaceTheme); plays to completion.
    void play_song(std::string_view name);
    void play_break(world::BlockType block);
    void play_place();
    void play_footstep();
    void play_pickup();
    void play_click();
    void play_drop();
    void play_pistol();
    void play_reload();
    // Plays any original XACT cue by name (e.g. the per-gun shoot/reload cues).
    void play_cue(std::string_view cue);
    // Owns an explicitly stoppable XACT cue instance. Used by authored looping
    // effects such as the guided launcher's lock tone.
    void start_looping_cue(std::string_view cue);
    void stop_looping_cue(std::string_view cue);
    // 3D playback through the original XACT curves: the listener follows the
    // player each frame, cues attenuate/pan with the authored RPC curves.
    void update_listener(world::Vec3 position, float yaw);
    void play_cue_at(std::string_view cue, world::Vec3 position);
    // BaseZombie owns one SoundCue3D per enemy. A frame brackets the enemies
    // currently in Chase; each live cue is repositioned and restarted by XACT
    // only after its authored wave/cue finishes.
    void begin_enemy_voice_frame();
    void sync_enemy_voice(std::uint32_t id, std::string_view cue, world::Vec3 position);
    void end_enemy_voice_frame();
    void clear_enemy_voices();
    // FireballEntity owns one continuously playing 3D flight cue until impact
    // or removal. Position and velocity are reapplied as the projectile moves.
    void sync_fireball_cue(std::uint32_t id, std::string_view cue, world::Vec3 position,
                           world::Vec3 velocity);
    void stop_fireball_cue(std::uint32_t id);
    void clear_fireball_cues();
    // The four original ambient beds (Birds/Crickets/Drips/lostSouls) plus the
    // Outdoors reverb morph (1 = dry surface, 0 = cave preset).
    void set_ambience(float day, float night, float cave, float hell, float outdoors);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
