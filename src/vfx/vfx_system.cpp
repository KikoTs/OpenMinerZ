// The effects simulation (openminerz/vfx.hpp). Pure CPU, no graphics device:
// fixed pools, nothing allocated once the first frame has been built.

#include "openminerz/vfx.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openminerz::vfx
{
namespace
{

constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;

[[nodiscard]] constexpr float saturate(float value) noexcept
{
    return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
}

// The curl of a smooth periodic vector potential, tabulated once. Each
// component of the potential is a sum of sines with whole-number wave vectors,
// so the table tiles and its curl is exact and divergence-free.
constexpr int curl_extent = 32;
struct CurlTable final
{
    std::vector<Vec3> cells;

    CurlTable() : cells(static_cast<std::size_t>(curl_extent) * curl_extent * curl_extent)
    {
        struct Wave final
        {
            int kx, ky, kz;
            float phase, amplitude;
        };
        // Three waves per component of the potential.
        constexpr std::array<std::array<Wave, 3>, 3> waves{{
            {{{1, 2, 0, 0.3F, 1.0F}, {0, 1, 3, 1.7F, 0.55F}, {2, 0, 1, 4.1F, 0.8F}}},
            {{{0, 1, 2, 2.2F, 1.0F}, {3, 0, 1, 0.9F, 0.55F}, {1, 2, 2, 5.3F, 0.7F}}},
            {{{2, 0, 1, 3.6F, 1.0F}, {1, 3, 0, 2.9F, 0.55F}, {0, 2, 3, 1.1F, 0.6F}}},
        }};
        const float unit = two_pi / static_cast<float>(curl_extent);
        double energy = 0.0;
        for (int z = 0; z < curl_extent; ++z)
            for (int y = 0; y < curl_extent; ++y)
                for (int x = 0; x < curl_extent; ++x)
                {
                    // d[c][axis] = d(potential component c) / d(axis)
                    float d[3][3]{};
                    for (int component = 0; component < 3; ++component)
                        for (const Wave &wave : waves[static_cast<std::size_t>(component)])
                        {
                            const float angle =
                                unit * static_cast<float>(wave.kx * x + wave.ky * y + wave.kz * z) +
                                wave.phase;
                            const float slope = std::cos(angle) * wave.amplitude;
                            d[component][0] += slope * static_cast<float>(wave.kx);
                            d[component][1] += slope * static_cast<float>(wave.ky);
                            d[component][2] += slope * static_cast<float>(wave.kz);
                        }
                    const Vec3 curl{d[2][1] - d[1][2], d[0][2] - d[2][0], d[1][0] - d[0][1]};
                    cells[static_cast<std::size_t>((z * curl_extent + y) * curl_extent + x)] = curl;
                    energy += static_cast<double>(dot(curl, curl));
                }
        const float scale =
            1.0F / std::sqrt(static_cast<float>(energy / static_cast<double>(cells.size())));
        for (Vec3 &cell : cells)
            cell = cell * scale;
    }

    [[nodiscard]] const Vec3 &at(int x, int y, int z) const noexcept
    {
        constexpr int mask = curl_extent - 1;
        return cells[static_cast<std::size_t>(((z & mask) * curl_extent + (y & mask)) * curl_extent +
                                              (x & mask))];
    }
};

[[nodiscard]] const CurlTable &curl_table()
{
    static const CurlTable table;
    return table;
}

[[nodiscard]] Vec3 mix(Vec3 a, Vec3 b, float share) noexcept
{
    return a + (b - a) * share;
}

// An orthonormal pair perpendicular to unit `axis`.
void perpendicular_basis(Vec3 axis, Vec3 &side, Vec3 &other) noexcept
{
    const Vec3 helper = std::abs(axis.y) < 0.92F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    side = normalized(cross(helper, axis), {1.0F, 0.0F, 0.0F});
    other = cross(axis, side);
}

[[nodiscard]] bool is_gas(Material material) noexcept
{
    return material == Material::smoke || material == Material::flame;
}

} // namespace

void Frame::clear() noexcept
{
    soft.clear();
    sharp.clear();
    distortion.clear();
    debris.clear();
    lights.clear();
    immersion = 0.0F;
    immersion_reach = 0.0F;
    immersion_color = {};
    immersion_light = {};
    shake = 0.0F;
    flash = 0.0F;
    flash_color = {};
}

Budget budget_for_density(float density) noexcept
{
    Budget budget;
    budget.density = std::clamp(density, 0.2F, 1.5F);
    budget.max_particles = static_cast<std::size_t>(
        std::clamp(8192.0F * budget.density, 1536.0F, 12288.0F));
    budget.max_lights = budget.density < 0.5F ? 4U : (budget.density < 0.9F ? 8U : 12U);
    if (budget.density > 1.25F)
        budget.max_lights = 16U;
    return budget;
}

Vec3 curl_noise(Vec3 position) noexcept
{
    const CurlTable &table = curl_table();
    const float fx = std::floor(position.x);
    const float fy = std::floor(position.y);
    const float fz = std::floor(position.z);
    const int x = static_cast<int>(fx);
    const int y = static_cast<int>(fy);
    const int z = static_cast<int>(fz);
    const float tx = position.x - fx;
    const float ty = position.y - fy;
    const float tz = position.z - fz;
    const Vec3 x00 = mix(table.at(x, y, z), table.at(x + 1, y, z), tx);
    const Vec3 x10 = mix(table.at(x, y + 1, z), table.at(x + 1, y + 1, z), tx);
    const Vec3 x01 = mix(table.at(x, y, z + 1), table.at(x + 1, y, z + 1), tx);
    const Vec3 x11 = mix(table.at(x, y + 1, z + 1), table.at(x + 1, y + 1, z + 1), tx);
    return mix(mix(x00, x10, ty), mix(x01, x11, ty), tz);
}

float lod_factor(const EffectDesc &desc, float distance) noexcept
{
    if (distance >= desc.cull_distance)
        return 0.0F;
    if (distance <= desc.lod_start)
        return 1.0F;
    if (distance >= desc.lod_end || desc.lod_end <= desc.lod_start)
        return desc.lod_floor;
    const float share = (distance - desc.lod_start) / (desc.lod_end - desc.lod_start);
    return 1.0F + (desc.lod_floor - 1.0F) * share;
}

std::uint16_t depth_sort_key(float view_depth, float far_depth) noexcept
{
    if (!(view_depth > 0.0F))
        return 65535U;
    if (view_depth >= far_depth)
        return 0U;
    // The square root spends the key's precision near the eye, where two
    // particles in the wrong order are easiest to see.
    const float share = std::sqrt(view_depth / far_depth);
    return static_cast<std::uint16_t>(65535U -
                                      static_cast<std::uint32_t>(share * 65535.0F));
}

void sort_by_key(std::vector<Instance> &instances, std::vector<std::uint16_t> &keys,
                 std::vector<Instance> &scratch, std::vector<std::uint16_t> &key_scratch)
{
    const std::size_t count = std::min(instances.size(), keys.size());
    if (count < 2U)
        return;
    scratch.resize(count);
    key_scratch.resize(count);
    // Two stable counting passes over the key's bytes.
    for (unsigned pass = 0U; pass < 2U; ++pass)
    {
        const unsigned shift = pass * 8U;
        std::array<std::uint32_t, 257> offsets{};
        for (std::size_t index = 0U; index < count; ++index)
            ++offsets[((keys[index] >> shift) & 0xFFU) + 1U];
        for (std::size_t bucket = 1U; bucket < offsets.size(); ++bucket)
            offsets[bucket] += offsets[bucket - 1U];
        for (std::size_t index = 0U; index < count; ++index)
        {
            const std::uint32_t destination = offsets[(keys[index] >> shift) & 0xFFU]++;
            scratch[destination] = instances[index];
            key_scratch[destination] = keys[index];
        }
        std::copy(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(count),
                  instances.begin());
        std::copy(key_scratch.begin(), key_scratch.begin() + static_cast<std::ptrdiff_t>(count),
                  keys.begin());
    }
}

float light_importance(const PointLight &light, Vec3 eye) noexcept
{
    const Vec3 offset = light.position - eye;
    const float brightness =
        0.2126F * light.color.r + 0.7152F * light.color.g + 0.0722F * light.color.b;
    const float radius_squared = light.radius * light.radius;
    return brightness * radius_squared / (dot(offset, offset) + radius_squared * 0.25F + 1.0F);
}

void select_lights(std::vector<PointLight> &lights, Vec3 eye, std::size_t limit)
{
    if (lights.size() <= limit)
        return;
    std::nth_element(lights.begin(), lights.begin() + static_cast<std::ptrdiff_t>(limit),
                     lights.end(), [eye](const PointLight &a, const PointLight &b) {
                         return light_importance(a, eye) > light_importance(b, eye);
                     });
    lights.resize(limit);
}

float near_fade(float view_depth, float size) noexcept
{
    const float begin = 0.10F + size * 0.50F;
    const float end = begin + 0.35F + size * 1.50F;
    const float share = saturate((view_depth - begin) / (end - begin));
    return share * share * (3.0F - 2.0F * share);
}

float light_falloff(float distance, float radius) noexcept
{
    if (radius <= 0.0F)
        return 0.0F;
    const float share = saturate(1.0F - distance / radius);
    return share * share;
}

System::System(std::size_t particle_capacity) : capacity_(std::max<std::size_t>(particle_capacity, 64U))
{
    particles_.reserve(capacity_);
    sort_keys_.reserve(capacity_);
    sort_key_scratch_.reserve(capacity_);
    sort_scratch_.reserve(capacity_);
    (void)curl_table();
}

void System::set_world(const WorldQuery *world) noexcept
{
    world_ = world;
}

void System::set_budget(const Budget &budget) noexcept
{
    budget_ = budget;
    budget_.max_particles = std::min(budget_.max_particles, capacity_);
}

const Budget &System::budget() const noexcept
{
    return budget_;
}

System::Effect *System::resolve(EffectHandle handle) noexcept
{
    if (!handle.valid() || handle.slot >= max_effects)
        return nullptr;
    Effect &effect = effects_[handle.slot];
    return effect.active && effect.generation == handle.generation ? &effect : nullptr;
}

const System::Effect *System::resolve(EffectHandle handle) const noexcept
{
    if (!handle.valid() || handle.slot >= max_effects)
        return nullptr;
    const Effect &effect = effects_[handle.slot];
    return effect.active && effect.generation == handle.generation ? &effect : nullptr;
}

float System::random01(Effect &effect) noexcept
{
    effect.random ^= effect.random << 13U;
    effect.random ^= effect.random >> 17U;
    effect.random ^= effect.random << 5U;
    return static_cast<float>(effect.random & 0x00FFFFFFU) / 16777216.0F;
}

void System::set_basis(Effect &effect) noexcept
{
    effect.params.direction = normalized(effect.params.direction);
    perpendicular_basis(effect.params.direction, effect.basis_x, effect.basis_z);
}

EffectHandle System::spawn(const EffectDesc &desc, const SpawnParams &params)
{
    for (std::size_t probe = 0U; probe < max_effects; ++probe)
    {
        const std::uint16_t slot =
            static_cast<std::uint16_t>((next_slot_ + probe) % max_effects);
        Effect &effect = effects_[slot];
        if (effect.active)
            continue;
        const std::uint16_t generation =
            static_cast<std::uint16_t>(effect.generation == 65535U ? 1U : effect.generation + 1U);
        effect = Effect{};
        effect.desc = &desc;
        effect.params = params;
        effect.previous_position = params.position;
        effect.generation = generation;
        effect.active = true;
        effect.emitting = true;
        effect.random = 0x9E3779B9U ^ (static_cast<std::uint32_t>(slot) * 0x85EBCA6BU) ^
                        (static_cast<std::uint32_t>(generation) << 16U) ^
                        static_cast<std::uint32_t>(clock_ * 977.0F);
        if (effect.random == 0U)
            effect.random = 0xC45A91E3U;
        set_basis(effect);
        effect.light = world_ != nullptr ? world_->light(params.position) : LightSample{};
        next_slot_ = static_cast<std::uint16_t>((slot + 1U) % max_effects);
        ++active_effects_;
        return {slot, generation};
    }
    return {};
}

bool System::alive(EffectHandle handle) const noexcept
{
    return resolve(handle) != nullptr;
}

void System::move(EffectHandle handle, Vec3 position, Vec3 velocity) noexcept
{
    if (Effect *effect = resolve(handle))
    {
        effect->params.position = position;
        effect->params.velocity = velocity;
    }
}

void System::set_direction(EffectHandle handle, Vec3 direction) noexcept
{
    if (Effect *effect = resolve(handle))
    {
        effect->params.direction = direction;
        set_basis(*effect);
    }
}

void System::set_intensity(EffectHandle handle, float intensity) noexcept
{
    if (Effect *effect = resolve(handle))
        effect->params.intensity = std::max(intensity, 0.0F);
}

void System::kill(EffectHandle handle, bool immediate) noexcept
{
    Effect *effect = resolve(handle);
    if (effect == nullptr)
        return;
    effect->emitting = false;
    if (!immediate)
        return;
    for (std::size_t index = 0U; index < particles_.size();)
        if (particles_[index].effect == handle.slot)
            remove_particle(index);
        else
            ++index;
    effect->active = false;
    --active_effects_;
}

void System::clear() noexcept
{
    particles_.clear();
    for (Effect &effect : effects_)
        effect.active = false;
    active_effects_ = 0U;
}

std::size_t System::particle_count() const noexcept
{
    return particles_.size();
}

std::size_t System::effect_count() const noexcept
{
    return active_effects_;
}

std::size_t System::particle_capacity() const noexcept
{
    return capacity_;
}

std::size_t System::particle_count(EffectHandle handle) const noexcept
{
    const Effect *effect = resolve(handle);
    return effect != nullptr ? effect->particles : 0U;
}

std::size_t System::dropped_particles() const noexcept
{
    return dropped_;
}

void System::remove_particle(std::size_t index) noexcept
{
    Effect &owner = effects_[particles_[index].effect];
    if (owner.particles > 0U)
        --owner.particles;
    particles_[index] = particles_.back();
    particles_.pop_back();
}

bool System::emit(Effect &effect, std::uint16_t slot, std::size_t emitter_index, Vec3 origin,
                  Vec3 inherited, float pre_age)
{
    const EffectDesc &desc = *effect.desc;
    const EmitterDesc &emitter = desc.emitters[emitter_index];
    // Debris is made of what was broken; with nothing named there is none.
    if (emitter.material == Material::debris && effect.params.debris_tile_count == 0U)
        return false;
    if (effect.particles >= static_cast<std::uint32_t>(std::max(desc.max_particles, 1)))
    {
        ++dropped_;
        return false;
    }
    Particle *particle{};
    const std::size_t limit = std::min(capacity_, budget_.max_particles);
    if (particles_.size() < limit)
    {
        particles_.emplace_back();
        particle = &particles_.back();
    }
    else
    {
        // The budget is full: take the place of the particle nearest its end
        // among a few probes, if it is no more important than this one.
        std::size_t victim = particles_.size();
        float oldest = 0.5F;
        for (int probe = 0; probe < 8; ++probe)
        {
            eviction_cursor_ = eviction_cursor_ * 1664525U + 1013904223U;
            const std::size_t index = (eviction_cursor_ >> 8U) % particles_.size();
            const Particle &candidate = particles_[index];
            const Effect &owner = effects_[candidate.effect];
            if (owner.desc == nullptr || owner.desc->priority > desc.priority)
                continue;
            const float spent = candidate.age / std::max(candidate.life, 1.0e-3F);
            if (spent > oldest)
            {
                oldest = spent;
                victim = index;
            }
        }
        ++dropped_;
        if (victim >= particles_.size())
            return false;
        Effect &owner = effects_[particles_[victim].effect];
        if (owner.particles > 0U)
            --owner.particles;
        particle = &particles_[victim];
        *particle = Particle{};
    }

    const float scale = effect.params.scale;
    const auto to_world = [&](Vec3 local) {
        return effect.basis_x * local.x + effect.params.direction * local.y +
               effect.basis_z * local.z;
    };
    Vec3 position = origin + to_world(emitter.offset) * scale;
    const Vec3 axis = normalized(to_world(emitter.direction), effect.params.direction);
    Vec3 heading = axis;
    if (emitter.shape == Shape::ring)
    {
        const float angle = random01(effect) * two_pi;
        const Vec3 outward = effect.basis_x * std::cos(angle) + effect.basis_z * std::sin(angle);
        position += outward * (emitter.shape_radius * scale);
        // Outward along the ring's plane, lifted toward the axis by `spread`.
        heading = normalized(outward + axis * (emitter.spread * random01(effect)), outward);
    }
    else
    {
        if (emitter.shape == Shape::sphere)
        {
            // Uniform in a ball by rejection (at most a few tries).
            Vec3 point{};
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                point = {random01(effect) * 2.0F - 1.0F, random01(effect) * 2.0F - 1.0F,
                         random01(effect) * 2.0F - 1.0F};
                if (dot(point, point) <= 1.0F)
                    break;
            }
            position += point * (emitter.shape_radius * scale);
        }
        const float cosine = 1.0F - 2.0F * saturate(emitter.spread) * random01(effect);
        const float sine = std::sqrt(std::max(0.0F, 1.0F - cosine * cosine));
        const float angle = random01(effect) * two_pi;
        Vec3 side;
        Vec3 other;
        perpendicular_basis(axis, side, other);
        heading = axis * cosine + side * (sine * std::cos(angle)) + other * (sine * std::sin(angle));
    }
    particle->velocity = heading * (emitter.speed.pick(random01(effect)) * scale) +
                         emitter.velocity_bias * scale + inherited * emitter.inherit_velocity;
    particle->life = std::max(emitter.life.pick(random01(effect)), 0.02F);
    particle->size = emitter.size.pick(random01(effect)) * scale;
    particle->rotation = emitter.rotation.pick(random01(effect));
    particle->spin = emitter.spin.pick(random01(effect));
    particle->seed = random01(effect);
    particle->effect = slot;
    particle->emitter = static_cast<std::uint8_t>(emitter_index);
    if (effect.params.debris_tile_count > 0U)
        particle->tile = effect.params.debris_tiles[static_cast<std::size_t>(
            random01(effect) * static_cast<float>(effect.params.debris_tile_count)) %
                                                      effect.params.debris_tile_count];
    particle->age = std::min(pre_age, particle->life * 0.5F);
    particle->position = position + particle->velocity * particle->age;
    ++effect.particles;
    return true;
}

void System::update(float delta, Vec3 listener)
{
    if (!(delta > 0.0F))
        return;
    delta = std::min(delta, 0.1F);
    clock_ += delta;

    for (std::size_t slot_index = 0U; slot_index < max_effects; ++slot_index)
    {
        Effect &effect = effects_[slot_index];
        if (!effect.active)
            continue;
        const std::uint16_t slot = static_cast<std::uint16_t>(slot_index);
        const EffectDesc &desc = *effect.desc;
        const float before = effect.age;
        effect.age += delta;
        const float distance = length(effect.params.position - listener);
        effect.lod = lod_factor(desc, distance) * budget_.density;
        if (before <= 0.0F && desc.flash > 0.0F && world_ != nullptr)
        {
            // A flash is seen, not felt: walk the sight line once.
            const Vec3 to_eye = listener - effect.params.position;
            const int steps = std::min(96, static_cast<int>(distance / 0.75F));
            for (int step = 2; step < steps; ++step)
            {
                const Vec3 point =
                    effect.params.position + to_eye * (static_cast<float>(step) / static_cast<float>(steps));
                if (world_->solid(static_cast<int>(std::floor(point.x)),
                                  static_cast<int>(std::floor(point.y)),
                                  static_cast<int>(std::floor(point.z))))
                {
                    effect.visible_flash = false;
                    break;
                }
            }
        }
        effect.light_timer += delta;
        if (effect.light_timer >= 0.5F && world_ != nullptr)
        {
            effect.light_timer = 0.0F;
            effect.light = world_->light(effect.params.position);
        }

        bool unfinished = false;
        const std::size_t emitter_count = std::min(desc.emitters.size(), max_emitters);
        for (std::size_t index = 0U; index < emitter_count && effect.emitting; ++index)
        {
            const EmitterDesc &emitter = desc.emitters[index];
            if (emitter.sub_only)
                continue;
            const float amount = effect.lod * effect.params.intensity;
            if (!effect.burst_done[index])
            {
                if (effect.age < emitter.delay)
                {
                    unfinished = true;
                }
                else
                {
                    effect.burst_done[index] = true;
                    if (emitter.burst > 0 && amount > 0.0F)
                    {
                        const float wanted = static_cast<float>(emitter.burst) * amount;
                        int count = static_cast<int>(wanted);
                        if (random01(effect) < wanted - static_cast<float>(count))
                            ++count;
                        // A burst that exists at all shows at least one particle.
                        count = std::max(count, 1);
                        for (int burst = 0; burst < count; ++burst)
                            (void)emit(effect, slot, index, effect.params.position,
                                       effect.params.velocity, 0.0F);
                    }
                }
            }
            if (emitter.rate <= 0.0F)
                continue;
            const float window_end =
                emitter.loop ? effect.age + 1.0F : emitter.delay + emitter.duration;
            if (effect.age < window_end)
                unfinished = true;
            const float from = std::max(before, emitter.delay);
            const float to = std::min(effect.age, window_end);
            if (to <= from || amount <= 0.0F)
                continue;
            effect.accumulators[index] += emitter.rate * amount * (to - from);
            const float total = effect.accumulators[index];
            int ordinal = 0;
            while (effect.accumulators[index] >= 1.0F)
            {
                effect.accumulators[index] -= 1.0F;
                // Lay the stream along the path the effect took this tick, and
                // age each particle by how long ago it would have been born.
                const float share =
                    saturate((static_cast<float>(ordinal) + 0.5F) / std::max(total, 1.0F));
                ++ordinal;
                const Vec3 origin =
                    mix(effect.params.position, effect.previous_position, share);
                (void)emit(effect, slot, index, origin, effect.params.velocity, share * delta);
            }
        }
        effect.previous_position = effect.params.position;
        if (!unfinished)
            effect.emitting = false;
        if (!effect.emitting && effect.particles == 0U)
        {
            float tail = std::max(desc.shake > 0.0F ? desc.shake_duration : 0.0F,
                                  desc.flash > 0.0F ? desc.flash_duration : 0.0F);
            for (const LightDesc &light : desc.lights)
                tail = std::max(tail, light.duration);
            if (effect.age >= tail)
            {
                effect.active = false;
                --active_effects_;
            }
        }
    }

    for (std::size_t index = 0U; index < particles_.size();)
    {
        Particle &particle = particles_[index];
        Effect &effect = effects_[particle.effect];
        const EmitterDesc &emitter = effect.desc->emitters[particle.emitter];
        particle.age += delta;
        if (particle.age >= particle.life)
        {
            if (emitter.sub_emitter >= 0 && emitter.sub_on_death > 0 &&
                static_cast<std::size_t>(emitter.sub_emitter) < effect.desc->emitters.size())
            {
                // Free the dying particle's place first: emit() may reuse any.
                const Vec3 position = particle.position;
                const Vec3 velocity = particle.velocity;
                const std::uint16_t slot = particle.effect;
                const int count = std::max(
                    1, static_cast<int>(static_cast<float>(emitter.sub_on_death) * effect.lod));
                remove_particle(index);
                for (int child = 0; child < count; ++child)
                    (void)emit(effect, slot, static_cast<std::size_t>(emitter.sub_emitter),
                               position, velocity, 0.0F);
                continue;
            }
            remove_particle(index);
            continue;
        }
        if (particle.resting == 0U)
        {
            Vec3 velocity = particle.velocity;
            if (emitter.turbulence > 0.0F)
            {
                const float frequency = 5.0F / std::max(emitter.turbulence_scale, 0.05F);
                const Vec3 sample{particle.position.x * frequency + particle.seed * 3.0F,
                                  particle.position.y * frequency - clock_ * 0.7F,
                                  particle.position.z * frequency + particle.seed * 5.0F};
                velocity += curl_noise(sample) * (emitter.turbulence * delta);
            }
            velocity.y += (emitter.buoyancy - emitter.gravity) * delta;
            if (emitter.drag > 0.0F)
                velocity = velocity * (1.0F / (1.0F + emitter.drag * delta));
            Vec3 next = particle.position + velocity * delta;
            if (emitter.collide && world_ != nullptr)
            {
                const auto cell = [](float value) { return static_cast<int>(std::floor(value)); };
                const bool gas = is_gas(emitter.material);
                const float bounce = gas ? 0.0F : emitter.bounce;
                bool hit = false;
                bool floor_hit = false;
                const Vec3 from = particle.position;
                if (world_->solid(cell(next.x), cell(from.y), cell(from.z)))
                {
                    velocity.x = -velocity.x * bounce;
                    next.x = from.x;
                    hit = true;
                }
                if (world_->solid(cell(next.x), cell(next.y), cell(from.z)))
                {
                    floor_hit = velocity.y < 0.0F;
                    velocity.y = -velocity.y * bounce;
                    next.y = from.y;
                    hit = true;
                }
                if (world_->solid(cell(next.x), cell(next.y), cell(next.z)))
                {
                    velocity.z = -velocity.z * bounce;
                    next.z = from.z;
                    hit = true;
                }
                if (hit && !gas)
                {
                    const float keep = 1.0F - saturate(emitter.friction);
                    velocity.x *= keep;
                    velocity.z *= keep;
                    particle.spin *= 0.5F;
                    if (floor_hit && dot(velocity, velocity) < 0.6F)
                    {
                        particle.resting = 1U;
                        velocity = {};
                    }
                }
            }
            particle.velocity = velocity;
            particle.position = next;
            particle.rotation += particle.spin * delta;
        }
        if (emitter.sub_emitter >= 0 && emitter.sub_rate > 0.0F && particle.resting == 0U &&
            static_cast<std::size_t>(emitter.sub_emitter) < effect.desc->emitters.size())
        {
            particle.trail += emitter.sub_rate * effect.lod * delta;
            if (particle.trail >= 1.0F)
            {
                // emit() may replace or append particles; work from copies.
                const Vec3 position = particle.position;
                const Vec3 velocity = particle.velocity;
                const std::uint16_t slot = particle.effect;
                int count = static_cast<int>(particle.trail);
                particle.trail -= static_cast<float>(count);
                count = std::min(count, 4);
                for (int child = 0; child < count; ++child)
                    (void)emit(effect, slot, static_cast<std::size_t>(emitter.sub_emitter),
                               position, velocity, 0.0F);
            }
        }
        ++index;
    }
}

void System::build_frame(const View &view, Frame &frame)
{
    frame.clear();
    sort_keys_.clear();
    float immersion_weight = 0.0F;
    float immersion_reach = 0.0F;
    Vec3 immersion_color{};
    float immersion_sky = 0.0F;
    float immersion_block = 0.0F;

    for (const Particle &particle : particles_)
    {
        const Effect &effect = effects_[particle.effect];
        const EmitterDesc &emitter = effect.desc->emitters[particle.emitter];
        const float life_share = saturate(particle.age / particle.life);
        const float lag =
            particle.resting != 0U ? 0.0F : std::min(view.lag_seconds, std::max(particle.age, 0.0F));
        const Vec3 position = particle.position - particle.velocity * lag;
        const float size = particle.size * emitter.size_over_life.at(life_share);
        const float alpha_raw = emitter.alpha.at(life_share);
        if (size <= 0.0F || alpha_raw <= 0.0F)
            continue;
        if (emitter.material == Material::debris)
        {
            DebrisCube cube;
            cube.position = position;
            cube.half_size = size;
            cube.angle = particle.rotation - particle.spin * lag;
            cube.axis = normalized(Vec3{particle.seed - 0.5F, 0.6F, std::fmod(particle.seed * 7.0F, 1.0F) - 0.5F});
            cube.tile = particle.tile;
            cube.alpha = alpha_raw;
            cube.sky_light = effect.light.sky;
            cube.block_light = effect.light.block;
            frame.debris.push_back(cube);
            continue;
        }
        const Vec3 offset = position - view.eye;
        const float depth = dot(offset, view.forward);
        const float speed = length(particle.velocity);
        const float reach = size + emitter.stretch * speed;
        if (depth < -reach)
            continue;
        const float distance = length(offset);
        float alpha = alpha_raw;
        const bool gas = is_gas(emitter.material);
        if (gas || emitter.material == Material::glow)
        {
            if (gas && emitter.temperature.at(life_share) < 0.6F)
            {
                // The smoke around the eye becomes one fog instead of a stack
                // of full-screen cards.
                const float inside = saturate(1.0F - distance / (size * 1.6F));
                const float weight = alpha_raw * inside * inside;
                if (weight > 0.0F)
                {
                    const Color tint = emitter.color.at(life_share);
                    frame.immersion += weight / (2.0F * size);
                    immersion_weight += weight;
                    immersion_reach += weight * size;
                    immersion_color += Vec3{tint.r * effect.params.tint.r,
                                            tint.g * effect.params.tint.g,
                                            tint.b * effect.params.tint.b} *
                                       weight;
                    immersion_sky += effect.light.sky * weight;
                    immersion_block += effect.light.block * weight;
                }
            }
            alpha *= near_fade(distance, size);
        }
        if (alpha <= 0.003F)
            continue;

        Instance instance;
        instance.position[0] = position.x;
        instance.position[1] = position.y;
        instance.position[2] = position.z;
        instance.size = size;
        const Color color = emitter.color.at(life_share);
        instance.color[0] = color.r * effect.params.tint.r;
        instance.color[1] = color.g * effect.params.tint.g;
        instance.color[2] = color.b * effect.params.tint.b;
        instance.alpha = alpha;
        const Vec3 axis = speed > 1.0e-4F ? particle.velocity * (1.0F / speed) : Vec3{0.0F, 1.0F, 0.0F};
        instance.axis[0] = axis.x;
        instance.axis[1] = axis.y;
        instance.axis[2] = axis.z;
        instance.stretch = emitter.stretch * speed;
        instance.rotation = particle.rotation - particle.spin * lag;
        instance.frame = particle.seed * 64.0F + particle.age * emitter.frame_rate;
        instance.temperature = emitter.temperature.at(life_share);
        instance.erosion = emitter.erosion.at(life_share);
        instance.sky_light = effect.light.sky;
        instance.block_light = effect.light.block;
        instance.material = static_cast<float>(emitter.material) + (emitter.cold ? 16.0F : 0.0F);
        instance.emissive = emitter.emissive.at(life_share) * effect.params.intensity;
        switch (emitter.material)
        {
        case Material::spark:
            frame.sharp.push_back(instance);
            break;
        case Material::shockwave:
        case Material::heat:
            instance.emissive = emitter.distortion * alpha;
            frame.distortion.push_back(instance);
            break;
        default:
            frame.soft.push_back(instance);
            sort_keys_.push_back(depth_sort_key(depth));
            break;
        }
    }
    sort_by_key(frame.soft, sort_keys_, sort_scratch_, sort_key_scratch_);
    if (immersion_weight > 0.0F)
    {
        const float inverse = 1.0F / immersion_weight;
        frame.immersion_reach = immersion_reach * inverse * 1.6F;
        frame.immersion_color = {immersion_color.x * inverse, immersion_color.y * inverse,
                                 immersion_color.z * inverse};
        frame.immersion_light = {immersion_sky * inverse, immersion_block * inverse};
    }

    Vec3 flash{};
    for (const Effect &effect : effects_)
    {
        if (!effect.active)
            continue;
        const EffectDesc &desc = *effect.desc;
        const Vec3 position = effect.params.position;
        const float distance = length(position - view.eye);
        for (const LightDesc &light : desc.lights)
        {
            float intensity{};
            if (light.duration > 0.0F)
            {
                if (effect.age >= light.duration)
                    continue;
                intensity = light.intensity.at(effect.age / light.duration);
            }
            else
            {
                if (!effect.emitting)
                    continue;
                intensity = light.intensity.at(0.0F);
            }
            if (light.flicker > 0.0F)
            {
                const float phase = clock_ * 9.0F + static_cast<float>(effect.generation) * 1.7F;
                const float wobble = 0.5F + 0.25F * std::sin(phase) + 0.15F * std::sin(phase * 2.31F + 1.3F) +
                                     0.10F * std::sin(phase * 5.17F + 0.4F);
                intensity *= 1.0F - light.flicker * saturate(wobble);
            }
            intensity *= effect.params.intensity;
            if (intensity <= 0.002F)
                continue;
            PointLight point;
            point.position = position + (effect.basis_x * light.offset.x +
                                         effect.params.direction * light.offset.y +
                                         effect.basis_z * light.offset.z) *
                                            effect.params.scale;
            point.radius = light.radius * effect.params.scale;
            point.color = {light.color.r * effect.params.tint.r * intensity,
                           light.color.g * effect.params.tint.g * intensity,
                           light.color.b * effect.params.tint.b * intensity};
            frame.lights.push_back(point);
        }
        if (desc.shake > 0.0F && effect.age < desc.shake_duration)
        {
            const float left = 1.0F - effect.age / desc.shake_duration;
            frame.shake += desc.shake * effect.params.intensity * left * left *
                           light_falloff(distance, desc.shake_radius * effect.params.scale);
        }
        if (desc.flash > 0.0F && effect.age < desc.flash_duration && effect.visible_flash)
        {
            const float left = 1.0F - effect.age / desc.flash_duration;
            const float amount = desc.flash * effect.params.intensity * left * left *
                                 light_falloff(distance, desc.flash_radius * effect.params.scale);
            frame.flash += amount;
            flash += Vec3{desc.flash_color.r, desc.flash_color.g, desc.flash_color.b} * amount;
        }
    }
    select_lights(frame.lights, view.eye, budget_.max_lights);
    frame.shake = saturate(frame.shake);
    if (frame.flash > 0.0F)
    {
        const float inverse = 1.0F / frame.flash;
        frame.flash_color = {flash.x * inverse, flash.y * inverse, flash.z * inverse};
        frame.flash = std::min(frame.flash, 1.0F);
    }
}

} // namespace openminerz::vfx
