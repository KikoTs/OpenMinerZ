#pragma once

// A made-up Xbox avatar in the add-on's `.ocavatar` format, built in memory.
//
// Nobody's real avatar may live in this repository, so every test that needs
// one generates it: a box per bone on whatever skeleton the caller supplies,
// with the layer stack a real export has (a body under a palette overlay and a
// decal, a head with face layers and their animation frames, an outfit, a bare
// hand shell). The textures are real PNGs, written with stored (uncompressed)
// deflate blocks so the add-on's own loader accepts the file and a forty-line
// decoder here can read them back without a PNG library.

#include "openminerz/xbox_avatar.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::test
{

inline constexpr std::array<int, 71> avatar_parents{
    -1, 0,  0,  0,  0,  1,  2,  2,  3,  3,  1,  6,  5,  6,  5,  8,  5,  8,  5,  14, 12, 11, 16, 15,
    14, 20, 20, 20, 22, 22, 22, 25, 25, 25, 28, 28, 28, 33, 33, 33, 33, 33, 33, 33, 36, 36, 36, 36,
    36, 36, 36, 37, 38, 39, 40, 43, 44, 45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};

inline std::uint32_t png_crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0xFFFFFFFFU)
{
    for (const std::uint8_t byte : bytes)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

// An RGBA PNG using stored deflate blocks.
inline std::vector<std::uint8_t> encode_png(int width, int height,
                                            std::span<const std::uint8_t> rgba)
{
    std::vector<std::uint8_t> png{0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    const auto big_endian = [](std::vector<std::uint8_t> &out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto chunk = [&](std::string_view type, const std::vector<std::uint8_t> &data) {
        big_endian(png, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body{type.begin(), type.end()};
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        big_endian(png, png_crc32(body) ^ 0xFFFFFFFFU);
    };
    std::vector<std::uint8_t> header;
    big_endian(header, static_cast<std::uint32_t>(width));
    big_endian(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U});
    chunk("IHDR", header);
    std::vector<std::uint8_t> raw;
    for (int row = 0; row < height; ++row)
    {
        raw.push_back(0U);
        const auto begin = rgba.begin() + static_cast<std::ptrdiff_t>(row) * width * 4;
        raw.insert(raw.end(), begin, begin + static_cast<std::ptrdiff_t>(width) * 4);
    }
    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    std::size_t offset{};
    do
    {
        const std::size_t length = std::min<std::size_t>(65'535U, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib.push_back(last ? 1U : 0U);
        zlib.push_back(static_cast<std::uint8_t>(length & 255U));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length & 255U));
        zlib.push_back(static_cast<std::uint8_t>((~length >> 8U) & 255U));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : raw)
    {
        a = (a + byte) % 65'521U;
        b = (b + a) % 65'521U;
    }
    big_endian(zlib, (b << 16U) | a);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    return png;
}

// Reads back exactly what encode_png writes; anything else is "does not decode".
inline bool decode_stored_png(std::span<const std::uint8_t> png, xbox_avatar::Image &image)
{
    int width{};
    int height{};
    if (!xbox_avatar::png_dimensions(png, width, height) || png.size() < 33U || png[24] != 8U ||
        png[25] != 6U)
        return false;
    std::vector<std::uint8_t> zlib;
    std::size_t offset = 8U;
    while (offset + 12U <= png.size())
    {
        const std::uint32_t length = (static_cast<std::uint32_t>(png[offset]) << 24U) |
                                     (static_cast<std::uint32_t>(png[offset + 1U]) << 16U) |
                                     (static_cast<std::uint32_t>(png[offset + 2U]) << 8U) |
                                     static_cast<std::uint32_t>(png[offset + 3U]);
        if (length > png.size() - offset - 12U)
            return false;
        if (std::memcmp(png.data() + offset + 4U, "IDAT", 4U) == 0)
            zlib.insert(zlib.end(), png.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                        png.begin() + static_cast<std::ptrdiff_t>(offset + 8U + length));
        offset += 12U + length;
    }
    std::vector<std::uint8_t> raw;
    std::size_t cursor = 2U;
    for (;;)
    {
        if (cursor + 5U > zlib.size())
            return false;
        const std::uint8_t header = zlib[cursor];
        const std::size_t length = zlib[cursor + 1U] | (static_cast<std::size_t>(zlib[cursor + 2U]) << 8U);
        cursor += 5U;
        if ((header & 6U) != 0U || cursor + length > zlib.size())
            return false;
        raw.insert(raw.end(), zlib.begin() + static_cast<std::ptrdiff_t>(cursor),
                   zlib.begin() + static_cast<std::ptrdiff_t>(cursor + length));
        cursor += length;
        if ((header & 1U) != 0U)
            break;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4U + 1U;
    if (raw.size() != stride * static_cast<std::size_t>(height))
        return false;
    image.width = width;
    image.height = height;
    image.rgba.clear();
    for (int row = 0; row < height; ++row)
    {
        if (raw[static_cast<std::size_t>(row) * stride] != 0U)
            return false;
        const auto begin = raw.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * stride + 1U);
        image.rgba.insert(image.rgba.end(), begin, begin + static_cast<std::ptrdiff_t>(stride - 1U));
    }
    return true;
}

inline std::vector<std::uint8_t> solid_png(int width, int height, std::array<std::uint8_t, 4> color)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4U)
        std::copy(color.begin(), color.end(), rgba.begin() + static_cast<std::ptrdiff_t>(pixel));
    return encode_png(width, height, rgba);
}

struct SyntheticAvatar final
{
    // The export's own (left-handed) local bind offsets and per-bone build.
    std::array<std::array<float, 3>, 71> offsets{};
    std::array<std::array<float, 3>, 71> scales{};
    std::int32_t version{4};
    float box{0.03F};
    std::array<std::uint8_t, 4> skin{199U, 152U, 76U, 255U};
    std::array<std::uint8_t, 4> cloth{60U, 90U, 160U, 255U};

    SyntheticAvatar()
    {
        for (auto &scale : scales)
            scale = {1.0F, 1.0F, 1.0F};
        // An invented skeleton: nothing here is anyone's measured data.
        offsets[0] = {0.0F, 0.80F, 0.01F};
        for (std::size_t bone = 1; bone < offsets.size(); ++bone)
        {
            const float side = bone % 2U == 0U ? 1.0F : -1.0F;
            offsets[bone] = {side * 0.04F * static_cast<float>(1U + bone % 3U),
                             (bone % 5U < 2U ? -0.07F : 0.06F), 0.01F * static_cast<float>(bone % 4U) - 0.015F};
        }
    }
};

class AvatarFileWriter final
{
  public:
    template <typename Value> void put(const Value &value)
    {
        const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
        bytes.insert(bytes.end(), raw, raw + sizeof(Value));
    }
    void string(std::string_view text)
    {
        std::size_t length = text.size();
        do
        {
            std::uint8_t byte = static_cast<std::uint8_t>(length & 0x7FU);
            length >>= 7U;
            if (length != 0U)
                byte |= 0x80U;
            bytes.push_back(byte);
        } while (length != 0U);
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    void matrix(const std::array<float, 16> &matrix)
    {
        for (const float value : matrix)
            put(value);
    }
    std::vector<std::uint8_t> bytes;
};

struct SyntheticVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0F, 1.0F, 0.0F};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> bones{};
    std::array<std::uint8_t, 4> weights{255U, 0U, 0U, 0U};
    std::array<std::uint8_t, 4> color{255U, 255U, 255U, 255U};
};

struct SyntheticBatch final
{
    std::string name;
    std::uint32_t category{};
    std::uint8_t palette_mask{};
    std::array<float, 3> palette{};
    std::vector<SyntheticVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
    std::vector<std::uint8_t> png;
};

// A box around `center`, bound to `bone` (and partly to `second` when given).
inline void append_box(SyntheticBatch &batch, std::array<float, 3> center, float half, int bone,
                       int second = -1)
{
    const auto first = static_cast<std::uint16_t>(batch.vertices.size());
    for (int corner = 0; corner < 8; ++corner)
    {
        SyntheticVertex vertex;
        const std::array<float, 3> sign{corner & 1 ? 1.0F : -1.0F, corner & 2 ? 1.0F : -1.0F,
                                        corner & 4 ? 1.0F : -1.0F};
        vertex.position = {center[0] + sign[0] * half, center[1] + sign[1] * half, center[2] + sign[2] * half};
        vertex.normal = {sign[0] * 0.57735F, sign[1] * 0.57735F, sign[2] * 0.57735F};
        vertex.uv = {corner & 1 ? 1.0F : 0.0F, corner & 2 ? 1.0F : 0.0F};
        vertex.bones = {static_cast<std::uint8_t>(bone), static_cast<std::uint8_t>(second < 0 ? 0 : second), 0U, 0U};
        if (second >= 0 && (corner & 2) == 0)
            vertex.weights = {155U, 100U, 0U, 0U};
        batch.vertices.push_back(vertex);
    }
    constexpr std::array<std::uint16_t, 36> box{0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                                2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    for (const std::uint16_t index : box)
        batch.indices.push_back(static_cast<std::uint16_t>(first + index));
}

// The batches of the synthetic avatar, before serialisation, so a test can
// reach in and damage one.
inline std::vector<SyntheticBatch> synthetic_batches(const SyntheticAvatar &avatar)
{
    std::array<std::array<float, 3>, 71> absolute{};
    for (std::size_t bone = 0; bone < absolute.size(); ++bone)
    {
        absolute[bone] = avatar.offsets[bone];
        if (avatar_parents[bone] >= 0)
            for (std::size_t axis = 0; axis < 3U; ++axis)
                absolute[bone][axis] += absolute[static_cast<std::size_t>(avatar_parents[bone])][axis];
    }
    const std::array<float, 3> skin{avatar.skin[0] / 255.0F, avatar.skin[1] / 255.0F, avatar.skin[2] / 255.0F};
    constexpr std::string_view id = "-0000-0001-0000-000000000000";
    std::vector<SyntheticBatch> batches;

    // The head and its face layers: two animation frames of one usage, of
    // which only frame zero is ever drawn.
    SyntheticBatch head;
    head.name = "00000001" + std::string{id} + ":head:0";
    head.category = 1U;
    head.diffuse = skin;
    append_box(head, absolute[19], avatar.box * 1.6F, 19);
    batches.push_back(head);
    std::vector<std::uint8_t> face(8U * 8U * 4U, 0U);
    for (std::size_t pixel = 0; pixel < 64U; ++pixel)
    {
        face[pixel * 4U] = 20U;
        face[pixel * 4U + 3U] = pixel % 8U < 4U ? 0U : 255U;
    }
    for (int frame = 0; frame < 2; ++frame)
    {
        SyntheticBatch layer = head;
        layer.name = "00000001" + std::string{id} + ":face-layer-9-frame-" + std::to_string(frame) + ":0";
        layer.diffuse = {1.0F, 1.0F, 1.0F};
        layer.png = encode_png(8, 8, face);
        for (SyntheticVertex &vertex : layer.vertices)
            vertex.uv = {vertex.uv[0] * 1.3F - 0.15F, vertex.uv[1] * 1.3F - 0.15F};
        batches.push_back(std::move(layer));
    }

    // The body: a base, a palette overlay that is opaque everywhere (and so is
    // the only layer of the three that shows), and a decal that is empty.
    SyntheticBatch body;
    body.name = "00000002" + std::string{id} + ":body:0";
    body.category = 2U;
    body.palette_mask = 7U;
    body.palette = skin;
    for (int bone = 0; bone < 71; ++bone)
        if (bone != 19)
            append_box(body, absolute[static_cast<std::size_t>(bone)], avatar.box, bone,
                       bone > 0 && bone % 3 == 0 ? avatar_parents[static_cast<std::size_t>(bone)] : -1);
    body.png = solid_png(4, 4, {247U, 195U, 153U, 250U});
    batches.push_back(body);
    SyntheticBatch palette = body;
    palette.name += ":material-overlay-palette";
    palette.palette_mask = 0U;
    palette.png = solid_png(2, 2, avatar.skin);
    batches.push_back(std::move(palette));
    SyntheticBatch decal = body;
    decal.name += ":material-overlay-decal";
    decal.palette_mask = 0U;
    decal.png = solid_png(2, 2, {0U, 0U, 0U, 0U});
    batches.push_back(std::move(decal));

    // An outfit over the spine, vertex-shaded, opaque.
    SyntheticBatch outfit;
    outfit.name = "00000218" + std::string{id} + ":model:0";
    outfit.category = 0x00000218U;
    outfit.palette_mask = 7U;
    for (const int bone : {1, 5, 10, 14})
        append_box(outfit, absolute[static_cast<std::size_t>(bone)], avatar.box * 1.5F, bone);
    for (std::size_t index = 0; index < outfit.vertices.size(); ++index)
        outfit.vertices[index].color = {static_cast<std::uint8_t>(120U + index % 100U), 200U, 220U, 255U};
    outfit.png = solid_png(4, 4, avatar.cloth);
    batches.push_back(std::move(outfit));

    // A hand component whose palette is the body's: the bare-hand shell, which
    // wears the body's material instead of its own grey shader masks.
    SyntheticBatch shell;
    shell.name = "00000080" + std::string{id} + ":model:0";
    shell.category = 0x00000080U;
    shell.palette_mask = 1U;
    shell.palette = skin;
    for (const int bone : {44, 45, 56, 57})
        append_box(shell, absolute[static_cast<std::size_t>(bone)], avatar.box * 1.2F, bone);
    for (SyntheticVertex &vertex : shell.vertices)
        vertex.color = {90U, 90U, 90U, 255U};
    shell.png = solid_png(2, 2, {128U, 128U, 128U, 255U});
    batches.push_back(std::move(shell));
    return batches;
}

inline std::vector<std::uint8_t> write_synthetic_avatar(const SyntheticAvatar &avatar,
                                                        const std::vector<SyntheticBatch> &batches)
{
    AvatarFileWriter writer;
    writer.put(xbox_avatar::file_signature);
    writer.put(avatar.version);
    writer.put(std::int32_t{71});
    std::array<std::array<float, 3>, 71> absolute{};
    for (std::size_t bone = 0; bone < absolute.size(); ++bone)
    {
        absolute[bone] = avatar.offsets[bone];
        if (avatar_parents[bone] >= 0)
            for (std::size_t axis = 0; axis < 3U; ++axis)
                absolute[bone][axis] += absolute[static_cast<std::size_t>(avatar_parents[bone])][axis];
        // The inverse of a pure translation: the mesh is authored at unit build.
        writer.matrix({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -absolute[bone][0], -absolute[bone][1],
                       -absolute[bone][2], 1});
    }
    if (avatar.version >= 2)
        for (std::size_t bone = 0; bone < absolute.size(); ++bone)
            writer.matrix({avatar.scales[bone][0], 0, 0, 0, 0, avatar.scales[bone][1], 0, 0, 0, 0,
                           avatar.scales[bone][2], 0, avatar.offsets[bone][0], avatar.offsets[bone][1],
                           avatar.offsets[bone][2], 1});
    writer.put(static_cast<std::int32_t>(batches.size()));
    for (const SyntheticBatch &batch : batches)
    {
        writer.string(batch.name);
        if (avatar.version >= 3)
        {
            writer.put(batch.category);
            writer.put(std::int32_t{0});
            writer.put(batch.palette_mask);
            for (int entry = 0; entry < 3; ++entry)
            {
                writer.put(batch.palette[0]);
                writer.put(batch.palette[1]);
                writer.put(batch.palette[2]);
                writer.put(1.0F);
            }
        }
        writer.put(static_cast<std::int32_t>(batch.vertices.size()));
        for (const SyntheticVertex &vertex : batch.vertices)
        {
            writer.put(vertex.position);
            writer.put(vertex.normal);
            writer.put(vertex.uv);
            writer.put(vertex.bones);
            writer.put(vertex.weights);
            writer.put(vertex.color);
        }
        writer.put(static_cast<std::int32_t>(batch.indices.size()));
        for (const std::uint16_t index : batch.indices)
            writer.put(index);
        writer.put(batch.diffuse);
        writer.put(static_cast<std::int32_t>(batch.png.size()));
        writer.bytes.insert(writer.bytes.end(), batch.png.begin(), batch.png.end());
    }
    return std::move(writer.bytes);
}

inline std::vector<std::uint8_t> build_synthetic_avatar(const SyntheticAvatar &avatar = {})
{
    return write_synthetic_avatar(avatar, synthetic_batches(avatar));
}

} // namespace openminerz::test
