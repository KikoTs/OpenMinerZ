-- Everything the Survival mod adds, declared once for both sides: the server
-- needs it to run the rules, every client to draw it (HUD bars, item names
-- and icons, the water block's texture).

local needs = require("./lib/needs")

-- Settings players change on Add-Ons > Survival (stored per machine; the
-- host's values are the ones that count in a hosted world).
mod.config.define("hunger_rate", {
	type = "number", default = 1, min = 0.25, max = 4, step = 0.25,
	label = "Hunger speed", description = "1 empties a full stomach in about 12 minutes.",
})
mod.config.define("thirst_rate", {
	type = "number", default = 1, min = 0.25, max = 4, step = 0.25,
	label = "Thirst speed", description = "1 empties a full canteen's worth in about 8 minutes.",
})
mod.config.define("empty_damage", {
	type = "integer", default = 3, min = 0, max = 20,
	label = "Damage when empty", description = "Health lost every second while hunger or thirst is at zero.",
})

-- Hunger and thirst: 100 is full. `decay` drains them every second on the
-- server; `hud` asks every client to draw a bar.
local function define_stats()
	stats.define("hunger", {
		min = 0, max = 100, initial = 100, persist = true,
		decay = 100 / (12 * 60) * mod.config.get("hunger_rate"),
		hud = { label = ui.text("hud.hunger"), color = "#d08c3c" },
	})
	stats.define("thirst", {
		min = 0, max = 100, initial = 100, persist = true,
		decay = 100 / (8 * 60) * mod.config.get("thirst_rate"),
		hud = { label = ui.text("hud.thirst"), color = "#3c8cdc" },
	})
end
define_stats()
events.on("config.changed", function(event)
	if event.mod == mod.id() then
		define_stats()
	end
end)

-- Fresh water. Players without this mod (and the retail save) see ice.
blocks.define("fresh_water", {
	name = "Fresh Water",
	fallback = "ice",
	texture = "textures/fresh_water",
	drops = "none",
})

-- Food and water. Each `fallback` is the retail item vanilla clients and the
-- save hold; `use` runs on the server instead of that item's own action.
items.define("berries", {
	name = "Wild Berries", description = "Tart but filling. +12 hunger, +4 thirst.",
	fallback = "stick", icon = "textures/icons/berries", stack = 32,
	use = function(player)
		needs.consume(player, "survival:berries", 12, 4)
	end,
})
items.define("ration", {
	name = "Trail Ration", description = "Dense and dry. +45 hunger, -5 thirst.",
	fallback = "stick", icon = "textures/icons/ration", stack = 16,
	use = function(player)
		needs.consume(player, "survival:ration", 45, -5)
	end,
})
items.define("canteen", {
	name = "Canteen (empty)", description = "Use it on fresh water to drink and fill it.",
	fallback = "compass", icon = "textures/icons/canteen", stack = 1,
	-- `target` is the block the player's crosshair is on (nil when none).
	use = function(player, _key, target)
		local aimed = target ~= nil and target.block == "survival:fresh_water"
		if not aimed and not needs.near_water(player) then
			players.message(player, ui.text("canteen.no_water"))
			return
		end
		if players.take(player, "survival:canteen", 1) == 1 then
			players.give(player, "survival:canteen_full", 1)
			stats.add(player, "thirst", 25)
			net.send_to(player, "consumed", { item = "survival:canteen" })
			players.message(player, ui.text("canteen.filled"))
		end
	end,
})
items.define("canteen_full", {
	name = "Canteen", description = "Fresh water. +40 thirst; gives the empty canteen back.",
	fallback = "compass", icon = "textures/icons/canteen_full", stack = 1,
	use = function(player)
		if needs.consume(player, "survival:canteen_full", 0, 40) then
			players.give(player, "survival:canteen", 1)
		end
	end,
})

-- Leaves sometimes hide berries (server/rules.luau rolls this table).
loot.define("leaf_litter", {
	rolls = 1,
	entries = { { item = "berries", weight = 1, min = 1, max = 3 } },
})

-- Crafted through the server, from the crafting screen (listed with the
-- tools and materials their fallbacks are) or recipes.request.
recipes.add({ result = "canteen", count = 1, ingredients = { { "iron", 2 }, { "stick", 1 } }, category = "Survival" })
recipes.add({ result = "ration", count = 2, ingredients = { { "berries", 4 }, { "coal", 1 } }, category = "Survival" })
