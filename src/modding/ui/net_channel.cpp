// The UI service's "omz.ui" messages over the mod channel (modding/net.hpp).

#include "openminerz/modding/net.hpp"
#include "openminerz/modding/ui.hpp"

namespace openminerz::modding::ui
{

ModNetUiChannel::ModNetUiChannel(UiService &service, ModNetServer *server, ModNetClient *client,
                                 std::function<double()> clock)
    : server_(server), client_(client)
{
    if (server_ != nullptr && server_->enabled())
        server_->on("", ui_channel_name,
                    [&service, clock = std::move(clock)](Handle player, const Value &payload) {
                        service.receive_from_client(player, payload, clock ? clock() : 0.0);
                    });
    if (client_ != nullptr && client_->enabled())
        client_->on("", ui_channel_name, [&service](const Value &payload) { service.receive_from_server(payload); });
}

ModNetUiChannel::~ModNetUiChannel()
{
    if (server_ != nullptr && server_->enabled())
        server_->off("", ui_channel_name);
    if (client_ != nullptr && client_->enabled())
        client_->off("", ui_channel_name);
}

void ModNetUiChannel::send_to_server(const Value &message)
{
    if (client_ != nullptr && client_->enabled())
        (void)client_->send("", ui_channel_name, message);
}

void ModNetUiChannel::send_to_player(Handle player, const Value &message)
{
    if (server_ != nullptr && server_->enabled())
        (void)server_->send_to(player, "", ui_channel_name, message);
}

std::vector<Handle> ModNetUiChannel::players() const
{
    return server_ != nullptr && server_->enabled() ? server_->ready_players() : std::vector<Handle>{};
}

} // namespace openminerz::modding::ui
