// The mod channel's sessions (openminerz/modding/net.hpp): RPC, replicated
// state and the content registry, framed by net/mod_channel.hpp.
//
// Messages (the payload of one reassembled mod_data message):
//   welcome       host -> client  u8 1, u8 channel version, u8 slot count,
//                                 handle player, f32 x6 client rate limits,
//                                 u32 client message limit
//   client_ready  client -> host  u8 2, u8 channel version
//   rpc           both            u8 3, u8 mod slot, string name, value
//   state         host -> client  u8 4, u8 flags (1 = snapshot complete),
//                                 key definitions [id, slot, key],
//                                 dropped stores [store],
//                                 groups [store, entries [key id, value]]
//   registry      host -> client  u8 5, u32 generation,
//                                 tables [category, entries [id, numeric]],
//                                 value extra
// Strings and counts are LEB128-prefixed; a store is u8 scope then, unless
// world, its handle (LEB128 kind, LEB128 id).

#include "openminerz/modding/net.hpp"
#include "openminerz/net/mod_channel.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace openminerz::modding
{
namespace
{

constexpr std::uint8_t channel_version = 1U;
constexpr std::uint8_t engine_slot = 0xFFU;
constexpr std::size_t maximum_name_bytes = 64U;
constexpr std::size_t maximum_key_ids = 65536U;
constexpr std::size_t maximum_client_queue_bytes = 64U * 1024U;
constexpr std::size_t maximum_early_messages = 256U;
constexpr std::size_t maximum_registry_message_bytes = net::maximum_mod_message_bytes;
constexpr std::uint8_t state_snapshot_complete = 1U;
// The client paces itself under the host's limits so an honest client never
// trips them, whatever its scripts do.
constexpr double client_rate_share = 0.9;
constexpr double client_burst_share = 0.5;

enum class MessageType : std::uint8_t
{
    welcome = 1,
    client_ready = 2,
    rpc = 3,
    state = 4,
    registry = 5
};

bool valid_name(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= maximum_name_bytes &&
           std::all_of(name.begin(), name.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '_' || character == '.' ||
                      character == ':' || character == '-' || character == '/';
           });
}

std::vector<std::string> synchronized_slots(const net::ModManifest &manifest)
{
    std::vector<std::string> slots;
    for (const net::ModDescriptor &mod : manifest.mods)
        if (mod.scope != net::ModScope::client)
            slots.push_back(mod.id);
    return slots;
}

std::size_t varint_size(std::uint64_t value) noexcept
{
    std::size_t size = 1U;
    while (value >= 0x80U)
    {
        value >>= 7U;
        ++size;
    }
    return size;
}

class Writer final
{
  public:
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value)
    {
        bytes.push_back(value);
    }
    void varint(std::uint64_t value)
    {
        while (value >= 0x80U)
        {
            bytes.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
            value >>= 7U;
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    void u32(std::uint32_t value)
    {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void f32(double value)
    {
        u32(std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    }
    void string(std::string_view text)
    {
        varint(text.size());
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    void handle(Handle value)
    {
        varint(static_cast<std::uint32_t>(value.kind));
        varint(value.id);
    }
    void store(const StoreRef &value)
    {
        u8(static_cast<std::uint8_t>(value.scope));
        if (value.scope != StoreScope::world)
            handle(value.owner);
    }
    void raw(std::span<const std::uint8_t> value)
    {
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

class Reader final
{
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}
    {
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool exhausted() const noexcept
    {
        return offset_ == bytes_.size();
    }
    [[nodiscard]] std::span<const std::uint8_t> rest() const noexcept
    {
        return bytes_.subspan(offset_);
    }

    bool u8(std::uint8_t &value) noexcept
    {
        if (remaining() < 1U)
            return false;
        value = bytes_[offset_++];
        return true;
    }
    bool varint(std::uint64_t &value) noexcept
    {
        value = 0U;
        for (std::uint32_t shift = 0U; shift < 64U; shift += 7U)
        {
            std::uint8_t part{};
            if (!u8(part) || (shift == 63U && (part & 0xFEU) != 0U))
                return false;
            value |= static_cast<std::uint64_t>(part & 0x7FU) << shift;
            if ((part & 0x80U) == 0U)
                return part != 0U || shift == 0U;
        }
        return false;
    }
    bool u32(std::uint32_t &value) noexcept
    {
        if (remaining() < 4U)
            return false;
        value = static_cast<std::uint32_t>(bytes_[offset_]) |
                (static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 8U) |
                (static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 16U) |
                (static_cast<std::uint32_t>(bytes_[offset_ + 3U]) << 24U);
        offset_ += 4U;
        return true;
    }
    bool f32(double &value) noexcept
    {
        std::uint32_t bits{};
        if (!u32(bits))
            return false;
        value = static_cast<double>(std::bit_cast<float>(bits));
        return std::isfinite(value);
    }
    bool string(std::string &text, std::size_t maximum)
    {
        std::uint64_t length{};
        if (!varint(length) || length > maximum || length > remaining())
            return false;
        text.assign(reinterpret_cast<const char *>(bytes_.data() + offset_),
                    static_cast<std::size_t>(length));
        offset_ += static_cast<std::size_t>(length);
        return true;
    }
    bool handle(Handle &value) noexcept
    {
        std::uint64_t kind{};
        if (!varint(kind) || kind > 0xFFFFFFFFU || !varint(value.id))
            return false;
        value.kind = static_cast<HandleKind>(kind);
        return true;
    }
    bool store(StoreRef &value) noexcept
    {
        std::uint8_t scope{};
        if (!u8(scope) || scope > static_cast<std::uint8_t>(StoreScope::entity))
            return false;
        value = {};
        value.scope = static_cast<StoreScope>(scope);
        if (value.scope != StoreScope::world && !handle(value.owner))
            return false;
        return value.valid();
    }
    bool value(Value &decoded, const ValueLimits &limits, std::string &error)
    {
        std::size_t consumed{};
        if (!decode_value_prefix(rest(), limits, decoded, consumed, error))
            return false;
        offset_ += consumed;
        return true;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

struct KeyRef final
{
    std::uint8_t slot{};
    std::string key;
    auto operator<=>(const KeyRef &) const = default;
};

std::string describe(const StoreRef &store)
{
    switch (store.scope)
    {
    case StoreScope::world:
        return "world";
    case StoreScope::player:
        return std::format("player {}", store.owner.id);
    case StoreScope::entity:
        return std::format("entity {}:{}", static_cast<std::uint32_t>(store.owner.kind),
                           store.owner.id);
    }
    return "store";
}

bool same_options(const ReplicatedKeyOptions &left, const ReplicatedKeyOptions &right) noexcept
{
    return left.is_public == right.is_public && left.priority == right.priority;
}

// Encodes and checks a registry; throws ScriptError when it cannot be sent.
std::vector<std::uint8_t> encode_registry(const ContentRegistry &registry,
                                          const ValueLimits &limits)
{
    if (registry.tables.size() > maximum_registry_tables)
        throw ScriptError{
            std::format("a content registry holds at most {} tables", maximum_registry_tables)};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(MessageType::registry));
    writer.u32(registry.generation);
    writer.varint(registry.tables.size());
    std::size_t entries{};
    for (const ContentRegistryTable &table : registry.tables)
    {
        if (!valid_name(table.category))
            throw ScriptError{"invalid content registry category '" + table.category + "'"};
        entries += table.entries.size();
        if (entries > maximum_registry_entries)
            throw ScriptError{std::format("a content registry holds at most {} entries",
                                          maximum_registry_entries)};
        writer.string(table.category);
        writer.varint(table.entries.size());
        for (const ContentRegistryEntry &entry : table.entries)
        {
            if (entry.id.empty() || entry.id.size() > maximum_registry_id_bytes)
                throw ScriptError{"invalid content registry id '" + entry.id + "'"};
            writer.string(entry.id);
            writer.varint(entry.numeric);
        }
    }
    std::string error;
    if (!encode_value(registry.extra, limits, writer.bytes, error))
        throw ScriptError{"content registry extra: " + error};
    if (writer.bytes.size() > maximum_registry_message_bytes)
        throw ScriptError{"the content registry is too large to send"};
    return std::move(writer.bytes);
}

bool decode_registry(Reader &reader, const ValueLimits &limits, ContentRegistry &registry)
{
    registry = {};
    std::uint64_t table_count{};
    if (!reader.u32(registry.generation) || !reader.varint(table_count) ||
        table_count > maximum_registry_tables)
        return false;
    std::size_t entries{};
    for (std::uint64_t table_index = 0U; table_index < table_count; ++table_index)
    {
        ContentRegistryTable table;
        std::uint64_t count{};
        if (!reader.string(table.category, maximum_name_bytes) || !valid_name(table.category) ||
            !reader.varint(count) || count > maximum_registry_entries - entries ||
            count > reader.remaining() / 2U)
            return false;
        entries += static_cast<std::size_t>(count);
        table.entries.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0U; index < count; ++index)
        {
            ContentRegistryEntry entry;
            std::uint64_t numeric{};
            if (!reader.string(entry.id, maximum_registry_id_bytes) || entry.id.empty() ||
                !reader.varint(numeric) || numeric > 0xFFFFFFFFU)
                return false;
            entry.numeric = static_cast<std::uint32_t>(numeric);
            table.entries.push_back(std::move(entry));
        }
        registry.tables.push_back(std::move(table));
    }
    std::string error;
    return reader.value(registry.extra, limits, error) && reader.exhausted();
}

struct OwnedChange final
{
    StoreRef store;
    std::uint8_t slot{};
    std::string key;
    Value value;
    Value old_value;
};

struct Subscription final
{
    StateFilter filter;
    StateListener listener;
};

// Delivers changes to the listeners registered when the call started; a
// listener may subscribe or unsubscribe while it runs.
void deliver_changes(const std::map<SubscriptionId, Subscription> &subscriptions,
                     std::span<const OwnedChange> changes,
                     const std::function<std::string_view(std::uint8_t)> &mod_name,
                     const ModNetLogSink &log)
{
    if (subscriptions.empty() || changes.empty())
        return;
    std::vector<std::pair<StateFilter, StateListener>> listeners;
    listeners.reserve(subscriptions.size());
    for (const auto &[id, subscription] : subscriptions)
        listeners.emplace_back(subscription.filter, subscription.listener);
    for (const OwnedChange &change : changes)
    {
        const std::string_view mod = mod_name(change.slot);
        for (const auto &[filter, listener] : listeners)
        {
            if (!filter.matches(change.store, mod, change.key))
                continue;
            try
            {
                listener(
                    StateChange{change.store, mod, change.key, change.value, change.old_value});
            }
            catch (const std::exception &exception)
            {
                if (log)
                    log(mod, LogLevel::error,
                        std::string{"replicated change listener failed: "} + exception.what());
            }
        }
    }
}

} // namespace

bool StoreRef::valid() const noexcept
{
    switch (scope)
    {
    case StoreScope::world:
        return !owner.valid() && owner.id == 0U;
    case StoreScope::player:
        return owner.kind == HandleKind::player;
    case StoreScope::entity:
        return owner.valid() && owner.kind != HandleKind::player;
    }
    return false;
}

bool StateFilter::matches(const StoreRef &store, std::string_view mod,
                          std::string_view name) const noexcept
{
    if (scope.has_value() && *scope != store.scope)
        return false;
    if (owner.valid() && owner != store.owner)
        return false;
    if (mod_id.has_value() && *mod_id != mod)
        return false;
    return !key.has_value() || *key == name;
}

// ===========================================================================
// Server
// ===========================================================================

class ModNetServer::Impl final
{
  public:
    Impl(const net::ModManifest &manifest, ModNetLimits limits)
        : enabled_{net::requires_extension(manifest)}, limits_{limits},
          slots_{synchronized_slots(manifest)}
    {
    }

    struct Entry final
    {
        Value value;
        ReplicatedKeyOptions options;
    };
    using Store = std::map<KeyRef, Entry>;

    struct DirtyKey final
    {
        StoreRef store;
        KeyRef key;
        auto operator<=>(const DirtyKey &) const = default;
    };
    struct DirtyInfo final
    {
        ReplicationPriority priority{ReplicationPriority::normal};
        std::uint64_t order{};
        bool snapshot{};
    };

    struct Peer final
    {
        net::PeerId id{};
        Handle player;
        std::uint64_t join_order{};
        bool ready{};
        bool misbehaving{};
        std::uint32_t next_sequence{1U};
        net::ModMessageAssembler assembler;
        net::TokenBucket frames;
        net::TokenBucket bytes;
        net::TokenBucket messages;
        net::TokenBucket replication;
        std::uint32_t violations{};
        std::deque<std::vector<std::uint8_t>> pending;
        std::size_t pending_bytes{};
        std::map<DirtyKey, DirtyInfo> dirty;
        std::vector<StoreRef> drops;
        std::set<StoreRef> entity_visible;
        std::map<KeyRef, std::uint32_t> key_ids;
        std::size_t snapshot_outstanding{};
        bool snapshot_complete_sent{};
    };

    struct Handler final
    {
        RpcServerHandler call;
        std::optional<PayloadSchema> schema;
    };

    [[nodiscard]] std::uint8_t slot(std::string_view mod_id) const
    {
        if (mod_id.empty())
            return engine_slot;
        for (std::size_t index = 0U; index < slots_.size(); ++index)
            if (slots_[index] == mod_id)
                return static_cast<std::uint8_t>(index);
        throw ScriptError{"mod '" + std::string{mod_id} +
                          "' is not a server/core mod of this session"};
    }

    [[nodiscard]] std::string_view mod_name(std::uint8_t index) const noexcept
    {
        return index < slots_.size() ? std::string_view{slots_[index]} : std::string_view{};
    }

    [[nodiscard]] bool valid_slot(std::uint8_t index) const noexcept
    {
        return index == engine_slot || index < slots_.size();
    }

    [[nodiscard]] std::optional<net::PeerId> peer_of_player(Handle player) const noexcept
    {
        for (const auto &[id, peer] : peers_)
            if (peer.player == player)
                return id;
        return std::nullopt;
    }

    void log(std::string_view mod, LogLevel level, std::string_view message) const
    {
        if (log_)
            log_(mod, level, message);
    }

    void emit(ModNetEvent event)
    {
        if (events_.size() >= maximum_queued_mod_net_events)
            events_.pop_front();
        events_.push_back(event);
        const std::vector<ModNetEventListener> listeners = listeners_;
        for (const ModNetEventListener &listener : listeners)
        {
            try
            {
                listener(event);
            }
            catch (const std::exception &exception)
            {
                log({}, LogLevel::error,
                    std::string{"net event listener failed: "} + exception.what());
            }
        }
    }

    void send(Peer &peer, std::span<const std::uint8_t> message)
    {
        for (std::vector<std::uint8_t> &frame :
             net::encode_mod_message(peer.next_sequence++, message))
        {
            ++counters_.frames_sent;
            counters_.bytes_sent += frame.size();
            sends_.push_back({peer.id, std::move(frame)});
        }
        ++counters_.messages_sent;
    }

    void violation(Peer &peer, std::string_view reason, bool frame_level)
    {
        ++(frame_level ? counters_.frames_rejected : counters_.messages_rejected);
        ++peer.violations;
        if (peer.violations <= 4U)
            log({}, LogLevel::warning,
                std::format("mod channel: dropped traffic from player {}: {}", peer.player.id,
                            reason));
        if (peer.misbehaving || peer.violations < limits_.violation_limit)
            return;
        peer.misbehaving = true;
        peer.assembler.reset();
        log({}, LogLevel::warning,
            std::format("mod channel: player {} exceeded {} violations and is ignored",
                        peer.player.id, limits_.violation_limit));
        emit({ModNetEventKind::peer_misbehaving, peer.id, peer.player, std::string{reason}});
    }

    [[nodiscard]] const Entry *find_entry(const StoreRef &store, const KeyRef &key) const
    {
        const auto found = stores_.find(store);
        if (found == stores_.end())
            return nullptr;
        const auto entry = found->second.find(key);
        return entry == found->second.end() ? nullptr : &entry->second;
    }

    [[nodiscard]] bool visible(const Peer &peer, const StoreRef &store,
                               const ReplicatedKeyOptions *options) const
    {
        if (!peer.ready || options == nullptr)
            return false;
        switch (store.scope)
        {
        case StoreScope::world:
            return true;
        case StoreScope::player:
            return options->is_public || store.owner == peer.player;
        case StoreScope::entity:
            return !interest_ || peer.entity_visible.contains(store);
        }
        return false;
    }

    void mark_dirty(Peer &peer, DirtyKey key, ReplicationPriority priority, bool snapshot)
    {
        const auto [found, inserted] =
            peer.dirty.try_emplace(std::move(key), DirtyInfo{priority, next_order_++, snapshot});
        if (inserted)
        {
            if (snapshot)
                ++peer.snapshot_outstanding;
            return;
        }
        found->second.priority = std::max(found->second.priority, priority);
        if (snapshot && !found->second.snapshot)
        {
            found->second.snapshot = true;
            ++peer.snapshot_outstanding;
        }
    }

    void forget_store_for(Peer &peer, const StoreRef &store, bool send_drop)
    {
        auto first = peer.dirty.lower_bound(DirtyKey{store, KeyRef{0U, {}}});
        while (first != peer.dirty.end() && first->first.store == store)
        {
            if (first->second.snapshot)
                --peer.snapshot_outstanding;
            first = peer.dirty.erase(first);
        }
        peer.entity_visible.erase(store);
        if (send_drop && std::find(peer.drops.begin(), peer.drops.end(), store) == peer.drops.end())
            peer.drops.push_back(store);
    }

    void notify(std::vector<OwnedChange> changes)
    {
        deliver_changes(
            subscriptions_, changes, [this](std::uint8_t index) { return mod_name(index); }, log_);
    }

    void refresh_interest(Peer &peer)
    {
        if (!interest_ || !peer.ready)
            return;
        std::vector<StoreRef> entities;
        for (const auto &[store, entries] : stores_)
            if (store.scope == StoreScope::entity)
                entities.push_back(store);
        for (const StoreRef &store : entities)
        {
            const bool interested = interest_(peer.player, store.owner);
            const bool held = peer.entity_visible.contains(store);
            if (interested && !held)
            {
                peer.entity_visible.insert(store);
                if (const auto found = stores_.find(store); found != stores_.end())
                    for (const auto &[key, entry] : found->second)
                        mark_dirty(peer, {store, key}, entry.options.priority, false);
            }
            else if (!interested && held)
            {
                forget_store_for(peer, store, true);
            }
        }
    }

    void begin_snapshot(Peer &peer)
    {
        peer.snapshot_complete_sent = false;
        if (interest_)
            for (const auto &[store, entries] : stores_)
                if (store.scope == StoreScope::entity && interest_(peer.player, store.owner))
                    peer.entity_visible.insert(store);
        for (const auto &[store, entries] : stores_)
            for (const auto &[key, entry] : entries)
                if (visible(peer, store, &entry.options))
                    mark_dirty(peer, {store, key}, entry.options.priority, true);
    }

    void flush(Peer &peer, double now)
    {
        const bool complete_due = !peer.snapshot_complete_sent && peer.snapshot_outstanding == 0U;
        if (peer.dirty.empty() && peer.drops.empty() && !complete_due)
            return;
        const double budget = peer.replication.available(now);
        std::vector<std::pair<const DirtyKey *, DirtyInfo>> order;
        order.reserve(peer.dirty.size());
        for (const auto &[key, info] : peer.dirty)
            order.emplace_back(&key, info);
        std::sort(order.begin(), order.end(), [](const auto &left, const auto &right) {
            if (left.second.priority != right.second.priority)
                return left.second.priority > right.second.priority;
            return left.second.order < right.second.order;
        });

        Writer definitions;
        std::size_t definition_count{};
        Writer drops;
        for (const StoreRef &store : peer.drops)
            drops.store(store);
        struct Group final
        {
            Writer entries;
            std::size_t count{};
        };
        std::map<StoreRef, Group> groups;
        std::size_t used = 8U + drops.bytes.size();
        const std::size_t message_cap = limits_.server_message_bytes > 256U
                                            ? limits_.server_message_bytes - 256U
                                            : limits_.server_message_bytes;
        std::vector<DirtyKey> sent;
        std::vector<std::uint8_t> value_bytes;
        std::string error;
        for (const auto &[key, info] : order)
        {
            if (budget <= 0.0)
                break;
            const Entry *entry = find_entry(key->store, key->key);
            value_bytes.clear();
            if (entry == nullptr || !visible(peer, key->store, &entry->options) ||
                !encode_value(entry->value, limits_.server_values, value_bytes, error))
            {
                value_bytes.assign(1U, 0U); // nil: erase on the client
            }
            const auto known = peer.key_ids.find(key->key);
            std::size_t cost = value_bytes.size();
            std::uint32_t key_id{};
            if (known == peer.key_ids.end())
            {
                if (peer.key_ids.size() >= maximum_key_ids)
                    continue;
                key_id = static_cast<std::uint32_t>(peer.key_ids.size());
                cost += varint_size(key_id) + 1U + varint_size(key->key.key.size()) +
                        key->key.key.size();
            }
            else
            {
                key_id = known->second;
            }
            cost += varint_size(key_id);
            if (!groups.contains(key->store))
                cost += 24U;
            if (!sent.empty() &&
                (static_cast<double>(used + cost) > budget || used + cost > message_cap))
                break;
            if (known == peer.key_ids.end())
            {
                peer.key_ids.emplace(key->key, key_id);
                definitions.varint(key_id);
                definitions.u8(key->key.slot);
                definitions.string(key->key.key);
                ++definition_count;
            }
            Group &group = groups[key->store];
            group.entries.varint(key_id);
            group.entries.raw(value_bytes);
            ++group.count;
            used += cost;
            sent.push_back(*key);
        }
        for (const DirtyKey &key : sent)
        {
            const auto found = peer.dirty.find(key);
            if (found == peer.dirty.end())
                continue;
            if (found->second.snapshot)
                --peer.snapshot_outstanding;
            peer.dirty.erase(found);
        }
        const bool complete = !peer.snapshot_complete_sent && peer.snapshot_outstanding == 0U;
        if (sent.empty() && peer.drops.empty() && !complete)
            return;

        Writer message;
        message.u8(static_cast<std::uint8_t>(MessageType::state));
        message.u8(complete ? state_snapshot_complete : std::uint8_t{0});
        message.varint(definition_count);
        message.raw(definitions.bytes);
        message.varint(peer.drops.size());
        message.raw(drops.bytes);
        message.varint(groups.size());
        for (const auto &[store, group] : groups)
        {
            message.store(store);
            message.varint(group.count);
            message.raw(group.entries.bytes);
        }
        peer.drops.clear();
        if (complete)
            peer.snapshot_complete_sent = true;
        peer.replication.consume(static_cast<double>(message.bytes.size()), now);
        counters_.replication_bytes += message.bytes.size();
        send(peer, message.bytes);
    }

    void mark_ready(Peer &peer)
    {
        peer.ready = true;
        begin_snapshot(peer);
        while (!peer.pending.empty())
        {
            send(peer, peer.pending.front());
            peer.pending.pop_front();
        }
        peer.pending_bytes = 0U;
        emit({ModNetEventKind::player_ready, peer.id, peer.player, {}});
    }

    void handle_message(net::PeerId peer_id, std::span<const std::uint8_t> message, double now)
    {
        const auto found = peers_.find(peer_id);
        if (found == peers_.end())
            return;
        Peer &peer = found->second;
        if (!peer.messages.take(1.0, now))
        {
            violation(peer, "message rate limit", false);
            return;
        }
        ++counters_.messages_received;
        Reader reader{message};
        std::uint8_t type{};
        static_cast<void>(reader.u8(type));
        if (type == static_cast<std::uint8_t>(MessageType::client_ready))
        {
            std::uint8_t version{};
            if (!reader.u8(version) || version != channel_version || !reader.exhausted())
            {
                violation(peer, "malformed client_ready", false);
                return;
            }
            if (peer.ready)
            {
                violation(peer, "duplicate client_ready", false);
                return;
            }
            mark_ready(peer);
            return;
        }
        if (type != static_cast<std::uint8_t>(MessageType::rpc))
        {
            // Welcome, state and registry flow only from the host: a client
            // that sends replicated state is refused before anything applies it.
            violation(peer,
                      std::format("message type {} is not accepted from clients",
                                  static_cast<unsigned>(type)),
                      false);
            return;
        }
        if (!peer.ready)
        {
            violation(peer, "rpc before client_ready", false);
            return;
        }
        std::uint8_t slot_index{};
        std::string name;
        if (!reader.u8(slot_index) || !valid_slot(slot_index) ||
            !reader.string(name, maximum_name_bytes) || !valid_name(name))
        {
            violation(peer, "malformed rpc header", false);
            return;
        }
        Value payload;
        std::string error;
        if (!decode_value(reader.rest(), limits_.client_values, payload, error))
        {
            violation(peer, "rpc payload: " + error, false);
            return;
        }
        const auto handler = handlers_.find(std::pair<std::uint8_t, std::string>{slot_index, name});
        const std::string_view mod = mod_name(slot_index);
        if (handler == handlers_.end())
        {
            ++counters_.rpc_dropped;
            log(mod, LogLevel::debug, "net: no handler for '" + name + "'");
            return;
        }
        if (handler->second.schema.has_value() &&
            !validate_payload(*handler->second.schema, payload, error))
        {
            ++counters_.rpc_dropped;
            log(mod, LogLevel::warning,
                std::format("net: dropped '{}' from player {}: {}", name, peer.player.id, error));
            return;
        }
        // Copied: the handler may replace or remove itself.
        const RpcServerHandler call = handler->second.call;
        const Handle player = peer.player;
        try
        {
            call(player, payload);
        }
        catch (const std::exception &exception)
        {
            log(mod, LogLevel::error,
                std::format("net: handler for '{}' failed: {}", name, exception.what()));
        }
    }

    std::vector<std::uint8_t> encode_rpc(std::string_view mod_id, std::string_view name,
                                         const Value &payload, const ValueLimits &limits,
                                         std::size_t maximum) const
    {
        if (!valid_name(name))
            throw ScriptError{"message names use 1-64 letters, digits, or _ . : - /"};
        Writer writer;
        writer.u8(static_cast<std::uint8_t>(MessageType::rpc));
        writer.u8(slot(mod_id));
        writer.string(name);
        std::string error;
        if (!encode_value(payload, limits, writer.bytes, error))
            throw ScriptError{"net payload: " + error};
        if (writer.bytes.size() > maximum)
            throw ScriptError{std::format("net payload exceeds {} bytes", maximum)};
        return std::move(writer.bytes);
    }

    std::vector<std::uint8_t> encode_welcome(const Peer &peer) const
    {
        Writer writer;
        writer.u8(static_cast<std::uint8_t>(MessageType::welcome));
        writer.u8(channel_version);
        writer.u8(static_cast<std::uint8_t>(slots_.size()));
        writer.handle(peer.player);
        writer.f32(limits_.client_frames_per_second);
        writer.f32(limits_.client_frame_burst);
        writer.f32(limits_.client_bytes_per_second);
        writer.f32(limits_.client_byte_burst);
        writer.f32(limits_.client_messages_per_second);
        writer.f32(limits_.client_message_burst);
        writer.u32(limits_.client_message_bytes);
        return std::move(writer.bytes);
    }

    bool enabled_{};
    ModNetLimits limits_;
    std::vector<std::string> slots_;
    std::map<net::PeerId, Peer> peers_;
    std::map<StoreRef, Store> stores_;
    std::size_t entry_count_{}; // values in stores_, all together
    std::map<std::pair<std::uint8_t, std::string>, Handler> handlers_;
    std::map<SubscriptionId, Subscription> subscriptions_;
    SubscriptionId next_subscription_{1U};
    std::function<bool(Handle, Handle)> interest_;
    ContentRegistry registry_;
    std::vector<std::uint8_t> registry_message_;
    std::vector<ModNetSend> sends_;
    std::deque<ModNetEvent> events_;
    std::vector<ModNetEventListener> listeners_;
    ModNetLogSink log_;
    ModNetCounters counters_;
    std::uint64_t next_order_{1U};
    std::uint64_t next_join_{1U};
};

ModNetServer::ModNetServer(const net::ModManifest &manifest, ModNetLimits limits)
    : impl_{std::make_unique<Impl>(manifest, limits)}
{
}

ModNetServer::~ModNetServer() = default;

bool ModNetServer::enabled() const noexcept
{
    return impl_->enabled_;
}

const ModNetLimits &ModNetServer::limits() const noexcept
{
    return impl_->limits_;
}

bool ModNetServer::add_peer(net::PeerId peer, Handle player, double now)
{
    Impl &impl = *impl_;
    if (!impl.enabled_ || peer == 0U || player.kind != HandleKind::player ||
        impl.peers_.contains(peer) || impl.peer_of_player(player).has_value())
        return false;
    Impl::Peer state;
    state.id = peer;
    state.player = player;
    state.join_order = impl.next_join_++;
    state.assembler = net::ModMessageAssembler{impl.limits_.client_message_bytes,
                                               impl.limits_.reassembly_timeout_seconds};
    state.frames = {impl.limits_.client_frames_per_second, impl.limits_.client_frame_burst};
    state.bytes = {impl.limits_.client_bytes_per_second, impl.limits_.client_byte_burst};
    state.messages = {impl.limits_.client_messages_per_second, impl.limits_.client_message_burst};
    state.replication = {impl.limits_.replication_bytes_per_second,
                         impl.limits_.replication_burst_bytes};
    // Prime every bucket's clock.
    static_cast<void>(state.frames.available(now));
    static_cast<void>(state.bytes.available(now));
    static_cast<void>(state.messages.available(now));
    static_cast<void>(state.replication.available(now));
    Impl::Peer &added = impl.peers_.emplace(peer, std::move(state)).first->second;
    impl.send(added, impl.encode_welcome(added));
    if (!impl.registry_message_.empty())
        impl.send(added, impl.registry_message_);
    return true;
}

void ModNetServer::remove_peer(net::PeerId peer)
{
    Impl &impl = *impl_;
    const auto found = impl.peers_.find(peer);
    if (found == impl.peers_.end())
        return;
    const Handle player = found->second.player;
    impl.peers_.erase(found);
    impl.emit({ModNetEventKind::player_left, peer, player, {}});
}

void ModNetServer::receive(net::PeerId sender, std::span<const std::uint8_t> frame, double now)
{
    Impl &impl = *impl_;
    const auto found = impl.peers_.find(sender);
    if (!impl.enabled_ || found == impl.peers_.end())
    {
        ++impl.counters_.frames_rejected;
        return;
    }
    Impl::Peer &peer = found->second;
    if (peer.misbehaving)
        return;
    ++impl.counters_.frames_received;
    impl.counters_.bytes_received += frame.size();
    // Rate limits come first: a flood costs no decoding.
    if (!peer.frames.take(1.0, now) || !peer.bytes.take(static_cast<double>(frame.size()), now))
    {
        peer.assembler.reset();
        impl.violation(peer, "frame rate limit", true);
        return;
    }
    net::ModFrame decoded;
    net::ModFrameError error{};
    if (!net::decode_mod_frame(frame, decoded, error))
    {
        peer.assembler.reset();
        impl.violation(peer, std::string{"frame "} + net::mod_frame_error_name(error), true);
        return;
    }
    std::vector<std::uint8_t> message;
    const auto result = peer.assembler.accept(decoded, now, message, error);
    if (result == net::ModMessageAssembler::Result::rejected)
    {
        impl.violation(peer, std::string{"fragment "} + net::mod_frame_error_name(error), true);
        return;
    }
    if (result == net::ModMessageAssembler::Result::complete)
        impl.handle_message(sender, message, now);
}

void ModNetServer::update(double now)
{
    Impl &impl = *impl_;
    if (!impl.enabled_)
        return;
    for (auto &[id, peer] : impl.peers_)
    {
        if (peer.assembler.expire(now))
            impl.log({}, LogLevel::debug,
                     std::format("mod channel: player {} stalled mid-message", peer.player.id));
        impl.refresh_interest(peer);
    }
    for (auto &[id, peer] : impl.peers_)
        if (peer.ready && !peer.misbehaving)
            impl.flush(peer, now);
}

std::vector<ModNetSend> ModNetServer::take_sends()
{
    return std::exchange(impl_->sends_, {});
}

std::vector<ModNetEvent> ModNetServer::take_events()
{
    std::vector<ModNetEvent> events{impl_->events_.begin(), impl_->events_.end()};
    impl_->events_.clear();
    return events;
}

void ModNetServer::add_event_listener(ModNetEventListener listener)
{
    if (listener)
        impl_->listeners_.push_back(std::move(listener));
}

void ModNetServer::set_log_sink(ModNetLogSink sink)
{
    impl_->log_ = std::move(sink);
}

bool ModNetServer::has_peer(net::PeerId peer) const noexcept
{
    return impl_->peers_.contains(peer);
}

bool ModNetServer::peer_ready(net::PeerId peer) const noexcept
{
    const auto found = impl_->peers_.find(peer);
    return found != impl_->peers_.end() && found->second.ready;
}

std::optional<Handle> ModNetServer::player_of(net::PeerId peer) const noexcept
{
    const auto found = impl_->peers_.find(peer);
    return found == impl_->peers_.end() ? std::nullopt
                                        : std::optional<Handle>{found->second.player};
}

std::optional<net::PeerId> ModNetServer::peer_of(Handle player) const noexcept
{
    return impl_->peer_of_player(player);
}

std::vector<Handle> ModNetServer::ready_players() const
{
    std::vector<const Impl::Peer *> ready;
    for (const auto &[id, peer] : impl_->peers_)
        if (peer.ready)
            ready.push_back(&peer);
    std::sort(ready.begin(), ready.end(), [](const Impl::Peer *left, const Impl::Peer *right) {
        return left->join_order < right->join_order;
    });
    std::vector<Handle> players;
    players.reserve(ready.size());
    for (const Impl::Peer *peer : ready)
        players.push_back(peer->player);
    return players;
}

std::uint32_t ModNetServer::violations(net::PeerId peer) const noexcept
{
    const auto found = impl_->peers_.find(peer);
    return found == impl_->peers_.end() ? 0U : found->second.violations;
}

const ModNetCounters &ModNetServer::counters() const noexcept
{
    return impl_->counters_;
}

std::size_t ModNetServer::pending_replication(net::PeerId peer) const noexcept
{
    const auto found = impl_->peers_.find(peer);
    return found == impl_->peers_.end() ? 0U : found->second.dirty.size();
}

void ModNetServer::on(std::string_view mod_id, std::string_view name, RpcServerHandler handler,
                      std::optional<PayloadSchema> schema)
{
    Impl &impl = *impl_;
    if (!valid_name(name))
        throw ScriptError{"message names use 1-64 letters, digits, or _ . : - /"};
    const std::uint8_t slot_index = impl.slot(mod_id);
    impl.handlers_.insert_or_assign(std::pair<std::uint8_t, std::string>{slot_index, name},
                                    Impl::Handler{std::move(handler), std::move(schema)});
}

void ModNetServer::off(std::string_view mod_id, std::string_view name)
{
    Impl &impl = *impl_;
    std::uint8_t slot_index{};
    try
    {
        slot_index = impl.slot(mod_id);
    }
    catch (const ScriptError &)
    {
        return;
    }
    impl.handlers_.erase(std::pair<std::uint8_t, std::string>{slot_index, name});
}

bool ModNetServer::send_to(Handle player, std::string_view mod_id, std::string_view name,
                           const Value &payload)
{
    Impl &impl = *impl_;
    const std::vector<std::uint8_t> message = impl.encode_rpc(
        mod_id, name, payload, impl.limits_.server_values, impl.limits_.server_message_bytes);
    const std::optional<net::PeerId> peer_id = impl.peer_of_player(player);
    if (!impl.enabled_ || !peer_id.has_value())
        return false;
    Impl::Peer &peer = impl.peers_.at(*peer_id);
    if (peer.ready)
    {
        impl.send(peer, message);
        return true;
    }
    if (peer.pending.size() >= impl.limits_.pending_messages ||
        peer.pending_bytes + message.size() > impl.limits_.pending_bytes)
    {
        impl.log(mod_id, LogLevel::warning,
                 std::format("net: '{}' to player {} dropped: too many messages before its "
                             "channel opened",
                             name, player.id));
        return false;
    }
    peer.pending_bytes += message.size();
    peer.pending.push_back(message);
    return true;
}

std::size_t ModNetServer::broadcast(std::string_view mod_id, std::string_view name,
                                    const Value &payload)
{
    Impl &impl = *impl_;
    const std::vector<std::uint8_t> message = impl.encode_rpc(
        mod_id, name, payload, impl.limits_.server_values, impl.limits_.server_message_bytes);
    if (!impl.enabled_)
        return 0U;
    std::size_t addressed{};
    for (auto &[id, peer] : impl.peers_)
        if (peer.ready)
        {
            impl.send(peer, message);
            ++addressed;
        }
    return addressed;
}

void ModNetServer::set(const StoreRef &store, std::string_view mod_id, std::string_view key,
                       Value value, std::optional<ReplicatedKeyOptions> options)
{
    Impl &impl = *impl_;
    if (!store.valid())
        throw ScriptError{"replicated scope must be \"world\", a player or an entity handle"};
    if (!valid_name(key))
        throw ScriptError{"replicated keys use 1-64 letters, digits, or _ . : - /"};
    KeyRef ref{impl.slot(mod_id), std::string{key}};
    if (!value.is_nil())
    {
        std::vector<std::uint8_t> scratch;
        std::string error;
        if (!encode_value(value, impl.limits_.server_values, scratch, error))
            throw ScriptError{"replicated value: " + error};
        if (scratch.size() > maximum_replicated_value_bytes)
            throw ScriptError{std::format("a replicated value encodes to at most {} bytes",
                                          maximum_replicated_value_bytes)};
    }
    auto store_found = impl.stores_.find(store);
    Impl::Entry *existing = nullptr;
    if (store_found != impl.stores_.end())
        if (const auto entry = store_found->second.find(ref); entry != store_found->second.end())
            existing = &entry->second;
    if (existing == nullptr && value.is_nil())
        return;
    // (Integration) What a client can hold is what the host may keep: past
    // it a script fills the host's memory with values nobody receives.
    if (existing == nullptr && impl.entry_count_ >= maximum_client_state_entries)
        throw ScriptError{std::format("replicated state holds at most {} values in a session; "
                                      "remove some (replicated.clear, nil values) first",
                                      maximum_client_state_entries)};
    const ReplicatedKeyOptions old_options =
        existing != nullptr ? existing->options : ReplicatedKeyOptions{};
    const ReplicatedKeyOptions new_options = options.value_or(old_options);
    if (existing != nullptr && existing->value == value && same_options(old_options, new_options))
        return;

    std::vector<net::PeerId> watchers;
    for (const auto &[id, peer] : impl.peers_)
        if (impl.visible(peer, store, existing != nullptr ? &old_options : nullptr))
            watchers.push_back(id);
    Value old = existing != nullptr ? existing->value : Value{};
    if (value.is_nil())
    {
        store_found->second.erase(ref);
        --impl.entry_count_;
        if (store_found->second.empty())
            impl.stores_.erase(store_found);
    }
    else
    {
        if (existing == nullptr)
            ++impl.entry_count_;
        Impl::Entry &entry = impl.stores_[store][ref];
        entry.value = value;
        entry.options = new_options;
    }
    for (auto &[id, peer] : impl.peers_)
    {
        const bool now_visible = impl.visible(peer, store, value.is_nil() ? nullptr : &new_options);
        if (now_visible || std::find(watchers.begin(), watchers.end(), id) != watchers.end())
            impl.mark_dirty(peer, {store, ref}, new_options.priority, false);
    }
    std::vector<OwnedChange> changes;
    changes.push_back({store, ref.slot, ref.key, std::move(value), std::move(old)});
    impl.notify(std::move(changes));
}

const Value *ModNetServer::get(const StoreRef &store, std::string_view mod_id,
                               std::string_view key) const
{
    const Impl &impl = *impl_;
    const Impl::Entry *entry = impl.find_entry(store, KeyRef{impl.slot(mod_id), std::string{key}});
    return entry == nullptr ? nullptr : &entry->value;
}

Map ModNetServer::all(const StoreRef &store, std::string_view mod_id) const
{
    const Impl &impl = *impl_;
    const std::uint8_t slot_index = impl.slot(mod_id);
    Map result;
    if (const auto found = impl.stores_.find(store); found != impl.stores_.end())
        for (const auto &[key, entry] : found->second)
            if (key.slot == slot_index)
                result.emplace_back(key.key, entry.value);
    return result;
}

void ModNetServer::drop_store(const StoreRef &store)
{
    Impl &impl = *impl_;
    const auto found = impl.stores_.find(store);
    std::vector<OwnedChange> changes;
    // A peer is told to drop only a store it could see something of.
    std::vector<net::PeerId> holders;
    if (found != impl.stores_.end())
    {
        for (const auto &[id, peer] : impl.peers_)
            if (std::any_of(found->second.begin(), found->second.end(), [&](const auto &entry) {
                    return impl.visible(peer, store, &entry.second.options);
                }))
                holders.push_back(id);
        for (auto &[key, entry] : found->second)
            changes.push_back({store, key.slot, key.key, Value{}, std::move(entry.value)});
        impl.entry_count_ -= found->second.size();
        impl.stores_.erase(found);
    }
    for (auto &[id, peer] : impl.peers_)
        impl.forget_store_for(peer, store,
                              std::find(holders.begin(), holders.end(), id) != holders.end());
    impl.notify(std::move(changes));
}

void ModNetServer::clear(const StoreRef &store, std::string_view mod_id)
{
    const std::uint8_t slot_index = impl_->slot(mod_id);
    std::vector<std::string> keys;
    if (const auto found = impl_->stores_.find(store); found != impl_->stores_.end())
        for (const auto &[key, entry] : found->second)
            if (key.slot == slot_index)
                keys.push_back(key.key);
    for (const std::string &key : keys)
        set(store, mod_id, key, Value{});
}

std::vector<StoreRef> ModNetServer::stores(StoreScope scope) const
{
    std::vector<StoreRef> result;
    for (const auto &[store, entries] : impl_->stores_)
        if (store.scope == scope)
            result.push_back(store);
    return result;
}

SubscriptionId ModNetServer::subscribe(StateFilter filter, StateListener listener)
{
    const SubscriptionId id = impl_->next_subscription_++;
    impl_->subscriptions_.emplace(id, Subscription{std::move(filter), std::move(listener)});
    return id;
}

void ModNetServer::unsubscribe(SubscriptionId id) noexcept
{
    impl_->subscriptions_.erase(id);
}

void ModNetServer::set_entity_interest(std::function<bool(Handle player, Handle entity)> interest)
{
    const bool had_interest = static_cast<bool>(impl_->interest_);
    impl_->interest_ = std::move(interest);
    if (impl_->interest_ && !had_interest)
    {
        // Until now every entity store was visible: each ready peer holds them
        // all, and the next update() drops what the new rule hides.
        for (auto &[id, peer] : impl_->peers_)
            if (peer.ready)
                for (const auto &[store, entries] : impl_->stores_)
                    if (store.scope == StoreScope::entity)
                        peer.entity_visible.insert(store);
        return;
    }
    if (!impl_->interest_)
    {
        // Every entity store is visible again: send what each peer lacks.
        for (auto &[id, peer] : impl_->peers_)
            if (peer.ready)
                for (const auto &[store, entries] : impl_->stores_)
                    if (store.scope == StoreScope::entity && !peer.entity_visible.contains(store))
                        for (const auto &[key, entry] : entries)
                            impl_->mark_dirty(peer, {store, key}, entry.options.priority, false);
        for (auto &[id, peer] : impl_->peers_)
            peer.entity_visible.clear();
    }
}

void ModNetServer::set_content_registry(ContentRegistry registry)
{
    Impl &impl = *impl_;
    registry.generation = impl.registry_.generation + 1U;
    std::vector<std::uint8_t> message = encode_registry(registry, impl.limits_.server_values);
    impl.registry_ = std::move(registry);
    impl.registry_message_ = std::move(message);
    if (!impl.enabled_)
        return;
    for (auto &[id, peer] : impl.peers_)
        impl.send(peer, impl.registry_message_);
}

const ContentRegistry &ModNetServer::content_registry() const noexcept
{
    return impl_->registry_;
}

std::optional<std::uint8_t> ModNetServer::slot_of(std::string_view mod_id) const noexcept
{
    try
    {
        return impl_->slot(mod_id);
    }
    catch (const ScriptError &)
    {
        return std::nullopt;
    }
}

// ===========================================================================
// Client
// ===========================================================================

class ModNetClient::Impl final
{
  public:
    Impl(const net::ModManifest &manifest, ModNetLimits limits)
        : enabled_{net::requires_extension(manifest)}, limits_{limits},
          slots_{synchronized_slots(manifest)},
          assembler_{maximum_registry_message_bytes, limits.reassembly_timeout_seconds},
          host_message_bytes_{limits.client_message_bytes}
    {
        set_pacing(limits.client_frames_per_second, limits.client_frame_burst,
                   limits.client_bytes_per_second, limits.client_byte_burst,
                   limits.client_messages_per_second, limits.client_message_burst);
    }

    struct Handler final
    {
        RpcClientHandler call;
        std::optional<PayloadSchema> schema;
    };
    struct OutFrame final
    {
        std::vector<std::uint8_t> bytes;
        bool first{};
    };

    void set_pacing(double frames, double frame_burst, double bytes, double byte_burst,
                    double messages, double message_burst)
    {
        frames_ = {frames * client_rate_share, std::max(1.0, frame_burst * client_burst_share)};
        bytes_ = {bytes * client_rate_share,
                  std::max(static_cast<double>(net::maximum_extension_packet_size),
                           byte_burst * client_burst_share)};
        messages_ = {messages * client_rate_share,
                     std::max(1.0, message_burst * client_burst_share)};
    }

    [[nodiscard]] std::uint8_t slot(std::string_view mod_id) const
    {
        if (mod_id.empty())
            return engine_slot;
        for (std::size_t index = 0U; index < slots_.size(); ++index)
            if (slots_[index] == mod_id)
                return static_cast<std::uint8_t>(index);
        throw ScriptError{"mod '" + std::string{mod_id} +
                          "' is not a server/core mod of this session"};
    }

    [[nodiscard]] std::string_view mod_name(std::uint8_t index) const noexcept
    {
        return index < slots_.size() ? std::string_view{slots_[index]} : std::string_view{};
    }

    [[nodiscard]] bool valid_slot(std::uint8_t index) const noexcept
    {
        return index == engine_slot || index < slots_.size();
    }

    void log(std::string_view mod, LogLevel level, std::string_view message) const
    {
        if (log_)
            log_(mod, level, message);
    }

    void reject(std::string_view reason)
    {
        ++counters_.messages_rejected;
        if (counters_.messages_rejected <= 8U)
            log({}, LogLevel::warning,
                std::string{"mod channel: ignored host message: "} + std::string{reason});
    }

    void emit(ModNetEvent event)
    {
        if (events_.size() >= maximum_queued_mod_net_events)
            events_.pop_front();
        events_.push_back(event);
        const std::vector<ModNetEventListener> listeners = listeners_;
        for (const ModNetEventListener &listener : listeners)
        {
            try
            {
                listener(event);
            }
            catch (const std::exception &exception)
            {
                log({}, LogLevel::error,
                    std::string{"net event listener failed: "} + exception.what());
            }
        }
    }

    void queue_frames(std::span<const std::uint8_t> message)
    {
        bool first = true;
        for (std::vector<std::uint8_t> &frame : net::encode_mod_message(next_sequence_++, message))
        {
            queued_bytes_ += frame.size();
            frame_queue_.push_back({std::move(frame), first});
            first = false;
        }
    }

    void dispatch_rpc(std::span<const std::uint8_t> message)
    {
        Reader reader{message.subspan(1U)};
        std::uint8_t slot_index{};
        std::string name;
        if (!reader.u8(slot_index) || !valid_slot(slot_index) ||
            !reader.string(name, maximum_name_bytes) || !valid_name(name))
        {
            reject("malformed rpc header");
            return;
        }
        Value payload;
        std::string error;
        if (!decode_value(reader.rest(), limits_.server_values, payload, error))
        {
            reject("rpc payload: " + error);
            return;
        }
        const std::string_view mod = mod_name(slot_index);
        const auto handler = handlers_.find(std::pair<std::uint8_t, std::string>{slot_index, name});
        if (handler == handlers_.end())
        {
            ++counters_.rpc_dropped;
            log(mod, LogLevel::debug, "net: no handler for '" + name + "'");
            return;
        }
        if (handler->second.schema.has_value() &&
            !validate_payload(*handler->second.schema, payload, error))
        {
            ++counters_.rpc_dropped;
            log(mod, LogLevel::warning,
                std::format("net: dropped '{}' from host: {}", name, error));
            return;
        }
        const RpcClientHandler call = handler->second.call;
        try
        {
            call(payload);
        }
        catch (const std::exception &exception)
        {
            log(mod, LogLevel::error,
                std::format("net: handler for '{}' failed: {}", name, exception.what()));
        }
    }

    void handle_welcome(Reader &reader)
    {
        std::uint8_t version{};
        std::uint8_t slot_count{};
        Handle player;
        double rates[6]{};
        std::uint32_t message_bytes{};
        bool valid = reader.u8(version) && reader.u8(slot_count) && reader.handle(player);
        for (double &rate : rates)
            valid = valid && reader.f32(rate) && rate > 0.0;
        valid = valid && reader.u32(message_bytes) && reader.exhausted();
        if (!valid || version != channel_version || slot_count != slots_.size() ||
            player.kind != HandleKind::player || message_bytes == 0U)
        {
            reject("malformed welcome");
            return;
        }
        if (welcomed_)
        {
            reject("duplicate welcome");
            return;
        }
        welcomed_ = true;
        local_player_ = player;
        host_message_bytes_ =
            std::min<std::uint32_t>(message_bytes, net::maximum_mod_message_bytes);
        set_pacing(rates[0], rates[1], rates[2], rates[3], rates[4], rates[5]);
        emit({ModNetEventKind::welcome, 0U, player, {}});
    }

    void handle_state(Reader &reader)
    {
        std::uint8_t flags{};
        std::uint64_t definition_count{};
        if (!reader.u8(flags) || (flags & ~state_snapshot_complete) != 0U ||
            !reader.varint(definition_count) || definition_count > maximum_key_ids ||
            definition_count > reader.remaining() / 3U)
        {
            reject("malformed state header");
            return;
        }
        std::map<std::uint64_t, KeyRef> definitions;
        for (std::uint64_t index = 0U; index < definition_count; ++index)
        {
            std::uint64_t id{};
            KeyRef key;
            if (!reader.varint(id) || id >= maximum_key_ids || !reader.u8(key.slot) ||
                !valid_slot(key.slot) || !reader.string(key.key, maximum_name_bytes) ||
                !valid_name(key.key))
            {
                reject("malformed key definition");
                return;
            }
            if (const auto existing = key_ids_.find(id);
                (existing != key_ids_.end() && existing->second != key) ||
                !definitions.emplace(id, std::move(key)).second)
            {
                reject("conflicting key definition");
                return;
            }
        }
        std::uint64_t drop_count{};
        if (!reader.varint(drop_count) || drop_count > reader.remaining())
        {
            reject("malformed store drops");
            return;
        }
        std::vector<StoreRef> drops;
        for (std::uint64_t index = 0U; index < drop_count; ++index)
        {
            StoreRef store;
            if (!reader.store(store))
            {
                reject("malformed store drop");
                return;
            }
            drops.push_back(store);
        }
        std::uint64_t group_count{};
        if (!reader.varint(group_count) || group_count > reader.remaining())
        {
            reject("malformed state groups");
            return;
        }
        struct Update final
        {
            StoreRef store;
            KeyRef key;
            Value value;
        };
        std::vector<Update> updates;
        for (std::uint64_t group = 0U; group < group_count; ++group)
        {
            StoreRef store;
            std::uint64_t count{};
            if (!reader.store(store) || !reader.varint(count) || count > reader.remaining() / 2U)
            {
                reject("malformed state group");
                return;
            }
            for (std::uint64_t index = 0U; index < count; ++index)
            {
                std::uint64_t id{};
                Value value;
                std::string error;
                if (!reader.varint(id))
                {
                    reject("malformed state entry");
                    return;
                }
                const auto defined = definitions.find(id);
                const auto known = key_ids_.find(id);
                if (defined == definitions.end() && known == key_ids_.end())
                {
                    reject("state entry names an undefined key");
                    return;
                }
                if (!reader.value(value, limits_.server_values, error))
                {
                    reject("state value: " + error);
                    return;
                }
                updates.push_back({store,
                                   defined != definitions.end() ? defined->second : known->second,
                                   std::move(value)});
            }
        }
        if (!reader.exhausted())
        {
            reject("state message has trailing bytes");
            return;
        }

        // Valid as a whole: apply it.
        for (auto &[id, key] : definitions)
            key_ids_.insert_or_assign(id, std::move(key));
        std::vector<OwnedChange> changes;
        for (const StoreRef &store : drops)
        {
            const auto found = stores_.find(store);
            if (found == stores_.end())
                continue;
            for (auto &[key, value] : found->second)
                changes.push_back({store, key.slot, key.key, Value{}, std::move(value)});
            entry_count_ -= found->second.size();
            stores_.erase(found);
        }
        for (Update &update : updates)
        {
            auto store = stores_.find(update.store);
            Value *current = nullptr;
            if (store != stores_.end())
                if (const auto entry = store->second.find(update.key); entry != store->second.end())
                    current = &entry->second;
            if (update.value.is_nil())
            {
                if (current == nullptr)
                    continue;
                changes.push_back(
                    {update.store, update.key.slot, update.key.key, Value{}, std::move(*current)});
                store->second.erase(update.key);
                --entry_count_;
                if (store->second.empty())
                    stores_.erase(store);
                continue;
            }
            if (current == nullptr)
            {
                if (entry_count_ >= maximum_client_state_entries)
                {
                    reject("replicated state exceeds the client entry limit");
                    continue;
                }
                ++entry_count_;
                stores_[update.store].emplace(update.key, update.value);
                changes.push_back({update.store, update.key.slot, update.key.key,
                                   std::move(update.value), Value{}});
                continue;
            }
            if (*current == update.value)
                continue;
            Value old = std::exchange(*current, update.value);
            changes.push_back({update.store, update.key.slot, update.key.key,
                               std::move(update.value), std::move(old)});
        }
        deliver_changes(
            subscriptions_, changes, [this](std::uint8_t index) { return mod_name(index); }, log_);
        if ((flags & state_snapshot_complete) != 0U && !snapshot_ready_)
        {
            snapshot_ready_ = true;
            emit({ModNetEventKind::ready, 0U, local_player_, {}});
        }
    }

    void handle_message(std::span<const std::uint8_t> message)
    {
        ++counters_.messages_received;
        const auto type = static_cast<MessageType>(message[0]);
        if (type != MessageType::registry && message.size() > limits_.server_message_bytes)
        {
            reject("message exceeds the host message limit");
            return;
        }
        Reader reader{message.subspan(1U)};
        switch (type)
        {
        case MessageType::welcome:
            handle_welcome(reader);
            return;
        case MessageType::registry: {
            ContentRegistry registry;
            if (!decode_registry(reader, limits_.server_values, registry))
            {
                reject("malformed content registry");
                return;
            }
            registry_ = std::move(registry);
            emit({ModNetEventKind::registry, 0U, local_player_, {}});
            return;
        }
        case MessageType::state:
            handle_state(reader);
            return;
        case MessageType::rpc:
            if (!ready_requested_)
            {
                if (early_messages_.size() >= maximum_early_messages)
                {
                    reject("too many messages before the client was ready");
                    return;
                }
                early_messages_.emplace_back(message.begin(), message.end());
                return;
            }
            dispatch_rpc(message);
            return;
        case MessageType::client_ready:
            break;
        }
        reject(std::format("unexpected message type {}", static_cast<unsigned>(type)));
    }

    bool enabled_{};
    ModNetLimits limits_;
    std::vector<std::string> slots_;
    net::ModMessageAssembler assembler_;
    std::uint32_t host_message_bytes_{};
    net::TokenBucket frames_;
    net::TokenBucket bytes_;
    net::TokenBucket messages_;
    bool welcomed_{};
    bool ready_requested_{};
    bool ready_sent_{};
    bool snapshot_ready_{};
    Handle local_player_;
    std::uint32_t next_sequence_{1U};
    std::deque<std::vector<std::uint8_t>> rpc_queue_;
    std::deque<OutFrame> frame_queue_;
    std::size_t queued_bytes_{};
    std::vector<std::vector<std::uint8_t>> sends_;
    std::deque<std::vector<std::uint8_t>> early_messages_;
    std::map<StoreRef, std::map<KeyRef, Value>> stores_;
    std::size_t entry_count_{};
    std::map<std::uint64_t, KeyRef> key_ids_;
    std::optional<ContentRegistry> registry_;
    std::map<std::pair<std::uint8_t, std::string>, Handler> handlers_;
    std::map<SubscriptionId, Subscription> subscriptions_;
    SubscriptionId next_subscription_{1U};
    std::deque<ModNetEvent> events_;
    std::vector<ModNetEventListener> listeners_;
    ModNetLogSink log_;
    ModNetCounters counters_;
};

ModNetClient::ModNetClient(const net::ModManifest &manifest, ModNetLimits limits)
    : impl_{std::make_unique<Impl>(manifest, limits)}
{
}

ModNetClient::~ModNetClient() = default;

bool ModNetClient::enabled() const noexcept
{
    return impl_->enabled_;
}

void ModNetClient::receive(std::span<const std::uint8_t> frame, double now)
{
    Impl &impl = *impl_;
    if (!impl.enabled_)
    {
        ++impl.counters_.frames_rejected;
        return;
    }
    ++impl.counters_.frames_received;
    impl.counters_.bytes_received += frame.size();
    net::ModFrame decoded;
    net::ModFrameError error{};
    if (!net::decode_mod_frame(frame, decoded, error))
    {
        ++impl.counters_.frames_rejected;
        impl.assembler_.reset();
        impl.log({}, LogLevel::warning,
                 std::string{"mod channel: bad frame from host: "} +
                     net::mod_frame_error_name(error));
        return;
    }
    std::vector<std::uint8_t> message;
    const auto result = impl.assembler_.accept(decoded, now, message, error);
    if (result == net::ModMessageAssembler::Result::rejected)
    {
        ++impl.counters_.frames_rejected;
        impl.log({}, LogLevel::warning,
                 std::string{"mod channel: bad fragment from host: "} +
                     net::mod_frame_error_name(error));
        return;
    }
    if (result == net::ModMessageAssembler::Result::complete)
        impl.handle_message(message);
}

void ModNetClient::update(double now)
{
    Impl &impl = *impl_;
    if (!impl.enabled_)
        return;
    static_cast<void>(impl.assembler_.expire(now));
    if (impl.welcomed_ && impl.ready_requested_ && !impl.ready_sent_)
    {
        const std::uint8_t ready[]{static_cast<std::uint8_t>(MessageType::client_ready),
                                   channel_version};
        impl.queue_frames(ready);
        impl.ready_sent_ = true;
    }
    if (impl.ready_sent_)
        while (!impl.rpc_queue_.empty())
        {
            const std::vector<std::uint8_t> message = std::move(impl.rpc_queue_.front());
            impl.rpc_queue_.pop_front();
            impl.queued_bytes_ -= message.size();
            impl.queue_frames(message);
        }
    while (!impl.frame_queue_.empty())
    {
        Impl::OutFrame &frame = impl.frame_queue_.front();
        const double size = static_cast<double>(frame.bytes.size());
        if (impl.frames_.available(now) < 1.0 || impl.bytes_.available(now) < size ||
            (frame.first && impl.messages_.available(now) < 1.0))
            break;
        impl.frames_.consume(1.0, now);
        impl.bytes_.consume(size, now);
        if (frame.first)
            impl.messages_.consume(1.0, now);
        ++impl.counters_.frames_sent;
        impl.counters_.bytes_sent += frame.bytes.size();
        impl.queued_bytes_ -= frame.bytes.size();
        impl.sends_.push_back(std::move(frame.bytes));
        impl.frame_queue_.pop_front();
    }
}

void ModNetClient::set_ready(double now)
{
    Impl &impl = *impl_;
    if (!impl.enabled_ || impl.ready_requested_)
        return;
    impl.ready_requested_ = true;
    std::deque<std::vector<std::uint8_t>> early = std::move(impl.early_messages_);
    impl.early_messages_.clear();
    for (const std::vector<std::uint8_t> &message : early)
        impl.dispatch_rpc(message);
    update(now);
}

std::vector<std::vector<std::uint8_t>> ModNetClient::take_sends()
{
    return std::exchange(impl_->sends_, {});
}

std::vector<ModNetEvent> ModNetClient::take_events()
{
    std::vector<ModNetEvent> events{impl_->events_.begin(), impl_->events_.end()};
    impl_->events_.clear();
    return events;
}

void ModNetClient::add_event_listener(ModNetEventListener listener)
{
    if (listener)
        impl_->listeners_.push_back(std::move(listener));
}

void ModNetClient::set_log_sink(ModNetLogSink sink)
{
    impl_->log_ = std::move(sink);
}

bool ModNetClient::welcomed() const noexcept
{
    return impl_->welcomed_;
}

bool ModNetClient::ready() const noexcept
{
    return impl_->snapshot_ready_;
}

Handle ModNetClient::local_player() const noexcept
{
    return impl_->local_player_;
}

const ModNetCounters &ModNetClient::counters() const noexcept
{
    return impl_->counters_;
}

std::size_t ModNetClient::queued_bytes() const noexcept
{
    return impl_->queued_bytes_;
}

void ModNetClient::on(std::string_view mod_id, std::string_view name, RpcClientHandler handler,
                      std::optional<PayloadSchema> schema)
{
    Impl &impl = *impl_;
    if (!valid_name(name))
        throw ScriptError{"message names use 1-64 letters, digits, or _ . : - /"};
    const std::uint8_t slot_index = impl.slot(mod_id);
    impl.handlers_.insert_or_assign(std::pair<std::uint8_t, std::string>{slot_index, name},
                                    Impl::Handler{std::move(handler), std::move(schema)});
}

void ModNetClient::off(std::string_view mod_id, std::string_view name)
{
    Impl &impl = *impl_;
    std::uint8_t slot_index{};
    try
    {
        slot_index = impl.slot(mod_id);
    }
    catch (const ScriptError &)
    {
        return;
    }
    impl.handlers_.erase(std::pair<std::uint8_t, std::string>{slot_index, name});
}

bool ModNetClient::send(std::string_view mod_id, std::string_view name, const Value &payload)
{
    Impl &impl = *impl_;
    if (!valid_name(name))
        throw ScriptError{"message names use 1-64 letters, digits, or _ . : - /"};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(MessageType::rpc));
    writer.u8(impl.slot(mod_id));
    writer.string(name);
    std::string error;
    if (!encode_value(payload, impl.limits_.client_values, writer.bytes, error))
        throw ScriptError{"net payload: " + error};
    if (writer.bytes.size() > impl.host_message_bytes_)
        throw ScriptError{std::format("net payload exceeds {} bytes", impl.host_message_bytes_)};
    if (!impl.enabled_ || impl.queued_bytes_ + writer.bytes.size() > maximum_client_queue_bytes)
        return false;
    impl.queued_bytes_ += writer.bytes.size();
    impl.rpc_queue_.push_back(std::move(writer.bytes));
    ++impl.counters_.messages_sent;
    return true;
}

const Value *ModNetClient::get(const StoreRef &store, std::string_view mod_id,
                               std::string_view key) const
{
    const Impl &impl = *impl_;
    const auto found = impl.stores_.find(store);
    if (found == impl.stores_.end())
        return nullptr;
    const auto entry = found->second.find(KeyRef{impl.slot(mod_id), std::string{key}});
    return entry == found->second.end() ? nullptr : &entry->second;
}

Map ModNetClient::all(const StoreRef &store, std::string_view mod_id) const
{
    const Impl &impl = *impl_;
    const std::uint8_t slot_index = impl.slot(mod_id);
    Map result;
    if (const auto found = impl.stores_.find(store); found != impl.stores_.end())
        for (const auto &[key, value] : found->second)
            if (key.slot == slot_index)
                result.emplace_back(key.key, value);
    return result;
}

SubscriptionId ModNetClient::subscribe(StateFilter filter, StateListener listener)
{
    const SubscriptionId id = impl_->next_subscription_++;
    impl_->subscriptions_.emplace(id, Subscription{std::move(filter), std::move(listener)});
    return id;
}

void ModNetClient::unsubscribe(SubscriptionId id) noexcept
{
    impl_->subscriptions_.erase(id);
}

std::vector<StoreRef> ModNetClient::stores(StoreScope scope) const
{
    std::vector<StoreRef> result;
    for (const auto &[store, entries] : impl_->stores_)
        if (store.scope == scope)
            result.push_back(store);
    return result;
}

const std::optional<ContentRegistry> &ModNetClient::content_registry() const noexcept
{
    return impl_->registry_;
}

void pump_mod_net_loopback(ModNetServer &server, ModNetClient &client, net::PeerId local_peer,
                           double now, const std::function<void(ModNetSend &&)> &remote,
                           std::size_t max_rounds)
{
    for (std::size_t round = 0U; round < max_rounds; ++round)
    {
        bool moved = false;
        client.update(now);
        for (const std::vector<std::uint8_t> &frame : client.take_sends())
        {
            server.receive(local_peer, frame, now);
            moved = true;
        }
        for (ModNetSend &send : server.take_sends())
        {
            if (send.peer == local_peer)
            {
                client.receive(send.frame, now);
                moved = true;
            }
            else if (remote)
            {
                remote(std::move(send));
            }
        }
        if (!moved)
            return;
    }
}

} // namespace openminerz::modding
