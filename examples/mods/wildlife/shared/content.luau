-- Wildlife: three animals, the meat they drop and how to cook it. The spawn
-- rules here are all it takes for animals to appear: the server rolls them
-- around every player by biome, light and time of day.

-- Eating restores health, and hunger too when a survival mod defines it.
local function eat(player: Handle, item: string, health: number, hunger: number)
	if players.take(player, item, 1) ~= 1 then
		return
	end
	players.heal(player, health)
	if table.find(stats.list(), "hunger") then
		stats.add(player, "hunger", hunger)
	end
end

items.define("raw_venison", {
	name = "Raw Venison", description = "Better cooked. +2 health (+8 hunger with Survival).",
	fallback = "stick", icon = "textures/icons/raw_venison", stack = 16,
	use = function(player)
		eat(player, "wildlife:raw_venison", 2, 8)
	end,
})
items.define("cooked_venison", {
	name = "Cooked Venison", description = "+8 health (+35 hunger with Survival).",
	fallback = "stick", icon = "textures/icons/cooked_venison", stack = 16,
	use = function(player)
		eat(player, "wildlife:cooked_venison", 8, 35)
	end,
})
recipes.add({ result = "cooked_venison", count = 2, ingredients = { { "raw_venison", 2 }, { "coal", 1 } }, category = "Wildlife" })

loot.define("deer", { rolls = 1, entries = { { item = "raw_venison", min = 1, max = 3 } } })
loot.define("rabbit", { rolls = 1, entries = { { item = "raw_venison", min = 1, max = 1 } } })
loot.define("boar", { rolls = 1, entries = { { item = "raw_venison", min = 2, max = 4 } } })

-- Passive animals wander and run from whatever hurts them; a neutral one
-- fights back. `health` is in retail enemy units (a pistol round is about 1).
creatures.define("deer", {
	name = ui.text("deer"), model = "models/deer", behaviour = "passive",
	health = 5, speed = 1.6, run_speed = 6, size = { 0.8, 1.5 }, drops = "deer",
	spawn = { biomes = { "classic", "lagoon", "mountain" }, time = "day", surface = true,
		chance = 0.04, max = 4, group_min = 2, group_max = 3 },
})
creatures.define("rabbit", {
	name = ui.text("rabbit"), model = "models/rabbit", behaviour = "passive",
	health = 1, speed = 2, run_speed = 7, size = { 0.4, 0.45 }, drops = "rabbit",
	spawn = { biomes = { "classic", "lagoon", "desert", "arctic" }, time = "any", surface = true,
		chance = 0.05, max = 3 },
})
creatures.define("boar", {
	name = ui.text("boar"), model = "models/boar", behaviour = "neutral",
	health = 8, damage = 6, speed = 1.4, run_speed = 4.5, attack_range = 1.4, attack_cooldown = 1.5,
	size = { 0.8, 0.9 }, drops = "boar",
	spawn = { biomes = { "classic", "mountain" }, time = "any", surface = true, chance = 0.025, max = 2 },
})
