-- Hunger and thirst: two stats every player has, kept by the server and
-- replicated to that player, whose client draws them next to the game's own
-- health (client/hud.luau). The rules that go with them (damage when empty, a
-- half-full start after dying) are in server/needs.luau.

-- Settings players change on Add-Ons > Wilderness (stored per machine; the
-- host's values are the ones that count in a hosted world).
mod.config.define("hunger_minutes", {
	type = "number", default = 20, min = 5, max = 60, step = 5,
	label = ui.text("setting.hunger"), description = ui.text("setting.hunger_hint"),
})
mod.config.define("thirst_minutes", {
	type = "number", default = 14, min = 5, max = 60, step = 1,
	label = ui.text("setting.thirst"), description = ui.text("setting.thirst_hint"),
})
mod.config.define("empty_damage", {
	type = "integer", default = 2, min = 0, max = 10,
	label = ui.text("setting.damage"), description = ui.text("setting.damage_hint"),
})
mod.config.define("starter_kit", {
	type = "boolean", default = true,
	label = ui.text("setting.kit"), description = ui.text("setting.kit_hint"),
})

-- 100 is full; `decay` drains a stat every second on the server. No `hud`
-- field: this mod draws its own bars.
local function define_stats()
	stats.define("hunger", {
		min = 0, max = 100, initial = 100, persist = true,
		decay = 100 / (mod.config.get("hunger_minutes") * 60),
	})
	stats.define("thirst", {
		min = 0, max = 100, initial = 100, persist = true,
		decay = 100 / (mod.config.get("thirst_minutes") * 60),
	})
end
define_stats()
events.on("config.changed", function(event)
	if event.mod == mod.id() then
		define_stats()
	end
end)
