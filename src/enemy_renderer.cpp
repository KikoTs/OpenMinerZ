#include "enemy_renderer.hpp"

#include "modding/graphics/mod_surfaces.hpp"


#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace openminerz
{
namespace
{

using Matrix4 = std::array<float, 16>;

struct BoneTransform final
{
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
};

Matrix4 identity_matrix() noexcept
{
    Matrix4 result{};
    result[0] = result[5] = result[10] = result[15] = 1.0F;
    return result;
}

Matrix4 multiply_matrix(const Matrix4 &left, const Matrix4 &right) noexcept
{
    Matrix4 result{};
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
            for (std::size_t inner = 0; inner < 4U; ++inner)
                result[row * 4U + column] += left[row * 4U + inner] * right[inner * 4U + column];
    return result;
}

Matrix4 compose_matrix(const BoneTransform &transform) noexcept
{
    const auto &q = transform.rotation;
    const float xx = q[0] * q[0] * 2.0F;
    const float yy = q[1] * q[1] * 2.0F;
    const float zz = q[2] * q[2] * 2.0F;
    const float xy = q[0] * q[1] * 2.0F;
    const float xz = q[0] * q[2] * 2.0F;
    const float yz = q[1] * q[2] * 2.0F;
    const float wx = q[3] * q[0] * 2.0F;
    const float wy = q[3] * q[1] * 2.0F;
    const float wz = q[3] * q[2] * 2.0F;
    Matrix4 result = identity_matrix();
    result[0] = (1.0F - yy - zz) * transform.scale[0];
    result[1] = (xy + wz) * transform.scale[0];
    result[2] = (xz - wy) * transform.scale[0];
    result[4] = (xy - wz) * transform.scale[1];
    result[5] = (1.0F - xx - zz) * transform.scale[1];
    result[6] = (yz + wx) * transform.scale[1];
    result[8] = (xz + wy) * transform.scale[2];
    result[9] = (yz - wx) * transform.scale[2];
    result[10] = (1.0F - xx - yy) * transform.scale[2];
    result[12] = transform.translation[0];
    result[13] = transform.translation[1];
    result[14] = transform.translation[2];
    return result;
}

std::array<float, 3> transform_point(const std::array<float, 3> &point,
                                     const Matrix4 &matrix) noexcept
{
    return {point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8] + matrix[12],
            point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9] + matrix[13],
            point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]};
}

std::array<float, 3> transform_normal(const std::array<float, 3> &normal,
                                      const Matrix4 &matrix) noexcept
{
    return {normal[0] * matrix[0] + normal[1] * matrix[4] + normal[2] * matrix[8],
            normal[0] * matrix[1] + normal[1] * matrix[5] + normal[2] * matrix[9],
            normal[0] * matrix[2] + normal[1] * matrix[6] + normal[2] * matrix[10]};
}

BoneTransform interpolate_transform(const BoneTransform &first, const BoneTransform &second,
                                    float amount) noexcept
{
    BoneTransform result;
    float cosine = 0.0F;
    for (std::size_t component = 0; component < 4U; ++component)
        cosine += first.rotation[component] * second.rotation[component];
    const float sign = cosine < 0.0F ? -1.0F : 1.0F;
    float length_squared = 0.0F;
    for (std::size_t component = 0; component < 3U; ++component)
    {
        result.translation[component] =
            first.translation[component] +
            (second.translation[component] - first.translation[component]) * amount;
        result.scale[component] =
            first.scale[component] + (second.scale[component] - first.scale[component]) * amount;
    }
    for (std::size_t component = 0; component < 4U; ++component)
    {
        result.rotation[component] =
            first.rotation[component] +
            (second.rotation[component] * sign - first.rotation[component]) * amount;
        length_squared += result.rotation[component] * result.rotation[component];
    }
    const float inverse_length = 1.0F / std::max(0.000001F, std::sqrt(length_squared));
    for (float &component : result.rotation)
        component *= inverse_length;
    return result;
}

class Reader final
{
  public:
    explicit Reader(const std::filesystem::path &path)
        : stream_(path, std::ios::binary), remaining_(std::filesystem::file_size(path))
    {
        if (!stream_)
            throw std::runtime_error{"could not open skinned enemy model"};
    }

    template <typename Value> Value read()
    {
        Value value{};
        bytes(&value, sizeof(value));
        return value;
    }

    void bytes(void *destination, std::size_t count)
    {
        if (count > remaining_)
            throw std::runtime_error{"truncated skinned enemy model"};
        stream_.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
        if (!stream_)
            throw std::runtime_error{"failed reading skinned enemy model"};
        remaining_ -= count;
    }

    [[nodiscard]] std::string dotnet_string()
    {
        std::uint32_t length{};
        int shift{};
        for (int index = 0; index < 5; ++index)
        {
            const auto value = read<std::uint8_t>();
            length |= static_cast<std::uint32_t>(value & 0x7FU) << shift;
            if ((value & 0x80U) == 0U)
            {
                if (length > 4096U)
                    throw std::runtime_error{"invalid skinned model string"};
                std::string result(length, '\0');
                bytes(result.data(), result.size());
                return result;
            }
            shift += 7;
        }
        throw std::runtime_error{"invalid skinned model string prefix"};
    }

  private:
    std::ifstream stream_;
    std::size_t remaining_{};
};

struct SkinnedVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> bones{};
    std::array<float, 4> weights{};
};

struct SkinnedPart final
{
    SDL_Texture *texture{};
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct SkinnedClip final
{
    int bone_count{};
    int frame_count{};
    float duration{};
    int sample_rate{};
    std::vector<BoneTransform> frames;
};

struct SkinnedModel final
{
    int bone_count{};
    std::vector<int> parents;
    std::vector<Matrix4> bind_local;
    std::vector<Matrix4> inverse_bind;
    std::vector<SkinnedPart> parts;
    std::unordered_map<std::string, SkinnedClip> clips;
    bool loaded{};
};

} // namespace

class EnemyModelLibrary::Impl final
{
  public:
    Impl(SDL_Renderer *renderer, const std::filesystem::path &derived) : renderer_(renderer)
    {
        static constexpr std::array<const char *, 8> model_files{
            "enemy_zombie.omsk",          "enemy_skeleton_zombie.omsk",
            "enemy_skeleton_archer.omsk", "enemy_skeleton_axes.omsk",
            "enemy_skeleton_sword.omsk",  "enemy_felguard.omsk",
            "enemy_alien.omsk",           "dragon_body.omsk"};
        for (std::size_t model = 0; model < model_files.size(); ++model)
        {
            try
            {
                load_model(models_[model], derived / model_files[model]);
                loaded_ = loaded_ || models_[model].loaded;
            }
            catch (const std::exception &error)
            {
                models_[model].loaded = false;
                SDL_Log("Enemy model %s failed: %s", model_files[model], error.what());
            }
        }
        derived_ = derived;
        load_textures();
    }

    void load_textures()
    {
        for (std::size_t texture = 0; texture < textures_.size(); ++texture)
        {
            const auto path = derived_ / ("enemy_texture_" + std::to_string(texture) + ".bmp");
            // (Graphics area) a texture pack may replace it.
            SDL_Surface *surface = load_content_surface(path, ContentKind::entities);
            if (surface == nullptr)
                continue;
            textures_[texture] = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_DestroySurface(surface);
            if (textures_[texture] != nullptr)
                SDL_SetTextureScaleMode(textures_[texture], SDL_SCALEMODE_LINEAR);
        }
        for (std::size_t texture = 0; texture < dragon_textures_.size(); ++texture)
        {
            const auto path = derived_ / ("dragon_texture_" + std::to_string(texture) + ".bmp");
            SDL_Surface *surface = load_content_surface(path, ContentKind::entities);
            if (surface == nullptr)
                continue;
            dragon_textures_[texture] = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_DestroySurface(surface);
            if (dragon_textures_[texture] != nullptr)
                SDL_SetTextureScaleMode(dragon_textures_[texture], SDL_SCALEMODE_LINEAR);
        }
    }

    // (Visuals) the skins again, through the packs installed now.
    std::vector<SDL_Texture *> reload_textures()
    {
        std::vector<SDL_Texture *> old;
        for (SDL_Texture *&texture : textures_)
            old.push_back(std::exchange(texture, nullptr));
        for (SDL_Texture *&texture : dragon_textures_)
            old.push_back(std::exchange(texture, nullptr));
        load_textures();
        std::erase(old, nullptr);
        return old;
    }

    ~Impl()
    {
        for (SDL_Texture *texture : textures_)
            SDL_DestroyTexture(texture);
        for (SDL_Texture *texture : dragon_textures_)
            SDL_DestroyTexture(texture);
        for (SkinnedModel &model : models_)
            for (SkinnedPart &part : model.parts)
                SDL_DestroyTexture(part.texture);
    }

    void load_model(SkinnedModel &model, const std::filesystem::path &path)
    {
        Reader reader{path};
        if (reader.read<std::uint32_t>() != 0x4B534D4FU || reader.read<std::int32_t>() != 1)
            throw std::runtime_error{"invalid skinned enemy model"};
        model.bone_count = reader.read<std::int32_t>();
        if (model.bone_count <= 0 || model.bone_count > 256)
            throw std::runtime_error{"unsupported enemy skeleton"};
        model.parents.resize(static_cast<std::size_t>(model.bone_count));
        model.bind_local.resize(static_cast<std::size_t>(model.bone_count));
        for (int bone = 0; bone < model.bone_count; ++bone)
        {
            (void)reader.dotnet_string();
            model.parents[static_cast<std::size_t>(bone)] = reader.read<std::int32_t>();
            for (float &component : model.bind_local[static_cast<std::size_t>(bone)])
                component = reader.read<float>();
        }
        const int inverse_count = reader.read<std::int32_t>();
        if (inverse_count < 0 || inverse_count > model.bone_count)
            throw std::runtime_error{"invalid enemy inverse bind data"};
        model.inverse_bind.resize(static_cast<std::size_t>(model.bone_count), identity_matrix());
        for (int bone = 0; bone < inverse_count; ++bone)
            for (float &component : model.inverse_bind[static_cast<std::size_t>(bone)])
                component = reader.read<float>();
        const int part_count = reader.read<std::int32_t>();
        if (part_count <= 0 || part_count > 64)
            throw std::runtime_error{"invalid enemy part count"};
        model.parts.resize(static_cast<std::size_t>(part_count));
        for (SkinnedPart &part : model.parts)
        {
            const int width = reader.read<std::int32_t>();
            const int height = reader.read<std::int32_t>();
            if (width > 0 && height > 0)
            {
                if (width > 4096 || height > 4096)
                    throw std::runtime_error{"invalid enemy texture"};
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                                 static_cast<std::size_t>(height) * 4U);
                reader.bytes(pixels.data(), pixels.size());
                SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                                                             pixels.data(), width * 4);
                if (surface != nullptr)
                {
                    part.texture = SDL_CreateTextureFromSurface(renderer_, surface);
                    SDL_DestroySurface(surface);
                    if (part.texture != nullptr)
                        SDL_SetTextureScaleMode(part.texture, SDL_SCALEMODE_LINEAR);
                }
            }
            const int vertex_count = reader.read<std::int32_t>();
            if (vertex_count <= 0 || vertex_count > 200000)
                throw std::runtime_error{"invalid enemy vertex count"};
            part.vertices.resize(static_cast<std::size_t>(vertex_count));
            for (SkinnedVertex &vertex : part.vertices)
            {
                for (float &component : vertex.position)
                    component = reader.read<float>();
                for (float &component : vertex.normal)
                    component = reader.read<float>();
                for (float &component : vertex.uv)
                    component = reader.read<float>();
                reader.bytes(vertex.bones.data(), vertex.bones.size());
                for (float &weight : vertex.weights)
                    weight = reader.read<float>();
            }
            const int index_count = reader.read<std::int32_t>();
            if (index_count <= 0 || index_count > 1000000 || index_count % 3 != 0)
                throw std::runtime_error{"invalid enemy index count"};
            part.indices.resize(static_cast<std::size_t>(index_count));
            for (std::uint32_t &index : part.indices)
            {
                index = reader.read<std::uint32_t>();
                if (index >= part.vertices.size())
                    throw std::runtime_error{"invalid enemy triangle"};
            }
        }
        const int clip_count = reader.read<std::int32_t>();
        if (clip_count < 0 || clip_count > 256)
            throw std::runtime_error{"invalid enemy clip count"};
        for (int clip_index = 0; clip_index < clip_count; ++clip_index)
        {
            const std::string name = reader.dotnet_string();
            SkinnedClip clip;
            clip.bone_count = reader.read<std::int32_t>();
            clip.frame_count = reader.read<std::int32_t>();
            clip.duration = reader.read<float>();
            clip.sample_rate = reader.read<std::int32_t>();
            if (clip.bone_count != model.bone_count || clip.frame_count <= 0 ||
                clip.frame_count > 3600 || clip.sample_rate <= 0)
                throw std::runtime_error{"unsupported enemy clip"};
            clip.frames.resize(static_cast<std::size_t>(clip.bone_count) *
                               static_cast<std::size_t>(clip.frame_count));
            for (BoneTransform &transform : clip.frames)
            {
                for (float &component : transform.translation)
                    component = reader.read<float>();
                for (float &component : transform.rotation)
                    component = reader.read<float>();
                for (float &component : transform.scale)
                    component = reader.read<float>();
            }
            model.clips.emplace(name, std::move(clip));
        }
        model.loaded = true;
    }

    [[nodiscard]] const SkinnedModel &model(world::EnemyModelKind kind) const noexcept
    {
        return models_[std::min<std::size_t>(static_cast<std::size_t>(kind), models_.size() - 1U)];
    }

    void append_enemy(EntityBatch &batch, const world::Enemy &enemy, float sun_light,
                      float torch_light, const std::array<float, 3> &sun_color, float daylight,
                      bool enhanced)
    {
        const world::EnemyStats &stats = world::enemy_stats(enemy.type);
        const float suppressed = torch_light * (1.0F - sun_light * daylight * 0.85F);
        append_skinned(batch, model(stats.model), textures_[stats.texture_index], enemy.clip,
                       enemy.clip_time, enemy.clip_loops, enemy.position, enemy.yaw, sun_light,
                       sun_color, {suppressed, suppressed * 0.92F, suppressed * 0.75F},
                       torch_light, 1.0F, {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, false, false, enhanced);
    }

    void append_dragon(EntityBatch &batch, const world::Dragon &dragon,
                       const std::array<float, 3> &sun_color, bool enhanced)
    {
        const auto texture_index = std::min<std::size_t>(static_cast<std::size_t>(dragon.kind),
                                                         dragon_textures_.size() - 1U);
        // DragonPartEntity: half scale and a (0, -11.75, +2) root-frame drop —
        // the authored model is built ~23.5 units above its origin at double
        // size, and this transform hangs it correctly off the flight root.
        append_skinned(batch, models_[7], dragon_textures_[texture_index], dragon.clip,
                       dragon.clip_time, true, dragon.position, dragon.yaw, 1.0F, sun_color,
                       {0.0F, 0.0F, 0.0F}, 0.0F, 0.5F, {0.0F, -11.75F, 2.0F}, dragon.pitch,
                       dragon.roll, true, true, enhanced);
    }

    void append_skinned(EntityBatch &batch, const SkinnedModel &skinned, SDL_Texture *skin,
                        std::string_view clip_name, float clip_time, bool clip_loops,
                        world::Vec3 position, float yaw, float sun_light,
                        const std::array<float, 3> &sun_color,
                        const std::array<float, 3> &torch_add, float torch_light,
                        float model_scale = 1.0F,
                        std::array<float, 3> model_offset = {0.0F, 0.0F, 0.0F}, float pitch = 0.0F,
                        float roll = 0.0F, bool flip_forward = false,
                        bool source_dragon_attitude = false, bool albedo = false)
    {
        if (!skinned.loaded)
            return;
        const auto clip_iterator = skinned.clips.find(std::string{clip_name});
        // Pose the skeleton: clip sample when available, bind pose otherwise.
        pose_.assign(static_cast<std::size_t>(skinned.bone_count), identity_matrix());
        if (clip_iterator != skinned.clips.end())
        {
            const SkinnedClip &clip = clip_iterator->second;
            float seconds = clip_time;
            if (clip_loops && clip.duration > 0.0F)
                seconds = std::fmod(std::max(0.0F, seconds), clip.duration);
            else
                seconds = std::clamp(seconds, 0.0F, clip.duration);
            const float fractional = seconds * static_cast<float>(clip.sample_rate);
            const int first_frame =
                std::clamp(static_cast<int>(std::floor(fractional)), 0, clip.frame_count - 1);
            const int second_frame = std::min(first_frame + 1, clip.frame_count - 1);
            const float amount =
                first_frame == second_frame
                    ? 0.0F
                    : std::clamp(fractional - static_cast<float>(first_frame), 0.0F, 1.0F);
            const BoneTransform *first =
                clip.frames.data() +
                static_cast<std::size_t>(first_frame) * static_cast<std::size_t>(clip.bone_count);
            const BoneTransform *second =
                clip.frames.data() +
                static_cast<std::size_t>(second_frame) * static_cast<std::size_t>(clip.bone_count);
            for (int bone = 0; bone < skinned.bone_count; ++bone)
                pose_[static_cast<std::size_t>(bone)] =
                    compose_matrix(interpolate_transform(first[bone], second[bone], amount));
        }
        else
        {
            for (int bone = 0; bone < skinned.bone_count; ++bone)
                pose_[static_cast<std::size_t>(bone)] =
                    skinned.bind_local[static_cast<std::size_t>(bone)];
        }
        absolute_.resize(static_cast<std::size_t>(skinned.bone_count));
        palette_.resize(static_cast<std::size_t>(skinned.bone_count));
        std::array<std::array<float, 3>, 3> root_attitude{};
        if (source_dragon_attitude)
        {
            // DragonEntity uses XNA CreateFromYawPitchRoll and Local -Z as
            // forward. Consume the same basis as flight and hit detection so
            // a bank cannot visually rotate around a different axis.
            const world::DragonBasis basis = world::dragon_attitude_basis(yaw, pitch, roll);
            root_attitude = {{{basis.right.x, basis.right.y, basis.right.z},
                              {basis.up.x, basis.up.y, basis.up.z},
                              {-basis.forward.x, -basis.forward.y, -basis.forward.z}}};
        }
        else
        {
            // Ordinary exported enemies retain the verified (-yaw) convention.
            const float facing = -yaw;
            const float cr = std::cos(roll);
            const float sr = std::sin(roll);
            const float cp = std::cos(pitch);
            const float sp = std::sin(pitch);
            const float cf = std::cos(facing);
            const float sf = std::sin(facing);
            const std::array<std::array<float, 3>, 3> local_attitude{{
                {cr, sr * cp, sr * sp},
                {-sr, cr * cp, cr * sp},
                {0.0F, -sp, cp},
            }};
            for (std::size_t row = 0; row < 3U; ++row)
            {
                const auto &a = local_attitude[row];
                root_attitude[row] = {a[0] * cf + a[2] * sf, a[1], -a[0] * sf + a[2] * cf};
            }
        }
        Matrix4 entity = identity_matrix();
        for (std::size_t row = 0; row < 3U; ++row)
        {
            entity[row * 4U + 0U] = root_attitude[row][0] * model_scale;
            entity[row * 4U + 1U] = root_attitude[row][1] * model_scale;
            entity[row * 4U + 2U] = root_attitude[row][2] * model_scale;
        }
        if (flip_forward)
        {
            // DragonPartEntity bakes a pi yaw into every part: the authored
            // dragon meshes face backwards. RotY(pi) innermost = negate the
            // model-space x and z rows of the composed rotation.
            for (std::size_t column = 0; column < 3U; ++column)
            {
                entity[column] = -entity[column];
                entity[8U + column] = -entity[8U + column];
            }
        }
        // DragonPartEntity's local offset is transformed by the parent's full
        // yaw/pitch/roll (but not its own half-scale or pi-yaw). Omitting pitch
        // and roll here made the mesh pivot away from its flight root in turns.
        entity[12] = position.x + model_offset[0] * root_attitude[0][0] +
                     model_offset[1] * root_attitude[1][0] + model_offset[2] * root_attitude[2][0];
        entity[13] = position.y + model_offset[0] * root_attitude[0][1] +
                     model_offset[1] * root_attitude[1][1] + model_offset[2] * root_attitude[2][1];
        entity[14] = position.z + model_offset[0] * root_attitude[0][2] +
                     model_offset[1] * root_attitude[1][2] + model_offset[2] * root_attitude[2][2];
        for (int bone = 0; bone < skinned.bone_count; ++bone)
        {
            const auto index = static_cast<std::size_t>(bone);
            const int parent = skinned.parents[index];
            absolute_[index] =
                parent < 0
                    ? multiply_matrix(pose_[index], entity)
                    : multiply_matrix(pose_[index], absolute_[static_cast<std::size_t>(parent)]);
            palette_[index] = multiply_matrix(skinned.inverse_bind[index], absolute_[index]);
        }
        for (const SkinnedPart &part : skinned.parts)
        {
            EntitySpan span;
            span.texture = skin != nullptr ? skin : part.texture;
            span.first = static_cast<std::uint32_t>(batch.vertices.size());
            for (const std::uint32_t index : part.indices)
            {
                const SkinnedVertex &source = part.vertices[index];
                std::array<float, 3> skinned_position{};
                std::array<float, 3> normal{};
                float total_weight{};
                for (std::size_t influence = 0; influence < 4U; ++influence)
                {
                    const float weight = source.weights[influence];
                    if (weight <= 0.0F)
                        continue;
                    const auto bone = static_cast<std::size_t>(
                        std::min<int>(source.bones[influence], skinned.bone_count - 1));
                    const auto moved = transform_point(source.position, palette_[bone]);
                    const auto bent = transform_normal(source.normal, palette_[bone]);
                    for (std::size_t axis = 0; axis < 3U; ++axis)
                    {
                        skinned_position[axis] += moved[axis] * weight;
                        normal[axis] += bent[axis] * weight;
                    }
                    total_weight += weight;
                }
                if (total_weight <= 0.0001F)
                {
                    skinned_position = transform_point(source.position, entity);
                    normal = transform_normal(source.normal, entity);
                }
                const float normal_length = std::sqrt(
                    normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                const float up = normal_length > 0.0001F ? normal[1] / normal_length : 1.0F;
                const float shade = std::clamp(0.58F + up * 0.28F, 0.35F, 1.0F);
                const float sun_gain =
                    (0.06F + 0.94F * sun_light * sun_light * (3.0F - 2.0F * sun_light)) * shade;
                EntityVertex vertex;
                vertex.x = skinned_position[0];
                vertex.y = skinned_position[1];
                vertex.z = skinned_position[2];
                vertex.u = source.uv[0];
                vertex.v = source.uv[1];
                // The Original presentation's light, baked; the Enhanced
                // shader lights the albedo (white) from sky_light and
                // torch_light below instead.
                vertex.red = albedo ? 1.0F : std::min(1.0F, sun_color[0] * sun_gain + torch_add[0]);
                vertex.green = albedo ? 1.0F : std::min(1.0F, sun_color[1] * sun_gain + torch_add[1]);
                vertex.blue = albedo ? 1.0F : std::min(1.0F, sun_color[2] * sun_gain + torch_add[2]);
                if (normal_length > 0.0001F)
                {
                    vertex.normal_x = normal[0] / normal_length;
                    vertex.normal_y = normal[1] / normal_length;
                    vertex.normal_z = normal[2] / normal_length;
                }
                vertex.sky_light = std::clamp(sun_light, 0.0F, 1.0F);
                vertex.torch_light = std::clamp(torch_light, 0.0F, 1.0F);
                batch.vertices.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
            if (span.count > 0U)
                batch.spans.push_back(span);
        }
    }

    [[nodiscard]] float clip_duration(world::EnemyModelKind kind,
                                      std::string_view clip) const noexcept
    {
        const SkinnedModel &skinned = model(kind);
        const auto found = skinned.clips.find(std::string{clip});
        return found == skinned.clips.end() ? 0.0F : found->second.duration;
    }

    SDL_Renderer *renderer_{};
    std::filesystem::path derived_;
    std::array<SkinnedModel, 8> models_{};
    std::array<SDL_Texture *, 14> textures_{};
    std::array<SDL_Texture *, 5> dragon_textures_{};
    std::vector<Matrix4> pose_;
    std::vector<Matrix4> absolute_;
    std::vector<Matrix4> palette_;
    bool loaded_{};
};

EnemyModelLibrary::EnemyModelLibrary(SDL_Renderer *renderer,
                                     const std::filesystem::path &derived_directory)
    : impl_(std::make_unique<Impl>(renderer, derived_directory))
{
}

EnemyModelLibrary::~EnemyModelLibrary() = default;

bool EnemyModelLibrary::loaded() const noexcept
{
    return impl_->loaded_;
}

float EnemyModelLibrary::clip_duration(world::EnemyModelKind model,
                                       std::string_view clip) const noexcept
{
    return impl_->clip_duration(model, clip);
}

void EnemyModelLibrary::append_enemy(EntityBatch &batch, const world::Enemy &enemy, float sun_light,
                                     float torch_light, const std::array<float, 3> &sun_color,
                                     float daylight, bool enhanced)
{
    impl_->append_enemy(batch, enemy, sun_light, torch_light, sun_color, daylight, enhanced);
}

void EnemyModelLibrary::append_dragon(EntityBatch &batch, const world::Dragon &dragon,
                                      const std::array<float, 3> &sun_color, bool enhanced)
{
    impl_->append_dragon(batch, dragon, sun_color, enhanced);
}

std::vector<SDL_Texture *> EnemyModelLibrary::reload_textures()
{
    return impl_->reload_textures();
}

} // namespace openminerz
