// ModNetGameplaySync: the gameplay service's messages over the mod channel's
// engine slot. See openminerz/modding/gameplay_net.hpp.

#include "openminerz/modding/gameplay_net.hpp"

namespace openminerz::modding::gameplay
{

ModNetGameplaySync::~ModNetGameplaySync()
{
    attach(nullptr, nullptr);
}

void ModNetGameplaySync::attach(ModNetServer *server, ModNetClient *client)
{
    if (server_ != nullptr && server_ != server)
        server_->off({}, gameplay_message_name);
    if (client_ != nullptr && client_ != client)
        client_->off({}, gameplay_message_name);
    server_ = server;
    client_ = client;
    if (server_ != nullptr)
        server_->on({}, gameplay_message_name, [this](Handle player, const Value &payload) {
            // The sender is the channel's attribution of the frame, never a
            // field of the payload.
            if (const auto gid = service_.player_gid(player))
                service_.receive_from_client(*gid, payload);
        });
    if (client_ != nullptr)
        client_->on({}, gameplay_message_name,
                    [this](const Value &payload) { service_.receive_from_server(payload); });
}

void ModNetGameplaySync::to_player(std::uint8_t gid, const Value &message, bool reliable)
{
    static_cast<void>(reliable); // the channel is reliable-ordered throughout
    if (server_ == nullptr)
        return;
    const Handle player = service_.player_handle(gid);
    try
    {
        if (!player.valid() || !server_->send_to(player, {}, gameplay_message_name, message))
            ++dropped_;
    }
    catch (const ScriptError &)
    {
        ++dropped_;
    }
}

void ModNetGameplaySync::to_all(const Value &message, bool reliable)
{
    static_cast<void>(reliable);
    if (server_ == nullptr)
        return;
    // (Integration) A mod entity that is gone takes the replicated values
    // scripts kept on it (replicated.set(entity, ...)) with it.
    if (const Value *type = message.field("t"); type != nullptr && type->type() == ValueType::string &&
                                                type->as_string() == "ent-")
        if (const Value *id = message.field("id"); id != nullptr && id->type() == ValueType::integer)
            server_->drop_store(
                StoreRef::entity(Handle{HandleKind::entity, static_cast<std::uint64_t>(id->as_integer())}));
    // Everyone but this process's own client, which the loopback served.
    const Handle local = service_.local_player_handle();
    for (const Handle player : server_->ready_players())
    {
        if (player == local)
            continue;
        try
        {
            if (!server_->send_to(player, {}, gameplay_message_name, message))
                ++dropped_;
        }
        catch (const ScriptError &)
        {
            ++dropped_;
        }
    }
}

void ModNetGameplaySync::to_server(const Value &message)
{
    if (client_ == nullptr)
        return;
    try
    {
        if (!client_->send({}, gameplay_message_name, message))
            ++dropped_;
    }
    catch (const ScriptError &)
    {
        ++dropped_;
    }
}

} // namespace openminerz::modding::gameplay
