#pragma once

// mod-world: world generators from server and core mods.
//
// Scripts declare a world in `shared/` with the `terrain` and `noise`
// libraries (plain Luau tables; docs/modding/world-generation.md):
//
//   terrain.biome("redwood", { height = noise.add(14, noise.fbm({scale = 300}) * ...),
//                              surface = {{block = "grass"}, {block = "dirt", depth = 3}},
//                              trees = {{shape = "cone", chance = 0.02, height = {14, 24}}} })
//   terrain.define({ layout = { bands = {{to = 300, biome = "retail:classic"},
//                                        {to = 25000, biome = "redwood", blend = 200}} } })
//
// The service only collects those definitions per side. Once a side's scripts
// have loaded, build() resolves them (mod block keys through the gameplay
// registry) into a world::TerrainDefinition and compiles a world::CustomTerrain,
// which the app installs in every WorldGenerator of the session (chunks, far
// terrain, the gameplay backend). Nothing is evaluated in Luau and nothing runs
// on the chunk workers but native code (see the note on hooks below).
//
// Who may define a world: a mod whose calls carry `gameplay` (the server VM)
// or `network` (a server mod's client VM) - server and core mods. A client
// mod's call is refused: it could never be on a stock server's terrain.
//
// Per-chunk Luau hooks are deliberately not offered: chunks are generated on
// worker threads (and far terrain on more), while every Luau VM is
// main-thread-only with a wall-clock budget, so a hook would either serialize
// generation or need a VM per worker whose output could not be checked for
// determinism. Decoration stays with the server-side `worldgen.chunk` event,
// whose edits travel as ordinary chunk data.
//
// Retail parity: with no generator defined, nothing here is consulted, no
// sidecar record is written and no channel key is sent.

#include "openminerz/modding/api.hpp"
#include "openminerz/world/custom_terrain.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::world
{
class WorldGenerator;
}

namespace openminerz::modding
{
namespace worldgen
{

// The app's narrow interface (src/app_mod_world.inc). Main thread only.
class WorldgenBackend
{
  public:
    virtual ~WorldgenBackend() = default;
    // The retail fallback of a mod block (blocks.define) as the side's own
    // definitions know it; nullopt for an unknown key.
    [[nodiscard]] virtual std::optional<std::uint8_t> block_fallback(Side side,
                                                                     std::string_view key) const = 0;
    // The generator the session's terrain comes from (terrain.info,
    // terrain.height); null outside a world.
    [[nodiscard]] virtual const world::WorldGenerator *generator() const = 0;
    // The world seed scripts sample noise with (noise.sample).
    [[nodiscard]] virtual std::int32_t world_seed() const = 0;
};

// How a client shows a mod biome (presentation; not part of the digest).
struct BiomePresentation final
{
    std::string key; // "mod:name"
    std::string mod;
    std::string name;
    // atmosphere.push layer values (fog_color, sky_zenith, lut, ...); nil for
    // none.
    Value atmosphere;
    std::string music; // "music/track" in the mod's assets, "" for none
    double music_volume{1.0};
};

class WorldgenService final
{
  public:
    WorldgenService(ScriptHost &host, WorldgenBackend &backend);
    ~WorldgenService();
    WorldgenService(const WorldgenService &) = delete;
    WorldgenService &operator=(const WorldgenService &) = delete;

    void register_api(ApiRegistry &registry);

    // After a side's VM loaded its scripts: compiles what that side defined.
    // Null and an empty `error` when no mod defined a world; null and a
    // reason when the definition cannot be built (an unknown block or biome,
    // a bad expression).
    [[nodiscard]] std::shared_ptr<const world::CustomTerrain> build(Side side, std::string &error);
    [[nodiscard]] bool defines_world(Side side) const noexcept;
    // The mod (id, version) whose terrain.define the side ran.
    [[nodiscard]] std::pair<std::string, std::string> world_mod(Side side) const;
    [[nodiscard]] const std::vector<BiomePresentation> &biomes(Side side) const noexcept;
    // The terrain the session runs (terrain.info answers from it); null for
    // retail. The app sets it after installing.
    void set_active(std::shared_ptr<const world::CustomTerrain> terrain) noexcept;
    [[nodiscard]] const std::shared_ptr<const world::CustomTerrain> &active() const noexcept;
    // Forgets a side's definitions (its VM stopped).
    void clear(Side side);

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

[[nodiscard]] std::unique_ptr<WorldgenService> register_worldgen_api(ApiRegistry &registry,
                                                                     ScriptHost &host,
                                                                     WorldgenBackend &backend);

// ---------------------------------------------------------------------------
// The world's record of its generator (<world>.omw.mods/worldgen.omzw)
// ---------------------------------------------------------------------------

inline constexpr std::string_view generator_record_file = "worldgen.omzw";

struct GeneratorRecord final
{
    std::string mod_id;
    std::string mod_version;
    std::string digest;
    bool operator==(const GeneratorRecord &) const = default;
};

// nullopt when the folder has no record (a retail world). A record that
// cannot be read is reported through `error` and treated as present but
// unknown (mod_id "?"), so such a world is refused rather than regenerated.
[[nodiscard]] std::optional<GeneratorRecord> read_generator_record(
    const std::filesystem::path &sidecar_directory, std::string *error = nullptr);
bool write_generator_record(const std::filesystem::path &sidecar_directory,
                            const GeneratorRecord &record, std::string *error = nullptr);

enum class GeneratorVerdict : std::uint8_t
{
    retail,         // no record, no generator: retail terrain
    keep_retail,    // no record (a retail world) but a mod defines one: stays retail
    use,            // record and generator agree (or a new world): use it
    changed,        // same mod, different digest: use it, warn
    missing,        // the record's mod defines no world now: refuse to open
};

// What opening a world with `recorded` (nullopt: none) means when the
// session's mods define `current` (nullopt: no generator). `new_world` is a
// world created now (it takes the generator and its record).
[[nodiscard]] GeneratorVerdict judge_generator(const std::optional<GeneratorRecord> &recorded,
                                               const std::optional<GeneratorRecord> &current,
                                               bool new_world) noexcept;
// The message a player sees for a verdict (empty for retail / use).
[[nodiscard]] std::string generator_message(GeneratorVerdict verdict,
                                            const std::optional<GeneratorRecord> &recorded,
                                            const std::optional<GeneratorRecord> &current);

// The engine key (world store, engine namespace) a host publishes so its
// clients use the terrain it uses: {mod, version, digest} or {retail = true}.
inline constexpr std::string_view terrain_channel_key = "terrain";

} // namespace worldgen
} // namespace openminerz::modding
