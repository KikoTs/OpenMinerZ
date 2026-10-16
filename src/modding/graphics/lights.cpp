// Lighting props for mods: the store, the per-frame evaluation and culling,
// and the reference of the view-space light clusters the lit shaders read
// (openminerz/modding/graphics.hpp; the GPU side is mod_light_gpu.cpp).

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openminerz::modding::graphics
{
namespace
{

float hash01(std::uint64_t value) noexcept
{
    value ^= value >> 33U;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33U;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33U;
    return static_cast<float>(value >> 40U) / static_cast<float>(1U << 24U);
}

// Smooth 1-D value noise in 0..1.
float value_noise(std::uint64_t seed, double x) noexcept
{
    const double cell = std::floor(x);
    const float t = static_cast<float>(x - cell);
    const auto index = static_cast<std::uint64_t>(static_cast<std::int64_t>(cell));
    const float a = hash01(seed * 0x9E3779B97F4A7C15ULL + index);
    const float b = hash01(seed * 0x9E3779B97F4A7C15ULL + index + 1U);
    const float smooth = t * t * (3.0F - 2.0F * t);
    return a + (b - a) * smooth;
}

float sanitize(float value, float low, float high, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

} // namespace

std::optional<std::uint64_t> LightStore::create(std::string_view mod_id, const LightDesc &desc)
{
    if (entries_.size() >= max_lights || count_for(mod_id) >= max_lights_per_mod)
        return std::nullopt;
    Entry entry;
    entry.id = next_id_++;
    entry.mod_id = mod_id;
    entry.desc = desc;
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

LightStore::Entry *LightStore::find(std::uint64_t id) noexcept
{
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), id,
                         [](const Entry &entry, std::uint64_t key) { return entry.id < key; });
    return found != entries_.end() && found->id == id ? &*found : nullptr;
}

const LightStore::Entry *LightStore::find(std::uint64_t id) const noexcept
{
    return const_cast<LightStore *>(this)->find(id);
}

bool LightStore::remove(std::uint64_t id) noexcept
{
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), id,
                         [](const Entry &entry, std::uint64_t key) { return entry.id < key; });
    if (found == entries_.end() || found->id != id)
        return false;
    entries_.erase(found);
    return true;
}

void LightStore::remove_mod(std::string_view mod_id) noexcept
{
    std::erase_if(entries_, [&](const Entry &entry) { return entry.mod_id == mod_id; });
}

void LightStore::clear() noexcept
{
    entries_.clear();
}

std::size_t LightStore::count_for(std::string_view mod_id) const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [&](const Entry &entry) { return entry.mod_id == mod_id; }));
}

ViewFrustum make_frustum(Vec3 eye, Vec3 forward, Vec3 right, Vec3 up, float vertical_fov_radians,
                         float aspect, float near_plane, float far_plane) noexcept
{
    ViewFrustum frustum;
    frustum.eye = eye;
    forward = vfx::normalized(forward, {0.0F, 0.0F, -1.0F});
    right = vfx::normalized(right, {1.0F, 0.0F, 0.0F});
    up = vfx::normalized(up, {0.0F, 1.0F, 0.0F});
    const float half_y = std::tan(std::clamp(vertical_fov_radians, 0.01F, 3.1F) * 0.5F);
    const float half_x = half_y * std::max(aspect, 0.01F);
    const auto plane = [&](Vec3 normal, Vec3 point) {
        normal = vfx::normalized(normal);
        return std::array<float, 4>{normal.x, normal.y, normal.z, -vfx::dot(normal, point)};
    };
    frustum.planes[0] = plane(forward, eye + forward * near_plane);
    frustum.planes[1] = plane(forward * -1.0F, eye + forward * far_plane);
    // Side planes pass through the eye; their normals point inward.
    frustum.planes[2] = plane(vfx::cross(up, forward + right * half_x), eye); // right
    frustum.planes[3] = plane(vfx::cross(forward - right * half_x, up), eye); // left
    frustum.planes[4] = plane(vfx::cross(forward + up * half_y, right), eye); // top
    frustum.planes[5] = plane(vfx::cross(right, forward - up * half_y), eye); // bottom
    return frustum;
}

bool sphere_visible(const ViewFrustum &frustum, Vec3 center, float radius) noexcept
{
    for (const auto &plane : frustum.planes)
        if (plane[0] * center.x + plane[1] * center.y + plane[2] * center.z + plane[3] < -radius)
            return false;
    return true;
}

void LightFrame::clear() noexcept
{
    lights.clear();
    considered = 0U;
    culled = 0U;
}

float light_modulation(const LightDesc &desc, std::uint64_t seed, double seconds) noexcept
{
    float value = 1.0F;
    const float flicker = sanitize(desc.flicker, 0.0F, 1.0F, 0.0F);
    if (flicker > 0.0F)
    {
        const double speed = sanitize(desc.flicker_speed, 0.0F, 60.0F, 9.0F);
        // Two octaves: a slow sway and a fast crackle, as a flame does.
        const float noise = value_noise(seed, seconds * speed) * 0.65F +
                            value_noise(seed ^ 0xA5A5A5A5ULL, seconds * speed * 2.7) * 0.35F;
        value *= 1.0F - flicker * noise;
    }
    const float pulse = sanitize(desc.pulse, 0.0F, 1.0F, 0.0F);
    if (pulse > 0.0F)
    {
        const double cycles = seconds * sanitize(desc.pulse_speed, 0.0F, 30.0F, 1.0F) +
                              sanitize(desc.pulse_phase, 0.0F, 1.0F, 0.0F);
        const float wave =
            0.5F + 0.5F * static_cast<float>(std::cos(cycles * 2.0 * std::numbers::pi));
        value *= 1.0F - pulse * (1.0F - wave);
    }
    return std::clamp(value, 0.0F, 1.0F);
}

float light_attenuation(float distance, float radius) noexcept
{
    if (radius <= 0.0F || distance >= radius)
        return 0.0F;
    const float share = 1.0F - std::max(distance, 0.0F) / radius;
    return share * share;
}

void build_light_frame(const LightStore &store, double seconds, const ViewFrustum &frustum,
                       float max_distance, std::size_t max_visible, const TargetLocator &locate,
                       LightFrame &out)
{
    out.lights.clear();
    out.considered = 0U;
    out.culled = 0U;
    struct Candidate final
    {
        GpuLight light;
        float importance{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(std::min<std::size_t>(store.size(), 1024U));
    for (const LightStore::Entry &entry : store.entries())
    {
        const LightDesc &desc = entry.desc;
        if (!desc.enabled)
            continue;
        ++out.considered;
        Vec3 position = desc.position;
        Vec3 direction = desc.direction;
        if (entry.attachment && entry.attachment->target.valid())
        {
            Vec3 target{};
            float yaw{};
            if (!locate || !locate(entry.attachment->target, target, yaw))
            {
                ++out.culled;
                continue;
            }
            Vec3 offset = entry.attachment->offset;
            if (entry.attachment->rotate)
            {
                const float sine = std::sin(yaw);
                const float cosine = std::cos(yaw);
                offset = {offset.x * cosine - offset.z * sine, offset.y,
                          offset.x * sine + offset.z * cosine};
                direction = {direction.x * cosine - direction.z * sine, direction.y,
                             direction.x * sine + direction.z * cosine};
            }
            position = target + offset;
        }
        const float radius = sanitize(desc.radius, 0.5F, 64.0F, 8.0F);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            vfx::length(position - frustum.eye) - radius > max_distance ||
            !sphere_visible(frustum, position, radius))
        {
            ++out.culled;
            continue;
        }
        const float strength =
            sanitize(desc.intensity, 0.0F, 64.0F, 1.0F) * light_modulation(desc, entry.id, seconds);
        const Color color{sanitize(desc.color.r, 0.0F, 16.0F, 1.0F) * strength,
                          sanitize(desc.color.g, 0.0F, 16.0F, 1.0F) * strength,
                          sanitize(desc.color.b, 0.0F, 16.0F, 1.0F) * strength};
        if (color.r + color.g + color.b <= 1.0e-4F)
        {
            ++out.culled;
            continue;
        }
        Candidate candidate;
        GpuLight &light = candidate.light;
        light.position[0] = position.x;
        light.position[1] = position.y;
        light.position[2] = position.z;
        light.radius = radius;
        light.color[0] = color.r;
        light.color[1] = color.g;
        light.color[2] = color.b;
        if (desc.shape == LightShape::spot)
        {
            const Vec3 axis = vfx::normalized(direction, {0.0F, -1.0F, 0.0F});
            const float outer = sanitize(desc.outer_angle, 1.0F, 89.0F, 40.0F);
            const float inner =
                std::min(sanitize(desc.inner_angle, 0.0F, 89.0F, 25.0F), outer - 0.5F);
            constexpr float degrees = std::numbers::pi_v<float> / 180.0F;
            light.direction[0] = axis.x;
            light.direction[1] = axis.y;
            light.direction[2] = axis.z;
            light.spot_cos_outer = std::cos(outer * degrees);
            light.spot_cos_inner = std::cos(std::max(inner, 0.0F) * degrees);
        }
        // Ranked by the light as set, not as it flickers this frame, so a
        // still camera keeps a still order and no light pops in a full cluster.
        const float base = sanitize(desc.intensity, 0.0F, 64.0F, 1.0F);
        candidate.importance =
            vfx::light_importance({{position.x, position.y, position.z},
                                   radius,
                                   {sanitize(desc.color.r, 0.0F, 16.0F, 1.0F) * base,
                                    sanitize(desc.color.g, 0.0F, 16.0F, 1.0F) * base,
                                    sanitize(desc.color.b, 0.0F, 16.0F, 1.0F) * base}},
                                  frustum.eye);
        candidates.push_back(candidate);
    }
    if (candidates.size() > max_visible)
    {
        std::nth_element(
            candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(max_visible),
            candidates.end(),
            [](const Candidate &a, const Candidate &b) { return a.importance > b.importance; });
        out.culled += candidates.size() - max_visible;
        candidates.resize(max_visible);
    }
    // Most important first, so a full cluster keeps the lights that matter.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.importance > b.importance; });
    out.lights.reserve(candidates.size());
    for (const Candidate &candidate : candidates)
        out.lights.push_back(candidate.light);
}

// --- Light clusters (mirrored by mod_light_hlsl and the cluster build of
// mod_light_gpu.cpp; change all three together) -------------------------------

float ClusterGrid::log2_growth() noexcept
{
    return std::log2(last_depth / first_depth) / static_cast<float>(slices - 2);
}

float ClusterGrid::slice_start(int slice) noexcept
{
    return slice <= 0 ? 0.0F
                      : first_depth * std::exp2(static_cast<float>(slice - 1) * log2_growth());
}

ClusterGrid make_cluster_grid(const ClusterView &view, std::span<const GpuLight> lights) noexcept
{
    ClusterGrid grid;
    grid.view = view;
    grid.tiles_x = std::clamp((std::max(view.width, 1) + ClusterGrid::tile_pixels - 1) /
                                  ClusterGrid::tile_pixels,
                              1, ClusterGrid::max_tiles_x);
    grid.tiles_y = std::clamp((std::max(view.height, 1) + ClusterGrid::tile_pixels - 1) /
                                  ClusterGrid::tile_pixels,
                              1, ClusterGrid::max_tiles_y);
    for (const GpuLight &light : lights)
        grid.far_depth = std::max(
            grid.far_depth,
            to_view_space(view, {light.position[0], light.position[1], light.position[2]}).z +
                light.radius);
    return grid;
}

Vec3 to_view_space(const ClusterView &view, Vec3 position) noexcept
{
    const Vec3 offset = position - view.eye;
    return {vfx::dot(offset, view.right), vfx::dot(offset, view.up),
            vfx::dot(offset, view.forward)};
}

ClusterLookup cluster_of(const ClusterGrid &grid, Vec3 position) noexcept
{
    const Vec3 local = to_view_space(grid.view, position);
    if (local.z > grid.far_depth)
        return {ClusterLookup::Kind::unlit, 0U};
    if (!(local.z > 0.0F))
        return {ClusterLookup::Kind::off_grid, 0U};
    const float ndc_x = local.x * grid.view.scale_x / local.z;
    const float ndc_y = local.y * grid.view.scale_y / local.z;
    if (!(std::abs(ndc_x) <= ClusterGrid::ndc_margin) ||
        !(std::abs(ndc_y) <= ClusterGrid::ndc_margin))
        return {ClusterLookup::Kind::off_grid, 0U};
    const int tile_x =
        std::clamp(static_cast<int>(std::floor((ndc_x * 0.5F + 0.5F) *
                                               static_cast<float>(grid.tiles_x))),
                   0, grid.tiles_x - 1);
    const int tile_y =
        std::clamp(static_cast<int>(std::floor((ndc_y * 0.5F + 0.5F) *
                                               static_cast<float>(grid.tiles_y))),
                   0, grid.tiles_y - 1);
    const int slice =
        local.z < ClusterGrid::first_depth
            ? 0
            : std::min(ClusterGrid::slices - 1,
                       1 + static_cast<int>(std::floor(std::log2(local.z / ClusterGrid::first_depth) /
                                                       ClusterGrid::log2_growth())));
    return {ClusterLookup::Kind::cluster,
            static_cast<std::uint32_t>((slice * grid.tiles_y + tile_y) * grid.tiles_x + tile_x)};
}

ClusterBox cluster_box(const ClusterGrid &grid, std::uint32_t cluster) noexcept
{
    const auto tiles_x = static_cast<std::uint32_t>(grid.tiles_x);
    const auto tiles_y = static_cast<std::uint32_t>(grid.tiles_y);
    const std::uint32_t tile_x = cluster % tiles_x;
    const std::uint32_t tile_y = (cluster / tiles_x) % tiles_y;
    const auto slice = static_cast<int>(cluster / (tiles_x * tiles_y));
    // A tile's NDC span; the edge tiles reach out to the margin.
    const auto span = [](std::uint32_t tile, std::uint32_t tiles, float &low, float &high) {
        low = tile == 0U ? -ClusterGrid::ndc_margin
                         : -1.0F + 2.0F * static_cast<float>(tile) / static_cast<float>(tiles);
        high = tile + 1U == tiles
                   ? ClusterGrid::ndc_margin
                   : -1.0F + 2.0F * static_cast<float>(tile + 1U) / static_cast<float>(tiles);
    };
    float low_x{};
    float high_x{};
    float low_y{};
    float high_y{};
    span(tile_x, tiles_x, low_x, high_x);
    span(tile_y, tiles_y, low_y, high_y);
    const float near_depth = ClusterGrid::slice_start(slice);
    const float far_depth = slice == ClusterGrid::slices - 1
                                ? std::max(grid.far_depth, near_depth)
                                : ClusterGrid::slice_start(slice + 1);
    // x = ndc * depth / scale is linear in depth, so the box spans the four
    // corners at the slice's two depths.
    const auto extent = [&](float low, float high, float scale, float &minimum, float &maximum) {
        const float a = low * near_depth;
        const float b = low * far_depth;
        const float c = high * near_depth;
        const float d = high * far_depth;
        minimum = std::min(std::min(a, b), std::min(c, d)) / scale;
        maximum = std::max(std::max(a, b), std::max(c, d)) / scale;
    };
    ClusterBox box;
    extent(low_x, high_x, std::max(grid.view.scale_x, 1.0e-6F), box.minimum.x, box.maximum.x);
    extent(low_y, high_y, std::max(grid.view.scale_y, 1.0e-6F), box.minimum.y, box.maximum.y);
    box.minimum.z = near_depth;
    box.maximum.z = far_depth;
    // Float maths on either side may put a point a hair outside its box.
    const float pad = 0.01F + 0.002F * far_depth;
    box.minimum = box.minimum - Vec3{pad, pad, pad};
    box.maximum = box.maximum + Vec3{pad, pad, pad};
    return box;
}

void build_light_clusters(std::span<const GpuLight> lights, const ClusterGrid &grid,
                          ClusterLists &out)
{
    const std::size_t clusters = grid.cluster_count();
    out.counts.assign(clusters, 0U);
    out.indices.assign(clusters * ClusterGrid::max_per_cluster, 0U);
    for (std::size_t cluster = 0U; cluster < clusters; ++cluster)
    {
        const ClusterBox box = cluster_box(grid, static_cast<std::uint32_t>(cluster));
        std::uint32_t &count = out.counts[cluster];
        for (std::size_t index = 0U; index < lights.size(); ++index)
        {
            const GpuLight &light = lights[index];
            const Vec3 center = to_view_space(
                grid.view, {light.position[0], light.position[1], light.position[2]});
            const Vec3 nearest{std::clamp(center.x, box.minimum.x, box.maximum.x) - center.x,
                               std::clamp(center.y, box.minimum.y, box.maximum.y) - center.y,
                               std::clamp(center.z, box.minimum.z, box.maximum.z) - center.z};
            if (vfx::dot(nearest, nearest) > light.radius * light.radius)
                continue;
            if (count < ClusterGrid::max_per_cluster)
                out.indices[cluster * ClusterGrid::max_per_cluster + count] =
                    static_cast<std::uint16_t>(index);
            ++count;
        }
    }
}

LightClusterStats cluster_stats(const ClusterGrid &grid,
                                std::span<const std::uint32_t> counts) noexcept
{
    LightClusterStats stats;
    stats.tiles_x = grid.tiles_x;
    stats.tiles_y = grid.tiles_y;
    stats.clusters = std::min(grid.cluster_count(), counts.size());
    for (std::size_t cluster = 0U; cluster < stats.clusters; ++cluster)
    {
        const std::uint32_t count = counts[cluster];
        if (count == 0U)
            continue;
        ++stats.lit_clusters;
        stats.references += std::min(count, ClusterGrid::max_per_cluster);
        stats.dropped += count - std::min(count, ClusterGrid::max_per_cluster);
        stats.busiest = std::max(stats.busiest, count);
    }
    stats.valid = true;
    return stats;
}

Color light_at(const LightFrame &frame, Vec3 position, const Vec3 *normal) noexcept
{
    return light_at(std::span<const GpuLight>{frame.lights}, position, normal);
}

Color light_at(std::span<const GpuLight> lights, Vec3 position, const Vec3 *normal) noexcept
{
    Color total{0.0F, 0.0F, 0.0F};
    for (const GpuLight &light : lights)
    {
        const Vec3 offset{light.position[0] - position.x, light.position[1] - position.y,
                          light.position[2] - position.z};
        const float distance = vfx::length(offset);
        float share = light_attenuation(distance, light.radius);
        if (share <= 0.0F)
            continue;
        const Vec3 to_light = offset * (1.0F / std::max(distance, 0.001F));
        if (normal != nullptr)
            share *= std::clamp(vfx::dot(*normal, to_light) * 0.85F + 0.15F, 0.0F, 1.0F);
        if (light.spot_cos_outer > -1.5F)
        {
            const float cosine =
                -vfx::dot(to_light, {light.direction[0], light.direction[1], light.direction[2]});
            const float span = std::max(light.spot_cos_inner - light.spot_cos_outer, 1.0e-4F);
            const float cone = std::clamp((cosine - light.spot_cos_outer) / span, 0.0F, 1.0F);
            share *= cone * cone * (3.0F - 2.0F * cone);
        }
        total.r += light.color[0] * share;
        total.g += light.color[1] * share;
        total.b += light.color[2] * share;
    }
    return total;
}

} // namespace openminerz::modding::graphics
