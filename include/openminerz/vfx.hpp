#pragma once

// The Enhanced ("Rebirth") presentation's effects layer: smoke, fire, sparks,
// explosions, shockwaves, debris, and the light they throw.
//
// PRESENTATION ONLY. Nothing here reads or changes what the simulation or the
// network sees: an effect may LOOK bigger than the explosion that caused it, it
// can never BE bigger. The Original ("Classic") presentation never constructs a
// vfx::System; it keeps the 2011 game's particle recipes, drawn the old way.
//
// HOW IT FITS TOGETHER
//
//   EffectDesc   a recipe: emitters (+ sub-emitters), lights, camera shake and
//                flash. Plain data with static storage; authored once.
//   System       the simulation. spawn() an effect, update() on the fixed tick,
//                build_frame() for a camera. Fixed pools, no allocation in play.
//   Frame        what one camera sees: sorted particle instances, debris cubes,
//                point lights, the smoke the eye stands in, shake and flash.
//   VfxRenderer  (src/vfx/vfx_renderer.hpp) draws a Frame inside the post chain,
//                after the resolve and before motion blur, depth of field and
//                bloom: soft, lit, shadowed, fogged by the one atmosphere, into
//                a reduced-resolution buffer with a depth-aware upsample.
//
// HOW TO ADD AN EFFECT
//
//   1. Write a function that returns a `const EffectDesc &` built once (a
//      function-local static), in src/vfx/vfx_library.cpp or a file of your own
//      next to it (src/vfx/vfx_library_combat.cpp is the place for weapon,
//      dragon and mining effects). Declare it in namespace vfx::effects below.
//   2. List it in all_effects() (vfx_library.cpp) so `find_effect("name")` and
//      the capture switch OPENMINERZ_DIAGNOSTIC_VFX=name,dist=8,age=0.4 find it.
//   3. Spawn it: WorldPreviewRenderer::spawn_effect(desc, params) from the
//      renderer or the app (it does nothing in the Classic presentation), or
//      System::spawn directly. Keep the handle only if the effect follows
//      something (System::move) or is stopped early (System::kill).
//
// A WORKED EXAMPLE: a muzzle puff with a few sparks and a brief light.
//
//   const EffectDesc &muzzle_puff()
//   {
//       static const EffectDesc desc = [] {
//           EffectDesc effect;
//           effect.name = "muzzle_puff";
//           effect.cull_distance = 60.0F;       // spawns nothing farther away
//           EmitterDesc smoke;                  // 1) a small rising puff
//           smoke.material = Material::smoke;
//           smoke.burst = 4;
//           smoke.life = {0.5F, 0.9F};
//           smoke.speed = {0.4F, 1.2F};         // along the spawn direction
//           smoke.spread = 0.35F;               // 0 a line .. 1 every direction
//           smoke.drag = 3.0F;
//           smoke.buoyancy = 0.6F;
//           smoke.turbulence = 0.8F;
//           smoke.size = {0.10F, 0.16F};
//           smoke.size_over_life = Curve::line(1.0F, 3.2F);
//           smoke.alpha = Curve::keys({{0.0F, 0.0F}, {0.12F, 0.5F}, {1.0F, 0.0F}});
//           smoke.erosion = Curve::line(0.05F, 0.7F);  // breaks up as it ages
//           smoke.color = ColorCurve::constant({0.55F, 0.55F, 0.56F});
//           effect.emitters.push_back(smoke);
//           EmitterDesc sparks;                 // 2) sparks that bounce
//           sparks.material = Material::spark;
//           sparks.burst = 6;
//           sparks.life = {0.15F, 0.4F};
//           sparks.speed = {4.0F, 9.0F};
//           sparks.spread = 0.25F;
//           sparks.gravity = 9.0F;
//           sparks.collide = true;
//           sparks.bounce = 0.4F;
//           sparks.size = {0.012F, 0.02F};
//           sparks.stretch = 0.035F;            // seconds of motion drawn as a streak
//           sparks.emissive = Curve::line(9.0F, 2.0F);     // HDR, feeds the bloom
//           sparks.color = ColorCurve::keys({{0.0F, {1.0F, 0.8F, 0.5F}},
//                                            {1.0F, {1.0F, 0.25F, 0.03F}}});
//           effect.emitters.push_back(sparks);
//           LightDesc light;                    // 3) lights terrain and creatures
//           light.color = {1.0F, 0.62F, 0.28F};
//           light.radius = 6.0F;
//           light.duration = 0.07F;
//           light.intensity = Curve::line(2.2F, 0.0F);
//           effect.lights.push_back(light);
//           return effect;
//       }();
//       return desc;
//   }
//
//   SpawnParams params;
//   params.position = muzzle;  params.direction = aim;  params.velocity = player_velocity;
//   renderer.spawn_effect(muzzle_puff(), params);
//
// UNITS: blocks (metres), seconds, linear-light colour. Sizes are billboard
// half-extents. Curves run over a particle's life (0 birth .. 1 death).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace openminerz::vfx
{

struct Vec3 final
{
    float x{};
    float y{};
    float z{};

    constexpr Vec3 operator+(Vec3 other) const noexcept
    {
        return {x + other.x, y + other.y, z + other.z};
    }
    constexpr Vec3 operator-(Vec3 other) const noexcept
    {
        return {x - other.x, y - other.y, z - other.z};
    }
    constexpr Vec3 operator*(float scale) const noexcept
    {
        return {x * scale, y * scale, z * scale};
    }
    constexpr Vec3 &operator+=(Vec3 other) noexcept
    {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }
};

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 a) noexcept
{
    return std::sqrt(dot(a, a));
}
// The unit vector along `a`, or `fallback` when `a` has no direction.
[[nodiscard]] inline Vec3 normalized(Vec3 a, Vec3 fallback = {0.0F, 1.0F, 0.0F}) noexcept
{
    const float size = length(a);
    return size > 1.0e-6F ? a * (1.0F / size) : fallback;
}

struct Color final
{
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
};

// A value drawn uniformly between two bounds for each particle.
struct Range final
{
    float min{};
    float max{};

    constexpr Range() noexcept = default;
    constexpr Range(float value) noexcept : min(value), max(value) {} // NOLINT: a fixed value
    constexpr Range(float low, float high) noexcept : min(low), max(high) {}
    [[nodiscard]] constexpr float pick(float unit) const noexcept
    {
        return min + (max - min) * unit;
    }
};

// A piecewise-linear curve over 0..1 with up to six keys. Before the first key
// and after the last it holds their values.
struct Curve final
{
    static constexpr std::size_t capacity = 6U;
    std::array<float, capacity> time{};
    std::array<float, capacity> value{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    std::uint8_t count{1U};

    [[nodiscard]] static constexpr Curve constant(float amount) noexcept
    {
        Curve curve;
        curve.value[0] = amount;
        return curve;
    }
    [[nodiscard]] static constexpr Curve line(float from, float to) noexcept
    {
        Curve curve;
        curve.count = 2U;
        curve.time = {0.0F, 1.0F};
        curve.value[0] = from;
        curve.value[1] = to;
        return curve;
    }
    // Keys as {time, value}, in rising time order.
    [[nodiscard]] static constexpr Curve keys(
        std::initializer_list<std::pair<float, float>> list) noexcept
    {
        Curve curve;
        curve.count = 0U;
        for (const auto &[at_time, amount] : list)
        {
            if (curve.count >= capacity)
                break;
            curve.time[curve.count] = at_time;
            curve.value[curve.count] = amount;
            ++curve.count;
        }
        if (curve.count == 0U)
            curve.count = 1U;
        return curve;
    }
    [[nodiscard]] constexpr float at(float t) const noexcept
    {
        if (count <= 1U || t <= time[0])
            return value[0];
        for (std::size_t index = 1U; index < count; ++index)
            if (t < time[index])
            {
                const float span = time[index] - time[index - 1U];
                const float share = span > 1.0e-6F ? (t - time[index - 1U]) / span : 1.0F;
                return value[index - 1U] + (value[index] - value[index - 1U]) * share;
            }
        return value[count - 1U];
    }
};

// The same for a colour, with up to four keys.
struct ColorCurve final
{
    static constexpr std::size_t capacity = 4U;
    std::array<float, capacity> time{};
    std::array<Color, capacity> value{};
    std::uint8_t count{1U};

    [[nodiscard]] static constexpr ColorCurve constant(Color color) noexcept
    {
        ColorCurve curve;
        curve.value[0] = color;
        return curve;
    }
    [[nodiscard]] static constexpr ColorCurve keys(
        std::initializer_list<std::pair<float, Color>> list) noexcept
    {
        ColorCurve curve;
        curve.count = 0U;
        for (const auto &[at_time, color] : list)
        {
            if (curve.count >= capacity)
                break;
            curve.time[curve.count] = at_time;
            curve.value[curve.count] = color;
            ++curve.count;
        }
        if (curve.count == 0U)
            curve.count = 1U;
        return curve;
    }
    [[nodiscard]] constexpr Color at(float t) const noexcept
    {
        if (count <= 1U || t <= time[0])
            return value[0];
        for (std::size_t index = 1U; index < count; ++index)
            if (t < time[index])
            {
                const float span = time[index] - time[index - 1U];
                const float share = span > 1.0e-6F ? (t - time[index - 1U]) / span : 1.0F;
                const Color &a = value[index - 1U];
                const Color &b = value[index];
                return {a.r + (b.r - a.r) * share, a.g + (b.g - a.g) * share,
                        a.b + (b.b - a.b) * share};
            }
        return value[count - 1U];
    }
};

// What a particle is made of, which decides how it is shaded and blended.
enum class Material : std::uint8_t
{
    // Lit gas from the billowing flipbook: takes sun, sky, shadow and nearby
    // effect lights, has a lit and a dark side. With `temperature` above zero
    // it glows as fire and darkens into smoke as it cools.
    smoke,
    // The same gas from the flame flipbook (tongues that taper upward).
    flame,
    // A hot streak along its motion. Emissive, additive, full resolution.
    spark,
    // A soft round emissive glow (flash, flare, ember halo). Additive.
    glow,
    // Refraction only: an expanding ring that bends the picture behind it.
    shockwave,
    // Refraction only: shimmering hot air.
    heat,
    // A small lit cube of a block's own texture, drawn with the world's
    // opaque geometry (it is shadowed, shaded and occluded like a block).
    debris,
};

enum class Shape : std::uint8_t
{
    // Every particle starts at the emitter.
    point,
    // Anywhere inside a sphere of `shape_radius`.
    sphere,
    // On a ring of `shape_radius` around the effect's direction, moving
    // outward along the ring's plane (a ground dust ring).
    ring,
};

struct EmitterDesc final
{
    Material material{Material::smoke};
    // --- When and how many. A burst at `delay`, a stream of `rate` per second
    // for `duration` after it, or for as long as the effect lives (`loop`).
    int burst{};
    float rate{};
    float delay{};
    float duration{};
    bool loop{};
    // Spawned only by another emitter's particles (see `sub_emitter`).
    bool sub_only{};
    // --- Where and which way. Directions are in the effect's frame: +Y is
    // the direction the effect was spawned with.
    Shape shape{Shape::point};
    float shape_radius{};
    Vec3 offset{};
    Vec3 direction{0.0F, 1.0F, 0.0F};
    // 0 exactly along `direction` .. 0.5 a hemisphere .. 1 every direction.
    float spread{1.0F};
    Range speed{};
    // Added in world space after the above (an upward kick, a drift).
    Vec3 velocity_bias{};
    // Share of the effect's own velocity a new particle starts with.
    float inherit_velocity{};
    // --- How long and how it moves.
    Range life{1.0F};
    // Speed lost per second (v *= exp(-drag * dt)).
    float drag{};
    // Downward and upward acceleration, blocks per second squared.
    float gravity{};
    float buoyancy{};
    // Curl-noise swirl: acceleration, and how large the eddies are (blocks).
    float turbulence{};
    float turbulence_scale{2.0F};
    // Sparks and debris stop at terrain: speed kept along the normal after a
    // bounce (0 sticks) and share of sliding speed lost.
    bool collide{};
    float bounce{0.35F};
    float friction{0.4F};
    // --- How it looks.
    Range size{0.1F};
    Curve size_over_life{};
    Range rotation{0.0F, 6.2831853F};
    Range spin{};
    ColorCurve color{};
    Curve alpha{};
    // HDR gain of emissive materials; for gas, the gain of its glow.
    Curve emissive{};
    // Gas only: how hot. 0 smoke; around 1 orange fire; 2 and up white-hot.
    Curve temperature{Curve::constant(0.0F)};
    // Gas only: how much of the puff's thin edge is eaten away (0 .. 1), so it
    // tears into wisps instead of fading like a card.
    Curve erosion{Curve::constant(0.0F)};
    // A cold palette (blue-white) for the glow of gas and sparks.
    bool cold{};
    // Spark: seconds of its own motion drawn as a streak.
    float stretch{};
    // Refraction materials: strength of the bend (fractions of the screen).
    float distortion{0.02F};
    // Flipbook frames per second (gas).
    float frame_rate{14.0F};
    // --- Sub-emitters: index of another emitter of the same effect whose
    // particles are born at this one's particles. `sub_rate` per second per
    // particle while it lives (a trail), `sub_on_death` when it ends.
    int sub_emitter{-1};
    float sub_rate{};
    int sub_on_death{};
};

// A point light that follows the effect. It lights terrain, creatures, other
// particles and the first-person body.
struct LightDesc final
{
    Color color{1.0F, 0.6F, 0.3F};
    float radius{8.0F};
    // Over `duration` seconds; a duration of zero lasts as long as the effect
    // does and reads the curve at 0.
    Curve intensity{};
    float duration{};
    // 0 steady .. 1 flickers down to nothing.
    float flicker{};
    Vec3 offset{};
};

struct EffectDesc final
{
    std::string_view name{};
    std::vector<EmitterDesc> emitters;
    std::vector<LightDesc> lights;
    // Camera shake at the effect, how long it lasts and how far it is felt.
    float shake{};
    float shake_duration{0.5F};
    float shake_radius{30.0F};
    // A brief brightening of the whole picture for an eye that can see it.
    float flash{};
    float flash_duration{0.12F};
    float flash_radius{30.0F};
    Color flash_color{1.0F, 0.86F, 0.66F};
    // Level of detail: emission thins from full at `lod_start` to `lod_floor`
    // of it at `lod_end`, and nothing is spawned beyond `cull_distance`.
    float lod_start{32.0F};
    float lod_end{96.0F};
    float lod_floor{0.3F};
    float cull_distance{220.0F};
    // The most particles one instance of this effect may have alive.
    int max_particles{512};
    // Higher keeps its particles when the global budget is full.
    int priority{};
};

struct SpawnParams final
{
    Vec3 position{};
    // The effect's +Y: a surface normal, a barrel, "up" by default.
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Vec3 velocity{};
    // Multiplies sizes, speeds and radii: the same recipe, bigger.
    float scale{1.0F};
    // Multiplies emission counts, light and glow.
    float intensity{1.0F};
    Color tint{};
    // Tiles of the blocks an explosion broke, for debris: retail atlas tiles
    // (0..63), or (Polish) a mod block's BlockTextureTable tiles (64 and up),
    // which the renderer draws from its mod debris atlas. With none named,
    // debris emitters spawn nothing.
    std::array<std::uint8_t, 4> debris_tiles{};
    std::uint8_t debris_tile_count{};
};

struct EffectHandle final
{
    std::uint16_t slot{};
    std::uint16_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return generation != 0U;
    }
};

// What the world tells the effects: where terrain is, and how much sky and
// block (torch, lava) light reaches a point. Implemented by the renderer.
struct LightSample final
{
    float sky{1.0F};
    float block{};
};
class WorldQuery
{
  public:
    virtual ~WorldQuery() = default;
    [[nodiscard]] virtual bool solid(int x, int y, int z) const = 0;
    [[nodiscard]] virtual LightSample light(Vec3 position) const = 0;
};

// Global limits, derived from the "Particles" quality setting.
struct Budget final
{
    std::size_t max_particles{8192U};
    // Emission multiplier (the particle density setting).
    float density{1.0F};
    std::size_t max_lights{12U};
};
[[nodiscard]] Budget budget_for_density(float density) noexcept;

// One particle as the GPU reads it (80 bytes; the input layout of
// vfx_renderer.cpp mirrors this field for field).
struct Instance final
{
    float position[3]{};
    float size{};
    float color[3]{};
    float alpha{};
    // Unit direction of motion and the streak's extra half-length (sparks).
    float axis[3]{};
    float stretch{};
    float rotation{};
    float frame{};
    float temperature{};
    float erosion{};
    float sky_light{1.0F};
    float block_light{};
    // Material as a float, plus 16 when the palette is cold.
    float material{};
    float emissive{};
};
static_assert(sizeof(Instance) == 80U, "the vfx input layout requires the 80-byte instance");

struct DebrisCube final
{
    Vec3 position{};
    float half_size{};
    // Rotation about a fixed tilted axis.
    float angle{};
    Vec3 axis{0.0F, 1.0F, 0.0F};
    std::uint8_t tile{};
    float alpha{1.0F};
    float sky_light{1.0F};
    float block_light{};
};

struct PointLight final
{
    Vec3 position{};
    float radius{};
    Color color{};
};

struct View final
{
    Vec3 eye{};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    // How far behind the simulation this draw sits (fixed-tick interpolation).
    float lag_seconds{};
};

// Everything one camera draws of the effects this frame.
struct Frame final
{
    // Gas and glows, farthest first: one sorted, premultiplied pass.
    std::vector<Instance> soft;
    // Sparks and embers: additive, full resolution.
    std::vector<Instance> sharp;
    // Shockwaves and heat: refraction only.
    std::vector<Instance> distortion;
    std::vector<DebrisCube> debris;
    std::vector<PointLight> lights;
    // The smoke the eye stands in: optical depth per block along a sight
    // line, its colour and the light that reaches it. Drawn as one fog, not
    // as full-screen cards.
    float immersion{};
    // How far along a sight line that smoke reaches (blocks).
    float immersion_reach{};
    Color immersion_color{};
    LightSample immersion_light{};
    // 0..1 camera shake and the brightening of the picture.
    float shake{};
    float flash{};
    Color flash_color{};

    void clear() noexcept;
};

// --- Pure helpers, exposed for the renderer and the tests. ----------------

// A divergence-free swirl at `position` (unit-ish magnitude): the curl of a
// smooth periodic potential, read from a small table.
[[nodiscard]] Vec3 curl_noise(Vec3 position) noexcept;
// Emission multiplier for an effect `distance` blocks from the listener.
[[nodiscard]] float lod_factor(const EffectDesc &desc, float distance) noexcept;
// Sort key of a view depth: larger depth, smaller key, so an ascending sort
// draws the farthest first. Monotonic; depths past `far_depth` share key 0.
[[nodiscard]] std::uint16_t depth_sort_key(float view_depth, float far_depth = 400.0F) noexcept;
// Stable sort of `instances` by `keys` (ascending), in place. `scratch` is
// working storage kept by the caller so nothing is allocated per frame.
void sort_by_key(std::vector<Instance> &instances, std::vector<std::uint16_t> &keys,
                 std::vector<Instance> &scratch, std::vector<std::uint16_t> &key_scratch);
// How much a light matters to a viewer at `eye`: brightness over the square of
// its distance, softened by its radius. Used to pick the lights that fit the cap.
[[nodiscard]] float light_importance(const PointLight &light, Vec3 eye) noexcept;
// Keeps the `limit` most important lights (order not preserved).
void select_lights(std::vector<PointLight> &lights, Vec3 eye, std::size_t limit);
// Fade of a particle of half-extent `size` whose centre is `view_depth` in
// front of the eye: 0 at the lens, 1 once it is a few of its own sizes away.
// It bounds how much of the screen any one particle can cover.
[[nodiscard]] float near_fade(float view_depth, float size) noexcept;
// The light a point `distance` from a light of `radius` receives (0 .. 1).
[[nodiscard]] float light_falloff(float distance, float radius) noexcept;

class System final
{
  public:
    static constexpr std::size_t max_effects = 384U;
    // Emitters of one effect past this many are ignored.
    static constexpr std::size_t max_emitters = 12U;

    explicit System(std::size_t particle_capacity = 16384U);

    void set_world(const WorldQuery *world) noexcept;
    void set_budget(const Budget &budget) noexcept;
    [[nodiscard]] const Budget &budget() const noexcept;

    // Starts an effect. `desc` must outlive it (recipes are static). Returns
    // an invalid handle when nothing could be started.
    EffectHandle spawn(const EffectDesc &desc, const SpawnParams &params);
    [[nodiscard]] bool alive(EffectHandle handle) const noexcept;
    // Follow a moving point: streams are laid along the path it took.
    void move(EffectHandle handle, Vec3 position, Vec3 velocity) noexcept;
    void set_direction(EffectHandle handle, Vec3 direction) noexcept;
    void set_intensity(EffectHandle handle, float intensity) noexcept;
    // Stops emitting; particles already born live out their lives. `immediate`
    // removes those too.
    void kill(EffectHandle handle, bool immediate = false) noexcept;
    void clear() noexcept;

    // Advances everything by `delta` seconds. `listener` is the eye (levels
    // of detail and what is worth simulating).
    void update(float delta, Vec3 listener);
    void build_frame(const View &view, Frame &frame);

    [[nodiscard]] std::size_t particle_count() const noexcept;
    [[nodiscard]] std::size_t effect_count() const noexcept;
    [[nodiscard]] std::size_t particle_capacity() const noexcept;
    // Particles of one effect (0 for a dead handle).
    [[nodiscard]] std::size_t particle_count(EffectHandle handle) const noexcept;
    // Particles turned away or evicted because a budget was full, ever.
    [[nodiscard]] std::size_t dropped_particles() const noexcept;

  private:
    struct Particle final
    {
        Vec3 position{};
        Vec3 velocity{};
        float age{};
        float life{1.0F};
        float size{};
        float rotation{};
        float spin{};
        float seed{};
        float trail{};
        std::uint16_t effect{};
        std::uint8_t emitter{};
        std::uint8_t resting{};
        std::uint8_t tile{};
    };
    struct Effect final
    {
        const EffectDesc *desc{};
        SpawnParams params{};
        Vec3 previous_position{};
        Vec3 basis_x{1.0F, 0.0F, 0.0F};
        Vec3 basis_z{0.0F, 0.0F, 1.0F};
        float age{};
        float lod{1.0F};
        LightSample light{};
        float light_timer{};
        std::array<float, max_emitters> accumulators{};
        std::array<bool, max_emitters> burst_done{};
        std::uint16_t generation{};
        std::uint32_t particles{};
        std::uint32_t random{};
        bool active{};
        bool emitting{};
        bool visible_flash{true};
    };

    [[nodiscard]] Effect *resolve(EffectHandle handle) noexcept;
    [[nodiscard]] const Effect *resolve(EffectHandle handle) const noexcept;
    [[nodiscard]] float random01(Effect &effect) noexcept;
    bool emit(Effect &effect, std::uint16_t slot, std::size_t emitter_index, Vec3 origin,
              Vec3 inherited, float pre_age);
    void set_basis(Effect &effect) noexcept;
    void remove_particle(std::size_t index) noexcept;

    std::vector<Particle> particles_;
    std::size_t capacity_{};
    std::array<Effect, max_effects> effects_{};
    std::uint16_t next_slot_{};
    std::size_t active_effects_{};
    const WorldQuery *world_{};
    Budget budget_{};
    float clock_{};
    std::size_t dropped_{};
    std::uint32_t eviction_cursor_{};
    std::vector<std::uint16_t> sort_keys_;
    std::vector<std::uint16_t> sort_key_scratch_;
    std::vector<Instance> sort_scratch_;
};

// --- The recipes (vfx_library.cpp). ----------------------------------------
namespace effects
{
// Smoke.
[[nodiscard]] const EffectDesc &impact_puff();     // a bullet or blade striking a surface
[[nodiscard]] const EffectDesc &dust_puff();       // dry dust kicked off a surface; tint it
[[nodiscard]] const EffectDesc &lingering_smoke(); // a slow column that hangs and drifts
[[nodiscard]] const EffectDesc &fuse_smoke();      // the thin thread above a lit charge
// Fire.
[[nodiscard]] const EffectDesc &torch_flame();     // looping: flame, embers, heat, flicker
[[nodiscard]] const EffectDesc &fire();            // looping: a block-sized fire
[[nodiscard]] const EffectDesc &lava_bubble();     // one bubble bursting on a lava surface
[[nodiscard]] const EffectDesc &lava_spurt();      // a jet of sparks and a lick of flame
[[nodiscard]] const EffectDesc &rocket_trail();    // looping: exhaust flame and smoke
// Explosions, smallest to largest.
[[nodiscard]] const EffectDesc &explosion_grenade();
[[nodiscard]] const EffectDesc &explosion_rocket();
[[nodiscard]] const EffectDesc &explosion_tnt();
[[nodiscard]] const EffectDesc &explosion_c4();
} // namespace effects

// Every registered recipe, for diagnostics and tests.
[[nodiscard]] std::span<const EffectDesc *const> all_effects();
[[nodiscard]] const EffectDesc *find_effect(std::string_view name) noexcept;

} // namespace openminerz::vfx
