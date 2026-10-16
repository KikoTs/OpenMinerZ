// glTF 2.0 props and creatures for mods (openminerz/modding/graphics.hpp):
// bounded parsing with cgltf, pose evaluation and skinning matrices, and the
// per-session cache of loaded models.

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4018 4100 4189 4244 4267 4456 4701 4702 4706 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace openminerz::modding::graphics
{
namespace
{

class Failure final
{
  public:
    explicit Failure(std::string message) : message_(std::move(message))
    {
    }
    [[nodiscard]] const std::string &message() const noexcept
    {
        return message_;
    }

  private:
    std::string message_;
};

[[noreturn]] void fail(std::string message)
{
    throw Failure{std::move(message)};
}

const char *cgltf_reason(cgltf_result result) noexcept
{
    switch (result)
    {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 is not supported";
    default:
        return "error";
    }
}

struct DataDeleter final
{
    void operator()(cgltf_data *data) const noexcept
    {
        cgltf_free(data);
    }
};
using DataPointer = std::unique_ptr<cgltf_data, DataDeleter>;

// Base64 payload of a data: URI and its decoded length; empty when not base64.
std::pair<const char *, std::size_t> base64_payload(const char *uri)
{
    if (uri == nullptr || std::strncmp(uri, "data:", 5U) != 0)
        return {nullptr, 0U};
    const char *comma = std::strchr(uri, ',');
    if (comma == nullptr || comma - uri < 7 || std::strncmp(comma - 7, ";base64", 7U) != 0)
        return {nullptr, 0U};
    const char *payload = comma + 1;
    std::size_t length = std::strlen(payload);
    std::size_t padding = 0U;
    while (length > 0U && payload[length - 1U] == '=')
    {
        --length;
        ++padding;
    }
    (void)padding;
    return {payload, length * 3U / 4U};
}

// A relative file URI resolved inside `base` (the model's folder), or empty.
std::optional<std::filesystem::path> relative_file(const std::filesystem::path &base,
                                                   const char *uri)
{
    if (uri == nullptr || std::strstr(uri, "://") != nullptr)
        return std::nullopt;
    std::string decoded{uri};
    decoded.resize(cgltf_decode_uri(decoded.data()));
    return contained_path(base, decoded);
}

void load_buffers(cgltf_data &data, const cgltf_options &options, const std::filesystem::path &base,
                  const ModelLimits &limits)
{
    std::size_t total{};
    for (cgltf_size index = 0U; index < data.buffers_count; ++index)
    {
        cgltf_buffer &buffer = data.buffers[index];
        if (buffer.size > limits.max_file_bytes || (total += buffer.size) > limits.max_file_bytes)
            fail("buffers are larger than the model limit");
        if (buffer.data != nullptr)
            continue;
        if (buffer.uri == nullptr)
        {
            // The GLB binary chunk.
            if (index != 0U || data.bin == nullptr)
                fail("buffer " + std::to_string(index) + " has no data");
            if (data.bin_size < buffer.size)
                fail("the GLB binary chunk is shorter than its buffer");
            buffer.data = const_cast<void *>(data.bin);
            buffer.data_free_method = cgltf_data_free_method_none;
            continue;
        }
        if (const auto [payload, decoded] = base64_payload(buffer.uri); payload != nullptr)
        {
            if (decoded < buffer.size)
                fail("embedded buffer " + std::to_string(index) + " is shorter than declared");
            void *bytes{};
            if (cgltf_load_buffer_base64(&options, buffer.size, payload, &bytes) !=
                cgltf_result_success)
                fail("embedded buffer " + std::to_string(index) + " is not valid base64");
            buffer.data = bytes;
            buffer.data_free_method = cgltf_data_free_method_memory_free;
            continue;
        }
        const auto path = relative_file(base, buffer.uri);
        if (!path)
            fail(std::string{"buffer URI \""} + buffer.uri +
                 "\" must be a file beside the model (no '..', no absolute paths)");
        std::string error;
        const auto bytes = read_file_bounded(*path, limits.max_file_bytes, &error);
        if (!bytes)
            fail(error);
        if (bytes->size() < buffer.size)
            fail(path->filename().string() + " is shorter than its buffer");
        void *copy = std::malloc(std::max<std::size_t>(buffer.size, 1U));
        if (copy == nullptr)
            fail("out of memory");
        if (buffer.size > 0U)
            std::memcpy(copy, bytes->data(), buffer.size);
        buffer.data = copy;
        buffer.data_free_method = cgltf_data_free_method_memory_free;
    }
}

int index_of(const cgltf_data &data, const cgltf_node *node)
{
    if (node == nullptr)
        return -1;
    const std::ptrdiff_t index = node - data.nodes;
    if (index < 0 || static_cast<cgltf_size>(index) >= data.nodes_count)
        fail("node reference out of range");
    return static_cast<int>(index);
}

Image load_texture_image(const cgltf_texture *texture, const std::filesystem::path &base,
                         const ModelLimits &limits, const cgltf_options &options)
{
    if (texture == nullptr || texture->image == nullptr)
        return {};
    const cgltf_image &image = *texture->image;
    std::string error;
    std::optional<Image> decoded;
    if (image.buffer_view != nullptr)
    {
        const std::uint8_t *bytes = cgltf_buffer_view_data(image.buffer_view);
        if (bytes == nullptr)
            fail("embedded image has no data");
        decoded = decode_image({bytes, image.buffer_view->size}, &error, limits.images);
    }
    else if (const auto [payload, length] = base64_payload(image.uri); payload != nullptr)
    {
        if (length > limits.images.max_file_bytes)
            fail("embedded image too large");
        void *bytes{};
        if (cgltf_load_buffer_base64(&options, length, payload, &bytes) != cgltf_result_success)
            fail("embedded image is not valid base64");
        decoded =
            decode_image({static_cast<const std::uint8_t *>(bytes), length}, &error, limits.images);
        std::free(bytes);
    }
    else if (const auto path = relative_file(base, image.uri))
        decoded = load_image(*path, &error, limits.images);
    else
        fail("image URI must be a file beside the model");
    if (!decoded)
        fail("texture: " + error);
    return std::move(*decoded);
}

std::vector<float> unpack(const cgltf_accessor *accessor, std::size_t components,
                          std::size_t max_count)
{
    if (accessor == nullptr)
        return {};
    if (accessor->count > max_count)
        fail("accessor has too many elements");
    if (cgltf_num_components(accessor->type) != components)
        fail("accessor has the wrong element type");
    std::vector<float> values(accessor->count * components);
    if (!values.empty() &&
        cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
        fail("accessor could not be read");
    for (const float value : values)
        if (!std::isfinite(value))
            fail("accessor holds a non-finite number");
    return values;
}

void read_primitive(Model &model, const cgltf_data &data, const cgltf_primitive &source, int node,
                    int skin, const ModelLimits &limits)
{
    if (source.type != cgltf_primitive_type_triangles)
        return; // points and lines are not drawn
    const cgltf_accessor *positions{};
    const cgltf_accessor *normals{};
    const cgltf_accessor *uvs{};
    const cgltf_accessor *joints{};
    const cgltf_accessor *weights{};
    for (cgltf_size index = 0U; index < source.attributes_count; ++index)
    {
        const cgltf_attribute &attribute = source.attributes[index];
        if (attribute.index != 0)
            continue;
        switch (attribute.type)
        {
        case cgltf_attribute_type_position:
            positions = attribute.data;
            break;
        case cgltf_attribute_type_normal:
            normals = attribute.data;
            break;
        case cgltf_attribute_type_texcoord:
            uvs = attribute.data;
            break;
        case cgltf_attribute_type_joints:
            joints = attribute.data;
            break;
        case cgltf_attribute_type_weights:
            weights = attribute.data;
            break;
        default:
            break;
        }
    }
    if (positions == nullptr)
        fail("a primitive has no POSITION");
    const std::size_t count = positions->count;
    if (model.vertex_count + count > limits.max_vertices)
        fail("the model has more than " + std::to_string(limits.max_vertices) + " vertices");
    ModelPrimitive primitive;
    primitive.node = node;
    primitive.material =
        source.material != nullptr ? static_cast<int>(source.material - data.materials) : -1;
    primitive.skin = joints != nullptr && weights != nullptr ? skin : -1;
    const auto position_values = unpack(positions, 3U, limits.max_vertices);
    const auto normal_values = normals != nullptr && normals->count == count
                                   ? unpack(normals, 3U, limits.max_vertices)
                                   : std::vector<float>{};
    const auto uv_values = uvs != nullptr && uvs->count == count
                               ? unpack(uvs, 2U, limits.max_vertices)
                               : std::vector<float>{};
    const auto weight_values = primitive.skin >= 0 && weights->count == count
                                   ? unpack(weights, 4U, limits.max_vertices)
                                   : std::vector<float>{};
    if (primitive.skin >= 0 && (weight_values.empty() || joints->count != count ||
                                cgltf_num_components(joints->type) != 4U))
        primitive.skin = -1;
    const std::size_t joint_count =
        primitive.skin >= 0 ? model.skins[static_cast<std::size_t>(primitive.skin)].joints.size()
                            : 0U;
    primitive.vertices.resize(count);
    for (std::size_t index = 0U; index < count; ++index)
    {
        ModelVertex &vertex = primitive.vertices[index];
        vertex.position = {position_values[index * 3U], position_values[index * 3U + 1U],
                           position_values[index * 3U + 2U]};
        if (!normal_values.empty())
            vertex.normal = {normal_values[index * 3U], normal_values[index * 3U + 1U],
                             normal_values[index * 3U + 2U]};
        if (!uv_values.empty())
            vertex.uv = {uv_values[index * 2U], uv_values[index * 2U + 1U]};
        if (primitive.skin >= 0)
        {
            cgltf_uint indices[4]{};
            if (!cgltf_accessor_read_uint(joints, index, indices, 4U))
                fail("JOINTS_0 could not be read");
            float total{};
            for (std::size_t slot = 0U; slot < 4U; ++slot)
            {
                const float weight = std::max(0.0F, weight_values[index * 4U + slot]);
                if (weight > 0.0F && indices[slot] >= joint_count)
                    fail("a vertex names a joint the skin does not have");
                vertex.joints[slot] =
                    static_cast<std::uint16_t>(std::min<cgltf_uint>(indices[slot], 0xFFFFU));
                vertex.weights[slot] = weight;
                total += weight;
            }
            if (total > 1.0e-6F)
                for (float &weight : vertex.weights)
                    weight /= total;
            else
                vertex.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        }
    }
    if (source.indices != nullptr)
    {
        const cgltf_size index_count = source.indices->count;
        if (index_count % 3U != 0U)
            fail("triangle index count is not a multiple of three");
        if (model.triangle_count * 3U + index_count > limits.max_indices)
            fail("the model has more than " + std::to_string(limits.max_indices) + " indices");
        primitive.indices.resize(index_count);
        for (cgltf_size index = 0U; index < index_count; ++index)
        {
            const cgltf_size value = cgltf_accessor_read_index(source.indices, index);
            if (value >= count)
                fail("a triangle index is out of range");
            primitive.indices[index] = static_cast<std::uint32_t>(value);
        }
    }
    else
    {
        if (count % 3U != 0U)
            fail("triangle vertex count is not a multiple of three");
        primitive.indices.resize(count);
        for (std::size_t index = 0U; index < count; ++index)
            primitive.indices[index] = static_cast<std::uint32_t>(index);
    }
    if (normal_values.empty())
    {
        // Area-weighted smooth normals.
        for (ModelVertex &vertex : primitive.vertices)
            vertex.normal = {};
        for (std::size_t index = 0U; index + 2U < primitive.indices.size(); index += 3U)
        {
            ModelVertex &a = primitive.vertices[primitive.indices[index]];
            ModelVertex &b = primitive.vertices[primitive.indices[index + 1U]];
            ModelVertex &c = primitive.vertices[primitive.indices[index + 2U]];
            const Vec3 face = vfx::cross(b.position - a.position, c.position - a.position);
            a.normal += face;
            b.normal += face;
            c.normal += face;
        }
        for (ModelVertex &vertex : primitive.vertices)
            vertex.normal = vfx::normalized(vertex.normal);
    }
    model.vertex_count += count;
    model.triangle_count += primitive.indices.size() / 3U;
    if (model.primitives.size() >= limits.max_primitives)
        fail("the model has more than " + std::to_string(limits.max_primitives) + " primitives");
    model.primitives.push_back(std::move(primitive));
}

void read_model(Model &model, const cgltf_data &data, const std::filesystem::path &base,
                const ModelLimits &limits, const cgltf_options &options)
{
    if (data.nodes_count > limits.max_nodes)
        fail("the model has more than " + std::to_string(limits.max_nodes) + " nodes");
    if (data.materials_count > limits.max_materials)
        fail("the model has more than " + std::to_string(limits.max_materials) + " materials");
    if (data.animations_count > limits.max_animations)
        fail("the model has more than " + std::to_string(limits.max_animations) + " animations");
    model.nodes.resize(data.nodes_count);
    for (cgltf_size index = 0U; index < data.nodes_count; ++index)
    {
        const cgltf_node &source = data.nodes[index];
        ModelNode &node = model.nodes[index];
        node.name = source.name != nullptr ? std::string{source.name}.substr(0U, 128U) : "";
        node.parent = index_of(data, source.parent);
        if (source.has_matrix)
        {
            // Decompose a TRS matrix (no shear, as glTF requires).
            const float *m = source.matrix;
            node.translation = {m[12], m[13], m[14]};
            const float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
            const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
            node.scale = {sx, sy, sz};
            const float r00 = m[0] / std::max(sx, 1e-8F), r01 = m[4] / std::max(sy, 1e-8F),
                        r02 = m[8] / std::max(sz, 1e-8F);
            const float r10 = m[1] / std::max(sx, 1e-8F), r11 = m[5] / std::max(sy, 1e-8F),
                        r12 = m[9] / std::max(sz, 1e-8F);
            const float r20 = m[2] / std::max(sx, 1e-8F), r21 = m[6] / std::max(sy, 1e-8F),
                        r22 = m[10] / std::max(sz, 1e-8F);
            const float trace = r00 + r11 + r22;
            std::array<float, 4> q{};
            if (trace > 0.0F)
            {
                const float s = std::sqrt(trace + 1.0F) * 2.0F;
                q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25F * s};
            }
            else if (r00 > r11 && r00 > r22)
            {
                const float s = std::sqrt(1.0F + r00 - r11 - r22) * 2.0F;
                q = {0.25F * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
            }
            else if (r11 > r22)
            {
                const float s = std::sqrt(1.0F + r11 - r00 - r22) * 2.0F;
                q = {(r01 + r10) / s, 0.25F * s, (r12 + r21) / s, (r02 - r20) / s};
            }
            else
            {
                const float s = std::sqrt(1.0F + r22 - r00 - r11) * 2.0F;
                q = {(r02 + r20) / s, (r12 + r21) / s, 0.25F * s, (r10 - r01) / s};
            }
            node.rotation = q;
        }
        else
        {
            if (source.has_translation)
                node.translation = {source.translation[0], source.translation[1],
                                    source.translation[2]};
            if (source.has_rotation)
                node.rotation = {source.rotation[0], source.rotation[1], source.rotation[2],
                                 source.rotation[3]};
            if (source.has_scale)
                node.scale = {source.scale[0], source.scale[1], source.scale[2]};
        }
    }
    // Parents before children; anything unreachable is a loop.
    std::vector<std::vector<int>> children(model.nodes.size());
    for (std::size_t index = 0U; index < model.nodes.size(); ++index)
        if (model.nodes[index].parent >= 0)
            children[static_cast<std::size_t>(model.nodes[index].parent)].push_back(
                static_cast<int>(index));
    for (std::size_t index = 0U; index < model.nodes.size(); ++index)
        if (model.nodes[index].parent < 0)
            model.node_order.push_back(static_cast<int>(index));
    for (std::size_t cursor = 0U; cursor < model.node_order.size(); ++cursor)
        for (const int child : children[static_cast<std::size_t>(model.node_order[cursor])])
            model.node_order.push_back(child);
    if (model.node_order.size() != model.nodes.size())
        fail("the node hierarchy has a loop");

    model.materials.resize(data.materials_count);
    for (cgltf_size index = 0U; index < data.materials_count; ++index)
    {
        const cgltf_material &source = data.materials[index];
        ModelMaterial &material = model.materials[index];
        material.name = source.name != nullptr ? std::string{source.name}.substr(0U, 128U) : "";
        if (source.has_pbr_metallic_roughness)
        {
            const auto &pbr = source.pbr_metallic_roughness;
            for (std::size_t channel = 0U; channel < 4U; ++channel)
                material.color_factor[channel] =
                    std::clamp(pbr.base_color_factor[channel], 0.0F, 1.0F);
            material.base_color =
                load_texture_image(pbr.base_color_texture.texture, base, limits, options);
        }
        // (Polish) KHR_materials_emissive_strength scales the factor past 1.
        const float strength =
            source.has_emissive_strength != 0
                ? std::clamp(source.emissive_strength.emissive_strength, 0.0F, 16.0F)
                : 1.0F;
        material.emissive = {std::clamp(source.emissive_factor[0] * strength, 0.0F, 16.0F),
                             std::clamp(source.emissive_factor[1] * strength, 0.0F, 16.0F),
                             std::clamp(source.emissive_factor[2] * strength, 0.0F, 16.0F)};
        if (material.emissive.x > 0.0F || material.emissive.y > 0.0F || material.emissive.z > 0.0F)
            material.emissive_texture =
                load_texture_image(source.emissive_texture.texture, base, limits, options);
        material.blend = source.alpha_mode == cgltf_alpha_mode_blend;
        material.mask = source.alpha_mode == cgltf_alpha_mode_mask;
        material.alpha_cutoff = std::clamp(source.alpha_cutoff, 0.0F, 1.0F);
        material.double_sided = source.double_sided != 0;
    }

    model.skins.resize(data.skins_count);
    for (cgltf_size index = 0U; index < data.skins_count; ++index)
    {
        const cgltf_skin &source = data.skins[index];
        if (source.joints_count > limits.max_joints)
            fail("a skin has more than " + std::to_string(limits.max_joints) + " joints");
        ModelSkin &skin = model.skins[index];
        for (cgltf_size joint = 0U; joint < source.joints_count; ++joint)
            skin.joints.push_back(index_of(data, source.joints[joint]));
        const auto matrices = unpack(source.inverse_bind_matrices, 16U, limits.max_joints);
        skin.inverse_bind.assign(skin.joints.size(), matrix_identity());
        if (!matrices.empty())
        {
            if (matrices.size() / 16U < skin.joints.size())
                fail("a skin has fewer inverse bind matrices than joints");
            for (std::size_t joint = 0U; joint < skin.joints.size(); ++joint)
                std::copy_n(matrices.begin() + static_cast<std::ptrdiff_t>(joint * 16U), 16U,
                            skin.inverse_bind[joint].begin());
        }
    }

    for (cgltf_size index = 0U; index < data.nodes_count; ++index)
    {
        const cgltf_node &node = data.nodes[index];
        if (node.mesh == nullptr)
            continue;
        const int skin = node.skin != nullptr ? static_cast<int>(node.skin - data.skins) : -1;
        for (cgltf_size primitive = 0U; primitive < node.mesh->primitives_count; ++primitive)
            read_primitive(model, data, node.mesh->primitives[primitive], static_cast<int>(index),
                           skin, limits);
    }
    if (model.primitives.empty() && limits.require_mesh)
        fail("the model has no triangle meshes");

    std::size_t keys{};
    for (cgltf_size index = 0U; index < data.animations_count; ++index)
    {
        const cgltf_animation &source = data.animations[index];
        ModelAnimation animation;
        animation.name = source.name != nullptr ? std::string{source.name}.substr(0U, 128U)
                                                : "animation" + std::to_string(index);
        for (cgltf_size channel_index = 0U; channel_index < source.channels_count; ++channel_index)
        {
            const cgltf_animation_channel &channel = source.channels[channel_index];
            if (channel.target_node == nullptr || channel.sampler == nullptr)
                continue;
            AnimationChannel out;
            out.node = index_of(data, channel.target_node);
            std::size_t components = 3U;
            switch (channel.target_path)
            {
            case cgltf_animation_path_type_translation:
                out.path = AnimationChannel::Path::translation;
                break;
            case cgltf_animation_path_type_rotation:
                out.path = AnimationChannel::Path::rotation;
                components = 4U;
                break;
            case cgltf_animation_path_type_scale:
                out.path = AnimationChannel::Path::scale;
                break;
            default:
                continue; // morph weights are not supported
            }
            switch (channel.sampler->interpolation)
            {
            case cgltf_interpolation_type_step:
                out.interpolation = AnimationChannel::Interpolation::step;
                break;
            case cgltf_interpolation_type_cubic_spline:
                out.interpolation = AnimationChannel::Interpolation::cubic;
                break;
            default:
                out.interpolation = AnimationChannel::Interpolation::linear;
                break;
            }
            out.times = unpack(channel.sampler->input, 1U, limits.max_keys);
            out.values = unpack(channel.sampler->output, components, limits.max_keys * 3U);
            const std::size_t per_key =
                components *
                (out.interpolation == AnimationChannel::Interpolation::cubic ? 3U : 1U);
            if (out.times.empty() || out.values.size() != out.times.size() * per_key)
                fail("animation \"" + animation.name + "\" has mismatched keys");
            for (std::size_t key = 1U; key < out.times.size(); ++key)
                if (out.times[key] < out.times[key - 1U])
                    fail("animation \"" + animation.name + "\" has keys out of order");
            if ((keys += out.times.size()) > limits.max_keys)
                fail("the model has too many animation keys");
            animation.duration = std::max(animation.duration, out.times.back());
            animation.channels.push_back(std::move(out));
        }
        model.animations.push_back(std::move(animation));
    }

    // Bind-pose bounds.
    std::vector<Matrix4> world;
    evaluate_pose(model, -1, 0.0F, false, world);
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
              std::numeric_limits<float>::lowest()};
    float radius{};
    for (const ModelPrimitive &primitive : model.primitives)
    {
        const Matrix4 &matrix = primitive.skin >= 0 || primitive.node < 0
                                    ? matrix_identity()
                                    : world[static_cast<std::size_t>(primitive.node)];
        for (const ModelVertex &vertex : primitive.vertices)
        {
            const Vec3 point = transform_point(matrix, vertex.position);
            low = {std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
            high = {std::max(high.x, point.x), std::max(high.y, point.y),
                    std::max(high.z, point.z)};
            radius = std::max(radius, vfx::length(point));
        }
    }
    if (model.primitives.empty())
        low = high = {}; // (Animation packs) an armature on its own
    model.bounds_min = low;
    model.bounds_max = high;
    model.radius = radius;
}

std::array<float, 4> normalize_quat(std::array<float, 4> q) noexcept
{
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length < 1.0e-8F)
        return {0.0F, 0.0F, 0.0F, 1.0F};
    for (float &value : q)
        value /= length;
    return q;
}

// Samples one channel into `out` (3 or 4 floats).
void sample_channel(const AnimationChannel &channel, float time, float *out)
{
    const std::size_t components = channel.path == AnimationChannel::Path::rotation ? 4U : 3U;
    const bool cubic = channel.interpolation == AnimationChannel::Interpolation::cubic;
    const std::size_t stride = components * (cubic ? 3U : 1U);
    const std::size_t value_offset = cubic ? components : 0U;
    const auto value = [&](std::size_t key, std::size_t component) {
        return channel.values[key * stride + value_offset + component];
    };
    const std::size_t count = channel.times.size();
    if (count == 1U || time <= channel.times.front())
    {
        for (std::size_t component = 0U; component < components; ++component)
            out[component] = value(0U, component);
        return;
    }
    if (time >= channel.times.back())
    {
        for (std::size_t component = 0U; component < components; ++component)
            out[component] = value(count - 1U, component);
        return;
    }
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const std::size_t next = static_cast<std::size_t>(upper - channel.times.begin());
    const std::size_t previous = next - 1U;
    const float span = channel.times[next] - channel.times[previous];
    const float t = span > 1.0e-8F ? (time - channel.times[previous]) / span : 0.0F;
    if (channel.interpolation == AnimationChannel::Interpolation::step)
    {
        for (std::size_t component = 0U; component < components; ++component)
            out[component] = value(previous, component);
        return;
    }
    if (cubic)
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        for (std::size_t component = 0U; component < components; ++component)
        {
            const float p0 = value(previous, component);
            const float m0 = channel.values[previous * stride + 2U * components + component] * span;
            const float p1 = value(next, component);
            const float m1 = channel.values[next * stride + component] * span;
            out[component] = (2.0F * t3 - 3.0F * t2 + 1.0F) * p0 + (t3 - 2.0F * t2 + t) * m0 +
                             (-2.0F * t3 + 3.0F * t2) * p1 + (t3 - t2) * m1;
        }
        if (components == 4U)
        {
            const auto q = normalize_quat({out[0], out[1], out[2], out[3]});
            std::copy(q.begin(), q.end(), out);
        }
        return;
    }
    if (components == 4U)
    {
        // Spherical, along the shorter arc.
        std::array<float, 4> a{value(previous, 0U), value(previous, 1U), value(previous, 2U),
                               value(previous, 3U)};
        std::array<float, 4> b{value(next, 0U), value(next, 1U), value(next, 2U), value(next, 3U)};
        float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (cosine < 0.0F)
        {
            for (float &component : b)
                component = -component;
            cosine = -cosine;
        }
        float wa = 1.0F - t;
        float wb = t;
        if (cosine < 0.9995F)
        {
            const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
            const float sine = std::sin(angle);
            wa = std::sin((1.0F - t) * angle) / sine;
            wb = std::sin(t * angle) / sine;
        }
        const auto q = normalize_quat({a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb,
                                       a[2] * wa + b[2] * wb, a[3] * wa + b[3] * wb});
        std::copy(q.begin(), q.end(), out);
        return;
    }
    for (std::size_t component = 0U; component < components; ++component)
        out[component] =
            value(previous, component) + (value(next, component) - value(previous, component)) * t;
}

} // namespace

int Model::find_animation(std::string_view name) const noexcept
{
    for (std::size_t index = 0U; index < animations.size(); ++index)
        if (animations[index].name == name)
            return static_cast<int>(index);
    return -1;
}

Matrix4 matrix_identity() noexcept
{
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix4 matrix_multiply(const Matrix4 &left, const Matrix4 &right) noexcept
{
    Matrix4 result{};
    for (std::size_t column = 0U; column < 4U; ++column)
        for (std::size_t row = 0U; row < 4U; ++row)
        {
            float sum{};
            for (std::size_t k = 0U; k < 4U; ++k)
                sum += left[k * 4U + row] * right[column * 4U + k];
            result[column * 4U + row] = sum;
        }
    return result;
}

Matrix4 matrix_compose(const Vec3 &translation, const std::array<float, 4> &rotation,
                       const Vec3 &scale) noexcept
{
    const auto q = normalize_quat(rotation);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1.0F - 2.0F * (y * y + z * z)) * scale.x,
            (2.0F * (x * y + z * w)) * scale.x,
            (2.0F * (x * z - y * w)) * scale.x,
            0.0F,
            (2.0F * (x * y - z * w)) * scale.y,
            (1.0F - 2.0F * (x * x + z * z)) * scale.y,
            (2.0F * (y * z + x * w)) * scale.y,
            0.0F,
            (2.0F * (x * z + y * w)) * scale.z,
            (2.0F * (y * z - x * w)) * scale.z,
            (1.0F - 2.0F * (x * x + y * y)) * scale.z,
            0.0F,
            translation.x,
            translation.y,
            translation.z,
            1.0F};
}

Vec3 transform_point(const Matrix4 &m, const Vec3 &p) noexcept
{
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

Vec3 transform_direction(const Matrix4 &m, const Vec3 &d) noexcept
{
    return {m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
            m[2] * d.x + m[6] * d.y + m[10] * d.z};
}

namespace
{

// Each node's local transform with `animation` (or none) sampled at `seconds`.
void sample_local_pose(const Model &model, int animation, float seconds, bool loop,
                       std::vector<ModelNode> &local)
{
    local = model.nodes;
    if (animation < 0 || static_cast<std::size_t>(animation) >= model.animations.size())
        return;
    const ModelAnimation &clip = model.animations[static_cast<std::size_t>(animation)];
    float time = std::max(0.0F, seconds);
    if (loop && clip.duration > 1.0e-6F)
        time = std::fmod(time, clip.duration);
    for (const AnimationChannel &channel : clip.channels)
    {
        if (channel.node < 0 || static_cast<std::size_t>(channel.node) >= local.size())
            continue;
        ModelNode &node = local[static_cast<std::size_t>(channel.node)];
        float value[4]{};
        sample_channel(channel, time, value);
        switch (channel.path)
        {
        case AnimationChannel::Path::translation:
            node.translation = {value[0], value[1], value[2]};
            break;
        case AnimationChannel::Path::rotation:
            node.rotation = {value[0], value[1], value[2], value[3]};
            break;
        case AnimationChannel::Path::scale:
            node.scale = {value[0], value[1], value[2]};
            break;
        }
    }
}

void compose_world(const Model &model, const std::vector<ModelNode> &local,
                   std::vector<Matrix4> &world)
{
    world.assign(model.nodes.size(), matrix_identity());
    for (const int index : model.node_order)
    {
        const ModelNode &node = local[static_cast<std::size_t>(index)];
        const Matrix4 matrix = matrix_compose(node.translation, node.rotation, node.scale);
        world[static_cast<std::size_t>(index)] =
            node.parent >= 0 ? matrix_multiply(world[static_cast<std::size_t>(node.parent)], matrix)
                             : matrix;
    }
}

} // namespace

void evaluate_pose(const Model &model, int animation, float seconds, bool loop,
                   std::vector<Matrix4> &world)
{
    std::vector<ModelNode> local;
    sample_local_pose(model, animation, seconds, loop, local);
    compose_world(model, local, world);
}

void sample_animation(const Model &model, int animation, float seconds,
                      std::vector<ModelNode> &local)
{
    sample_local_pose(model, animation, seconds, false, local);
}

void evaluate_blended_pose(const Model &model, std::span<const PoseLayer> layers,
                           std::vector<Matrix4> &world)
{
    float total{};
    for (const PoseLayer &layer : layers)
        total += std::isfinite(layer.weight) ? std::max(layer.weight, 0.0F) : 0.0F;
    if (layers.size() <= 1U || total <= 1.0e-6F)
    {
        const PoseLayer only = layers.empty() ? PoseLayer{} : layers.front();
        evaluate_pose(model, only.animation, only.seconds, only.loop, world);
        return;
    }
    // Translation and scale are averaged; rotations are summed on the same
    // hemisphere and normalised, which is a fine blend for a short fade.
    std::vector<ModelNode> blended(model.nodes.size());
    std::vector<ModelNode> local;
    bool first = true;
    for (const PoseLayer &layer : layers)
    {
        const float weight =
            (std::isfinite(layer.weight) ? std::max(layer.weight, 0.0F) : 0.0F) / total;
        if (weight <= 0.0F)
            continue;
        sample_local_pose(model, layer.animation, layer.seconds, layer.loop, local);
        for (std::size_t index = 0U; index < local.size(); ++index)
        {
            const ModelNode &node = local[index];
            ModelNode &out = blended[index];
            if (first)
            {
                out.name = node.name;
                out.parent = node.parent;
                out.translation = {};
                out.rotation = {0.0F, 0.0F, 0.0F, 0.0F};
                out.scale = {};
            }
            out.translation += node.translation * weight;
            out.scale += node.scale * weight;
            const float alignment = out.rotation[0] * node.rotation[0] +
                                    out.rotation[1] * node.rotation[1] +
                                    out.rotation[2] * node.rotation[2] +
                                    out.rotation[3] * node.rotation[3];
            const float sign = alignment < 0.0F ? -weight : weight;
            for (std::size_t component = 0U; component < 4U; ++component)
                out.rotation[component] += node.rotation[component] * sign;
        }
        first = false;
    }
    for (ModelNode &node : blended)
        node.rotation = normalize_quat(node.rotation);
    compose_world(model, blended, world);
}

void skin_matrices(const Model &model, int skin, const std::vector<Matrix4> &world,
                   std::vector<Matrix4> &out)
{
    out.clear();
    if (skin < 0 || static_cast<std::size_t>(skin) >= model.skins.size())
        return;
    const ModelSkin &source = model.skins[static_cast<std::size_t>(skin)];
    out.reserve(source.joints.size());
    for (std::size_t joint = 0U; joint < source.joints.size(); ++joint)
    {
        const int node = source.joints[joint];
        const Matrix4 &joint_world = node >= 0 && static_cast<std::size_t>(node) < world.size()
                                         ? world[static_cast<std::size_t>(node)]
                                         : matrix_identity();
        out.push_back(matrix_multiply(joint_world, source.inverse_bind[joint]));
    }
}

std::shared_ptr<Model> parse_model(std::span<const std::uint8_t> bytes,
                                   const std::filesystem::path &base_directory, std::string *error,
                                   const ModelLimits &limits)
{
    try
    {
        if (bytes.empty())
            fail("empty model file");
        if (bytes.size() > limits.max_file_bytes)
            fail("model file larger than the limit");
        cgltf_options options{};
        cgltf_data *parsed{};
        const cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &parsed);
        DataPointer data{parsed};
        if (result != cgltf_result_success || data == nullptr)
            fail(std::string{"not a glTF 2.0 file ("} + cgltf_reason(result) + ")");
        load_buffers(*data, options, base_directory, limits);
        if (const cgltf_result valid = cgltf_validate(data.get()); valid != cgltf_result_success)
            fail(std::string{"glTF validation failed ("} + cgltf_reason(valid) + ")");
        auto model = std::make_shared<Model>();
        read_model(*model, *data, base_directory, limits, options);
        return model;
    }
    catch (const Failure &failure)
    {
        if (error != nullptr)
            *error = failure.message();
    }
    catch (const std::bad_alloc &)
    {
        if (error != nullptr)
            *error = "out of memory";
    }
    return nullptr;
}

std::shared_ptr<Model> load_model(const std::filesystem::path &path, std::string *error,
                                  const ModelLimits &limits)
{
    const auto bytes = read_file_bounded(path, limits.max_file_bytes, error);
    if (!bytes)
        return nullptr;
    auto model = parse_model(*bytes, path.parent_path(), error, limits);
    if (model == nullptr && error != nullptr)
        *error = path.filename().string() + ": " + *error;
    return model;
}

std::shared_ptr<const Model> ModelCache::get(const AssetStack &stack, std::string_view name,
                                             std::string *error)
{
    if (generation_ != stack.generation())
    {
        models_.clear();
        failures_.clear();
        generation_ = stack.generation();
    }
    if (const auto found = models_.find(name); found != models_.end())
        return found->second;
    if (const auto failed = failures_.find(name); failed != failures_.end())
    {
        if (error != nullptr)
            *error = failed->second;
        return nullptr;
    }
    static constexpr std::array<std::string_view, 2> extensions{".glb", ".gltf"};
    std::string message;
    std::shared_ptr<const Model> model;
    if (const auto resolved = stack.resolve(name, extensions))
        model = load_model(resolved->path, &message);
    else
        message = "no model \"" + std::string{name} + "\" in any enabled mod";
    if (model == nullptr)
    {
        failures_.emplace(std::string{name}, message);
        if (error != nullptr)
            *error = message;
        return nullptr;
    }
    models_.emplace(std::string{name}, model);
    return model;
}

void ModelCache::clear() noexcept
{
    models_.clear();
    failures_.clear();
}

} // namespace openminerz::modding::graphics
