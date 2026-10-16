#pragma once

// Weapon, dragon, mining and creature effects of the Enhanced ("Rebirth")
// presentation, authored on the effects layer of openminerz/vfx.hpp. The
// recipes are in src/vfx/vfx_library_combat.cpp and are listed in
// vfx::all_effects(), so `vfx::find_effect("dragon_fireball")` and the capture
// switch OPENMINERZ_DIAGNOSTIC_VFX=<name> find every one of them.
//
// PRESENTATION ONLY, Enhanced only. An effect here may LOOK bigger than what
// caused it; it is never read back. In particular the dragon's fireball is
// DRAWN with a head about 1.6 blocks in radius, while what it HITS stays
// world::fireball_sweep's retail 1.14-unit cube (+/-0.57), its speed
// DragonType.FireballVelocity and its blast HandleDetonateFireballMessage's.
//
// HOW THE ORIGINAL STAGES THEM (DNA.Drawing.Particles, sizes are sprite
// diameters in world units; read from the shipped 1.6.3.8 / 1.9.9.8 content):
//   FireBallEffect  local, 10/s, 1 s, 3-5 -> 5-20      the roiling head
//   FireGlow        local additive, 10/s, 1 s, 6-12 -> 10-30
//   FireTrail       50/s, <=1 s, 1-15 -> 0             flame that shrinks away
//   SmokeTrail      50/s, <=3 s, 0-3 -> 13-15          smoke that swells
//   Detonate        FlashEffect + FirePuff + BigSmokePuff + BigRockBlast
// with exactly TWO looks, chosen by DragonType.DamageType: DESTRUCTION (the
// fire, forest, sand and undead dragons) and ICE (IceBallEffect, IceGlow,
// IceTrail, PuffTrail, IceFlash, IcePuff, BigPuff, BigSnowBlast). The dragon's
// death is FlashEffect + FirePuff + BigSmokePuff (or the ice three) on the body.
// Digging is SparksEffect + RocksEffect at the struck point; a bullet on
// terrain SparksEffect + SmokeEffect; a laser bolt BlasterFlash (at 0.01
// scale) + SparksEffect + SmokeEffect; a shot on a creature SmokeEffect; a shot
// on the dragon FlashEffect; the muzzle flash a one-frame additive model at the
// barrel tip scaled 0.75-1.25 with a random roll.
// Origins, directions and timings of all of those are kept.

#include "openminerz/vfx.hpp"
#include "openminerz/world/block.hpp"

#include <cstdint>
#include <span>

namespace openminerz::vfx
{

// What a block is made of, as far as something striking it is concerned.
enum class Surface : std::uint8_t
{
    stone,
    dirt,
    sand,
    snow,
    ice,
    wood,
    leaves,
    metal,
    ore,
    crystal,
    lava,
    slime,
    glass,
    count,
};
[[nodiscard]] Surface surface_of(world::BlockType block) noexcept;

// The colour of the dust a block gives off (linear), from the block's own.
[[nodiscard]] Color dust_color(world::BlockType block) noexcept;

// What struck it.
enum class Strike : std::uint8_t
{
    // One swing of a tool that has not broken the block yet.
    tool,
    // The block coming apart (the swing or blast that removed it).
    shatter,
    bullet,
    // A laser bolt. Its coloured flash and sparks are effects::bolt_impact().
    bolt,
    // A block set down by a player.
    place,
    count,
};

// How much dust a strike raises: SpawnParams::intensity and ::scale for the
// dust recipe of that strike (effects::strike_dust), tinted with dust_color().
struct DustAmount final
{
    float intensity{1.0F};
    float scale{1.0F};
};
[[nodiscard]] DustAmount strike_dust_amount(Strike strike, Surface surface) noexcept;

enum class Muzzle : std::uint8_t
{
    pistol,
    rifle,
    shotgun,
    // An energy weapon: tint it with the bolt's colour.
    laser,
    count,
};

// How large a dragon fireball is DRAWN (blocks): the radius of the head and of
// the halo around it. world::fireball_sweep never sees these.
inline constexpr float fireball_drawn_radius = 1.6F;
inline constexpr float fireball_halo_radius = 4.6F;
// The same for the Original presentation's procedural volume, for comparison.
inline constexpr float classic_fireball_drawn_radius = 0.65F;
// Seconds into the dragon's attack clip at which the shot leaves (retail's
// 1.1333 s); the mouth glow builds up to it. Read by presentation only.
inline constexpr float dragon_shot_clip_seconds = 1.1333334F;
// 0 outside the wind-up, rising to 1 as the shot leaves, then gone.
[[nodiscard]] float dragon_charge(bool attacking, float clip_seconds) noexcept;

namespace effects
{
// --- Blocks. Pass the struck block's atlas tiles in SpawnParams::debris_tiles
// and the surface normal (or the open side) as the direction. ---
// Chips of the block itself, sparks and a contact flash. Untinted.
[[nodiscard]] const EffectDesc &surface_strike(Strike strike, Surface surface);
// The dust of the same strike: tint with dust_color(), size with
// strike_dust_amount().
[[nodiscard]] const EffectDesc &strike_dust(Strike strike);

// --- The dragon. `ice` selects the ICE package. ---
// Looping; follow the fireball with System::move and set_direction.
[[nodiscard]] const EffectDesc &fireball_flight(bool ice);
// The burst out of the mouth as a shot leaves, along the shot.
[[nodiscard]] const EffectDesc &fireball_launch(bool ice);
// Looping; the glow gathering in the mouth. Drive it with set_intensity.
[[nodiscard]] const EffectDesc &dragon_breath(bool ice);
// What a fire dragon's shot adds to the shared explosion: a rain of embers.
[[nodiscard]] const EffectDesc &fireball_embers();
// One patch of ground left burning by it. Spawn a few around the impact.
[[nodiscard]] const EffectDesc &ground_fire();
// The ice dragon's whole detonation, and the frost it leaves on the ground.
[[nodiscard]] const EffectDesc &iceball_burst();
[[nodiscard]] const EffectDesc &ground_frost();
// A shot striking the dragon, and the dragon dying.
[[nodiscard]] const EffectDesc &dragon_hit();
[[nodiscard]] const EffectDesc &dragon_death(bool ice);

// --- Weapons. ---
// Along the barrel. Follows the shooter for its few hundredths of a second.
[[nodiscard]] const EffectDesc &muzzle_flash(Muzzle muzzle);
// One spent case; pass a brass-coloured tile in debris_tiles.
[[nodiscard]] const EffectDesc &shell_casing();
// A bolt burning into something: tint with the bolt's colour.
[[nodiscard]] const EffectDesc &bolt_impact();
// Behind a launcher as a rocket leaves (direction = out of the back).
[[nodiscard]] const EffectDesc &rocket_backblast();
// A blade on something hard. Tint for a laser sword.
[[nodiscard]] const EffectDesc &melee_sparks();
// Looping; the spitting fuse of a thrown grenade.
[[nodiscard]] const EffectDesc &grenade_fuse();

// --- Creatures and the rest. ---
[[nodiscard]] const EffectDesc &creature_hit();
[[nodiscard]] const EffectDesc &creature_death();
// A player or creature appearing out of thin air.
[[nodiscard]] const EffectDesc &arrival();
[[nodiscard]] const EffectDesc &pickup_sparkle();
[[nodiscard]] const EffectDesc &pickup_collect();
// Dry ground under a running foot; tint with dust_color().
[[nodiscard]] const EffectDesc &footstep_dust();
} // namespace effects

// Every recipe above, for all_effects().
[[nodiscard]] std::span<const EffectDesc *const> combat_effects();

} // namespace openminerz::vfx
