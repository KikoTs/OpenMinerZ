// mod-world: the `terrain` and `noise` script libraries, the build of a mod
// world generator from its definitions, and the world's generator record
// (openminerz/modding/worldgen.hpp).
//
// Conventions a mod author sees:
//   * a noise expression is a number or a table built by noise.* (plain data;
//     it can be stored, passed around and reused)
//   * biomes are "<mod id>:<name>"; a bare name is the defining mod's own; the
//     retail terrain is "retail" (the whole retail generator) or
//     "retail:classic" ... "retail:descent" (one retail biome)
//   * blocks are retail names ("rock"), a mod's own short names, or mod keys
//   * heights and levels are world Y (the retail sea plane is at 1.5, the
//     bottom of the world at -64, the top at 63)

#include "openminerz/modding/worldgen.hpp"

#include "openminerz/modding/gameplay.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace openminerz::modding::worldgen
{
namespace
{

[[nodiscard]] ParamSpec param(std::string name, ValueType type, std::string doc,
                              bool optional = false)
{
    return ParamSpec{std::move(name), type, optional, std::move(doc)};
}

constexpr ValueType any_type = ValueType::any;
constexpr ValueType text_type = ValueType::string;
constexpr ValueType number_type = ValueType::number;
constexpr ValueType table_type = ValueType::map;
constexpr ValueType list_type = ValueType::array;
constexpr ValueType boolean_type = ValueType::boolean;
constexpr ValueType nothing_type = ValueType::nil;

[[nodiscard]] const Value &arg(std::span<const Value> args, std::size_t index) noexcept
{
    static const Value nil;
    return index < args.size() ? args[index] : nil;
}

[[nodiscard]] Value make_map(std::initializer_list<std::pair<std::string, Value>> fields)
{
    return Value{Map(fields.begin(), fields.end())};
}

[[nodiscard]] bool valid_short_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64U)
        return false;
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '.' || character == '-';
    });
}

[[nodiscard]] std::string qualify(std::string_view mod, std::string_view name)
{
    const auto colon = name.find(':');
    if (colon != std::string_view::npos)
    {
        if (!valid_short_name(name.substr(colon + 1U)))
            throw ScriptError{std::format("'{}' is not a valid name", name)};
        return std::string{name};
    }
    if (!valid_short_name(name))
        throw ScriptError{std::format(
            "'{}' is not a valid name (lower-case letters, digits, '_', '.', '-')", name)};
    return std::format("{}:{}", mod, name);
}

// A definition may come only from a server or core mod (see worldgen.hpp).
void require_world_mod(const CallContext &context, std::string_view function)
{
    if ((context.capabilities & (capability::gameplay | capability::network)) == 0U)
        throw ScriptError{std::format(
            "{} needs a server or core mod: a client mod can never change the terrain", function)};
}

[[nodiscard]] double number_field(const Value &table, std::string_view key, double fallback)
{
    const Value *found = table.field(key);
    if (found == nullptr || found->is_nil())
        return fallback;
    if (!found->is_number())
        throw ScriptError{std::format("'{}' must be a number", key)};
    const double value = found->as_number();
    if (!std::isfinite(value))
        throw ScriptError{std::format("'{}' must be finite", key)};
    return value;
}

[[nodiscard]] bool bool_field(const Value &table, std::string_view key, bool fallback)
{
    const Value *found = table.field(key);
    if (found == nullptr || found->is_nil())
        return fallback;
    if (found->type() != ValueType::boolean)
        throw ScriptError{std::format("'{}' must be true or false", key)};
    return found->as_bool();
}

[[nodiscard]] std::string string_field(const Value &table, std::string_view key,
                                       std::string_view fallback)
{
    const Value *found = table.field(key);
    if (found == nullptr || found->is_nil())
        return std::string{fallback};
    if (found->type() != ValueType::string)
        throw ScriptError{std::format("'{}' must be a string", key)};
    return found->as_string();
}

// Unknown fields are mistakes (a typo would otherwise silently do nothing).
void only_fields(const Value &table, std::string_view what,
                 std::initializer_list<std::string_view> known)
{
    if (table.type() != ValueType::map)
        return;
    for (const auto &[key, value] : table.as_map())
        if (std::find(known.begin(), known.end(), key) == known.end())
            throw ScriptError{std::format("{}: unknown field '{}'", what, key)};
}

// --- noise expressions as script values -------------------------------------------

constexpr std::string_view op_names[]{"const", "x",      "y",        "z",   "distance",
                                      "fbm",   "ridged", "billow",   "cellular", "add",
                                      "mul",   "sub",    "min",      "max", "abs",
                                      "clamp", "lerp",   "curve",    "select"};

[[nodiscard]] std::optional<world::NoiseOp> op_from(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < std::size(op_names); ++index)
        if (op_names[index] == name)
            return static_cast<world::NoiseOp>(index);
    return std::nullopt;
}

void check_expression(const Value &value, std::string_view what, int depth = 0)
{
    if (depth > 64)
        throw ScriptError{std::format("{}: the expression is nested too deeply", what)};
    if (value.is_number())
        return;
    if (value.type() != ValueType::map || value.field("op") == nullptr ||
        value.field("op")->type() != ValueType::string ||
        !op_from(value.field("op")->as_string()).has_value())
        throw ScriptError{std::format("{}: expected a number or a noise expression (noise.*)", what)};
    only_fields(value, what,
                {"op", "args", "value", "scale", "vertical_scale", "octaves", "gain", "lacunarity",
                 "seed", "three_d", "mode", "jitter", "low", "high", "points", "smooth"});
    if (const Value *inputs = value.field("args"); inputs != nullptr && !inputs->is_nil())
    {
        if (inputs->type() != ValueType::array)
            throw ScriptError{std::format("{}: 'args' must be a list", what)};
        for (const Value &input : inputs->as_array())
            check_expression(input, what, depth + 1);
    }
}

[[nodiscard]] Value expression(std::string_view op, Map fields, Array inputs = {})
{
    fields.insert(fields.begin(), {"op", Value{std::string{op}}});
    if (!inputs.empty())
        fields.emplace_back("args", Value{std::move(inputs)});
    return Value{std::move(fields)};
}

[[nodiscard]] Map noise_options(const Value &options, std::string_view what, bool cellular)
{
    if (!options.is_nil() && options.type() != ValueType::map)
        throw ScriptError{std::format("{}: options must be a table", what)};
    if (cellular)
        only_fields(options, what, {"scale", "seed", "jitter", "mode", "three_d", "vertical_scale"});
    else
        only_fields(options, what,
                    {"scale", "octaves", "gain", "lacunarity", "seed", "three_d", "vertical_scale"});
    Map fields;
    if (options.type() != ValueType::map)
        return fields;
    for (const auto &[key, value] : options.as_map())
    {
        if (key == "mode")
        {
            const std::string mode = value.as_string();
            if (mode != "distance" && mode != "edge" && mode != "cell")
                throw ScriptError{std::format("{}: mode is distance, edge or cell", what)};
        }
        else if (key == "three_d")
            static_cast<void>(value.as_bool());
        else if (!value.is_number() || !std::isfinite(value.as_number()))
            throw ScriptError{std::format("{}: '{}' must be a number", what, key)};
        fields.emplace_back(key, value);
    }
    const double scale = number_field(options, "scale", 64.0);
    if (!(scale >= 0.001) || scale > 1.0e7)
        throw ScriptError{std::format("{}: scale must be 0.001 .. 10,000,000 blocks", what)};
    return fields;
}

} // namespace

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------

class WorldgenService::Impl final
{
  public:
    Impl(ScriptHost &script_host, WorldgenBackend &world_backend)
        : host{script_host}, backend{world_backend}
    {
    }

    struct BiomeSource final
    {
        std::string key;
        std::string mod;
        Value definition;
    };

    struct SideState final
    {
        std::vector<BiomeSource> biomes;
        std::vector<BiomePresentation> presentation;
        std::optional<Value> world; // terrain.define's table
        std::string world_mod;
        std::string world_mod_version;
    };

    ScriptHost &host;
    WorldgenBackend &backend;
    std::array<SideState, 2> sides;
    std::shared_ptr<const world::CustomTerrain> active;

    [[nodiscard]] SideState &state(Side side) noexcept
    {
        return sides[side == Side::server ? 1U : 0U];
    }
    [[nodiscard]] const SideState &state(Side side) const noexcept
    {
        return sides[side == Side::server ? 1U : 0U];
    }

    void define_biome(const CallContext &context, std::string_view name, const Value &definition)
    {
        require_world_mod(context, "terrain.biome");
        if (definition.type() != ValueType::map)
            throw ScriptError{"terrain.biome: the definition must be a table"};
        const std::string key = qualify(context.mod_id, name);
        if (key.starts_with("retail:") || world::retail_biome_from_name(name).has_value())
            throw ScriptError{std::format("'{}' is a retail biome name", name)};
        only_fields(definition, "terrain.biome " + key,
                    {"name", "like", "retail", "height", "density", "overhang", "surface",
                     "underwater", "filler", "fluid", "caves", "ores", "trees", "atmosphere",
                     "music"});
        if (const Value *height = definition.field("height"); height != nullptr && !height->is_nil())
            check_expression(*height, key + " height");
        else if (definition.field("retail") == nullptr)
            throw ScriptError{std::format("biome '{}' needs a height", key)};
        if (const Value *density = definition.field("density"); density != nullptr && !density->is_nil())
            check_expression(*density, key + " density");
        const std::string like = string_field(definition, "like", "classic");
        if (!world::retail_biome_from_name(like).has_value())
            throw ScriptError{std::format(
                "biome '{}': like must be classic, lagoon, desert, mountain, arctic or descent", key)};
        SideState &side = state(context.side);
        const auto existing = std::ranges::find(side.biomes, key, &BiomeSource::key);
        if (existing != side.biomes.end())
            existing->definition = definition;
        else
            side.biomes.push_back({key, std::string{context.mod_id}, definition});
        static_cast<void>(world::biome_id(key));
        // Presentation for the client.
        BiomePresentation look;
        look.key = key;
        look.mod = std::string{context.mod_id};
        look.name = string_field(definition, "name", name);
        if (const Value *atmosphere = definition.field("atmosphere");
            atmosphere != nullptr && !atmosphere->is_nil())
        {
            if (atmosphere->type() != ValueType::map)
                throw ScriptError{std::format("biome '{}': atmosphere must be a table", key)};
            look.atmosphere = *atmosphere;
        }
        if (const Value *music = definition.field("music"); music != nullptr && !music->is_nil())
        {
            if (music->type() == ValueType::string)
                look.music = music->as_string();
            else if (music->type() == ValueType::map)
            {
                look.music = string_field(*music, "track", "");
                look.music_volume = std::clamp(number_field(*music, "volume", 1.0), 0.0, 1.0);
            }
            else
                throw ScriptError{std::format("biome '{}': music is a track or {{track, volume}}", key)};
        }
        const auto shown = std::ranges::find(side.presentation, key, &BiomePresentation::key);
        if (shown != side.presentation.end())
            *shown = std::move(look);
        else
            side.presentation.push_back(std::move(look));
    }

    void define_world(const CallContext &context, const Value &definition, std::string_view version)
    {
        require_world_mod(context, "terrain.define");
        if (definition.type() != ValueType::map)
            throw ScriptError{"terrain.define: the definition must be a table"};
        only_fields(definition, "terrain.define",
                    {"layout", "sea_level", "spawn", "origin_tower", "bedrock_depth"});
        SideState &side = state(context.side);
        if (side.world.has_value() && side.world_mod != context.mod_id)
            throw ScriptError{std::format("the world is already defined by mod '{}'", side.world_mod)};
        const Value *layout = definition.field("layout");
        if (layout == nullptr || layout->type() != ValueType::map)
            throw ScriptError{"terrain.define needs a layout table"};
        side.world = definition;
        side.world_mod = std::string{context.mod_id};
        side.world_mod_version = std::string{version};
    }

    // --- the build -----------------------------------------------------------------------

    struct Builder final
    {
        Impl &impl;
        Side side;
        world::TerrainDefinition definition;
        std::map<std::string, std::int32_t, std::less<>> biome_indices;

        [[nodiscard]] world::NoiseRef noise(const Value &value, std::string_view what)
        {
            if (value.is_nil())
                return world::no_noise;
            check_expression(value, what);
            return compile(value);
        }

        world::NoiseRef compile(const Value &value)
        {
            world::NoiseNode node;
            if (value.is_number())
            {
                node.op = world::NoiseOp::constant;
                node.value = value.as_number();
                return push(std::move(node));
            }
            node.op = *op_from(value.field("op")->as_string());
            if (const Value *inputs = value.field("args"); inputs != nullptr && !inputs->is_nil())
                for (const Value &input : inputs->as_array())
                    node.inputs.push_back(compile(input));
            node.value = number_field(value, "value", 0.0);
            node.scale = number_field(value, "scale", 64.0);
            node.vertical_scale = number_field(value, "vertical_scale", 0.0);
            node.octaves = static_cast<int>(number_field(value, "octaves", 4.0));
            node.gain = number_field(value, "gain", 0.5);
            node.lacunarity = number_field(value, "lacunarity", 2.0);
            node.jitter = std::clamp(number_field(value, "jitter", 1.0), 0.0, 1.0);
            node.three_d = bool_field(value, "three_d", false);
            node.low = number_field(value, "low", 0.0);
            node.high = number_field(value, "high", 1.0);
            node.smooth = bool_field(value, "smooth", false);
            const std::string mode = string_field(value, "mode", "distance");
            node.cellular = mode == "edge"   ? world::CellularMode::edge
                            : mode == "cell" ? world::CellularMode::cell
                                             : world::CellularMode::distance;
            // An unseeded field is seeded by what it is, so the same
            // expression is the same field wherever it appears (noise.sample
            // included) and editing another part of the world leaves it be.
            const double seed = number_field(value, "seed", -1.0);
            node.seed = seed >= 0.0 ? static_cast<std::uint32_t>(std::fmod(seed, 4294967296.0))
                                    : content_seed(node);
            if (const Value *points = value.field("points"); points != nullptr && !points->is_nil())
                for (const Value &point : points->as_array())
                {
                    const Array &pair = point.as_array();
                    if (pair.size() != 2U)
                        throw ScriptError{"curve points are {input, output} pairs"};
                    node.points.push_back({pair[0].as_number(), pair[1].as_number()});
                }
            return push(std::move(node));
        }

        world::NoiseRef push(world::NoiseNode node)
        {
            if (definition.nodes.size() >= 65536U)
                throw ScriptError{"the world has more than 65,536 noise nodes"};
            definition.nodes.push_back(std::move(node));
            return static_cast<world::NoiseRef>(definition.nodes.size() - 1U);
        }

        [[nodiscard]] world::TerrainBlock block(const Value &value, std::string_view mod,
                                                std::string_view what)
        {
            if (value.type() != ValueType::string)
                throw ScriptError{std::format("{}: a block is a name", what)};
            const std::string &name = value.as_string();
            if (name == "air" || name == "empty")
                return {world::BlockType::empty, 0U};
            if (const auto retail = gameplay::retail_block_from_name(name))
                return {static_cast<world::BlockType>(*retail), 0U};
            const std::string key = qualify(mod, name);
            const auto fallback = impl.backend.block_fallback(side, key);
            if (!fallback.has_value())
                throw ScriptError{std::format(
                    "{}: unknown block '{}' (a retail name or a blocks.define key)", what, name)};
            const auto found = std::ranges::find(definition.palette, key);
            std::size_t index = static_cast<std::size_t>(found - definition.palette.begin());
            if (found == definition.palette.end())
            {
                if (definition.palette.size() >= 255U)
                    throw ScriptError{"a world uses at most 255 mod blocks"};
                definition.palette.push_back(key);
                definition.palette_fallback.push_back(static_cast<world::BlockType>(*fallback));
                index = definition.palette.size() - 1U;
            }
            return {static_cast<world::BlockType>(*fallback), static_cast<std::uint8_t>(index + 1U)};
        }

        [[nodiscard]] std::vector<world::TerrainLayer> layers(const Value *value, std::string_view mod,
                                                               std::string_view what)
        {
            std::vector<world::TerrainLayer> result;
            if (value == nullptr || value->is_nil())
                return result;
            if (value->type() == ValueType::string)
            {
                result.push_back({block(*value, mod, what), 1});
                return result;
            }
            for (const Value &entry : value->as_array())
            {
                if (entry.type() == ValueType::string)
                {
                    result.push_back({block(entry, mod, what), 1});
                    continue;
                }
                only_fields(entry, what, {"block", "depth"});
                const Value *named = entry.field("block");
                if (named == nullptr)
                    throw ScriptError{std::format("{}: each layer needs a block", what)};
                result.push_back({block(*named, mod, what),
                                  static_cast<int>(std::clamp(number_field(entry, "depth", 1.0), 1.0, 128.0))});
            }
            if (result.size() > 16U)
                throw ScriptError{std::format("{}: at most 16 layers", what)};
            return result;
        }

        [[nodiscard]] static std::array<int, 2> range(const Value &table, std::string_view key,
                                                      std::array<int, 2> fallback)
        {
            const Value *found = table.field(key);
            if (found == nullptr || found->is_nil())
                return fallback;
            if (found->is_number())
            {
                const int value = static_cast<int>(found->as_number());
                return {value, value};
            }
            const Array &pair = found->as_array();
            if (pair.size() != 2U)
                throw ScriptError{std::format("'{}' is a number or {{min, max}}", key)};
            return {static_cast<int>(pair[0].as_number()), static_cast<int>(pair[1].as_number())};
        }

        [[nodiscard]] std::int32_t biome(std::string_view name, std::string_view mod)
        {
            std::string key;
            world::RetailReuse reuse = world::RetailReuse::none;
            if (name == "retail" || name == "retail:world")
            {
                key = "retail:world";
                reuse = world::RetailReuse::world;
            }
            else if (name.starts_with("retail:"))
            {
                const auto kind = world::retail_biome_from_name(name.substr(7U));
                if (!kind.has_value())
                    throw ScriptError{std::format("unknown retail biome '{}'", name)};
                key = std::string{name};
                reuse = static_cast<world::RetailReuse>(static_cast<int>(*kind) + 2);
            }
            else
                key = qualify(mod, name);
            if (const auto found = biome_indices.find(key); found != biome_indices.end())
                return found->second;
            world::TerrainBiome compiled;
            compiled.key = key;
            if (reuse != world::RetailReuse::none)
            {
                compiled.retail = reuse;
                compiled.name = key;
                compiled.like = reuse == world::RetailReuse::world
                                    ? world::BiomeKind::classic
                                    : static_cast<world::BiomeKind>(static_cast<int>(reuse) - 2);
            }
            else
            {
                const SideState &state = impl.state(side);
                const auto source = std::ranges::find(state.biomes, key, &BiomeSource::key);
                if (source == state.biomes.end())
                    throw ScriptError{std::format("the layout names biome '{}', which no terrain.biome defines", key)};
                compile_biome(compiled, *source);
            }
            if (definition.biomes.size() >= 4096U)
                throw ScriptError{"a world has at most 4,096 biomes"};
            definition.biomes.push_back(std::move(compiled));
            const auto index = static_cast<std::int32_t>(definition.biomes.size() - 1U);
            biome_indices.emplace(key, index);
            return index;
        }

        void compile_biome(world::TerrainBiome &biome, const BiomeSource &source)
        {
            const Value &def = source.definition;
            const std::string &mod = source.mod;
            const std::string &key = source.key;
            biome.name = string_field(def, "name", key);
            biome.like = *world::retail_biome_from_name(string_field(def, "like", "classic"));
            const std::string retail = string_field(def, "retail", "");
            if (!retail.empty())
            {
                if (retail == "world")
                    biome.retail = world::RetailReuse::world;
                else if (const auto kind = world::retail_biome_from_name(retail))
                    biome.retail = static_cast<world::RetailReuse>(static_cast<int>(*kind) + 2);
                else
                    throw ScriptError{std::format("biome '{}': retail is world or a retail biome", key)};
            }
            biome.height = noise(def.field_or("height", Value{}), key + " height");
            biome.density = noise(def.field_or("density", Value{}), key + " density");
            biome.density_strength = std::clamp(number_field(def, "overhang", biome.density != world::no_noise ? 8.0 : 0.0), 0.0, 64.0);
            biome.surface = layers(def.field("surface"), mod, key + " surface");
            if (biome.surface.empty() && biome.retail == world::RetailReuse::none)
                biome.surface = {{{world::BlockType::grass}, 1}, {{world::BlockType::dirt}, 3}};
            biome.underwater = layers(def.field("underwater"), mod, key + " underwater");
            if (const Value *filler = def.field("filler"); filler != nullptr && !filler->is_nil())
                biome.filler = block(*filler, mod, key + " filler");
            if (const Value *fluid = def.field("fluid"); fluid != nullptr && !fluid->is_nil())
                biome.fluid = block(*fluid, mod, key + " fluid");
            // Caves: on with defaults, false for none, or a table.
            biome.caves.seed = mix_seed(key, 1U);
            if (const Value *caves = def.field("caves"); caves != nullptr && !caves->is_nil())
            {
                if (caves->type() == ValueType::boolean)
                    biome.caves.enabled = caves->as_bool();
                else
                {
                    only_fields(*caves, key + " caves",
                                {"tunnels", "scale", "threshold", "width", "y", "breach_surface", "seed"});
                    biome.caves.tunnels = bool_field(*caves, "tunnels", true);
                    biome.caves.scale = number_field(*caves, "scale", 48.0);
                    biome.caves.threshold = number_field(*caves, "threshold", -0.55);
                    biome.caves.width = number_field(*caves, "width", 0.06);
                    const auto y = range(*caves, "y", {-58, 40});
                    biome.caves.y_min = y[0];
                    biome.caves.y_max = y[1];
                    biome.caves.breach_surface = bool_field(*caves, "breach_surface", true);
                    if (caves->field("seed") != nullptr)
                        biome.caves.seed = static_cast<std::uint32_t>(number_field(*caves, "seed", 0.0));
                }
            }
            if (const Value *ores = def.field("ores"); ores != nullptr && !ores->is_nil())
            {
                std::uint32_t index{};
                for (const Value &entry : ores->as_array())
                {
                    only_fields(entry, key + " ore", {"block", "inside", "chance", "size", "spacing", "y", "seed"});
                    world::TerrainOre ore;
                    const Value *named = entry.field("block");
                    if (named == nullptr)
                        throw ScriptError{std::format("{} ore: needs a block", key)};
                    ore.block = block(*named, mod, key + " ore");
                    if (const Value *in = entry.field("inside"); in != nullptr && !in->is_nil())
                    {
                        if (in->type() == ValueType::string)
                            ore.in.push_back(block(*in, mod, key + " ore in"));
                        else
                            for (const Value &host_block : in->as_array())
                                ore.in.push_back(block(host_block, mod, key + " ore in"));
                    }
                    ore.chance = std::clamp(number_field(entry, "chance", 0.1), 0.0, 1.0);
                    ore.size = static_cast<int>(number_field(entry, "size", 2.0));
                    ore.spacing = static_cast<int>(number_field(entry, "spacing", 12.0));
                    const auto y = range(entry, "y", {-62, 40});
                    ore.y_min = y[0];
                    ore.y_max = y[1];
                    ore.seed = entry.field("seed") != nullptr
                                   ? static_cast<std::uint32_t>(number_field(entry, "seed", 0.0))
                                   : mix_seed(key, 100U + index);
                    biome.ores.push_back(std::move(ore));
                    ++index;
                }
            }
            if (const Value *trees = def.field("trees"); trees != nullptr && !trees->is_nil())
            {
                std::uint32_t index{};
                for (const Value &entry : trees->as_array())
                {
                    only_fields(entry, key + " tree",
                                {"shape", "chance", "height", "radius", "thick", "trunk", "leaves", "on"});
                    world::TerrainTree tree;
                    const std::string shape = string_field(entry, "shape", "retail");
                    static constexpr std::array<std::string_view, 6> shapes{"retail", "round", "cone",
                                                                            "column", "dead", "boulder"};
                    const auto found = std::ranges::find(shapes, shape);
                    if (found == shapes.end())
                        throw ScriptError{std::format(
                            "{} tree: shape is retail, round, cone, column, dead or boulder", key)};
                    tree.shape = static_cast<world::TreeShape>(found - shapes.begin());
                    tree.chance = std::clamp(number_field(entry, "chance", 0.01), 0.0, 1.0);
                    const auto height = range(entry, "height", {5, 8});
                    tree.height_min = height[0];
                    tree.height_max = height[1];
                    tree.radius = static_cast<int>(number_field(entry, "radius", 3.0));
                    tree.thick = bool_field(entry, "thick", false);
                    if (const Value *trunk = entry.field("trunk"); trunk != nullptr && !trunk->is_nil())
                        tree.trunk = block(*trunk, mod, key + " tree trunk");
                    else if (tree.shape == world::TreeShape::boulder)
                        tree.trunk = {world::BlockType::rock, 0U};
                    if (const Value *leaves = entry.field("leaves"); leaves != nullptr && !leaves->is_nil())
                        tree.leaves = block(*leaves, mod, key + " tree leaves");
                    if (const Value *on = entry.field("on"); on != nullptr && !on->is_nil())
                    {
                        if (on->type() == ValueType::string)
                            tree.on.push_back(block(*on, mod, key + " tree ground"));
                        else
                            for (const Value &ground : on->as_array())
                                tree.on.push_back(block(ground, mod, key + " tree ground"));
                    }
                    tree.seed = mix_seed(key, 200U + index);
                    biome.trees.push_back(std::move(tree));
                    ++index;
                }
            }
        }

        [[nodiscard]] static std::uint32_t content_seed(const world::NoiseNode &node) noexcept
        {
            const std::string text = std::format(
                "{} {} {} {} {} {} {} {} {}", static_cast<int>(node.op), node.scale,
                node.vertical_scale, node.octaves, node.gain, node.lacunarity, node.three_d,
                static_cast<int>(node.cellular), node.jitter);
            return mix_seed(text, 0x5EEDU) | 0x80000000U;
        }

        [[nodiscard]] static std::uint32_t mix_seed(std::string_view key, std::uint32_t salt) noexcept
        {
            std::uint32_t hash = 2166136261U ^ salt;
            for (const char character : key)
            {
                hash ^= static_cast<std::uint8_t>(character);
                hash *= 16777619U;
            }
            return hash;
        }

        std::int32_t layout(const Value &value, std::string_view mod, int depth)
        {
            if (depth > 6)
                throw ScriptError{"layouts nest at most six deep"};
            if (value.type() != ValueType::map)
                throw ScriptError{"a layout is a table"};
            only_fields(value, "layout", {"by", "warp", "beyond", "period", "bands"});
            const auto index = static_cast<std::int32_t>(definition.layouts.size());
            definition.layouts.emplace_back();
            world::TerrainLayout compiled;
            if (const Value *by = value.field("by"); by != nullptr && !by->is_nil() &&
                                                    !(by->type() == ValueType::string &&
                                                      by->as_string() == "distance"))
            {
                compiled.input = world::LayoutInput::noise;
                compiled.select = noise(*by, "layout by");
            }
            compiled.warp = noise(value.field_or("warp", Value{}), "layout warp");
            const std::string beyond = string_field(value, "beyond", "mirror");
            compiled.beyond = beyond == "repeat"   ? world::LayoutBeyond::repeat
                              : beyond == "extend" ? world::LayoutBeyond::extend
                                                   : world::LayoutBeyond::mirror;
            if (beyond != "repeat" && beyond != "extend" && beyond != "mirror")
                throw ScriptError{"layout beyond is mirror, repeat or extend"};
            compiled.period = std::max(0.0, number_field(value, "period", 0.0));
            const Value *bands = value.field("bands");
            if (bands == nullptr || bands->type() != ValueType::array || bands->as_array().empty())
                throw ScriptError{"a layout needs a list of bands"};
            if (bands->as_array().size() > 256U)
                throw ScriptError{"a layout has at most 256 bands"};
            double previous = -std::numeric_limits<double>::infinity();
            const Array &entries = bands->as_array();
            for (std::size_t band = 0U; band < entries.size(); ++band)
            {
                const Value &entry = entries[band];
                only_fields(entry, "layout band", {"to", "blend", "biome", "layout"});
                world::LayoutBand compiled_band;
                const bool last = band + 1U == entries.size();
                const Value *to = entry.field("to");
                if ((to == nullptr || to->is_nil()) && !last)
                    throw ScriptError{"every band but the last needs 'to'"};
                compiled_band.to = to != nullptr && !to->is_nil() ? to->as_number() : 0.0;
                if (!last && !(compiled_band.to > previous))
                    throw ScriptError{"band 'to' values must increase"};
                previous = compiled_band.to;
                compiled_band.blend = std::max(0.0, number_field(entry, "blend", 0.0));
                const Value *biome_name = entry.field("biome");
                const Value *nested = entry.field("layout");
                if ((biome_name == nullptr) == (nested == nullptr))
                    throw ScriptError{"each band names a biome or a nested layout"};
                if (biome_name != nullptr)
                    compiled_band.biome = biome(biome_name->as_string(), mod);
                else
                    compiled_band.layout = layout(*nested, mod, depth + 1);
                compiled.bands.push_back(compiled_band);
            }
            definition.layouts[static_cast<std::size_t>(index)] = std::move(compiled);
            return index;
        }
    };

    [[nodiscard]] std::shared_ptr<const world::CustomTerrain> build(Side side, std::string &error)
    {
        error.clear();
        const SideState &source = state(side);
        if (!source.world.has_value())
            return nullptr;
        try
        {
            Builder builder{*this, side, {}, {}};
            const Value &world = *source.world;
            builder.definition.mod_id = source.world_mod;
            builder.definition.mod_version = source.world_mod_version;
            builder.layout(*world.field("layout"), source.world_mod, 0);
            builder.definition.sea_level = static_cast<int>(number_field(world, "sea_level", 1.0));
            if (const Value *spawn = world.field("spawn"); spawn != nullptr && !spawn->is_nil())
            {
                builder.definition.spawn_x = static_cast<int>(std::floor(number_field(*spawn, "x", 8.0)));
                builder.definition.spawn_z = static_cast<int>(std::floor(number_field(*spawn, "z", -8.0)));
                if (std::abs(builder.definition.spawn_x) > 1'000'000 ||
                    std::abs(builder.definition.spawn_z) > 1'000'000)
                    throw ScriptError{"the spawn must be within 1,000,000 blocks of the origin"};
            }
            builder.definition.origin_tower = bool_field(world, "origin_tower", true);
            builder.definition.bedrock_depth =
                static_cast<int>(number_field(world, "bedrock_depth", 3.0));
            return std::make_shared<const world::CustomTerrain>(std::move(builder.definition));
        }
        catch (const ScriptError &failure)
        {
            error = std::format("{}: world generator: {}", source.world_mod, failure.what());
        }
        catch (const std::invalid_argument &failure)
        {
            error = std::format("{}: {}", source.world_mod, failure.what());
        }
        return nullptr;
    }

    [[nodiscard]] const world::CustomTerrain &require_active() const
    {
        if (active == nullptr)
            throw ScriptError{"no mod world generator runs in this session"};
        return *active;
    }
};

WorldgenService::WorldgenService(ScriptHost &host, WorldgenBackend &backend)
    : impl_(std::make_unique<Impl>(host, backend))
{
    // A side's definitions go with its VM (the runtime may outlive us).
    host.add_teardown_listener([impl = impl_.get(), alive = std::weak_ptr<bool>{alive_}](Side side) {
        if (!alive.expired())
            impl->state(side) = {};
    });
}

WorldgenService::~WorldgenService() = default;

std::shared_ptr<const world::CustomTerrain> WorldgenService::build(Side side, std::string &error)
{
    return impl_->build(side, error);
}

bool WorldgenService::defines_world(Side side) const noexcept
{
    return impl_->state(side).world.has_value();
}

std::pair<std::string, std::string> WorldgenService::world_mod(Side side) const
{
    const Impl::SideState &state = impl_->state(side);
    return {state.world_mod, state.world_mod_version};
}

const std::vector<BiomePresentation> &WorldgenService::biomes(Side side) const noexcept
{
    return impl_->state(side).presentation;
}

void WorldgenService::set_active(std::shared_ptr<const world::CustomTerrain> terrain) noexcept
{
    impl_->active = std::move(terrain);
}

const std::shared_ptr<const world::CustomTerrain> &WorldgenService::active() const noexcept
{
    return impl_->active;
}

void WorldgenService::clear(Side side)
{
    impl_->state(side) = {};
}

// ---------------------------------------------------------------------------
// The script libraries
// ---------------------------------------------------------------------------

void WorldgenService::register_api(ApiRegistry &registry)
{
    Impl &impl = *impl_;
    const auto pure = [&registry](std::string name, std::string doc, std::vector<ParamSpec> params,
                                  ValueType returns, std::string returns_doc, bool variadic,
                                  NativeFunction function) {
        registry.add_function(FunctionSpec{std::move(name), std::move(doc), std::move(params),
                                           returns, std::move(returns_doc), side_both,
                                           capability::none, variadic},
                              std::move(function));
    };
    const auto query = [&registry](std::string name, std::string doc, std::vector<ParamSpec> params,
                                   ValueType returns, std::string returns_doc,
                                   NativeFunction function) {
        registry.add_function(FunctionSpec{std::move(name), std::move(doc), std::move(params),
                                           returns, std::move(returns_doc), side_both,
                                           capability::query, false},
                              std::move(function));
    };

    // --- noise --------------------------------------------------------------------
    const auto octave_doc = std::string{
        "Options: scale (blocks per feature, default 64), octaves (1-10, default 4), gain "
        "(default 0.5), lacunarity (default 2), seed, three_d (true: varies with Y too), "
        "vertical_scale (3D, blocks per feature along Y)."};
    for (const auto &[name, what] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"fbm", "Smooth fractal noise in -1..1: hills, plains, climate."},
             {"ridged", "Ridged noise in 0..1: sharp crests where fbm crosses zero (mountain ridges)."},
             {"billow", "Billowy noise in -1..1: rounded bumps and sharp creases (dunes, clouds)."}})
    {
        pure(std::format("noise.{}", name), std::format("{} {}", what, octave_doc),
             {param("options", table_type, "See above.", true)}, table_type, "A noise expression.", false,
             [op = std::string{name}](CallContext &, std::span<const Value> args) -> Value {
                 return expression(op, noise_options(arg(args, 0), "noise." + op, false));
             });
    }
    pure("noise.cellular",
         "Cellular (Worley) noise. Options: scale, seed, jitter (0..1), three_d, mode: \"distance\" "
         "(to the nearest point, about 0..1), \"edge\" (0 on cell borders: cracks, plates), \"cell\" "
         "(a random 0..1 per cell: patches).",
         {param("options", table_type, "See above.", true)}, table_type, "A noise expression.", false,
         [](CallContext &, std::span<const Value> args) -> Value {
             return expression("cellular", noise_options(arg(args, 0), "noise.cellular", true));
         });
    for (const auto &[name, what] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"add", "The sum of its arguments (numbers or expressions)."},
             {"mul", "The product of its arguments."},
             {"min", "The smallest of its arguments."},
             {"max", "The largest of its arguments."}})
    {
        pure(std::format("noise.{}", name), std::string{what},
             {param("a", any_type, "A number or an expression."), param("b", any_type, "More of them.", true)},
             table_type, "A noise expression.", true,
             [op = std::string{name}](CallContext &, std::span<const Value> args) -> Value {
                 Array inputs;
                 for (const Value &input : args)
                 {
                     check_expression(input, "noise." + op);
                     inputs.push_back(input);
                 }
                 if (inputs.empty())
                     throw ScriptError{std::format("noise.{} needs an argument", op)};
                 return expression(op, {}, std::move(inputs));
             });
    }
    pure("noise.sub", "a - b.", {param("a", any_type, "A number or an expression."), param("b", any_type, "Subtracted.")},
         table_type, "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             check_expression(arg(args, 0), "noise.sub");
             check_expression(arg(args, 1), "noise.sub");
             return expression("sub", {}, Array{arg(args, 0), arg(args, 1)});
         });
    pure("noise.abs", "The absolute value.", {param("a", any_type, "A number or an expression.")}, table_type,
         "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             check_expression(arg(args, 0), "noise.abs");
             return expression("abs", {}, Array{arg(args, 0)});
         });
    pure("noise.clamp", "Limits a value to low..high.",
         {param("a", any_type, "A number or an expression."), param("low", number_type, "Lowest."),
          param("high", number_type, "Highest.")},
         table_type, "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             check_expression(arg(args, 0), "noise.clamp");
             return expression("clamp", {{"low", arg(args, 1)}, {"high", arg(args, 2)}}, Array{arg(args, 0)});
         });
    pure("noise.lerp", "a + (b - a) * t: blends two expressions.",
         {param("a", any_type, "At t = 0."), param("b", any_type, "At t = 1."), param("t", any_type, "The blend.")},
         table_type, "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             for (std::size_t index = 0U; index < 3U; ++index)
                 check_expression(arg(args, index), "noise.lerp");
             return expression("lerp", {}, Array{arg(args, 0), arg(args, 1), arg(args, 2)});
         });
    pure("noise.curve",
         "Maps a value through points {{input, output}, ...} (increasing inputs; flat outside "
         "them), straight between points or, with smooth = true, a smooth spline: terraces, "
         "plateaus, cliffs.",
         {param("a", any_type, "A number or an expression."), param("points", list_type, "{{in, out}, ...}"),
          param("smooth", boolean_type, "A spline instead of straight segments.", true)},
         table_type, "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             check_expression(arg(args, 0), "noise.curve");
             const Value &points = arg(args, 1);
             if (points.type() != ValueType::array || points.as_array().size() < 2U ||
                 points.as_array().size() > 64U)
                 throw ScriptError{"noise.curve needs 2 to 64 points"};
             for (const Value &point : points.as_array())
                 if (point.type() != ValueType::array || point.as_array().size() != 2U ||
                     !point.as_array()[0].is_number() || !point.as_array()[1].is_number())
                     throw ScriptError{"noise.curve points are {input, output} pairs"};
             Map fields{{"points", points}};
             if (arg(args, 2).type() == ValueType::boolean && arg(args, 2).as_bool())
                 fields.emplace_back("smooth", Value{true});
             return expression("curve", std::move(fields), Array{arg(args, 0)});
         });
    pure("noise.select",
         "Chooses between two expressions by a selector: `low` where the selector is under "
         "`from`, `high` above `to`, blended between.",
         {param("selector", any_type, "A number or an expression."), param("low", any_type, "Below `from`."),
          param("high", any_type, "Above `to`."), param("from", number_type, "Where the blend starts."),
          param("to", number_type, "Where it ends (greater than from).")},
         table_type, "A noise expression.", false, [](CallContext &, std::span<const Value> args) -> Value {
             for (std::size_t index = 0U; index < 3U; ++index)
                 check_expression(arg(args, index), "noise.select");
             if (!(arg(args, 4).as_number() > arg(args, 3).as_number()))
                 throw ScriptError{"noise.select needs to > from"};
             return expression("select", {{"low", arg(args, 3)}, {"high", arg(args, 4)}},
                               Array{arg(args, 0), arg(args, 1), arg(args, 2)});
         });
    for (const auto &[name, what] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"x", "World X of the point."},
             {"y", "World Y of the point (0 in a 2D expression such as a height)."},
             {"z", "World Z of the point."},
             {"distance", "Distance from the origin (sqrt(x^2 + z^2))."}})
    {
        pure(std::format("noise.{}", name), std::string{what}, {}, table_type, "A noise expression.",
             false, [op = std::string{name}](CallContext &, std::span<const Value>) -> Value {
                 return expression(op, {});
             });
    }
    query("noise.sample",
          "Evaluates an expression at a point with this world's seed, natively: the value the "
          "generator sees there.",
          {param("expression", any_type, "A number or an expression."), param("x", number_type, "World X."),
           param("z", number_type, "World Z."), param("y", number_type, "World Y (3D expressions).", true)},
          number_type, "The value.", [&impl](CallContext &, std::span<const Value> args) -> Value {
              check_expression(arg(args, 0), "noise.sample");
              Impl::Builder builder{impl, Side::client, {}, {}};
              const world::NoiseRef root = builder.compile(arg(args, 0));
              // A one-biome stand-in makes the expression evaluable.
              builder.definition.biomes.push_back({});
              builder.definition.biomes.back().height = root;
              builder.definition.layouts.push_back({});
              builder.definition.layouts.back().bands.push_back({0.0, 0.0, 0, -1});
              const world::CustomTerrain terrain{std::move(builder.definition)};
              const double y = arg(args, 3).is_number() ? arg(args, 3).as_number() : 0.0;
              return terrain.sample(root, impl.backend.world_seed(), arg(args, 1).as_number(), y,
                                    arg(args, 2).as_number());
          });

    // --- terrain ------------------------------------------------------------------
    registry.add_function(
        FunctionSpec{"terrain.biome",
                     "Defines a biome for a world generator (server and core mods, in shared/). "
                     "Fields: name, like (the retail biome the engine treats it as: sky, water; "
                     "default classic), height (expression: world Y of the ground), density "
                     "(3D expression) with overhang (blocks it may move the ground: arches, "
                     "overhangs, floating islands), surface ({{block, depth}, ...} top down; "
                     "default grass over 3 dirt), underwater (layers where the ground is below "
                     "the sea), filler (default rock), fluid (fills open cells under the sea "
                     "level: ice, surface_lava), caves (false, or {tunnels, scale, threshold, "
                     "width, y = {min, max}, breach_surface}), ores ({{block, inside, chance, size, "
                     "spacing, y}}), trees ({{shape = retail|round|cone|column|dead|boulder, "
                     "chance, height = {min, max}, radius, thick, trunk, leaves, on}}), "
                     "retail (\"world\" or a retail biome: reuse retail terrain), and for "
                     "clients atmosphere (atmosphere.push values) and music (a track or "
                     "{track, volume}).",
                     {param("name", text_type, "Short name or \"mod:name\"."),
                      param("definition", table_type, "The biome.")},
                     text_type, "The biome key.", side_both, capability::none, false},
        [&impl](CallContext &context, std::span<const Value> args) -> Value {
            const std::string name = arg(args, 0).as_string();
            impl.define_biome(context, name, arg(args, 1));
            return qualify(context.mod_id, name);
        });
    registry.add_function(
        FunctionSpec{"terrain.define",
                     "Replaces the world generator (one mod per world; server and core mods, in "
                     "shared/). Fields: layout = {by = \"distance\" or an expression, warp "
                     "(expression added to the distance: ragged band edges), beyond = mirror | "
                     "repeat | extend (past the last band; default mirror like retail's fold), "
                     "period (default the last band's `to`), bands = {{to, biome | layout, "
                     "blend}, ...}}, sea_level (world Y, default 1), spawn = {x, z}, "
                     "origin_tower (default true), bedrock_depth (0-8, default 3). Biomes are "
                     "terrain.biome names, \"retail\" (the whole retail generator) or "
                     "\"retail:classic\" ... \"retail:descent\". Saved worlds remember the "
                     "generator: a world made without it keeps retail terrain.",
                     {param("definition", table_type, "The world.")}, nothing_type, "", side_both,
                     capability::none, false},
        [&impl](CallContext &context, std::span<const Value> args) -> Value {
            impl.define_world(context, arg(args, 0), "");
            return {};
        });
    query("terrain.biomes", "Every biome the session's mods defined (keys).", {}, list_type, "Keys.",
          [&impl](CallContext &context, std::span<const Value>) -> Value {
              Array keys;
              for (const Impl::BiomeSource &source : impl.state(context.side).biomes)
                  keys.push_back(Value{source.key});
              return Value{std::move(keys)};
          });
    query("terrain.active", "Whether a mod world generator shapes this session's terrain.", {},
          boolean_type, "", [&impl](CallContext &, std::span<const Value>) -> Value {
              return impl.active != nullptr;
          });
    query("terrain.info",
          "What the generator makes of a column, without loading it: biome (key), height "
          "(world Y of the ground before caves), like (retail biome) and weights "
          "({{biome, weight}} where biomes blend). Nil when retail terrain runs.",
          {param("x", number_type, "World X."), param("z", number_type, "World Z.")}, table_type,
          "{biome, height, like, weights} or nil.",
          [&impl](CallContext &, std::span<const Value> args) -> Value {
              const world::WorldGenerator *generator = impl.backend.generator();
              if (impl.active == nullptr || generator == nullptr)
                  return {};
              const int x = static_cast<int>(std::floor(arg(args, 0).as_number()));
              const int z = static_cast<int>(std::floor(arg(args, 1).as_number()));
              std::array<world::BiomeWeight, world::maximum_biome_weights> weights{};
              const std::size_t count = impl.active->biome_weights(*generator, x, z, weights);
              const auto &biomes = impl.active->definition().biomes;
              Array listed;
              for (std::size_t index = 0U; index < count; ++index)
                  listed.push_back(Value{Array{Value{biomes[weights[index].biome].key},
                                               Value{static_cast<double>(weights[index].weight)}}});
              return make_map({{"biome", Value{biomes[weights[0].biome].key}},
                               {"height", impl.active->surface_height(*generator, x, z)},
                               {"like", Value{std::string{world::retail_biome_name(
                                            impl.active->retail_kind_at(*generator, x, z))}}},
                               {"weights", Value{std::move(listed)}}});
          });
}

std::unique_ptr<WorldgenService> register_worldgen_api(ApiRegistry &registry, ScriptHost &host,
                                                       WorldgenBackend &backend)
{
    auto service = std::make_unique<WorldgenService>(host, backend);
    service->register_api(registry);
    return service;
}

// ---------------------------------------------------------------------------
// The generator record
// ---------------------------------------------------------------------------

std::optional<GeneratorRecord> read_generator_record(const std::filesystem::path &sidecar_directory,
                                                     std::string *error)
{
    const std::filesystem::path path = sidecar_directory / generator_record_file;
    std::error_code exists_error;
    if (sidecar_directory.empty() || !std::filesystem::exists(path, exists_error))
        return std::nullopt;
    std::ifstream input{path, std::ios::binary};
    GeneratorRecord record;
    std::string line;
    bool header{};
    while (input && std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "omz-worldgen 1")
        {
            header = true;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0U, equals);
        const std::string value = line.substr(equals + 1U);
        if (key == "mod")
            record.mod_id = value;
        else if (key == "version")
            record.mod_version = value;
        else if (key == "digest")
            record.digest = value;
    }
    if (!header || record.mod_id.empty() || record.digest.empty())
    {
        if (error != nullptr)
            *error = std::format("{} cannot be read", path.string());
        record.mod_id = "?";
    }
    return record;
}

bool write_generator_record(const std::filesystem::path &sidecar_directory,
                            const GeneratorRecord &record, std::string *error)
{
    std::error_code make_error;
    std::filesystem::create_directories(sidecar_directory, make_error);
    const std::filesystem::path path = sidecar_directory / generator_record_file;
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output << "omz-worldgen 1\nmod=" << record.mod_id << "\nversion=" << record.mod_version
               << "\ndigest=" << record.digest << '\n';
        if (!output)
        {
            if (error != nullptr)
                *error = std::format("{} cannot be written", temporary.string());
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error)
    {
        if (error != nullptr)
            *error = rename_error.message();
        return false;
    }
    return true;
}

GeneratorVerdict judge_generator(const std::optional<GeneratorRecord> &recorded,
                                 const std::optional<GeneratorRecord> &current,
                                 bool new_world) noexcept
{
    if (!recorded.has_value())
    {
        if (!current.has_value())
            return GeneratorVerdict::retail;
        return new_world ? GeneratorVerdict::use : GeneratorVerdict::keep_retail;
    }
    if (!current.has_value() || current->mod_id != recorded->mod_id)
        return GeneratorVerdict::missing;
    return current->digest == recorded->digest ? GeneratorVerdict::use : GeneratorVerdict::changed;
}

std::string generator_message(GeneratorVerdict verdict, const std::optional<GeneratorRecord> &recorded,
                              const std::optional<GeneratorRecord> &current)
{
    switch (verdict)
    {
    case GeneratorVerdict::missing:
        return std::format(
            "This world's terrain comes from the mod '{}'{}. Install and enable it to open the world.",
            recorded.has_value() ? recorded->mod_id : std::string{"?"},
            recorded.has_value() && !recorded->mod_version.empty()
                ? std::format(" ({})", recorded->mod_version)
                : std::string{});
    case GeneratorVerdict::changed: {
        const std::string before = recorded.has_value() ? recorded->mod_version : std::string{};
        const std::string now = current.has_value() ? current->mod_version : std::string{};
        return std::format(
            "The mod '{}' generates terrain differently since this world was made{}. Places you "
            "have not changed may look different from before.",
            current.has_value() ? current->mod_id : std::string{},
            before != now ? std::format(" ({} -> {})", before, now) : std::string{});
    }
    case GeneratorVerdict::keep_retail:
        return std::format("This world was made with retail terrain: the mod '{}' shapes new worlds only.",
                           current.has_value() ? current->mod_id : std::string{});
    case GeneratorVerdict::retail:
    case GeneratorVerdict::use:
        break;
    }
    return {};
}

} // namespace openminerz::modding::worldgen
