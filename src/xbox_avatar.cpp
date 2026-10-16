#include "openminerz/xbox_avatar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

// The reader follows the add-on's own loader (AvatarAsset.Load / AvatarBatch.Read
// in KikoTs/castleforge-xbox-avatar, src/XboxAvatar/Runtime/AvatarRuntime.cs)
// field for field, and the draw rules follow its third-person path
// (ImportedAvatarModelEntity.Draw). Where this file is stricter than the add-on
// it says so: the add-on trusts a file it wrote itself, this one may be handed a
// stranger's.

namespace openminerz::xbox_avatar
{
namespace
{

// DNA.Avatars.Avatar.DefaultParentBones: the Xbox avatar hierarchy.
constexpr std::array<int, bone_count> parents{
    -1, 0,  0,  0,  0,  1,  2,  2,  3,  3,  1,  6,  5,  6,  5,  8,  5,  8,  5,  14, 12, 11, 16, 15,
    14, 20, 20, 20, 22, 22, 22, 25, 25, 25, 28, 28, 28, 33, 33, 33, 33, 33, 33, 33, 36, 36, 36, 36,
    36, 36, 36, 37, 38, 39, 40, 43, 44, 45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};

constexpr std::size_t vertex_bytes = 44U;

class Cursor final
{
  public:
    explicit Cursor(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    template <typename Value> [[nodiscard]] bool read(Value &value) noexcept
    {
        if (remaining() < sizeof(Value))
            return false;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(Value));
        offset_ += sizeof(Value);
        return true;
    }

    [[nodiscard]] bool take(std::size_t count, std::span<const std::uint8_t> &out) noexcept
    {
        if (remaining() < count)
            return false;
        out = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

    // BinaryReader.ReadString: a 7-bit encoded byte length, then UTF-8.
    [[nodiscard]] bool string(std::string &value, std::size_t maximum) noexcept
    {
        std::uint32_t length{};
        for (int shift = 0; shift < 35; shift += 7)
        {
            std::uint8_t byte{};
            if (!read(byte))
                return false;
            length |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U)
            {
                std::span<const std::uint8_t> text;
                if (length > maximum || !take(length, text))
                    return false;
                value.assign(reinterpret_cast<const char *>(text.data()), text.size());
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool matrix(rig::Matrix &value) noexcept
    {
        std::span<const std::uint8_t> raw;
        if (!take(sizeof(float) * value.size(), raw))
            return false;
        std::memcpy(value.data(), raw.data(), raw.size());
        return true;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

struct SourceVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> bindings{};
    std::array<std::uint8_t, 4> weights{};
    std::array<std::uint8_t, 4> color{};
};

SourceVertex vertex_at(std::span<const std::uint8_t> vertices, std::size_t index) noexcept
{
    SourceVertex vertex;
    const std::uint8_t *raw = vertices.data() + index * vertex_bytes;
    std::memcpy(vertex.position.data(), raw, 12U);
    std::memcpy(vertex.normal.data(), raw + 12U, 12U);
    std::memcpy(vertex.uv.data(), raw + 24U, 8U);
    std::memcpy(vertex.bindings.data(), raw + 32U, 4U);
    std::memcpy(vertex.weights.data(), raw + 36U, 4U);
    std::memcpy(vertex.color.data(), raw + 40U, 4U);
    return vertex;
}

struct RawBatch final
{
    std::string name;
    std::uint32_t category{};
    std::uint8_t palette_mask{};
    std::array<float, 3> palette0{};
    std::span<const std::uint8_t> vertices;
    std::uint32_t vertex_count{};
    std::span<const std::uint8_t> indices;
    std::uint32_t index_count{};
    std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
    std::span<const std::uint8_t> png;
    int face_usage{-1};
    int face_frame{-1};
    bool base_body{};
    bool overlay{};
    bool hand_component{};
    bool finger_geometry{};
    bool bare_hand_shell{};
};

bool contains_ci(std::string_view text, std::string_view needle) noexcept
{
    if (needle.empty() || needle.size() > text.size())
        return false;
    const auto lower = [](char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    };
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size() && match; ++index)
            match = lower(text[start + index]) == lower(needle[index]);
        if (match)
            return true;
    }
    return false;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && contains_ci(text.substr(0U, prefix.size()), prefix);
}

// AvatarBatch.ParseFaceLayerName: "...:face-layer-<usage>-frame-<frame>[:...]".
void parse_face_layer(RawBatch &batch) noexcept
{
    constexpr std::string_view marker = ":face-layer-";
    constexpr std::string_view frame_marker = "-frame-";
    const std::string_view name{batch.name};
    std::size_t usage_start = std::string_view::npos;
    for (std::size_t start = 0; start + marker.size() <= name.size(); ++start)
        if (contains_ci(name.substr(start, marker.size()), marker))
        {
            usage_start = start + marker.size();
            break;
        }
    if (usage_start == std::string_view::npos)
        return;
    std::size_t frame_start = std::string_view::npos;
    for (std::size_t start = usage_start; start + frame_marker.size() <= name.size(); ++start)
        if (contains_ci(name.substr(start, frame_marker.size()), frame_marker))
        {
            frame_start = start;
            break;
        }
    const auto number = [](std::string_view digits, int &value) {
        if (digits.empty() || digits.size() > 6U)
            return false;
        int result{};
        for (const char digit : digits)
        {
            if (digit < '0' || digit > '9')
                return false;
            result = result * 10 + (digit - '0');
        }
        value = result;
        return true;
    };
    int usage{};
    int frame{};
    if (frame_start == std::string_view::npos ||
        !number(name.substr(usage_start, frame_start - usage_start), usage))
        return;
    const std::size_t frame_digits = frame_start + frame_marker.size();
    const std::size_t frame_end = name.find(':', frame_digits);
    if (!number(name.substr(frame_digits, frame_end == std::string_view::npos
                                              ? std::string_view::npos
                                              : frame_end - frame_digits),
                frame))
        return;
    batch.face_usage = usage;
    batch.face_frame = frame;
}

// AvatarBatch.FingerWeight: weight on the articulated digits (not the prop and
// "special" attachment bones that share their index range).
float finger_weight(const SourceVertex &vertex) noexcept
{
    float result{};
    for (std::size_t influence = 0; influence < 4U; ++influence)
    {
        const int bone = vertex.bindings[influence];
        if (bone >= bone_first_finger && bone <= bone_last_finger && bone != bone_prop_left &&
            bone != bone_special_left && bone != rig::bone_prop_right && bone != bone_special_right)
            result += static_cast<float>(vertex.weights[influence]) / 255.0F;
    }
    return result;
}

// AvatarBatch.HasFingerTriangles.
bool has_finger_triangles(const RawBatch &batch) noexcept
{
    int triangles{};
    for (std::uint32_t triangle = 0; triangle + 2U < batch.index_count; triangle += 3U)
    {
        bool whole = true;
        for (std::uint32_t corner = 0; corner < 3U && whole; ++corner)
        {
            std::uint16_t index{};
            std::memcpy(&index, batch.indices.data() + (triangle + corner) * 2U, 2U);
            whole = finger_weight(vertex_at(batch.vertices, index)) >= 0.5F;
        }
        if (whole && ++triangles >= 10)
            return true;
    }
    return false;
}

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

template <typename Container> bool all_finite(const Container &values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](float value) { return finite(value); });
}

// Scale(1,1,-1) * M * Scale(1,1,-1): the same transform in the other handedness.
rig::Matrix reflect_z(rig::Matrix matrix) noexcept
{
    for (const std::size_t index : {2U, 6U, 14U, 8U, 9U, 11U})
        matrix[index] = -matrix[index];
    return matrix;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t hash = 0xCBF29CE484222325ULL) noexcept
{
    for (const std::uint8_t byte : bytes)
    {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

struct TextureStats final
{
    std::uint8_t minimum_alpha{255U};
    std::uint8_t maximum_alpha{255U};
};

bool cancelled(const std::atomic<bool> *cancel) noexcept
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

} // namespace

bool png_dimensions(std::span<const std::uint8_t> png, int &width, int &height) noexcept
{
    // Eight signature bytes, then IHDR: its first eight data bytes are width
    // and height, big-endian, at offsets 16..23.
    constexpr std::array<std::uint8_t, 8> signature{0x89U, 0x50U, 0x4EU, 0x47U,
                                                    0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (png.size() < 24U || !std::equal(signature.begin(), signature.end(), png.begin()) ||
        png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R')
        return false;
    const auto big_endian = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(png[offset]) << 24U) |
               (static_cast<std::uint32_t>(png[offset + 1U]) << 16U) |
               (static_cast<std::uint32_t>(png[offset + 2U]) << 8U) |
               static_cast<std::uint32_t>(png[offset + 3U]);
    };
    const std::uint32_t wide = big_endian(16U);
    const std::uint32_t high = big_endian(20U);
    if (wide == 0U || high == 0U || wide > 65'535U || high > 65'535U)
        return false;
    width = static_cast<int>(wide);
    height = static_cast<int>(high);
    return true;
}

std::shared_ptr<const Skin> load(std::span<const std::uint8_t> file, const ImageDecoder &decoder,
                                 std::string &error, const Limits &limits,
                                 const std::atomic<bool> *cancel)
{
    const auto fail = [&](std::string reason) -> std::shared_ptr<const Skin> {
        error = std::move(reason);
        return nullptr;
    };
    error.clear();
    try
    {
        if (file.size() > limits.file_bytes)
            return fail("the avatar file is larger than an avatar may be");
        Cursor cursor{file};
        std::uint32_t signature{};
        std::int32_t version{};
        std::int32_t bones{};
        if (!cursor.read(signature) || signature != file_signature)
            return fail("not an Xbox avatar file");
        if (!cursor.read(version) || version < 1 || version > newest_file_version)
            return fail("unsupported Xbox avatar file version");
        if (!cursor.read(bones) || bones != bone_count)
            return fail("an Xbox avatar file must carry 71 bones");

        std::array<rig::Matrix, bone_count> inverse_bind{};
        std::array<rig::Matrix, bone_count> source_local{};
        for (rig::Matrix &matrix : inverse_bind)
            if (!cursor.matrix(matrix) || !all_finite(matrix) ||
                std::any_of(matrix.begin(), matrix.end(),
                            [](float value) { return std::abs(value) > 1.0e4F; }))
                return fail("invalid bind pose");
        if (version >= 2)
        {
            for (rig::Matrix &matrix : source_local)
                if (!cursor.matrix(matrix) || !all_finite(matrix))
                    return fail("invalid source pose");
        }
        else
        {
            // Version 1 carried no source pose: it is the bind pose, bone by bone.
            std::array<rig::Matrix, bone_count> absolute{};
            for (std::size_t bone = 0; bone < absolute.size(); ++bone)
                absolute[bone] = rig::invert(inverse_bind[bone]);
            source_local[0] = absolute[0];
            for (std::size_t bone = 1; bone < absolute.size(); ++bone)
                source_local[bone] = rig::multiply(
                    absolute[bone], rig::invert(absolute[static_cast<std::size_t>(parents[bone])]));
            for (const rig::Matrix &matrix : source_local)
                if (!all_finite(matrix))
                    return fail("invalid bind pose");
        }

        std::int32_t batch_count{};
        if (!cursor.read(batch_count) || batch_count <= 0 ||
            static_cast<std::size_t>(batch_count) > limits.batches)
            return fail("invalid batch count");

        std::vector<RawBatch> batches(static_cast<std::size_t>(batch_count));
        std::size_t total_vertices{};
        for (RawBatch &batch : batches)
        {
            if (cancelled(cancel))
                return fail("cancelled");
            if (!cursor.string(batch.name, limits.name_bytes))
                return fail("invalid batch name");
            parse_face_layer(batch);
            if (version >= 3)
            {
                std::int32_t shader{};
                std::array<float, 12> palette{};
                std::span<const std::uint8_t> raw_palette;
                if (!cursor.read(batch.category) || !cursor.read(shader) ||
                    !cursor.read(batch.palette_mask) ||
                    !cursor.take(sizeof(float) * palette.size(), raw_palette))
                    return fail("truncated batch header");
                std::memcpy(palette.data(), raw_palette.data(), raw_palette.size());
                batch.palette0 = {palette[0], palette[1], palette[2]};
            }
            batch.base_body = starts_with_ci(batch.name, "00000002-") && contains_ci(batch.name, ":body:");
            batch.overlay = contains_ci(batch.name, "overlay") || contains_ci(batch.name, "decal");
            batch.hand_component = version >= 3 ? (batch.category & 0x00000080U) != 0U
                                                : starts_with_ci(batch.name, "00000880-");
            std::int32_t vertex_count{};
            if (!cursor.read(vertex_count) || vertex_count <= 0 ||
                static_cast<std::size_t>(vertex_count) > limits.batch_vertices ||
                !cursor.take(static_cast<std::size_t>(vertex_count) * vertex_bytes, batch.vertices))
                return fail("invalid vertex count in " + batch.name);
            batch.vertex_count = static_cast<std::uint32_t>(vertex_count);
            total_vertices += batch.vertex_count;
            if (total_vertices > limits.total_vertices)
                return fail("the avatar has too many vertices");
            std::int32_t index_count{};
            if (!cursor.read(index_count) || index_count <= 0 || index_count % 3 != 0 ||
                index_count > 1'000'000 ||
                !cursor.take(static_cast<std::size_t>(index_count) * 2U, batch.indices))
                return fail("invalid index count in " + batch.name);
            batch.index_count = static_cast<std::uint32_t>(index_count);
            for (std::uint32_t index = 0; index < batch.index_count; ++index)
            {
                // The draw call does not check indices; a peer-supplied file must
                // not be able to point one past the vertex array.
                std::uint16_t value{};
                std::memcpy(&value, batch.indices.data() + index * 2U, 2U);
                if (value >= batch.vertex_count)
                    return fail("index out of range in " + batch.name);
            }
            std::int32_t texture_length{};
            if (!cursor.read(batch.diffuse[0]) || !cursor.read(batch.diffuse[1]) ||
                !cursor.read(batch.diffuse[2]) || !all_finite(batch.diffuse) ||
                !cursor.read(texture_length) || texture_length < 0 ||
                !cursor.take(static_cast<std::size_t>(texture_length), batch.png))
                return fail("invalid material in " + batch.name);
            for (float &channel : batch.diffuse)
                channel = std::clamp(channel, 0.0F, 4.0F);
            if (!batch.png.empty())
            {
                int width{};
                int height{};
                if (!png_dimensions(batch.png, width, height))
                    return fail("texture is not a PNG in " + batch.name);
                if (width > limits.texture_side || height > limits.texture_side)
                    return fail("texture is out of range in " + batch.name);
            }
        }

        // Which batches are drawn. The add-on shows one frame of every animated
        // face layer, the one its expression selects; the game leaves the
        // expression neutral, which is frame zero of every layer.
        std::vector<RawBatch *> drawn;
        for (RawBatch &batch : batches)
            if (batch.face_usage < 0 || batch.face_frame == 0)
                drawn.push_back(&batch);
        for (RawBatch *batch : drawn)
        {
            if (cancelled(cancel))
                return fail("cancelled");
            batch->finger_geometry = has_finger_triangles(*batch);
        }
        // AvatarAsset.ChooseBaseBodyBatch: the first body layer that is not an
        // overlay, else the first of any kind.
        const RawBatch *base_body{};
        for (const RawBatch *batch : drawn)
            if (batch->base_body)
            {
                if (base_body == nullptr)
                    base_body = batch;
                if (!batch->overlay)
                {
                    base_body = batch;
                    break;
                }
            }
        for (RawBatch *batch : drawn)
        {
            if (!batch->hand_component || !batch->finger_geometry)
                continue;
            // A hand component whose palette is the body's is the bare-hand
            // shell; a differing palette, or a combined outfit, is a glove.
            const bool combined_clothing = (batch->category & 0x00000008U) != 0U ||
                                           (batch->category & 0x00800000U) != 0U;
            bool matches_skin = false;
            if (version >= 3 && base_body != nullptr && (batch->palette_mask & 1U) != 0U &&
                (base_body->palette_mask & 1U) != 0U)
            {
                float distance{};
                for (std::size_t channel = 0; channel < 3U; ++channel)
                {
                    const float delta = batch->palette0[channel] - base_body->palette0[channel];
                    distance += delta * delta;
                }
                matches_skin = distance <= 0.01F;
            }
            batch->bare_hand_shell = !combined_clothing && (version < 3 || matches_skin);
        }

        auto skin = std::make_shared<Skin>();
        skin->file_version = version;
        skin->source_batches = batches.size();

        // Bones. The export's frames are left-handed; conjugating by the Z
        // reflection moves a transform into the rig's handedness without
        // turning it into a mirror. Offsets reflect their Z; scale is unchanged.
        for (std::size_t bone = 0; bone < skin->bones.size(); ++bone)
        {
            Bone &target = skin->bones[bone];
            target.inverse_bind = reflect_z(inverse_bind[bone]);
            const rig::Matrix &source = source_local[bone];
            target.translation = {source[12], source[13], -source[14]};
            if (std::abs(target.translation.x) > limits.bone_offset_extent ||
                std::abs(target.translation.y) > limits.bone_offset_extent ||
                std::abs(target.translation.z) > limits.bone_offset_extent)
                return fail("bone offset out of range");
            const auto axis_scale = [&](std::size_t row) {
                const float length = std::sqrt(source[row * 4U] * source[row * 4U] +
                                               source[row * 4U + 1U] * source[row * 4U + 1U] +
                                               source[row * 4U + 2U] * source[row * 4U + 2U]);
                // AvatarAsset.SafeShapeScale: a bone that claims to have
                // collapsed or exploded keeps the stock size instead.
                return finite(length) && length >= limits.minimum_scale && length <= limits.maximum_scale
                           ? length
                           : 1.0F;
            };
            target.scale = {axis_scale(0U), axis_scale(1U), axis_scale(2U)};
            const rig::Vec3 parent =
                bone == 0U ? rig::Vec3{1.0F, 1.0F, 1.0F}
                           : skin->bones[static_cast<std::size_t>(parents[bone])].cumulative_scale;
            const auto bounded = [&](float value) {
                return std::clamp(value, limits.minimum_scale, limits.maximum_scale);
            };
            target.cumulative_scale = {bounded(parent.x * target.scale.x), bounded(parent.y * target.scale.y),
                                       bounded(parent.z * target.scale.z)};
            skin->build = std::max({skin->build, target.cumulative_scale.x, target.cumulative_scale.y,
                                    target.cumulative_scale.z});
        }

        // Textures: each distinct image is decoded once.
        struct TextureKey final
        {
            std::uint64_t hash{};
            std::span<const std::uint8_t> png;
            std::int32_t index{};
        };
        std::vector<TextureKey> texture_keys;
        std::vector<TextureStats> texture_stats;
        std::size_t texture_pixels{};
        const auto texture_for = [&](std::span<const std::uint8_t> png, std::int32_t &index) -> bool {
            index = -1;
            if (png.empty())
                return true;
            const std::uint64_t hash = fnv1a(png);
            for (const TextureKey &key : texture_keys)
                if (key.hash == hash && key.png.size() == png.size() &&
                    std::equal(png.begin(), png.end(), key.png.begin()))
                {
                    index = key.index;
                    return true;
                }
            int width{};
            int height{};
            if (!png_dimensions(png, width, height))
                return false;
            texture_pixels += static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            if (texture_pixels > limits.texture_pixels)
            {
                error = "the avatar's textures are too large";
                return false;
            }
            Image image;
            if (!decoder || !decoder(png, image) || image.width != width || image.height != height ||
                image.rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U)
            {
                // The add-on gives up on that one image and draws the surface
                // untextured rather than losing the whole avatar.
                texture_keys.push_back({hash, png, -1});
                return true;
            }
            TextureStats stats{255U, 0U};
            for (std::size_t alpha = 3U; alpha < image.rgba.size(); alpha += 4U)
            {
                stats.minimum_alpha = std::min(stats.minimum_alpha, image.rgba[alpha]);
                stats.maximum_alpha = std::max(stats.maximum_alpha, image.rgba[alpha]);
            }
            index = static_cast<std::int32_t>(skin->textures.size());
            skin->textures.push_back(std::move(image));
            texture_stats.push_back(stats);
            texture_keys.push_back({hash, png, index});
            return true;
        };

        // Geometry: layers painted over one surface share its skinning.
        struct GeometryKey final
        {
            std::uint64_t hash{};
            std::vector<std::uint8_t> bytes;
        };
        std::vector<GeometryKey> geometry_keys;
        struct LayerNote final
        {
            bool opaque_everywhere{};
            float minimum_alpha{};
        };
        std::vector<LayerNote> notes;

        for (const RawBatch *batch : drawn)
        {
            if (cancelled(cancel))
                return fail("cancelled");
            // The skinning identity of this batch: everything but UV and colour.
            std::vector<std::uint8_t> key(static_cast<std::size_t>(batch->vertex_count) * 32U);
            for (std::uint32_t index = 0; index < batch->vertex_count; ++index)
            {
                const std::uint8_t *raw = batch->vertices.data() + index * vertex_bytes;
                std::memcpy(key.data() + index * 32U, raw, 24U);
                std::memcpy(key.data() + index * 32U + 24U, raw + 32U, 8U);
            }
            const std::uint64_t hash = fnv1a(key);
            std::uint32_t geometry_index = static_cast<std::uint32_t>(skin->geometry.size());
            for (std::size_t existing = 0; existing < geometry_keys.size(); ++existing)
                if (geometry_keys[existing].hash == hash && geometry_keys[existing].bytes == key)
                {
                    geometry_index = static_cast<std::uint32_t>(existing);
                    break;
                }
            if (geometry_index == skin->geometry.size())
            {
                Geometry geometry;
                geometry.position.resize(batch->vertex_count);
                geometry.normal.resize(batch->vertex_count);
                geometry.bones.resize(batch->vertex_count);
                geometry.weights.resize(batch->vertex_count);
                geometry.head_only = true;
                for (std::uint32_t index = 0; index < batch->vertex_count; ++index)
                {
                    const SourceVertex source = vertex_at(batch->vertices, index);
                    if (!all_finite(source.position) || !all_finite(source.normal) ||
                        std::abs(source.position[0]) > limits.position_extent ||
                        std::abs(source.position[1]) > limits.position_extent ||
                        std::abs(source.position[2]) > limits.position_extent)
                        return fail("vertex out of range in " + batch->name);
                    geometry.position[index] = {source.position[0], source.position[1], -source.position[2]};
                    geometry.normal[index] = {source.normal[0], source.normal[1], -source.normal[2]};
                    // An influence that names a bone the skeleton does not have
                    // is skipped and the rest renormalised, as the add-on does.
                    float total{};
                    float head{};
                    std::array<float, 4> weights{};
                    std::array<std::uint8_t, 4> bones_used{};
                    for (std::size_t influence = 0; influence < 4U; ++influence)
                    {
                        if (source.bindings[influence] >= bone_count)
                            continue;
                        weights[influence] = static_cast<float>(source.weights[influence]) / 255.0F;
                        bones_used[influence] = source.bindings[influence];
                        total += weights[influence];
                        if (source.bindings[influence] == rig::bone_head)
                            head += weights[influence];
                    }
                    if (total <= 0.0001F)
                    {
                        // Unweighted: carried by the root rather than left
                        // behind in bind space.
                        weights = {1.0F, 0.0F, 0.0F, 0.0F};
                        bones_used = {};
                        total = 1.0F;
                        head = 0.0F;
                    }
                    for (float &weight : weights)
                        weight /= total;
                    geometry.bones[index] = bones_used;
                    geometry.weights[index] = weights;
                    if (head / total < 0.999F)
                        geometry.head_only = false;
                }
                skin->vertices += batch->vertex_count;
                skin->geometry.push_back(std::move(geometry));
                geometry_keys.push_back({hash, std::move(key)});
            }

            // ImportedAvatarModelEntity.Draw: a bare-hand shell wears the base
            // body's material and ignores its own vertex colours, which are
            // shader masks rather than colour.
            const RawBatch *material = batch->bare_hand_shell && base_body != nullptr ? base_body : batch;
            Layer layer;
            layer.name = batch->name;
            layer.geometry = geometry_index;
            layer.clamp = batch->face_usage >= 0;
            if (!texture_for(material->png, layer.texture))
                return fail(error.empty() ? "invalid texture in " + batch->name : error);
            TextureStats stats{};
            if (layer.texture >= 0)
                stats = texture_stats[static_cast<std::size_t>(layer.texture)];
            layer.uv.resize(batch->vertex_count);
            layer.color.resize(batch->vertex_count);
            std::uint8_t minimum_vertex_alpha{255U};
            std::uint8_t maximum_vertex_alpha{};
            for (std::uint32_t index = 0; index < batch->vertex_count; ++index)
            {
                const SourceVertex source = vertex_at(batch->vertices, index);
                float u = finite(source.uv[0]) ? std::clamp(source.uv[0], -64.0F, 64.0F) : 0.0F;
                float v = finite(source.uv[1]) ? std::clamp(source.uv[1], -64.0F, 64.0F) : 0.0F;
                // Files before version 4 stored mesh UVs flipped in V; face
                // layers were pre-inverted on import and are already right.
                if (version < 4 && batch->face_usage < 0)
                    v = 1.0F - v;
                layer.uv[index] = {u, v};
                if (batch->bare_hand_shell)
                {
                    layer.color[index] = {material->diffuse[0], material->diffuse[1], material->diffuse[2], 1.0F};
                    minimum_vertex_alpha = 255U;
                    maximum_vertex_alpha = 255U;
                    continue;
                }
                constexpr float scale = 1.0F / 255.0F;
                layer.color[index] = {material->diffuse[0] * static_cast<float>(source.color[0]) * scale,
                                      material->diffuse[1] * static_cast<float>(source.color[1]) * scale,
                                      material->diffuse[2] * static_cast<float>(source.color[2]) * scale,
                                      static_cast<float>(source.color[3]) * scale};
                minimum_vertex_alpha = std::min(minimum_vertex_alpha, source.color[3]);
                maximum_vertex_alpha = std::max(maximum_vertex_alpha, source.color[3]);
            }
            // A layer that can never contribute one eight-bit step is not drawn.
            const float maximum_alpha = static_cast<float>(maximum_vertex_alpha) *
                                        static_cast<float>(stats.maximum_alpha) / 255.0F;
            if (maximum_alpha < 1.5F)
                continue;
            layer.indices.resize(batch->index_count);
            std::memcpy(layer.indices.data(), batch->indices.data(), batch->indices.size());
            LayerNote note;
            note.opaque_everywhere = minimum_vertex_alpha == 255U && stats.minimum_alpha == 255U;
            note.minimum_alpha = static_cast<float>(minimum_vertex_alpha) *
                                 static_cast<float>(stats.minimum_alpha) / (255.0F * 255.0F);
            skin->layers.push_back(std::move(layer));
            notes.push_back(note);
        }

        // A layer that is opaque everywhere hides whatever was painted on the
        // same triangles before it. Those are skipped: same picture, a third of
        // the triangles for a typical avatar.
        std::vector<bool> hidden(skin->layers.size(), false);
        for (std::size_t later = 0; later < skin->layers.size(); ++later)
        {
            if (!notes[later].opaque_everywhere)
                continue;
            for (std::size_t earlier = 0; earlier < later; ++earlier)
                if (!hidden[earlier] && skin->layers[earlier].geometry == skin->layers[later].geometry &&
                    skin->layers[earlier].indices == skin->layers[later].indices)
                    hidden[earlier] = true;
        }
        std::vector<Layer> kept_layers;
        std::vector<bool> surface_started(skin->geometry.size(), false);
        for (std::size_t index = 0; index < skin->layers.size(); ++index)
        {
            if (hidden[index])
                continue;
            Layer &layer = skin->layers[index];
            // The first layer of a surface is its depth-writing base when it is
            // substantially solid; everything after it is painted over.
            const bool first = !surface_started[layer.geometry];
            surface_started[layer.geometry] = true;
            layer.blended = !(first && notes[index].minimum_alpha >= 0.5F);
            skin->triangles += layer.indices.size() / 3U;
            kept_layers.push_back(std::move(layer));
        }
        skin->layers = std::move(kept_layers);
        if (skin->layers.empty())
            return fail("the avatar has nothing to draw");
        if (skin->triangles > limits.drawn_triangles)
            return fail("the avatar has too many triangles");
        // Geometry no surviving layer uses is released.
        std::vector<bool> used(skin->geometry.size(), false);
        for (const Layer &layer : skin->layers)
            used[layer.geometry] = true;
        for (std::size_t index = 0; index < skin->geometry.size(); ++index)
            if (!used[index])
                skin->geometry[index] = Geometry{};
        error.clear();
        return skin;
    }
    catch (const std::exception &)
    {
        return fail("out of memory while reading the avatar");
    }
}

void pose(const Skin &skin, const rig::CharacterPose &character, Placement placement,
          Pose &result) noexcept
{
    const rig::RetailRig &rig = character.rig();
    const rig::BonePose &skeleton = character.skeleton();
    // The animated bone with this avatar's build on it. rig::compose is
    // Bone.ComposeTransform: scale, then rotation, translation written last.
    const auto animated = [&](std::size_t bone) {
        rig::BoneTransform transform = skeleton[bone];
        const rig::Vec3 &build = skin.bones[bone].scale;
        transform.scale = {transform.scale.x * build.x, transform.scale.y * build.y,
                           transform.scale.z * build.z};
        return transform;
    };
    const auto chained = [&](std::size_t bone) {
        // Avatar.OnUpdate: the animated rotation and scale, this skeleton's own
        // bind translation.
        rig::BoneTransform transform = animated(bone);
        transform.translation = skin.bones[bone].translation;
        return rig::multiply(rig::compose(transform), result.world[static_cast<std::size_t>(parents[bone])]);
    };

    if (placement == Placement::own_proportions)
    {
        // The root is the animated root times the rig's constant (the half turn
        // about Y, lifted to the hips), exactly as for the stock body; the lift
        // is this avatar's own, and root motion grows with the avatar's build
        // so a taller body still plants its feet.
        rig::BoneTransform root = animated(0U);
        const rig::Vec3 &build = skin.bones[0].scale;
        root.translation = {root.translation.x * build.x, root.translation.y * build.y,
                            root.translation.z * build.z};
        rig::Matrix adjust = rig.root_adjust;
        adjust[12] = skin.bones[0].translation.x;
        adjust[13] = skin.bones[0].translation.y;
        adjust[14] = skin.bones[0].translation.z;
        result.world[0] = rig::multiply(rig::compose(root), adjust);
        for (std::size_t bone = 1; bone < result.world.size(); ++bone)
            result.world[bone] = chained(bone);
    }
    else
    {
        // Which body bone each avatar bone shares its joint with.
        std::array<int, bone_count> body_bone;
        body_bone.fill(-1);
        for (std::size_t index = 0; index < rig.body.size(); ++index)
        {
            const int source = rig.body[index].source;
            if (source >= 0 && source < bone_count && body_bone[static_cast<std::size_t>(source)] < 0)
                body_bone[static_cast<std::size_t>(source)] = static_cast<int>(index);
        }
        const std::vector<rig::Matrix> &body_world = character.body_world();
        std::array<bool, bone_count> below_wrist{};
        for (std::size_t bone = 1; bone < result.world.size(); ++bone)
        {
            const int parent = parents[bone];
            below_wrist[bone] = parent == bone_wrist_left || parent == bone_wrist_right ||
                                below_wrist[static_cast<std::size_t>(parent)];
        }
        for (std::size_t bone = 0; bone < result.world.size(); ++bone)
        {
            const int body = body_bone[bone];
            if (body >= 0 && static_cast<std::size_t>(body) < body_world.size() && !below_wrist[bone])
            {
                // The stock joint, with the avatar's build about it.
                rig::Matrix world = body_world[static_cast<std::size_t>(body)];
                const rig::Vec3 &build = skin.bones[bone].cumulative_scale;
                for (std::size_t column = 0; column < 3U; ++column)
                {
                    world[column] *= build.x;
                    world[4U + column] *= build.y;
                    world[8U + column] *= build.z;
                }
                result.world[bone] = world;
            }
            else if (bone == 0U)
                result.world[0] = rig::multiply(rig::compose(animated(0U)), rig.root_adjust);
            else
                // Below each wrist the hand keeps its own offsets, so a hand of
                // any size stays whole around the stock wrist.
                result.world[bone] = chained(bone);
        }
    }
    for (std::size_t bone = 0; bone < result.world.size(); ++bone)
        result.palette[bone] = rig::multiply(skin.bones[bone].inverse_bind, result.world[bone]);
}

rig::Matrix held_item_parent(const rig::CharacterPose &character, const Pose &pose,
                             Placement placement) noexcept
{
    return placement == Placement::own_proportions ? pose.world[rig::bone_prop_right]
                                                    : character.right_hand();
}

void skin_geometry(const Geometry &geometry, const std::array<rig::Matrix, bone_count> &palette,
                   SkinnedGeometry &result)
{
    result.position.resize(geometry.position.size());
    result.normal.resize(geometry.position.size());
    for (std::size_t index = 0; index < geometry.position.size(); ++index)
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{};
        for (std::size_t influence = 0; influence < 4U; ++influence)
        {
            const float weight = geometry.weights[index][influence];
            if (weight == 0.0F)
                continue;
            const rig::Matrix &bone = palette[geometry.bones[index][influence]];
            const auto moved = rig::transform_point(geometry.position[index].data(), bone);
            const auto turned = rig::transform_vector(geometry.normal[index].data(), bone);
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                position[axis] += moved[axis] * weight;
                normal[axis] += turned[axis] * weight;
            }
        }
        result.position[index] = position;
        result.normal[index] = normal;
    }
}

Digest sha256(std::span<const std::uint8_t> bytes) noexcept
{
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
        0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
        0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
        0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
        0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
        0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
        0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
        0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
        0xC67178F2U};
    std::array<std::uint32_t, 8> state{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                       0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    const auto transform = [&](const std::uint8_t *block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index)
            words[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                           (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[index * 4U + 3U]);
        for (std::size_t index = 16U; index < words.size(); ++index)
        {
            const std::uint32_t small0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^
                                         (words[index - 15U] >> 3U);
            const std::uint32_t small1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^
                                         (words[index - 2U] >> 10U);
            words[index] = small1 + words[index - 7U] + small0 + words[index - 16U];
        }
        std::array<std::uint32_t, 8> v = state;
        for (std::size_t index = 0; index < words.size(); ++index)
        {
            const std::uint32_t big1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
            const std::uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t first = v[7] + big1 + choose + constants[index] + words[index];
            const std::uint32_t big0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
            const std::uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t second = big0 + majority;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + first;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = first + second;
        }
        for (std::size_t index = 0; index < state.size(); ++index)
            state[index] += v[index];
    };
    std::size_t offset{};
    for (; offset + 64U <= bytes.size(); offset += 64U)
        transform(bytes.data() + offset);
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = bytes.size() - offset;
    if (rest > 0U)
        std::memcpy(tail.data(), bytes.data() + offset, rest);
    tail[rest] = 0x80U;
    const std::size_t tail_blocks = rest + 9U > 64U ? 2U : 1U;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (std::size_t index = 0; index < 8U; ++index)
        tail[tail_blocks * 64U - 1U - index] = static_cast<std::uint8_t>(bit_length >> (index * 8U));
    for (std::size_t block = 0; block < tail_blocks; ++block)
        transform(tail.data() + block * 64U);
    Digest digest{};
    for (std::size_t word = 0; word < state.size(); ++word)
        for (std::size_t byte = 0; byte < 4U; ++byte)
            digest[word * 4U + byte] = static_cast<std::uint8_t>(state[word] >> ((3U - byte) * 8U));
    return digest;
}

std::string digest_text(const Digest &digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

} // namespace openminerz::xbox_avatar
