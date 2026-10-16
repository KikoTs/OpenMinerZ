// The gameplay sidecar: <world>/mods/gameplay.omzg.
//
// It holds what the retail save cannot: which cells are mod blocks, which
// chunks the world-generation pass has decorated, every player's mod stats,
// quests and mod item identities (by gamer tag), persistent mod entities and
// the difficulty scaling. Identities are stored as keys ("mod:name"), never as
// session ids, so a session with a different set of mods loads what it knows
// and drops the rest; the world file itself only ever holds retail blocks and
// items, which is why removing a mod leaves a world retail can still open.
//
// Encoding: "OMZG", a little-endian u32 format version, then one Value in a
// small tagged binary form (encode_value). Writes are atomic.

#include "internal.hpp"

#include "openminerz/atomic_file.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr std::uint32_t sidecar_version = 1U;
constexpr std::size_t maximum_sidecar_bytes = 256U * 1024U * 1024U;
constexpr int maximum_depth = 32;

enum class Tag : std::uint8_t
{
    nil = 0,
    boolean_false = 1,
    boolean_true = 2,
    integer = 3,
    number = 4,
    string = 5,
    vec3 = 6,
    handle = 7,
    array = 8,
    map = 9
};

void put_u32(std::string &out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

void put_u64(std::string &out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

void put_double(std::string &out, double value)
{
    put_u64(out, std::bit_cast<std::uint64_t>(value));
}

void put_string(std::string &out, std::string_view value)
{
    put_u32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

void encode(std::string &out, const Value &value, int depth)
{
    if (depth > maximum_depth)
    {
        out.push_back(static_cast<char>(Tag::nil));
        return;
    }
    switch (value.type())
    {
    case ValueType::boolean:
        out.push_back(static_cast<char>(value.as_bool() ? Tag::boolean_true : Tag::boolean_false));
        break;
    case ValueType::integer:
        out.push_back(static_cast<char>(Tag::integer));
        put_u64(out, static_cast<std::uint64_t>(value.as_integer()));
        break;
    case ValueType::number:
        out.push_back(static_cast<char>(Tag::number));
        put_double(out, value.as_number());
        break;
    case ValueType::string:
        out.push_back(static_cast<char>(Tag::string));
        put_string(out, value.as_string());
        break;
    case ValueType::vec3: {
        const Vec3 vector = value.as_vec3();
        out.push_back(static_cast<char>(Tag::vec3));
        put_double(out, vector.x);
        put_double(out, vector.y);
        put_double(out, vector.z);
        break;
    }
    case ValueType::handle: {
        const Handle handle = value.as_handle();
        out.push_back(static_cast<char>(Tag::handle));
        put_u32(out, static_cast<std::uint32_t>(handle.kind));
        put_u64(out, handle.id);
        break;
    }
    case ValueType::array: {
        out.push_back(static_cast<char>(Tag::array));
        const Array &array = value.as_array();
        put_u32(out, static_cast<std::uint32_t>(array.size()));
        for (const Value &item : array)
            encode(out, item, depth + 1);
        break;
    }
    case ValueType::map: {
        out.push_back(static_cast<char>(Tag::map));
        const Map &map = value.as_map();
        put_u32(out, static_cast<std::uint32_t>(map.size()));
        for (const auto &[key, item] : map)
        {
            put_string(out, key);
            encode(out, item, depth + 1);
        }
        break;
    }
    default:
        // nil, and callbacks, which never outlive their VM.
        out.push_back(static_cast<char>(Tag::nil));
        break;
    }
}

class Reader final
{
  public:
    explicit Reader(std::string_view bytes) : bytes_{bytes}
    {
    }

    [[nodiscard]] bool u8(std::uint8_t &value)
    {
        if (position_ >= bytes_.size())
            return false;
        value = static_cast<std::uint8_t>(bytes_[position_++]);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t &value)
    {
        if (bytes_.size() - position_ < 4U)
            return false;
        value = 0U;
        for (int index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[position_++])) << (8 * index);
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t &value)
    {
        if (bytes_.size() - position_ < 8U)
            return false;
        value = 0U;
        for (int index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes_[position_++])) << (8 * index);
        return true;
    }
    [[nodiscard]] bool text(std::string &value)
    {
        std::uint32_t size{};
        if (!u32(size) || bytes_.size() - position_ < size)
            return false;
        value.assign(bytes_.substr(position_, size));
        position_ += size;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - position_;
    }

  private:
    std::string_view bytes_;
    std::size_t position_{};
};

[[nodiscard]] bool decode(Reader &reader, Value &value, int depth)
{
    if (depth > maximum_depth)
        return false;
    std::uint8_t tag{};
    if (!reader.u8(tag))
        return false;
    switch (static_cast<Tag>(tag))
    {
    case Tag::nil:
        value = Value{};
        return true;
    case Tag::boolean_false:
        value = Value{false};
        return true;
    case Tag::boolean_true:
        value = Value{true};
        return true;
    case Tag::integer: {
        std::uint64_t bits{};
        if (!reader.u64(bits))
            return false;
        value = Value{static_cast<std::int64_t>(bits)};
        return true;
    }
    case Tag::number: {
        std::uint64_t bits{};
        if (!reader.u64(bits))
            return false;
        value = Value{std::bit_cast<double>(bits)};
        return true;
    }
    case Tag::string: {
        std::string text;
        if (!reader.text(text))
            return false;
        value = Value{std::move(text)};
        return true;
    }
    case Tag::vec3: {
        std::array<std::uint64_t, 3> bits{};
        if (!reader.u64(bits[0]) || !reader.u64(bits[1]) || !reader.u64(bits[2]))
            return false;
        value = Value{Vec3{std::bit_cast<double>(bits[0]), std::bit_cast<double>(bits[1]),
                           std::bit_cast<double>(bits[2])}};
        return true;
    }
    case Tag::handle: {
        std::uint32_t kind{};
        std::uint64_t id{};
        if (!reader.u32(kind) || !reader.u64(id))
            return false;
        value = Value{Handle{static_cast<HandleKind>(kind), id}};
        return true;
    }
    case Tag::array: {
        std::uint32_t count{};
        if (!reader.u32(count) || count > reader.remaining())
            return false;
        Array array;
        array.reserve(count);
        for (std::uint32_t index{}; index < count; ++index)
        {
            Value item;
            if (!decode(reader, item, depth + 1))
                return false;
            array.push_back(std::move(item));
        }
        value = Value{std::move(array)};
        return true;
    }
    case Tag::map: {
        std::uint32_t count{};
        if (!reader.u32(count) || count > reader.remaining())
            return false;
        Map map;
        map.reserve(count);
        for (std::uint32_t index{}; index < count; ++index)
        {
            std::string key;
            Value item;
            if (!reader.text(key) || !decode(reader, item, depth + 1))
                return false;
            map.emplace_back(std::move(key), std::move(item));
        }
        value = Value{std::move(map)};
        return true;
    }
    }
    return false;
}

[[nodiscard]] double number_field(const Value &table, std::string_view key, double fallback)
{
    const Value *field = table.field(key);
    return field != nullptr && field->is_number() ? field->as_number() : fallback;
}

[[nodiscard]] std::string text_field(const Value &table, std::string_view key)
{
    const Value *field = table.field(key);
    return field != nullptr && field->type() == ValueType::string ? field->as_string() : std::string{};
}

[[nodiscard]] Value progress_value(const QuestProgress &progress)
{
    Array objectives;
    for (const ObjectiveProgress &objective : progress.objectives)
        objectives.push_back(make_map({{"p", static_cast<std::int64_t>(objective.progress)}, {"d", objective.done}}));
    return make_map({{"stage", static_cast<std::int64_t>(progress.stage)},
                     {"done", progress.completed},
                     {"objectives", Value{std::move(objectives)}}});
}

} // namespace

std::string encode_sidecar_value(const Value &value)
{
    std::string out;
    encode(out, value, 0);
    return out;
}

bool decode_sidecar_value(std::string_view bytes, Value &value, std::string &error)
{
    Reader reader{bytes};
    if (!decode(reader, value, 0))
    {
        error = "the data is truncated or malformed";
        return false;
    }
    if (reader.remaining() != 0U)
    {
        error = "trailing bytes after the value";
        return false;
    }
    return true;
}

void GameplayService::Impl::save_sidecar()
{
    const std::filesystem::path directory = backend.sidecar_directory();
    if (directory.empty())
        return;
    Array overlay_cells;
    overlay_cells.reserve(overlay.size());
    for (const auto &[cell, id] : overlay)
        if (const BlockDef *block = server_registry.block(id))
            overlay_cells.push_back(Value{Array{Value{cell.x}, Value{cell.y}, Value{cell.z}, Value{block->key}}});
    Array decorated;
    decorated.reserve(decorated_chunks.size() * 2U);
    for (const auto &[x, z] : decorated_chunks)
    {
        decorated.emplace_back(x);
        decorated.emplace_back(z);
    }
    Map saved_players;
    for (const auto &[name, player] : persisted_players)
    {
        Map stats;
        for (const auto &[stat, value] : player.stats)
            stats.emplace_back(stat, value);
        Map quests;
        for (const auto &[id, progress] : player.quests)
            quests.emplace_back(id, progress_value(progress));
        Array completed;
        for (const std::string &id : player.completed_quests)
            completed.emplace_back(id);
        Array tags;
        for (const auto &[slot, key] : player.inventory_tags)
            tags.push_back(Value{Array{Value{static_cast<std::int64_t>(slot)}, Value{key}}});
        saved_players.emplace_back(name, make_map({{"stats", Value{std::move(stats)}},
                                                   {"quests", Value{std::move(quests)}},
                                                   {"completed", Value{std::move(completed)}},
                                                   {"tags", Value{std::move(tags)}}}));
    }
    Array saved_entities;
    for (const Entity &entity : entities)
    {
        const CreatureDef *creature = server_registry.creature(entity.creature);
        if (entity.removed || !entity.persistent || creature == nullptr)
            continue;
        saved_entities.push_back(make_map({{"kind", creature->key},
                                           {"p", vec3_value(entity.position)},
                                           {"yaw", entity.yaw},
                                           {"health", entity.health},
                                           {"max_health", entity.max_health},
                                           {"name", entity.name},
                                           {"data", entity.data}}));
    }
    Array per_type;
    for (const auto &[type, scales] : difficulty.per_type)
        per_type.push_back(make_map({{"type", type}, {"health", scales[0]}, {"damage", scales[1]}}));
    Array crates;
    for (const auto &[slot, id] : crate_tags)
        if (const ItemDef *item = server_registry.item(id))
            crates.push_back(Value{Array{Value{slot.first.x}, Value{slot.first.y}, Value{slot.first.z},
                                         Value{static_cast<std::int64_t>(slot.second)}, Value{item->key}}});
    const Value root = make_map({{"crates", Value{std::move(crates)}},
                                 {"overlay", Value{std::move(overlay_cells)}},
                                 {"decorated", Value{std::move(decorated)}},
                                 {"worldgen", worldgen_enabled},
                                 {"players", Value{std::move(saved_players)}},
                                 {"entities", Value{std::move(saved_entities)}},
                                 {"difficulty", make_map({{"active", difficulty.active},
                                                          {"enemy_health", difficulty.enemy_health},
                                                          {"enemy_damage", difficulty.enemy_damage},
                                                          {"spawn_rate", difficulty.spawn_rate},
                                                          {"per_type", Value{std::move(per_type)}}})}});
    std::string bytes = "OMZG";
    put_u32(bytes, sidecar_version);
    encode(bytes, root, 0);
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
    if (!write_file_atomically(directory / sidecar_file_name, std::span{data, bytes.size()}))
        log({}, LogLevel::error, "gameplay: the world's mod data could not be written");
}

void GameplayService::Impl::load_sidecar()
{
    loaded_sidecar = true;
    const std::filesystem::path directory = backend.sidecar_directory();
    if (directory.empty())
        return;
    const std::filesystem::path path = directory / sidecar_file_name;
    std::error_code error_code;
    const auto size = std::filesystem::file_size(path, error_code);
    if (error_code)
        return; // a world that never had mods
    if (size > maximum_sidecar_bytes)
    {
        log({}, LogLevel::error, "gameplay: the world's mod data is too large to load");
        return;
    }
    std::ifstream input{path, std::ios::binary};
    std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    Value root;
    std::string error;
    if (bytes.size() < 8U || bytes.compare(0, 4, "OMZG") != 0)
    {
        log({}, LogLevel::error, "gameplay: the world's mod data is not a gameplay sidecar");
        return;
    }
    std::uint32_t version{};
    std::memcpy(&version, bytes.data() + 4, 4U);
    if (version != sidecar_version || !decode_sidecar_value(std::string_view{bytes}.substr(8U), root, error))
    {
        log({}, LogLevel::error,
            std::format("gameplay: the world's mod data could not be read ({})",
                        version != sidecar_version ? "unknown version" : error));
        return;
    }
    std::size_t dropped{};
    if (const Value *cells = root.field("overlay"); cells != nullptr && cells->type() == ValueType::array)
        for (const Value &entry : cells->as_array())
        {
            if (entry.type() != ValueType::array || entry.as_array().size() != 4U)
                continue;
            const Array &fields = entry.as_array();
            if (!fields[0].is_number() || !fields[1].is_number() || !fields[2].is_number() ||
                fields[3].type() != ValueType::string)
                continue;
            const BlockDef *block = server_registry.block(fields[3].as_string());
            if (block == nullptr)
            {
                ++dropped;
                continue;
            }
            if (overlay.size() < limits.max_overlay_cells)
                overlay[{static_cast<int>(fields[0].as_number()), static_cast<int>(fields[1].as_number()),
                         static_cast<int>(fields[2].as_number())}] = block->id;
        }
    if (dropped != 0U)
        log({}, LogLevel::warning,
            std::format("gameplay: {} mod block(s) belong to mods that are not loaded; they stay as "
                        "their retail blocks",
                        dropped));
    if (const Value *chunks = root.field("decorated"); chunks != nullptr && chunks->type() == ValueType::array)
    {
        const Array &list = chunks->as_array();
        for (std::size_t index = 0U; index + 1U < list.size(); index += 2U)
            if (list[index].is_number() && list[index + 1U].is_number())
                decorated_chunks.insert({static_cast<int>(list[index].as_number()),
                                         static_cast<int>(list[index + 1U].as_number())});
    }
    if (const Value *enabled = root.field("worldgen"); enabled != nullptr && enabled->type() == ValueType::boolean)
        worldgen_enabled = worldgen_enabled || enabled->as_bool();
    if (const Value *saved = root.field("players"); saved != nullptr && saved->type() == ValueType::map)
        for (const auto &[name, entry] : saved->as_map())
        {
            PersistedPlayer player;
            if (const Value *stats = entry.field("stats"); stats != nullptr && stats->type() == ValueType::map)
                for (const auto &[stat, value] : stats->as_map())
                    if (value.is_number())
                        player.stats[stat] = value.as_number();
            if (const Value *quests = entry.field("quests"); quests != nullptr && quests->type() == ValueType::map)
                for (const auto &[id, progress_entry] : quests->as_map())
                {
                    const auto quest = server_registry.quests.find(id);
                    if (quest == server_registry.quests.end())
                        continue; // its mod is gone
                    QuestProgress progress;
                    progress.stage = static_cast<std::size_t>(std::clamp(number_field(progress_entry, "stage", 0.0), 0.0,
                                                                         static_cast<double>(quest->second.stages.size() - 1U)));
                    progress.completed = bool_or(progress_entry, "done", false);
                    progress.objectives.resize(quest->second.stages[progress.stage].objectives.size());
                    if (const Value *objectives = progress_entry.field("objectives");
                        objectives != nullptr && objectives->type() == ValueType::array)
                        for (std::size_t index{}; index < objectives->as_array().size() && index < progress.objectives.size(); ++index)
                        {
                            const Value &objective = objectives->as_array()[index];
                            progress.objectives[index].progress =
                                static_cast<std::uint32_t>(std::clamp(number_field(objective, "p", 0.0), 0.0, 1.0e6));
                            progress.objectives[index].done = bool_or(objective, "d", false);
                        }
                    player.quests.emplace(id, std::move(progress));
                }
            if (const Value *completed = entry.field("completed"); completed != nullptr && completed->type() == ValueType::array)
                for (const Value &id : completed->as_array())
                    if (id.type() == ValueType::string)
                        player.completed_quests.insert(id.as_string());
            if (const Value *tags = entry.field("tags"); tags != nullptr && tags->type() == ValueType::array)
                for (const Value &tag : tags->as_array())
                    if (tag.type() == ValueType::array && tag.as_array().size() == 2U &&
                        tag.as_array()[0].is_number() && tag.as_array()[1].type() == ValueType::string &&
                        server_registry.item(tag.as_array()[1].as_string()) != nullptr)
                        player.inventory_tags.emplace_back(
                            static_cast<std::uint16_t>(std::clamp(tag.as_array()[0].as_number(), 0.0, 127.0)),
                            tag.as_array()[1].as_string());
            persisted_players.insert_or_assign(name, std::move(player));
        }
    if (const Value *saved = root.field("entities"); saved != nullptr && saved->type() == ValueType::array)
        for (const Value &entry : saved->as_array())
        {
            const CreatureDef *creature = server_registry.creature(text_field(entry, "kind"));
            const Value *position = entry.field("p");
            const auto at = position != nullptr ? try_vec3(*position) : std::nullopt;
            if (creature == nullptr || !at.has_value())
                continue;
            Entity *restored = spawn_entity(*creature, *at,
                                            make_map({{"persistent", true},
                                                      {"name", text_field(entry, "name")},
                                                      {"yaw", number_field(entry, "yaw", 0.0)},
                                                      {"health", number_field(entry, "max_health", creature->health)}}));
            if (restored == nullptr)
                break;
            restored->health = static_cast<float>(std::clamp(number_field(entry, "health", restored->max_health), 0.01,
                                                             static_cast<double>(restored->max_health)));
            if (const Value *data = entry.field("data"))
                restored->data = *data;
        }
    if (const Value *scaling = root.field("difficulty"); scaling != nullptr && scaling->type() == ValueType::map)
    {
        difficulty.active = bool_or(*scaling, "active", false);
        difficulty.enemy_health = static_cast<float>(std::clamp(number_field(*scaling, "enemy_health", 1.0), 0.05, 100.0));
        difficulty.enemy_damage = static_cast<float>(std::clamp(number_field(*scaling, "enemy_damage", 1.0), 0.0, 100.0));
        difficulty.spawn_rate = static_cast<float>(std::clamp(number_field(*scaling, "spawn_rate", 1.0), 0.0, 20.0));
        if (const Value *types = scaling->field("per_type"); types != nullptr && types->type() == ValueType::array)
            for (const Value &entry : types->as_array())
            {
                const int type = static_cast<int>(number_field(entry, "type", -1.0));
                if (type >= 0 && type < 64)
                    difficulty.per_type[type] = {
                        static_cast<float>(std::clamp(number_field(entry, "health", 1.0), 0.05, 100.0)),
                        static_cast<float>(std::clamp(number_field(entry, "damage", 1.0), 0.0, 100.0))};
            }
        difficulty_dirty = difficulty.active;
    }
    // (Integration) Mod items in crates, by item key: [x, y, z, slot, key].
    if (const Value *crates = root.field("crates"); crates != nullptr && crates->type() == ValueType::array)
        for (const Value &entry : crates->as_array())
        {
            if (entry.type() != ValueType::array || entry.as_array().size() != 5U)
                continue;
            const Array &fields = entry.as_array();
            if (!fields[0].is_number() || !fields[1].is_number() || !fields[2].is_number() ||
                !fields[3].is_number() || fields[4].type() != ValueType::string)
                continue;
            const ItemDef *item = server_registry.item(fields[4].as_string());
            const double slot = fields[3].as_number();
            if (item == nullptr || slot < 0.0 || slot >= 32.0 || crate_tags.size() >= 65'536U)
                continue;
            crate_tags[{BlockPos{static_cast<int>(fields[0].as_number()), static_cast<int>(fields[1].as_number()),
                                 static_cast<int>(fields[2].as_number())},
                        static_cast<std::uint8_t>(slot)}] = item->id;
        }
}

} // namespace openminerz::modding::gameplay
