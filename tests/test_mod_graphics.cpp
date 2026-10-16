// The modding graphics and assets service: pack layering and name safety,
// glTF parsing (well-formed and hostile), animation packs, light culling and
// the light clusters, atmosphere and LUT blending, camera effects, sound decoding
// and mixing, effect recipes, and the script libraries through Luau.
// Everything runs in memory or in temporary folders; no device is needed.

#include "modding_test_harness.hpp"

#include "openminerz/graphics_settings.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/icon_model.hpp"
#include "openminerz/retail_rig.hpp"
#include "openminerz/world/sky.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
using namespace openminerz::modding;
namespace graphics = openminerz::modding::graphics;
using modding::testing::Harness;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_near(float actual, float expected, float tolerance, const std::string &message)
{
    if (!(std::fabs(actual - expected) <= tolerance))
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " +
                                 std::to_string(actual));
}

void check_ok(const RunResult &result, const std::string &what)
{
    if (!result.ok)
        throw std::runtime_error(what + ": " + result.error);
}

void check_fails(const RunResult &result, std::string_view needle, const std::string &what)
{
    if (result.ok)
        throw std::runtime_error(what + ": expected an error");
    if (result.error.find(needle) == std::string::npos)
        throw std::runtime_error(what + ": error was \"" + result.error + "\", expected \"" +
                                 std::string{needle} + "\"");
}

class TemporaryFolder final
{
  public:
    explicit TemporaryFolder(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                ("openminerz-mod-graphics-" + std::string{name}))
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryFolder(const TemporaryFolder &) = delete;
    TemporaryFolder &operator=(const TemporaryFolder &) = delete;
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

std::string base64(const std::vector<std::uint8_t> &bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0U;
    while (index + 2U < bytes.size())
    {
        const std::uint32_t value =
            (bytes[index] << 16U) | (bytes[index + 1U] << 8U) | bytes[index + 2U];
        out += alphabet[(value >> 18U) & 63U];
        out += alphabet[(value >> 12U) & 63U];
        out += alphabet[(value >> 6U) & 63U];
        out += alphabet[value & 63U];
        index += 3U;
    }
    if (index + 1U == bytes.size())
    {
        const std::uint32_t value = bytes[index] << 16U;
        out += alphabet[(value >> 18U) & 63U];
        out += alphabet[(value >> 12U) & 63U];
        out += "==";
    }
    else if (index + 2U == bytes.size())
    {
        const std::uint32_t value = (bytes[index] << 16U) | (bytes[index + 1U] << 8U);
        out += alphabet[(value >> 18U) & 63U];
        out += alphabet[(value >> 12U) & 63U];
        out += alphabet[(value >> 6U) & 63U];
        out += '=';
    }
    return out;
}

template <typename T> void append(std::vector<std::uint8_t> &bytes, const T &value)
{
    const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

std::span<const std::uint8_t> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

// A solid red 32-bit BMP (what stb_image reads like any other format).
std::vector<std::uint8_t> red_bmp(int width, int height)
{
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>('B'), static_cast<std::uint8_t>('M')};
    const auto pixels = static_cast<std::uint32_t>(width * height * 4);
    append(bytes, 54U + pixels);
    append(bytes, std::uint32_t{0});
    append(bytes, std::uint32_t{54});
    append(bytes, std::uint32_t{40});
    append(bytes, std::int32_t{width});
    append(bytes, std::int32_t{height});
    append(bytes, std::uint16_t{1});
    append(bytes, std::uint16_t{32});
    append(bytes, std::uint32_t{0});
    append(bytes, pixels);
    for (int index = 0; index < 4; ++index)
        append(bytes, std::uint32_t{0});
    for (int texel = 0; texel < width * height; ++texel)
        append(bytes, std::uint32_t{0xFFFF0000U}); // B G R A in memory: red, opaque
    return bytes;
}

// One triangle (three positions) and its indices, embedded as a data: URI.
// Joint 1 of a two-joint skin carries every vertex; one animation moves it.
std::string skinned_triangle_gltf(std::string_view buffer_uri_override = {},
                                  std::string_view extra_node = {})
{
    std::vector<std::uint8_t> buffer;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F})
        append(buffer, value); // 0: positions, 36 bytes
    for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2}})
        append(buffer, value);        // 36: indices, 6 bytes
    append(buffer, std::uint16_t{0}); // pad to 44
    for (int vertex = 0; vertex < 3; ++vertex)
        for (const std::uint8_t joint : std::array<std::uint8_t, 4>{1U, 0U, 0U, 0U})
            append(buffer, joint); // 44: joints, 12 bytes
    for (int vertex = 0; vertex < 3; ++vertex)
        for (const float weight : {1.0F, 0.0F, 0.0F, 0.0F})
            append(buffer, weight); // 56: weights, 48 bytes
    for (int joint = 0; joint < 2; ++joint)
        for (int element = 0; element < 16; ++element)
            append(buffer, element % 5 == 0 ? 1.0F : 0.0F); // 104: inverse binds, 128 bytes
    for (const float time : {0.0F, 1.0F})
        append(buffer, time); // 232: times, 8 bytes
    for (const float value : {0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F})
        append(buffer, value); // 240: translations, 24 bytes
    const std::string uri = buffer_uri_override.empty()
                                ? "data:application/octet-stream;base64," + base64(buffer)
                                : std::string{buffer_uri_override};
    return R"({
 "asset": {"version": "2.0"},
 "buffers": [{"byteLength": 264, "uri": ")" +
           uri + R"("}],
 "bufferViews": [
  {"buffer": 0, "byteOffset": 0, "byteLength": 36},
  {"buffer": 0, "byteOffset": 36, "byteLength": 6},
  {"buffer": 0, "byteOffset": 44, "byteLength": 12},
  {"buffer": 0, "byteOffset": 56, "byteLength": 48},
  {"buffer": 0, "byteOffset": 104, "byteLength": 128},
  {"buffer": 0, "byteOffset": 232, "byteLength": 8},
  {"buffer": 0, "byteOffset": 240, "byteLength": 24}],
 "accessors": [
  {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]},
  {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
  {"bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4"},
  {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"},
  {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"},
  {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
  {"bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC3"}],
 "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 1}]}],
 "skins": [{"joints": [1, 2], "inverseBindMatrices": 4}],
 "nodes": [
  {"name": "body", "mesh": 0, "skin": 0},
  {"name": "Root", "children": [2]},
  {"name": "Head"})" +
           std::string{extra_node} + R"(],
 "animations": [{"name": "Rise", "samplers": [{"input": 5, "output": 6}],
                 "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}}]}],
 "scenes": [{"nodes": [0, 1]}]
})";
}

// --- Fake backend ------------------------------------------------------------

class FakeBackend final : public graphics::GraphicsBackend
{
  public:
    bool effects{true};
    int spawned{};
    int killed{};
    std::vector<vfx::EffectHandle> alive;
    graphics::Mixer mixer_instance{48000};
    std::map<std::uint64_t, graphics::Vec3> entities;

    [[nodiscard]] bool locate(Handle target, graphics::Vec3 &position, float &yaw) const override
    {
        const auto found = entities.find(target.id);
        if (target.kind != HandleKind::entity || found == entities.end())
            return false;
        position = found->second;
        yaw = 0.0F;
        return true;
    }
    [[nodiscard]] bool effects_available() const override
    {
        return effects;
    }
    vfx::EffectHandle spawn_effect(const vfx::EffectDesc &, const vfx::SpawnParams &) override
    {
        ++spawned;
        alive.push_back({static_cast<std::uint16_t>(spawned), 1U});
        return alive.back();
    }
    [[nodiscard]] bool effect_alive(vfx::EffectHandle handle) const override
    {
        return std::any_of(alive.begin(), alive.end(),
                           [&](vfx::EffectHandle other) { return other.slot == handle.slot; });
    }
    void move_effect(vfx::EffectHandle, graphics::Vec3, graphics::Vec3) override
    {
    }
    void kill_effect(vfx::EffectHandle handle, bool) override
    {
        ++killed;
        std::erase_if(alive, [&](vfx::EffectHandle other) { return other.slot == handle.slot; });
    }
    [[nodiscard]] graphics::Mixer *mixer(bool) override
    {
        return &mixer_instance;
    }
};

// --- Assets ------------------------------------------------------------------

void asset_names_refuse_traversal()
{
    for (const std::string_view bad :
         {"", "../secret.png", "textures/../../x.png", "/etc/passwd", "C:/Windows/x.png",
          "textures\\blocks\\3.png", "textures//3.png", "./3.png", "textures/./3.png",
          "textures/blocks/3.png/..", "a:b.png", "tex tures/3.png", "\xC3\xA9.png"})
        check(!graphics::valid_asset_name(bad), "accepted bad name \"" + std::string{bad} + "\"");
    for (const std::string_view good :
         {"textures/blocks/3.png", "models/lantern.glb", "sounds/cues/Click.ogg", "a-b_c.d/e"})
        check(graphics::valid_asset_name(good), "refused good name " + std::string{good});
    std::string reason;
    check(!graphics::valid_asset_name("a/b/c/d/e/f/g/h/i.png", &reason) && !reason.empty(),
          "nine levels refused with a reason");

    TemporaryFolder folder{"traversal"};
    write_file(folder.path() / "outside.png", "x");
    write_file(folder.path() / "mod" / "assets" / "inside.png", "x");
    const auto root = folder.path() / "mod" / "assets";
    check(graphics::contained_path(root, "inside.png").has_value(), "inside resolves");
    check(!graphics::contained_path(root, "../../outside.png").has_value(), "escape refused");
    graphics::AssetStack stack;
    stack.set_sources({{"mod", root}});
    check(!stack.resolve("../outside.png").has_value(), "stack refuses escapes");
    check(!stack.resolve("@mod/../outside.png").has_value(), "pinned escape refused");
}

void packs_layer_in_load_order()
{
    TemporaryFolder folder{"layers"};
    const auto a = folder.path() / "a" / "assets";
    const auto b = folder.path() / "b" / "assets";
    write_file(a / "textures" / "blocks" / "3.png", "A");
    write_file(a / "textures" / "blocks" / "4.png", "A");
    write_file(b / "textures" / "blocks" / "3.png", "B");
    write_file(b / "sounds" / "bell.ogg", "B");
    graphics::AssetStack stack;
    stack.set_sources({{"a", a}, {"b", b}, {"missing", folder.path() / "nope"}});
    check(stack.sources().size() == 2U, "a missing folder is skipped");
    const auto generation = stack.generation();
    const auto three = stack.resolve("textures/blocks/3.png");
    check(three && three->mod_id == "b", "the later mod wins");
    const auto four = stack.resolve("textures/blocks/4.png");
    check(four && four->mod_id == "a", "an earlier mod still supplies what the later lacks");
    const auto pinned = stack.resolve("@a/textures/blocks/3.png");
    check(pinned && pinned->mod_id == "a", "a pinned name picks that mod");
    static constexpr std::array<std::string_view, 2> sound{".ogg", ".wav"};
    const auto bell = stack.resolve("sounds/bell", sound);
    check(bell && bell->name == "sounds/bell.ogg", "the extension is found");
    check(!stack.resolve("sounds/none", sound), "absent is absent");
    static constexpr std::array<std::string_view, 1> png{".png"};
    const auto listed = stack.list("textures/blocks", png);
    check(listed.size() == 2U && listed[0].mod_id == "b" && listed[1].mod_id == "a",
          "list keeps one winner per name");
    // Reordering is a new stack generation (caches reload).
    stack.set_sources({{"b", b}, {"a", a}});
    check(stack.generation() != generation, "a new generation");
    check(stack.resolve("textures/blocks/3.png")->mod_id == "a", "order decides");
    stack.clear();
    check(stack.empty() && !stack.resolve("textures/blocks/4.png"), "cleared");
}

void images_resample_and_tiles_compose()
{
    graphics::Image red;
    red.width = 4;
    red.height = 4;
    red.rgba.assign(4U * 4U * 4U, 0U);
    for (std::size_t texel = 0U; texel < 16U; ++texel)
    {
        red.rgba[texel * 4U] = 255U;
        red.rgba[texel * 4U + 3U] = 255U;
    }
    const graphics::Image small = graphics::resample(red, 2, 2);
    check(small.width == 2 && small.rgba[0] == 255U && small.rgba[1] == 0U, "box keeps colour");
    const graphics::Image large = graphics::resample(red, 16, 16);
    check(large.width == 16 && large.rgba[(5U * 16U + 7U) * 4U] == 255U, "bilinear grows");

    graphics::Image atlas;
    atlas.width = 64;
    atlas.height = 64;
    atlas.rgba.assign(64U * 64U * 4U, 10U);
    graphics::apply_tile_overrides(atlas, {{9, red}, {70, red}}, 8);
    // Tile 9 is column 1, row 1: texels 8..15.
    const auto texel = [&](int x, int y) {
        return atlas.rgba[(static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U];
    };
    check(texel(8, 8) == 255U && texel(15, 15) == 255U, "tile 9 replaced");
    check(texel(7, 8) == 10U && texel(16, 8) == 10U && texel(8, 7) == 10U, "neighbours untouched");

    // Garbage and oversized images are refused before decoding.
    std::string error;
    check(!graphics::decode_image(as_bytes("not an image"), &error) && !error.empty(),
          "garbage refused");
    std::vector<std::uint8_t> bmp;
    // A BMP header declaring 100000 x 100000.
    for (const char c : std::string_view{"BM"})
        bmp.push_back(static_cast<std::uint8_t>(c));
    append(bmp, std::uint32_t{54});
    append(bmp, std::uint32_t{0});
    append(bmp, std::uint32_t{54});
    append(bmp, std::uint32_t{40});
    append(bmp, std::int32_t{100000});
    append(bmp, std::int32_t{100000});
    append(bmp, std::uint16_t{1});
    append(bmp, std::uint16_t{24});
    for (int index = 0; index < 6; ++index)
        append(bmp, std::uint32_t{0});
    check(!graphics::decode_image(bmp, &error), "a huge declared image is refused");

    graphics::BlockTextureTable table;
    const auto first = table.tile_for("textures/ruins_brick");
    check(first && *first == 64U, "mod tiles start after retail's 64");
    check(table.tile_for("textures/ruins_brick") == first, "the same texture keeps its tile");
    const auto moss = table.tile_for("textures/moss");
    check(moss && *moss == 65U, "next tile");
}

void tile_overrides_are_collected_from_packs()
{
    TemporaryFolder folder{"tiles"};
    const auto root = folder.path() / "pack" / "assets" / "textures" / "blocks";
    const std::vector<std::uint8_t> bytes = red_bmp(2, 2);
    write_bytes(root / "3.bmp", bytes);
    write_bytes(root / "3_n.bmp", bytes);
    write_bytes(root / "99.bmp", bytes);
    write_bytes(root / "grass.bmp", bytes);
    graphics::AssetStack stack;
    stack.set_sources({{"pack", folder.path() / "pack" / "assets"}});
    std::vector<std::string> warnings;
    const graphics::TileOverrides overrides = graphics::collect_tile_overrides(stack, &warnings);
    check(overrides.diffuse.size() == 1U && overrides.diffuse.contains(3), "tile 3 found");
    check(overrides.normal_spec.size() == 1U, "its normal map found");
    check(overrides.diffuse.at(3).width == 2 && overrides.diffuse.at(3).rgba[0] == 255U,
          "the PNG decoded");
    check(warnings.size() == 2U, "out-of-range and misnamed tiles are reported");
}

// --- Models ------------------------------------------------------------------

void gltf_parses_skins_and_animations()
{
    std::string error;
    const std::string text = skinned_triangle_gltf();
    const auto model = graphics::parse_model(as_bytes(text), {}, &error);
    check(model != nullptr, "valid model: " + error);
    check(model->primitives.size() == 1U && model->triangle_count == 1U, "one triangle");
    check(model->skins.size() == 1U && model->primitives[0].skin == 0, "skinned");
    check(model->animations.size() == 1U && model->find_animation("Rise") == 0, "one clip");
    check_near(model->animations[0].duration, 1.0F, 1e-6F, "clip length");
    check(model->radius > 0.9F, "bounds measured");

    std::vector<graphics::Matrix4> world;
    graphics::evaluate_pose(*model, 0, 0.5F, false, world);
    std::vector<graphics::Matrix4> skin;
    graphics::skin_matrices(*model, 0, world, skin);
    check(skin.size() == 2U, "a matrix per joint");
    const graphics::Vec3 moved = graphics::transform_point(skin[1], {1.0F, 0.0F, 0.0F});
    check_near(moved.y, 1.0F, 1e-5F, "joint 1 halfway up at t=0.5");
    graphics::evaluate_pose(*model, 0, 1.5F, true, world);
    graphics::skin_matrices(*model, 0, world, skin);
    check_near(graphics::transform_point(skin[1], {}).y, 1.0F, 1e-5F, "looping wraps");
    graphics::evaluate_pose(*model, -1, 0.5F, false, world);
    graphics::skin_matrices(*model, 0, world, skin);
    check_near(graphics::transform_point(skin[1], {}).y, 0.0F, 1e-6F, "bind pose without a clip");
}

// (Polish) emissiveFactor, KHR_materials_emissive_strength and emissiveTexture.
void gltf_emissive_materials_load()
{
    std::vector<std::uint8_t> buffer;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F})
        append(buffer, value);
    for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2}})
        append(buffer, value);
    append(buffer, std::uint16_t{0});
    const std::string image = "data:image/bmp;base64," + base64(red_bmp(2, 2));
    const std::string text = R"({
 "asset": {"version": "2.0"},
 "extensionsUsed": ["KHR_materials_emissive_strength"],
 "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,)" +
                             base64(buffer) + R"("}],
 "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                 {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
 "accessors": [
  {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]},
  {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
 "images": [{"uri": ")" + image + R"("}],
 "textures": [{"source": 0}],
 "materials": [
  {"name": "lamp", "emissiveFactor": [1.0, 0.5, 0.25], "emissiveTexture": {"index": 0},
   "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}},
  {"name": "plain"},
  {"name": "dark", "emissiveTexture": {"index": 0}},
  {"name": "sun", "emissiveFactor": [40.0, 0.0, 0.0]}],
 "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
 "nodes": [{"mesh": 0}],
 "scenes": [{"nodes": [0]}]
})";
    std::string error;
    const auto model = graphics::parse_model(as_bytes(text), {}, &error);
    check(model != nullptr, "emissive model: " + error);
    check(model->materials.size() == 4U, "four materials");
    const graphics::ModelMaterial &lamp = model->materials[0];
    check_near(lamp.emissive.x, 4.0F, 1e-6F, "the strength scales red");
    check_near(lamp.emissive.y, 2.0F, 1e-6F, "and green");
    check_near(lamp.emissive.z, 1.0F, 1e-6F, "and blue");
    check(lamp.emissive_texture.width == 2 && lamp.emissive_texture.rgba.size() == 16U &&
              lamp.emissive_texture.rgba[0] == 255U && lamp.emissive_texture.rgba[1] == 0U,
          "the emissive texture is decoded");
    const graphics::ModelMaterial &plain = model->materials[1];
    check(plain.emissive.x == 0.0F && plain.emissive.y == 0.0F && plain.emissive.z == 0.0F &&
              plain.emissive_texture.empty(),
          "no emissive, no light");
    check(model->materials[2].emissive_texture.empty(),
          "a texture under a black factor gives no light and is not kept");
    check_near(model->materials[3].emissive.x, 16.0F, 0.0F, "the factor is capped");
}

void malformed_gltf_is_refused()
{
    std::string error;
    const auto refuse = [&](std::string_view text, const std::string &what) {
        error.clear();
        const auto model = graphics::parse_model(as_bytes(text), {}, &error);
        check(model == nullptr, what + " was accepted");
        check(!error.empty(), what + " gave no reason");
    };
    refuse("", "an empty file");
    refuse("{", "truncated JSON");
    refuse(R"({"asset":{"version":"1.0"}})", "glTF 1.0");
    refuse(R"({"asset":{"version":"2.0"}})", "a file without meshes");
    const std::string valid = skinned_triangle_gltf();
    refuse(valid.substr(0U, valid.size() / 2U), "half a file");
    refuse(skinned_triangle_gltf("../secret.bin"), "a buffer outside the model folder");
    refuse(skinned_triangle_gltf("C:/Windows/win.ini"), "an absolute buffer path");
    refuse(skinned_triangle_gltf("https://example.com/x.bin"), "a remote buffer");
    refuse(skinned_triangle_gltf("data:application/octet-stream;base64,AAAA"),
           "a buffer shorter than declared");
    {
        // A node that is its own ancestor.
        std::string loop = skinned_triangle_gltf();
        const auto at = loop.find(R"({"name": "Head"})");
        loop.replace(at, 16U, R"({"name": "Head", "children": [1]})");
        refuse(loop, "a node loop");
    }
    {
        // An index past the vertices.
        std::string bad = skinned_triangle_gltf();
        const auto at = bad.find(R"("count": 3, "type": "SCALAR")");
        bad.replace(at, 27U, R"("count": 4, "type": "SCALAR")");
        refuse(bad, "an accessor larger than its view");
    }
    {
        // A declared buffer of 2 GB.
        std::string huge = skinned_triangle_gltf();
        const auto at = huge.find(R"("byteLength": 264)");
        huge.replace(at, 17U, R"("byteLength": 2147483648)");
        refuse(huge, "a 2 GB buffer");
    }
    {
        // Over the vertex limit.
        graphics::ModelLimits limits;
        limits.max_vertices = 2U;
        const auto model = graphics::parse_model(as_bytes(valid), {}, &error, limits);
        check(model == nullptr && error.find("vertices") != std::string::npos,
              "the vertex limit holds");
    }
    // Random bytes never crash the parser.
    std::uint32_t seed = 12345U;
    for (int round = 0; round < 200; ++round)
    {
        std::string noisy = valid;
        for (int flips = 0; flips < 8; ++flips)
        {
            seed = seed * 1664525U + 1013904223U;
            noisy[seed % noisy.size()] = static_cast<char>(seed >> 24U);
        }
        (void)graphics::parse_model(as_bytes(noisy), {}, &error);
    }
}

void gltf_files_load_their_neighbours_only()
{
    TemporaryFolder folder{"neighbours"};
    std::vector<std::uint8_t> buffer;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F})
        append(buffer, value);
    write_bytes(folder.path() / "assets" / "models" / "tri.bin", buffer);
    const std::string text = R"({"asset":{"version":"2.0"},
      "buffers":[{"byteLength":36,"uri":"tri.bin"}],
      "bufferViews":[{"buffer":0,"byteLength":36}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
      "nodes":[{"mesh":0}]})";
    write_file(folder.path() / "assets" / "models" / "tri.gltf", text);
    graphics::AssetStack stack;
    stack.set_sources({{"m", folder.path() / "assets"}});
    graphics::ModelCache cache;
    std::string error;
    const auto model = cache.get(stack, "models/tri", &error);
    check(model != nullptr, "a .gltf with a .bin beside it loads: " + error);
    check(model->primitives[0].vertices[0].normal.z > 0.9F, "missing normals are generated");
    check(cache.get(stack, "models/tri") == model, "cached");
    check(cache.get(stack, "models/none", &error) == nullptr && !error.empty(), "absent refused");
}

void animation_packs_replace_registered_clips()
{
    rig::RetailRig rig;
    for (std::size_t bone = 0U; bone < rig.attachment.size(); ++bone)
        rig.attachment[bone].name = "Bone" + std::to_string(bone);
    rig.attachment[0].name = "Root";
    rig.attachment[rig::bone_head].name = "Head";
    rig::Clip old;
    old.duration_ticks = rig::ticks_from_seconds(1.0);
    for (std::size_t bone = 0U; bone < rig.attachment.size(); ++bone)
    {
        old.positions[bone] = {{7.0F, 7.0F, 7.0F}};
        old.rotations[bone] = {{}};
        old.scales[bone] = {{1.0F, 1.0F, 1.0F}};
    }
    rig.clips.push_back(old);
    rig.registrations.push_back({"Stand", 0, true, {}});
    rig.registrations.push_back({"Walk", 0, true, {}});

    std::string error;
    const std::string text = skinned_triangle_gltf();
    const auto model = graphics::parse_model(as_bytes(text), {}, &error);
    check(model != nullptr, error);
    const auto clip = graphics::clip_from_model(*model, rig, &rig.clips[0], &error);
    check(clip.has_value(), "converted: " + error);
    const auto head = static_cast<std::size_t>(rig::bone_head);
    check(clip->positions[head].size() == 31U, "30 Hz over one second");
    check_near(clip->positions[head][15].y, 1.0F, 1e-5F, "halfway at frame 15");
    check(clip->positions[0].size() == 1U && clip->positions[0][0].x == 7.0F,
          "a bone the clip does not animate keeps the replaced clip's keys");

    TemporaryFolder folder{"animpack"};
    write_file(folder.path() / "assets" / "animations" / "Stand.gltf", text);
    write_file(folder.path() / "assets" / "animations" / "Dance.gltf", text);
    graphics::AssetStack stack;
    stack.set_sources({{"anim", folder.path() / "assets"}});
    std::vector<std::string> warnings;
    const auto replaced = graphics::apply_animation_pack(rig, stack, &warnings);
    check(replaced.size() == 1U && replaced[0] == "Stand", "Stand replaced");
    check(warnings.size() == 1U, "an unregistered name is reported");
    check(rig.registrations[0].clip == 1 && rig.registrations[1].clip == 0,
          "Walk, which shared the old clip, is untouched");
    check(!graphics::animation_slots().empty(), "slots documented");
}

// --- Lights ------------------------------------------------------------------

graphics::ViewFrustum looking_down_negative_z()
{
    return graphics::make_frustum({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F},
                                  {0.0F, 1.0F, 0.0F}, 1.2F, 16.0F / 9.0F, 0.1F, 200.0F);
}

void light_culling_follows_the_frustum()
{
    const graphics::ViewFrustum frustum = looking_down_negative_z();
    check(graphics::sphere_visible(frustum, {0.0F, 0.0F, -10.0F}, 1.0F), "ahead is visible");
    check(!graphics::sphere_visible(frustum, {0.0F, 0.0F, 10.0F}, 1.0F), "behind is not");
    check(graphics::sphere_visible(frustum, {0.0F, 0.0F, 2.0F}, 3.0F),
          "a sphere around the eye is visible");
    check(!graphics::sphere_visible(frustum, {100.0F, 0.0F, -10.0F}, 1.0F), "far right is not");
    check(!graphics::sphere_visible(frustum, {0.0F, 100.0F, -10.0F}, 1.0F), "far up is not");
    check(!graphics::sphere_visible(frustum, {0.0F, -100.0F, -10.0F}, 1.0F), "far down is not");
    check(!graphics::sphere_visible(frustum, {0.0F, 0.0F, -400.0F}, 1.0F), "past far is not");

    graphics::LightStore store;
    graphics::LightDesc ahead;
    ahead.position = {0.0F, 0.0F, -10.0F};
    graphics::LightDesc behind = ahead;
    behind.position = {0.0F, 0.0F, 30.0F};
    graphics::LightDesc off = ahead;
    off.enabled = false;
    graphics::LightDesc attached = ahead;
    const auto a = store.create("m", ahead);
    (void)store.create("m", behind);
    (void)store.create("m", off);
    const auto d = store.create("m", attached);
    check(a && d, "created");
    store.find(*d)->attachment = graphics::Attachment{{HandleKind::entity, 7U}, {0, 1, 0}, true};
    graphics::LightFrame frame;
    const graphics::TargetLocator nowhere = [](Handle, graphics::Vec3 &, float &) { return false; };
    graphics::build_light_frame(store, 0.0, frustum, 160.0F, 256U, nowhere, frame);
    check(frame.lights.size() == 1U, "only the visible light");
    check(frame.considered == 3U && frame.culled == 2U, "disabled lights are not considered");

    const graphics::TargetLocator here = [](Handle, graphics::Vec3 &position, float &yaw) {
        position = {0.0F, 0.0F, -20.0F};
        yaw = 0.0F;
        return true;
    };
    graphics::build_light_frame(store, 0.0, frustum, 160.0F, 256U, here, frame);
    check(frame.lights.size() == 2U, "the attached light follows its target");

    // The cap keeps the most important lights.
    graphics::LightStore many;
    for (int index = 0; index < 600; ++index)
    {
        graphics::LightDesc light;
        light.position = {static_cast<float>(index % 20) - 10.0F, 0.0F,
                          -5.0F - static_cast<float>(index / 20)};
        light.intensity = index == 599 ? 50.0F : 1.0F;
        (void)many.create(index < 300 ? "a" : "b", light);
    }
    check(many.size() == 600U && many.count_for("a") == 300U, "per mod counts");
    graphics::build_light_frame(many, 0.0, frustum, 160.0F, 256U, here, frame);
    check(frame.lights.size() == 256U, "256 kept");
    check(frame.lights.front().color[0] > 10.0F, "the brightest first");
    check(frame.culled == 344U, "the rest reported");
    many.remove_mod("a");
    check(many.size() == 300U, "a mod's lights go with it");
}

// A camera at (0, 1, 0) looking down -Z with a 74-degree vertical view at
// 16:9, as the game's own.
graphics::ClusterView test_cluster_view(int width = 1600, int height = 900)
{
    graphics::ClusterView view;
    view.eye = {0.0F, 1.0F, 0.0F};
    view.right = {1.0F, 0.0F, 0.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.forward = {0.0F, 0.0F, -1.0F};
    view.scale_y = 1.0F / std::tan(37.0F * 3.14159265F / 180.0F);
    view.scale_x = view.scale_y * static_cast<float>(height) / static_cast<float>(width);
    view.width = width;
    view.height = height;
    return view;
}

void light_clusters_list_every_light()
{
    using graphics::ClusterGrid;
    using graphics::ClusterLookup;
    check_near(ClusterGrid::slice_start(0), 0.0F, 0.0F, "slice 0 starts at the eye");
    check_near(ClusterGrid::slice_start(1), ClusterGrid::first_depth, 1e-6F, "then first_depth");
    check_near(ClusterGrid::slice_start(ClusterGrid::slices - 1), ClusterGrid::last_depth, 1e-3F,
               "the last slice starts at last_depth");

    // Lights of every size, spread through the view and around it, one of
    // them a spot light and one right at the eye.
    std::uint32_t seed = 12345U;
    const auto random = [&seed](float low, float high) {
        seed = seed * 1664525U + 1013904223U;
        return low + (high - low) * static_cast<float>(seed >> 8U) / 16777216.0F;
    };
    std::vector<graphics::GpuLight> lights(180);
    for (graphics::GpuLight &light : lights)
    {
        const float depth = random(0.5F, 180.0F);
        light.position[0] = random(-1.2F, 1.2F) * depth;
        light.position[1] = 1.0F + random(-0.8F, 0.8F) * depth;
        light.position[2] = -depth;
        light.radius = random(0.5F, 12.0F);
        light.color[0] = light.color[1] = light.color[2] = 1.0F;
    }
    lights[7].spot_cos_outer = std::cos(0.6F);
    lights[7].spot_cos_inner = std::cos(0.4F);
    lights[11].position[0] = 0.2F;
    lights[11].position[1] = 1.1F;
    lights[11].position[2] = 0.3F;
    lights[11].radius = 3.0F;
    const graphics::ClusterGrid grid = graphics::make_cluster_grid(test_cluster_view(), lights);
    check(grid.tiles_x == 25 && grid.tiles_y == 15, "64-pixel tiles");
    check(grid.cluster_count() == 25U * 15U * ClusterGrid::slices, "tiles times slices");
    check(grid.far_depth > 180.0F && grid.far_depth < 193.0F, "the deepest sphere bounds the grid");
    graphics::ClusterLists lists;
    graphics::build_light_clusters(lights, grid, lists);
    check(lists.counts.size() == grid.cluster_count(), "a count per cluster");

    // Every light that reaches a point is listed in the point's cluster, and
    // the clustered sum equals the sum over every light.
    std::size_t in_clusters{};
    std::size_t off_grid{};
    std::size_t unlit{};
    const graphics::Vec3 up{0.0F, 1.0F, 0.0F};
    for (int sample = 0; sample < 60000; ++sample)
    {
        const float depth = random(-4.0F, 200.0F);
        const graphics::Vec3 point{random(-1.4F, 1.4F) * std::abs(depth) + random(-1.0F, 1.0F),
                                   1.0F + random(-0.9F, 0.9F) * std::abs(depth), -depth};
        const graphics::ClusterLookup lookup = graphics::cluster_of(grid, point);
        std::vector<graphics::GpuLight> listed;
        if (lookup.kind == ClusterLookup::Kind::cluster)
        {
            ++in_clusters;
            check(lists.counts[lookup.cluster] <= ClusterGrid::max_per_cluster,
                  "no cluster overflows here");
            const std::uint32_t count =
                std::min(lists.counts[lookup.cluster], ClusterGrid::max_per_cluster);
            for (std::uint32_t slot = 0U; slot < count; ++slot)
                listed.push_back(
                    lights[lists.indices[lookup.cluster * ClusterGrid::max_per_cluster + slot]]);
        }
        else if (lookup.kind == ClusterLookup::Kind::off_grid)
        {
            ++off_grid;
            listed = lights;
        }
        else
            ++unlit;
        for (std::size_t light = 0U; light < lights.size(); ++light)
        {
            const graphics::Vec3 offset{lights[light].position[0] - point.x,
                                        lights[light].position[1] - point.y,
                                        lights[light].position[2] - point.z};
            if (vfx::length(offset) >= lights[light].radius)
                continue;
            check(lookup.kind != ClusterLookup::Kind::unlit, "a lit point is never unlit");
            if (lookup.kind != ClusterLookup::Kind::cluster)
                continue;
            const auto first =
                lists.indices.begin() +
                static_cast<std::ptrdiff_t>(lookup.cluster * ClusterGrid::max_per_cluster);
            const auto last = first + static_cast<std::ptrdiff_t>(lists.counts[lookup.cluster]);
            check(std::find(first, last, static_cast<std::uint16_t>(light)) != last,
                  "a lit point finds its light in its cluster");
        }
        const graphics::Color all = graphics::light_at(lights, point, &up);
        const graphics::Color clustered = graphics::light_at(listed, point, &up);
        check_near(clustered.r, all.r, 1e-4F, "the clustered light is the whole light");
    }
    check(in_clusters > 30000U && off_grid > 1000U && unlit > 100U,
          "the samples cover clusters, off-grid points and points past every light");

    // A point behind the eye, far off the screen, or past every light.
    check(graphics::cluster_of(grid, {0.0F, 1.0F, 2.0F}).kind == ClusterLookup::Kind::off_grid,
          "behind the eye is off the grid");
    check(graphics::cluster_of(grid, {50.0F, 1.0F, -5.0F}).kind == ClusterLookup::Kind::off_grid,
          "off the screen is off the grid");
    check(graphics::cluster_of(grid, {0.0F, 1.0F, -400.0F}).kind == ClusterLookup::Kind::unlit,
          "past every light is unlit");
    const graphics::ClusterLookup near = graphics::cluster_of(grid, {0.0F, 1.0F, -0.5F});
    check(near.kind == ClusterLookup::Kind::cluster &&
              near.cluster < static_cast<std::uint32_t>(grid.tiles_x * grid.tiles_y),
          "half a block ahead is in slice 0");

    // No lights: nothing is ever lit, so there is nothing to look up.
    const graphics::ClusterGrid empty = graphics::make_cluster_grid(test_cluster_view(), {});
    check(empty.far_depth == 0.0F &&
              graphics::cluster_of(empty, {0.0F, 1.0F, -3.0F}).kind == ClusterLookup::Kind::unlit,
          "without lights every point ahead is unlit");

    // A huge window keeps its tile count bounded.
    const graphics::ClusterGrid wide =
        graphics::make_cluster_grid(test_cluster_view(20000, 900), lights);
    check(wide.tiles_x == ClusterGrid::max_tiles_x, "tiles across are capped");

    // A crowded cluster keeps the most important (first) lights and reports
    // the rest.
    std::vector<graphics::GpuLight> crowd(100);
    for (graphics::GpuLight &light : crowd)
    {
        light.position[1] = 1.0F;
        light.position[2] = -10.0F;
        light.radius = 3.0F;
    }
    const graphics::ClusterGrid crowded = graphics::make_cluster_grid(test_cluster_view(), crowd);
    graphics::build_light_clusters(crowd, crowded, lists);
    const graphics::ClusterLookup centre = graphics::cluster_of(crowded, {0.0F, 1.0F, -10.0F});
    check(centre.kind == ClusterLookup::Kind::cluster && lists.counts[centre.cluster] == 100U,
          "every light counted");
    bool ordered = true;
    for (std::uint32_t slot = 0U; slot < ClusterGrid::max_per_cluster; ++slot)
        ordered = ordered &&
                  lists.indices[centre.cluster * ClusterGrid::max_per_cluster + slot] == slot;
    check(ordered, "the first lights kept, in order");
    const graphics::LightClusterStats stats = graphics::cluster_stats(crowded, lists.counts);
    check(stats.valid && stats.busiest == 100U && stats.dropped >= 36U &&
              stats.references >= ClusterGrid::max_per_cluster && stats.lit_clusters > 0U,
          "the overflow is reported");
}

void light_falloff_and_modulation()
{
    check_near(graphics::light_attenuation(0.0F, 8.0F), 1.0F, 1e-6F, "full at the source");
    check_near(graphics::light_attenuation(4.0F, 8.0F), 0.25F, 1e-6F, "quarter at half");
    check_near(graphics::light_attenuation(8.0F, 8.0F), 0.0F, 1e-6F, "none at the radius");
    graphics::LightDesc steady;
    check_near(graphics::light_modulation(steady, 1U, 3.3), 1.0F, 1e-6F, "steady");
    graphics::LightDesc fire;
    fire.flicker = 1.0F;
    float low = 1.0F;
    float high = 0.0F;
    for (int step = 0; step < 500; ++step)
    {
        const float value = graphics::light_modulation(fire, 9U, step * 0.013);
        check(value >= 0.0F && value <= 1.0F, "bounded");
        low = std::min(low, value);
        high = std::max(high, value);
        check(value == graphics::light_modulation(fire, 9U, step * 0.013), "deterministic");
    }
    check(high - low > 0.2F, "a flickering light flickers");
    graphics::LightDesc pulse;
    pulse.pulse = 1.0F;
    pulse.pulse_speed = 1.0F;
    check_near(graphics::light_modulation(pulse, 1U, 0.0), 1.0F, 1e-5F, "pulse peak");
    check_near(graphics::light_modulation(pulse, 1U, 0.5), 0.0F, 1e-5F, "pulse trough");

    graphics::LightFrame frame;
    graphics::GpuLight light;
    light.position[1] = 2.0F;
    light.radius = 4.0F;
    light.color[0] = 2.0F;
    frame.lights.push_back(light);
    const graphics::Vec3 up{0.0F, 1.0F, 0.0F};
    const graphics::Vec3 down{0.0F, -1.0F, 0.0F};
    const graphics::Color facing = graphics::light_at(frame, {0.0F, 0.0F, 0.0F}, &up);
    const graphics::Color away = graphics::light_at(frame, {0.0F, 0.0F, 0.0F}, &down);
    check_near(facing.r, 0.5F, 1e-5F, "facing the light: full falloff");
    check(away.r < facing.r * 0.2F, "facing away: dim");
    frame.lights[0].spot_cos_outer = std::cos(0.3F);
    frame.lights[0].spot_cos_inner = std::cos(0.2F);
    frame.lights[0].direction[1] = -1.0F;
    check(graphics::light_at(frame, {0.0F, 0.0F, 0.0F}, &up).r > 0.4F, "inside the cone");
    check(graphics::light_at(frame, {1.5F, 1.0F, 0.0F}, &up).r == 0.0F, "outside the cone");
}

// --- Atmosphere ----------------------------------------------------------------

void atmosphere_layers_blend_and_fade()
{
    graphics::AtmosphereStack stack;
    graphics::AtmosphereLayerDesc dark;
    dark.values.fog_color = graphics::Color{0.0F, 0.0F, 0.0F};
    dark.values.exposure = -1.0F;
    dark.values.fog_end = 12.0F;
    dark.fade_in = 2.0F;
    dark.fade_out = 1.0F;
    dark.region.kind = graphics::AtmosphereRegion::Kind::box;
    dark.region.minimum = {0.0F, 0.0F, 0.0F};
    dark.region.maximum = {10.0F, 10.0F, 10.0F};
    dark.region.feather = 4.0F;
    const auto id = stack.create("m", dark);
    check(id.has_value(), "created");
    check(!stack.active(), "fading in starts at nothing");
    stack.step(1.0F, {5.0F, 5.0F, 5.0F});
    check_near(stack.weight(*id), 0.5F, 1e-5F, "half way through the fade");
    world::SkyState sky;
    sky.fog = {1.0F, 1.0F, 1.0F};
    stack.apply(sky);
    check_near(sky.fog.red, 0.5F, 1e-5F, "fog half way to black");
    PostEffects effects;
    float ev = 0.0F;
    stack.apply(effects, ev);
    check_near(ev, -0.5F, 1e-5F, "exposure half applied");
    float start = 20.0F;
    float end = 100.0F;
    check(stack.fog_band(start, end), "fog band set");
    check_near(end, 56.0F, 1e-4F, "fog end half way");
    // Outside the region by the whole feather: nothing.
    stack.step(5.0F, {20.0F, 5.0F, 5.0F});
    check(!stack.active(), "beyond the feather");
    stack.step(0.0F, {12.0F, 5.0F, 5.0F});
    check(stack.weight(*id) > 0.1F && stack.weight(*id) < 0.9F, "inside the feather is partial");
    // Priority: a higher layer applies over a lower one.
    graphics::AtmosphereLayerDesc red;
    red.values.fog_color = graphics::Color{1.0F, 0.0F, 0.0F};
    red.fade_in = 0.0F;
    red.priority = 10;
    graphics::AtmosphereLayerDesc blue = red;
    blue.values.fog_color = graphics::Color{0.0F, 0.0F, 1.0F};
    blue.priority = 5;
    graphics::AtmosphereStack ordered;
    (void)ordered.create("m", red);
    (void)ordered.create("m", blue);
    ordered.step(0.016F, {});
    world::SkyState over;
    ordered.apply(over);
    check(over.fog.red == 1.0F && over.fog.blue == 0.0F, "the higher priority wins");
    // Removal fades out, then drops the layer.
    check(stack.remove(*id), "removing");
    stack.step(0.5F, {5.0F, 5.0F, 5.0F});
    check(stack.size() == 1U, "still fading");
    stack.step(5.0F, {5.0F, 5.0F, 5.0F});
    check(stack.size() == 0U, "gone after its fade");
}

void luts_grade_colours()
{
    graphics::Image identity;
    constexpr int size = 16;
    identity.width = size * size;
    identity.height = size;
    identity.rgba.resize(static_cast<std::size_t>(identity.width) * size * 4U);
    for (int g = 0; g < size; ++g)
        for (int b = 0; b < size; ++b)
            for (int r = 0; r < size; ++r)
            {
                const std::size_t at = (static_cast<std::size_t>(g) * identity.width +
                                        static_cast<std::size_t>(b * size + r)) *
                                       4U;
                identity.rgba[at] = static_cast<std::uint8_t>(r * 255 / (size - 1));
                identity.rgba[at + 1U] = static_cast<std::uint8_t>(g * 255 / (size - 1));
                identity.rgba[at + 2U] = static_cast<std::uint8_t>(b * 255 / (size - 1));
                identity.rgba[at + 3U] = 255U;
            }
    check(graphics::valid_lut(identity), "a 256x16 strip is a LUT");
    const graphics::Color sample{0.3F, 0.6F, 0.9F};
    const graphics::Color graded = graphics::sample_lut(identity, sample);
    check_near(graded.r, 0.3F, 0.01F, "identity red");
    check_near(graded.g, 0.6F, 0.01F, "identity green");
    check_near(graded.b, 0.9F, 0.01F, "identity blue");
    graphics::Image inverted = identity;
    for (std::size_t texel = 0U; texel < inverted.rgba.size(); texel += 4U)
        inverted.rgba[texel] = static_cast<std::uint8_t>(255U - inverted.rgba[texel]);
    check_near(graphics::sample_lut(inverted, sample).r, 0.7F, 0.01F, "a graded LUT changes red");
    graphics::Image wrong;
    wrong.width = 100;
    wrong.height = 9;
    wrong.rgba.resize(3600U);
    check(!graphics::valid_lut(wrong), "a wrong shape is not a LUT");

    graphics::AtmosphereStack stack;
    graphics::AtmosphereLayerDesc low;
    low.values.lut = "luts/warm";
    low.values.lut_strength = 0.5F;
    low.fade_in = 0.0F;
    graphics::AtmosphereLayerDesc high = low;
    high.values.lut = "luts/cold";
    high.values.lut_strength = 1.0F;
    high.priority = 3;
    (void)stack.create("m", low);
    (void)stack.create("m", high);
    stack.step(0.0F, {});
    const graphics::ResolvedLut lut = stack.lut();
    check(lut.name == "luts/cold" && lut.strength == 1.0F, "the top-most LUT grades");
}

// --- Camera ----------------------------------------------------------------

void camera_effects_respect_accessibility()
{
    graphics::CameraEffects camera;
    camera.shake(1.0F, 1.0F);
    camera.fov_pulse(10.0F, 1.0F);
    camera.fade({0.0F, 0.0F, 0.0F}, 1.0F, 2.0F);
    camera.step(0.5F);
    graphics::CameraAccessibility normal;
    const graphics::CameraOutput full = camera.output(normal);
    check(full.fov > 9.0F, "the pulse peaks half way");
    check_near(full.fade_alpha, 0.25F, 1e-5F, "a quarter faded");
    float largest{};
    for (int step = 0; step < 50; ++step)
    {
        camera.step(0.005F);
        const auto output = camera.output(normal);
        largest = std::max(largest, std::fabs(output.yaw) + std::fabs(output.pitch));
        check(std::fabs(output.yaw) <= graphics::CameraEffects::max_shake_degrees * 0.0175F,
              "a shake is bounded");
    }
    check(largest > 0.0F, "the camera shakes");
    graphics::CameraAccessibility reduced;
    reduced.reduced_motion = true;
    const graphics::CameraOutput still = camera.output(reduced);
    check(still.yaw == 0.0F && still.pitch == 0.0F && still.roll == 0.0F && still.fov == 0.0F,
          "reduced motion: no shake or pulse");
    check(still.fade_alpha > 0.0F, "fades are not motion and stay");
    graphics::CameraAccessibility off;
    off.shake_scale = 0.0F;
    check(camera.output(off).yaw == 0.0F, "the explosion shake slider at zero");
    for (int second = 0; second < 5; ++second)
        camera.step(1.0F);
    const graphics::CameraOutput done = camera.output(normal);
    check(done.yaw == 0.0F && done.fov == 0.0F && done.fade_alpha == 1.0F, "settled");
    camera.fade({0.0F, 0.0F, 0.0F}, 0.0F, 1.0F);
    camera.step(0.5F);
    check_near(camera.output(normal).fade_alpha, 0.5F, 1e-5F, "fading back from where it was");
}

// --- Sound -----------------------------------------------------------------

std::vector<std::uint8_t> wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate,
                              std::uint16_t bits, const std::vector<std::uint8_t> &samples,
                              std::uint32_t declared_data = 0U, std::uint16_t block = 0U)
{
    std::vector<std::uint8_t> bytes;
    const auto text = [&](std::string_view value) {
        for (const char c : value)
            bytes.push_back(static_cast<std::uint8_t>(c));
    };
    text("RIFF");
    append(bytes, static_cast<std::uint32_t>(36U + samples.size()));
    text("WAVE");
    text("fmt ");
    append(bytes, std::uint32_t{16});
    append(bytes, format);
    append(bytes, channels);
    append(bytes, rate);
    const std::uint16_t align =
        block != 0U ? block : static_cast<std::uint16_t>(channels * bits / 8U);
    append(bytes, static_cast<std::uint32_t>(rate * align));
    append(bytes, align);
    append(bytes, bits);
    text("data");
    append(bytes, declared_data != 0U ? declared_data : static_cast<std::uint32_t>(samples.size()));
    bytes.insert(bytes.end(), samples.begin(), samples.end());
    return bytes;
}

void sound_decoding_is_bounded()
{
    std::vector<std::uint8_t> tone;
    for (int frame = 0; frame < 4800; ++frame)
        append(tone, static_cast<std::int16_t>(std::sin(frame * 0.0575) * 16000.0));
    std::string error;
    const auto mono = graphics::decode_wav(wav(1U, 1U, 48000U, 16U, tone), &error);
    check(mono.has_value(), "16-bit mono decodes: " + error);
    check(mono->channels == 1 && mono->frames() == 4800U && mono->sample_rate == 48000,
          "its shape");
    check_near(static_cast<float>(mono->seconds()), 0.1F, 1e-4F, "its length");

    std::vector<std::uint8_t> floats;
    for (int frame = 0; frame < 100; ++frame)
    {
        append(floats, 0.5F);
        append(floats, -0.5F);
    }
    const auto stereo = graphics::decode_wav(wav(3U, 2U, 44100U, 32U, floats), &error);
    check(stereo && stereo->channels == 2 && stereo->samples[0] > 16000 &&
              stereo->samples[1] < -16000,
          "float stereo decodes");
    std::vector<std::uint8_t> deep;
    for (int frame = 0; frame < 10; ++frame)
        for (const std::uint8_t byte : std::array<std::uint8_t, 3>{0x00U, 0x00U, 0x40U})
            deep.push_back(byte);
    const auto twenty_four = graphics::decode_wav(wav(1U, 1U, 22050U, 24U, deep), &error);
    check(twenty_four && twenty_four->samples[0] == 16384, "24-bit decodes");

    check(!graphics::decode_wav(as_bytes("RIFF"), &error), "a stub refused");
    check(!graphics::decode_wav(wav(2U, 1U, 48000U, 4U, tone), &error), "ADPCM refused");
    check(!graphics::decode_wav(wav(1U, 0U, 48000U, 16U, tone), &error), "no channels refused");
    check(!graphics::decode_wav(wav(1U, 1U, 1000U, 16U, tone), &error), "a silly rate refused");
    check(!graphics::decode_wav(wav(1U, 1U, 48000U, 16U, tone, 0U, 7U), &error),
          "a lying block alignment refused");
    const auto truncated =
        graphics::decode_wav(wav(1U, 1U, 48000U, 16U, tone, 0x7FFFFFF0U), &error);
    check(truncated && truncated->frames() == 4800U, "a truncated data chunk keeps what is there");
    graphics::AudioLimits short_limit;
    short_limit.max_seconds = 0.05;
    check(!graphics::decode_wav(wav(1U, 1U, 48000U, 16U, tone), &error, short_limit) &&
              error.find("seconds") != std::string::npos,
          "too long refused");
    graphics::AudioLimits small;
    small.max_file_bytes = 100U;
    check(!graphics::decode_audio(wav(1U, 1U, 48000U, 16U, tone), &error, small),
          "too big refused");
    std::vector<std::uint8_t> fake_ogg{'O', 'g', 'g', 'S'};
    fake_ogg.resize(4096U, 0x5AU);
    check(!graphics::decode_audio(fake_ogg, &error) && !error.empty(), "a corrupt Ogg is refused");
    check(graphics::SoundAsset::from_ogg_stream(fake_ogg, &error) == nullptr, "and cannot stream");
    check(!graphics::decode_audio(as_bytes("hello"), &error), "unknown formats refused");
}

float energy(const std::vector<float> &buffer, std::size_t channel)
{
    float total{};
    for (std::size_t index = channel; index < buffer.size(); index += 2U)
        total += buffer[index] * buffer[index];
    return total;
}

void mixer_positions_loops_and_crossfades()
{
    graphics::PcmBuffer pcm;
    pcm.channels = 1;
    pcm.sample_rate = 24000;
    for (int frame = 0; frame < 2400; ++frame)
        pcm.samples.push_back(static_cast<std::int16_t>(std::sin(frame * 0.1) * 20000.0));
    const auto sound = graphics::SoundAsset::from_pcm(pcm);
    graphics::Mixer mixer{48000};
    std::vector<float> out(4800U * 2U);

    const auto voice = mixer.play(sound, {});
    check(voice != 0U && mixer.playing(voice), "playing");
    mixer.mix(out.data(), 4800U);
    check(energy(out, 0) > 1.0F && std::fabs(energy(out, 0) - energy(out, 1)) < 1e-3F,
          "a 2D sound is centred");
    mixer.mix(out.data(), 4800U);
    check(!mixer.playing(voice), "a one-shot ends (0.1 s at half rate = 4800 frames)");

    // Positioned: to the right, louder on the right; beyond max distance, silent.
    mixer.set_listener({0.0F, 0.0F, 0.0F}, 0.0F);
    graphics::VoiceParams right;
    right.position = graphics::Vec3{8.0F, 0.0F, 0.0F};
    right.loop = true;
    const auto positioned = mixer.play(sound, right);
    mixer.mix(out.data(), 2400U);
    check(energy(out, 1) > energy(out, 0) * 4.0F, "panned right");
    mixer.set_position(positioned, {0.0F, 0.0F, -100.0F});
    mixer.mix(out.data(), 2400U);
    check(energy(out, 0) + energy(out, 1) == 0.0F, "silent beyond max distance");
    mixer.mix(out.data(), 4800U);
    check(mixer.playing(positioned), "a loop keeps playing");
    mixer.stop(positioned, 0.0F);
    check(!mixer.playing(positioned), "stopped");

    // Volume follows the game's settings.
    const auto quiet = mixer.play(sound, {});
    mixer.set_levels({1.0F, 1.0F, 0.0F, 1.0F, false});
    mixer.mix(out.data(), 1200U);
    check(energy(out, 0) == 0.0F, "effects volume at zero");
    mixer.set_levels({1.0F, 1.0F, 1.0F, 1.0F, true});
    mixer.mix(out.data(), 1200U);
    check(energy(out, 0) == 0.0F, "muted");
    mixer.set_levels({});
    mixer.stop(quiet);

    // Music crossfades.
    graphics::Mixer music{48000};
    music.play_music(sound, 0.0F, 1.0F, true);
    music.mix(out.data(), 480U);
    check(music.voice_count() == 1U, "one track");
    music.play_music(sound, 0.05F, 1.0F, true);
    music.mix(out.data(), 100U);
    check(music.voice_count() == 2U, "both during the crossfade");
    music.mix(out.data(), 4800U);
    check(music.voice_count() == 1U, "the old one gone after it");
    music.play_music(nullptr, 0.0F, 0.0F, false);
    music.mix(out.data(), 10U);
    check(music.voice_count() == 0U, "music stopped");
    for (int index = 0; index < 80; ++index)
        (void)music.play(sound, {});
    check(music.voice_count() == graphics::Mixer::max_voices, "voices are capped");
    const auto spatial = graphics::spatial_gain({}, 0.0F, {0.0F, 0.0F, -1.0F}, 2.0F, 32.0F);
    check_near(spatial.left, spatial.right, 1e-5F, "straight ahead is centred");
}

// (Polish) OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV: the mix as a WAV, with marks
// at the frames calls took effect.
void mixer_dumps_to_a_wav()
{
    graphics::PcmBuffer pcm;
    pcm.channels = 1;
    pcm.sample_rate = 48000;
    pcm.samples.assign(480U, std::int16_t{16384}); // a 10 ms block at half scale
    const auto sound = graphics::SoundAsset::from_pcm(pcm);
    graphics::Mixer mixer{48000};
    std::vector<float> out(1000U * 2U);
    mixer.mix(out.data(), 1000U); // before the recorder: not captured
    graphics::MixRecorder recorder{48000, 0.05}; // 2400 frames
    mixer.set_recorder(&recorder);
    mixer.mix(out.data(), 1000U); // silence: frames 0..999
    mixer.mark("go");
    check(mixer.play(sound, {}) != 0U, "playing");
    mixer.mix(out.data(), 1000U);
    mixer.mix(out.data(), 1000U); // only 400 more fit
    check(recorder.full() && recorder.frames() == 2400U, "the capture stops when full");
    mixer.set_recorder(nullptr);
    mixer.mix(out.data(), 1000U);
    check(recorder.frames() == 2400U, "a detached recorder takes nothing");
    check(recorder.marks().size() == 2U && recorder.marks()[0].frame == 1000U &&
              recorder.marks()[0].text == "go" && recorder.marks()[1].frame == 1000U &&
              recorder.marks()[1].text.starts_with("play voice=1 bus=effects"),
          "marks sit at the frame the next block starts");

    TemporaryFolder folder{"mixdump"};
    const std::filesystem::path wav = folder.path() / "mix.wav";
    std::string error;
    check(recorder.write(wav, error), "written: " + error);
    std::ifstream in{wav, std::ios::binary};
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{in}, {}};
    check(bytes.size() == 44U + 2400U * 4U, "header and 2400 stereo 16-bit frames");
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1U]) << 8U) |
               (static_cast<std::uint32_t>(bytes[at + 2U]) << 16U) |
               (static_cast<std::uint32_t>(bytes[at + 3U]) << 24U);
    };
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1U] << 8U));
    };
    check(std::memcmp(bytes.data(), "RIFF", 4U) == 0 && u32(4U) == 36U + 9600U &&
              std::memcmp(bytes.data() + 8U, "WAVEfmt ", 8U) == 0 && u32(16U) == 16U,
          "RIFF/WAVE/fmt header");
    check(u16(20U) == 1U && u16(22U) == 2U && u32(24U) == 48000U && u32(28U) == 192000U &&
              u16(32U) == 4U && u16(34U) == 16U,
          "PCM, stereo, 48 kHz, 16 bit");
    check(std::memcmp(bytes.data() + 36U, "data", 4U) == 0 && u32(40U) == 9600U, "data chunk");
    const auto sample = [&](std::size_t frame, std::size_t channel) {
        return static_cast<std::int16_t>(u16(44U + frame * 4U + channel * 2U));
    };
    check(sample(999U, 0U) == 0 && sample(1000U, 0U) > 15000 && sample(1000U, 1U) > 15000 &&
              sample(1479U, 1U) > 15000 && sample(1480U, 0U) == 0,
          "silence, then the 480-frame sound from frame 1000, in both channels");
    std::ifstream notes{folder.path() / "mix.wav.marks.txt"};
    const std::string text{std::istreambuf_iterator<char>{notes}, {}};
    check(text.find("2400 frames") != std::string::npos &&
              text.find("\n1000 0.0208 ") != std::string::npos &&
              text.find(" go\n") != std::string::npos,
          "the marks file: " + text);
}

// --- Effects ----------------------------------------------------------------

void effect_recipes_parse_with_paths()
{
    const Value recipe = Map{
        {"emitters",
         Array{Map{{"material", "spark"},
                   {"burst", 12},
                   {"life", Array{0.2, 0.5}},
                   {"speed", Map{{"min", 2.0}, {"max", 6.0}}},
                   {"size", 0.02},
                   {"alpha", Array{Array{0.0, 0.0}, Array{0.2, 1.0}, Array{1.0, 0.0}}},
                   {"color", Array{Array{0.0, Array{1.0, 0.8, 0.4}}, Array{1.0, "#ff2000"}}},
                   {"emissive", Map{{"from", 8.0}, {"to", 1.0}}},
                   {"gravity", 9.0},
                   {"collide", true}},
               Map{{"material", "smoke"}, {"rate", 5000}, {"loop", true}}}},
        {"lights", Array{Map{{"color", Array{1.0, 0.5, 0.2}}, {"radius", 6.0}, {"flicker", 0.4}}}},
        {"shake", 0.3},
    };
    const vfx::EffectDesc desc = graphics::EffectLibrary::parse(recipe);
    check(desc.emitters.size() == 2U && desc.lights.size() == 1U, "two emitters, one light");
    check(desc.emitters[0].material == vfx::Material::spark && desc.emitters[0].burst == 12,
          "material and burst");
    check(desc.emitters[0].life.min == 0.2F && desc.emitters[0].speed.max == 6.0F, "ranges");
    check(desc.emitters[0].alpha.count == 3U && desc.emitters[0].color.count == 2U, "curves");
    check_near(desc.emitters[0].color.value[1].g, 0x20 / 255.0F, 1e-5F, "hex colour");
    check(desc.emitters[1].rate == 2000.0F, "an excessive rate is clamped");

    const auto fails = [](const Value &bad, std::string_view needle) {
        try
        {
            (void)graphics::EffectLibrary::parse(bad);
        }
        catch (const ScriptError &error)
        {
            check(std::string_view{error.what()}.find(needle) != std::string_view::npos,
                  std::string{"error \""} + error.what() + "\" lacks \"" + std::string{needle} +
                      "\"");
            return;
        }
        throw std::runtime_error("expected a script error mentioning " + std::string{needle});
    };
    fails(Map{}, "at least one");
    fails(Map{{"emitters", Array{Map{{"life", "long"}}}}}, "emitters[1].life");
    fails(Map{{"emitters", Array{Map{{"material", "plasma"}}}}}, "emitters[1].material");
    fails(Map{{"emitters", Array{Map{{"alpha", Array{Array{0.5, 1.0}, Array{0.1, 0.0}}}}}}},
          "rising time");
    Array many;
    for (int index = 0; index < 13; ++index)
        many.push_back(Map{{"burst", 1}});
    fails(Map{{"emitters", many}}, "at most 12");
    fails(Map{{"lights", Array{Map{{"color", "red"}}}}}, "lights[1].color");

    graphics::EffectLibrary library;
    const vfx::EffectDesc &mine = library.define("dungeon", "torch_glow", recipe);
    check(mine.name == "dungeon:torch_glow", "full name");
    check(library.find("dungeon", "torch_glow") == &mine, "own name");
    check(library.find("other", "dungeon:torch_glow") == &mine, "qualified name");
    check(library.find("other", "torch_glow") == nullptr, "another mod's short name is not found");
    check(library.find("dungeon", "explosion_tnt") != nullptr, "built-ins are reachable");
    const vfx::EffectDesc &again = library.define("dungeon", "torch_glow", recipe);
    check(&again != &mine && mine.name == "dungeon:torch_glow",
          "redefinition keeps the old recipe alive for effects still playing it");
}

// --- Scripts ----------------------------------------------------------------

void scripts_drive_the_service()
{
    FakeBackend backend;
    graphics::GraphicsService service{backend};
    Harness harness;
    graphics::register_graphics_api(harness.registry(), harness.host(), service);
    backend.entities[42U] = {1.0F, 2.0F, 3.0F};
    const CapabilityMask client = modding::testing::client_mod_capabilities();

    check_ok(harness.run(R"(
        local a = light.create({position = vector.create(1, 2, 3), color = {1, 0.5, 0.2},
                                radius = 10, flicker = 0.3})
        assert(a ~= nil, "a light")
        light.set(a, {intensity = 2, color = "#00ff80"})
        local b = light.create({position = vector.create(0, 0, 0), shape = "spot",
                                direction = vector.create(0, -1, 0)})
        light.enable(b, false)
        assert(light.count() == 2)
        local ok = light.remove(b)
        assert(ok and light.count() == 1)
    )",
                         Side::client, client),
             "lights from a script");
    check(service.lights().size() == 1U, "one light left");
    const graphics::LightStore::Entry &entry = service.lights().entries().front();
    check(entry.mod_id == "test" && entry.desc.intensity == 2.0F && entry.desc.color.g == 1.0F,
          "the light's fields");

    check_fails(harness.run("light.create({radius = 'big'})", Side::client, client), "radius",
                "a bad field names itself");
    // A mod may not change another mod's light, even holding its handle.
    {
        const Handle foreign{HandleKind::light, entry.id};
        CallContext thief{"thief", Side::client, client, {}};
        const auto *set = harness.registry().find_function("light.set");
        check(set != nullptr, "light.set registered");
        const std::array<Value, 2> args{Value{foreign}, Value{Map{{"radius", 4.0}}}};
        bool refused{};
        try
        {
            (void)set->call(thief, args);
        }
        catch (const ScriptError &error)
        {
            refused = std::string_view{error.what()}.find("another mod") != std::string_view::npos;
        }
        check(refused, "another mod's light is refused");
        check(set->spec.sides == side_client && set->spec.requires_caps == capability::presentation,
              "client side, presentation capability");
    }

    check_ok(harness.run(R"(
        local name = fx.define("spark_burst", {emitters = {{material = "spark", burst = 8}}})
        assert(name == "test:spark_burst")
        local e = fx.spawn("spark_burst", vector.create(0, 1, 0), {scale = 2})
        assert(e ~= nil)
        local t = fx.spawn("torch_flame", vector.create(0, 1, 0), {attach = _G.target})
        assert(#fx.builtins() > 4)
        assert(fx.stop(e, true))
    )",
                         Side::client, client),
             "effects from a script");
    check(backend.spawned == 2 && backend.killed == 1, "spawned and stopped through the backend");
    backend.effects = false;
    check_ok(harness.run(R"(assert(fx.spawn("spark_burst", vector.create(0, 0, 0)) == nil))",
                         Side::client, client),
             "nil in the Original presentation");
    check_fails(harness.run(R"(fx.spawn("nope", vector.create(0, 0, 0)))", Side::client, client),
                "no effect", "an unknown recipe");

    check_ok(harness.run(R"(
        local layer = atmosphere.push({fog_color = {0, 0, 0}, fog_end = 16, exposure = -1,
                                       region = {min = vector.create(0, 0, 0),
                                                 max = vector.create(10, 10, 10), feather = 2},
                                       fade_in = 0.5})
        assert(layer ~= nil)
        camera.shake(0.5, 0.3)
        camera.fade({0, 0, 0}, 1, 2)
        camera.fov_pulse(8, 0.5)
    )",
                         Side::client, client),
             "atmosphere and camera from a script");
    service.step(0.25F, {5.0F, 5.0F, 5.0F});
    check(service.atmosphere().active(), "the layer applies inside its region");
    check(service.camera().active(), "camera effects run");

    check_fails(harness.run(R"(sound.play("sounds/missing"))", Side::client, client), "no sound",
                "a missing sound");
    check_fails(
        harness.run(R"(model.spawn("../escape", vector.create(0, 0, 0)))", Side::client, client),
        "asset name", "a model outside the assets folder");
    check_fails(harness.run(R"(model.spawn("models/missing", vector.create(0, 0, 0)))",
                            Side::client, client),
                "no model", "a missing model");

    // Presentation is client-only: a server VM has no light library at all.
    check_fails(harness.run("light.create({})", Side::server), "light", "no lights on the server");

    // The VM going away takes everything with it.
    harness.runtime().stop(Side::client);
    check(service.lights().size() == 0U && service.atmosphere().size() == 0U &&
              !service.camera().active(),
          "teardown clears the service");
}

void scripts_play_mod_assets()
{
    TemporaryFolder folder{"scriptassets"};
    const auto root = folder.path() / "mod" / "assets";
    std::vector<std::uint8_t> tone;
    for (int frame = 0; frame < 480; ++frame)
        append(tone, static_cast<std::int16_t>(frame * 30));
    write_bytes(root / "sounds" / "bell.wav", wav(1U, 1U, 48000U, 16U, tone));
    write_file(root / "models" / "tri.gltf", skinned_triangle_gltf());

    FakeBackend backend;
    graphics::GraphicsService service{backend};
    service.set_sources({{"test", root}});
    check(graphics::presentation_assets() != nullptr, "installed for the loaders");
    Harness harness;
    graphics::register_graphics_api(harness.registry(), harness.host(), service);
    const CapabilityMask client = modding::testing::client_mod_capabilities();
    check_ok(harness.run(R"(
        local s = sound.play("sounds/bell", {volume = 0.5, position = vector.create(1, 0, 0)})
        assert(s ~= nil, "a sound")
        assert(sound.stop(s, 0.1))
        assert(assets.exists("sounds/bell"))
        assert(not assets.exists("sounds/none"))
        local clips = model.clips("models/tri")
        assert(clips[1] == "Rise")
        local m = model.spawn("models/tri", vector.create(0, 64, 0), {yaw = 90, scale = 2,
                                                                      animation = "Rise"})
        assert(m ~= nil)
        model.play(m, "Rise", {loop = false})
        model.set(m, {position = vector.create(1, 64, 0), tint = {1, 0, 0}})
        assert(model.remove(m))
        assert(#assets.list("sounds") == 1)
    )",
                         Side::client, client),
             "assets from a script");
    service.set_sources({});
    check(graphics::presentation_assets() == nullptr, "an empty stack installs nothing");
}

// Modding, visuals area: a mod item's icon as the solid sprite pickups and the
// first-person hand draw (openminerz/modding/icon_model.hpp).
void item_icons_become_solid_sprites()
{
    graphics::Image icon;
    icon.width = 4;
    icon.height = 4;
    icon.rgba.assign(4U * 4U * 4U, 0U);
    const auto paint = [&](int x, int y, std::uint8_t red, std::uint8_t alpha) {
        const std::size_t at = (static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U;
        icon.rgba[at] = red;
        icon.rgba[at + 3U] = alpha;
    };
    // One opaque red texel at the top left, a faint one beside it (below the
    // threshold), and a 2x1 bar of the same colour at the bottom.
    paint(0, 0, 255U, 255U);
    paint(1, 0, 255U, 40U);
    paint(1, 3, 128U, 255U);
    paint(2, 3, 128U, 255U);
    const graphics::IconModel model = graphics::icon_model(icon, 32, 0.25F);
    check(model.resolution == 4, "a small icon keeps its size");
    // Texel (0,0): front + back + 4 walls = 6 quads. The bar: one merged run on
    // each broad face (2 quads), 2 end walls, 2 top and 2 bottom walls = 8.
    check(model.vertices.size() == (6U + 8U) * 6U,
          "solid texels only, runs merged: " + std::to_string(model.vertices.size()));
    float low_x = 1.0F;
    float high_y = -1.0F;
    float deepest = 0.0F;
    bool red_front{};
    for (const graphics::IconModelVertex &vertex : model.vertices)
    {
        low_x = std::min(low_x, vertex.position[0]);
        high_y = std::max(high_y, vertex.position[1]);
        deepest = std::max(deepest, std::abs(vertex.position[2]));
        red_front = red_front || (vertex.normal[2] > 0.5F && vertex.color[0] > 0.99F);
        check(vertex.color[3] == 1.0F, "every face is opaque");
    }
    check(low_x == -0.5F && high_y == 0.5F, "the icon spans the unit square, image top up");
    check(deepest == 0.125F, "centred on the thickness");
    check(red_front, "texel colours carry to the faces");
    // A clear icon has no geometry; a large one is reduced.
    graphics::Image clear;
    clear.width = 8;
    clear.height = 8;
    clear.rgba.assign(8U * 8U * 4U, 0U);
    check(graphics::icon_model(clear).empty(), "a clear icon has no model");
    graphics::Image large;
    large.width = 256;
    large.height = 256;
    large.rgba.assign(256U * 256U * 4U, 255U);
    const graphics::IconModel reduced = graphics::icon_model(large, 16);
    check(reduced.resolution == 16 && reduced.vertices.size() == (16U * 2U + 64U) * 6U,
          "a large icon is reduced: " + std::to_string(reduced.vertices.size()));
}

struct TestCase final
{
    const char *name;
    std::function<void()> body;
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"asset_names_refuse_traversal", asset_names_refuse_traversal},
        {"packs_layer_in_load_order", packs_layer_in_load_order},
        {"images_resample_and_tiles_compose", images_resample_and_tiles_compose},
        {"tile_overrides_are_collected_from_packs", tile_overrides_are_collected_from_packs},
        {"gltf_parses_skins_and_animations", gltf_parses_skins_and_animations},
        {"gltf_emissive_materials_load", gltf_emissive_materials_load},
        {"malformed_gltf_is_refused", malformed_gltf_is_refused},
        {"gltf_files_load_their_neighbours_only", gltf_files_load_their_neighbours_only},
        {"animation_packs_replace_registered_clips", animation_packs_replace_registered_clips},
        {"light_culling_follows_the_frustum", light_culling_follows_the_frustum},
        {"light_clusters_list_every_light", light_clusters_list_every_light},
        {"light_falloff_and_modulation", light_falloff_and_modulation},
        {"atmosphere_layers_blend_and_fade", atmosphere_layers_blend_and_fade},
        {"luts_grade_colours", luts_grade_colours},
        {"camera_effects_respect_accessibility", camera_effects_respect_accessibility},
        {"sound_decoding_is_bounded", sound_decoding_is_bounded},
        {"mixer_positions_loops_and_crossfades", mixer_positions_loops_and_crossfades},
        {"mixer_dumps_to_a_wav", mixer_dumps_to_a_wav}, // (Polish)
        {"effect_recipes_parse_with_paths", effect_recipes_parse_with_paths},
        {"scripts_drive_the_service", scripts_drive_the_service},
        {"scripts_play_mod_assets", scripts_play_mod_assets},
        {"item_icons_become_solid_sprites", item_icons_become_solid_sprites}, // Modding, visuals area
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " passed\n";
    return failures == 0U ? 0 : 1;
}
