// The starting points `openminerz_mod_sdk new` writes (templates.hpp).

#include "templates.hpp"

namespace openminerz::mod_sdk
{

namespace
{

const std::vector<TemplateFile> client_template{
    {"openminerz.mod", R"(# A client mod: presentation only, so it works on any server (vanilla too).
id={{id}}
version=0.1.0
scope=client
protocol=both
order=500
name={{id}}
author=you
description=Describe your mod here.
api=1
)"},
    {"client/main.luau", R"(-- Runs on this computer only. Client mods may restyle the interface, add HUD
-- widgets, lights, sounds and effects, and read the world, but never change
-- the game: that is why they work on any server.

-- Recolour a few of the game's inks (ui.colors() lists every name).
ui.theme({
	colors = { ["menu.focus"] = "#7fd0ff", ["menu.accent"] = "#7fd0ff" },
})

-- A small HUD panel. Units are a 1280x720 canvas that scales with the window.
local panel = ui.add_hud({
	kind = "panel", anchor = "top_left", x = 16, y = 16, padding = 6, background = "plate",
	children = { { kind = "label", id = "text", text = "" } },
})
local label = ui.find(panel, "text")

timer.every(1, function()
	ui.set(label, { text = ui.text("hud.day", { day = time.day() }) })
end)

-- A key players can rebind on Add-Ons > Add-On Key Bindings.
input.bind("hello", "F7", function()
	ui.toast(ui.text("toast.title"), ui.text("toast.text"))
end, { label = ui.text("key.hello") })

events.on("world.loaded", function()
	log.info("{{id}} is running")
end)
)"},
    {"lang/en.lang", R"(# English strings: ui.text("key", {placeholders}).
hud.day = Day {day}
toast.title = Hello from {{id}}
toast.text = Edit client/main.luau and use /mods reload to see changes.
key.hello = Say hello
)"},
    {"README.txt", R"(# {{id}}

A client mod for OpenMinerZ (made with `openminerz_mod_sdk new {{id}} --template client`).

- `client/main.luau` runs on this computer when a world opens.
- `lang/en.lang` holds the strings; `assets/` is for textures, sounds and fonts.

Check it with `openminerz_mod_sdk validate .`, try it with `openminerz_mod_sdk run .`,
then copy the folder into `%LOCALAPPDATA%\OpenMinerZ\Mods\`.
)"},
};

const std::vector<TemplateFile> server_template{
    {"openminerz.mod", R"(# A server mod: gameplay. Everyone in a session runs it; OpenMinerZ players
# joining a host download it.
id={{id}}
version=0.1.0
scope=server
protocol=both
order=500
name={{id}}
author=you
description=Describe your mod here.
api=1
)"},
    {"shared/content.luau", R"(-- Runs on both sides: definitions the server and every client need.

-- A mod item. `fallback` is the retail item vanilla clients and the save see.
items.define("lucky_coin", {
	name = "Lucky Coin",
	description = "Use it to heal a little.",
	fallback = "gold",
	stack = 16,
	use = function(player) -- runs on the server
		if players.take(player, "{{id}}:lucky_coin", 1) == 1 then
			players.heal(player, 10)
		end
	end,
})

recipes.add({ result = "lucky_coin", count = 2, ingredients = { { "gold", 1 } }, category = "{{id}}" })
)"},
    {"server/main.luau", R"(-- Runs on the server only (offline, or on the host). This is where the
-- game's rules live; clients cannot change anything here.

events.on("player.joined", function(event)
	players.give(event.player, "{{id}}:lucky_coin", 1)
	players.message(event.player, ui.text("welcome", { name = event.name }))
end)

-- /{{id}} in chat.
commands.register("{{id}}", "Says how long the world has been running", function(player)
	return ui.text("uptime", { seconds = math.floor(replicated.get("world", "uptime") or 0) })
end)

-- Server-owned state every client can read.
local uptime = 0
events.on("tick", function(event)
	uptime += event.dt
	if math.floor(uptime) ~= math.floor(uptime - event.dt) then
		replicated.set("world", "uptime", math.floor(uptime))
	end
end)

-- A message from a client. The sender comes from the connection; check
-- everything else it sends.
net.on("ping", function(player, payload)
	net.send_to(player, "pong", { text = payload.text })
end, { text = { type = "string", max = 64 } })
)"},
    {"client/main.luau", R"(-- Runs on each player's computer: presentation, and messages to the server.

local panel = ui.add_hud({
	kind = "panel", anchor = "top_left", x = 16, y = 16, padding = 6, background = "plate",
	children = { { kind = "label", id = "text", text = "" } },
})
local label = ui.find(panel, "text")

replicated.on_change("world", "uptime", function(value)
	ui.set(label, { text = ui.text("hud.uptime", { seconds = value or 0 }) })
end)

net.on("pong", function(payload)
	ui.toast(ui.text("pong"), payload.text)
end)

events.on("net.ready", function()
	net.send("ping", { text = "hello" })
end)
)"},
    {"lang/en.lang", R"(# English strings: ui.text("key", {placeholders}).
welcome = Welcome, {name}! Here is a lucky coin.
uptime = This world has been running for {seconds} seconds.
hud.uptime = World time: {seconds} s
pong = The server answered
)"},
    {"README.txt", R"(# {{id}}

A server mod for OpenMinerZ (made with `openminerz_mod_sdk new {{id}} --template server`).

- `shared/` runs on both sides: definitions (items, blocks, recipes, stats, ...).
- `server/` runs only where the game is simulated: the rules.
- `client/` runs on each player's computer: HUD, sounds, messages to the server.
- `lang/en.lang` holds the strings; `assets/` is for textures, models and sounds.

Check it with `openminerz_mod_sdk validate .`, try it with `openminerz_mod_sdk run . --chat "/{{id}}"`,
then copy the folder into `%LOCALAPPDATA%\OpenMinerZ\Mods\`.
)"},
};

} // namespace

const std::vector<TemplateFile> *template_files(std::string_view kind)
{
    if (kind == "client")
        return &client_template;
    if (kind == "server")
        return &server_template;
    return nullptr;
}

} // namespace openminerz::mod_sdk
