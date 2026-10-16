-- Food and drink: what grows, what the animals give, what cooks, and the
-- vessels that carry water and milk. Declared once for both sides: the server
-- runs the `use` callbacks, every client draws the names and icons.

local survival = require("./lib/survival")

-- What each meal does: hunger, thirst and health it gives, and the empty
-- container it leaves in your hands.
local MEALS = {
	apple = { hunger = 15, thirst = 4 },
	berries = { hunger = 8, thirst = 3 },
	raw_beef = { hunger = 10 },
	steak = { hunger = 40, health = 6 },
	raw_pork = { hunger = 8 },
	cooked_pork = { hunger = 35, health = 5 },
	raw_chicken = { hunger = 6 },
	cooked_chicken = { hunger = 28, health = 4 },
	raw_mutton = { hunger = 8 },
	cooked_mutton = { hunger = 32, health = 5 },
	stew = { hunger = 55, thirst = 15, health = 10, back = "wilderness:bowl" },
	milk_bucket = { hunger = 10, thirst = 25, back = "wilderness:bucket" },
	water_bucket = { thirst = 45, back = "wilderness:bucket" },
	water_bottle = { thirst = 25, back = "wilderness:bottle" },
	waterskin_full = { thirst = 35, back = "wilderness:waterskin" },
}

-- Raw meat and what it cooks into, on a stone oven or with coal.
local COOKED = {
	raw_beef = "steak", raw_pork = "cooked_pork", raw_chicken = "cooked_chicken", raw_mutton = "cooked_mutton",
}
local RAW = { "raw_beef", "raw_pork", "raw_chicken", "raw_mutton" } -- the same, in a fixed order

-- A cow gives milk again after this much of a day (a day is 16 minutes).
local MILK_WAIT = 0.05

-- "Crisp and sweet. +15 hunger, +4 thirst."
local function describe(name: string): string
	local meal = MEALS[name]
	local text = ui.text("about." .. name)
	if meal == nil then
		return text
	end
	local gains = {}
	for _, stat in { "hunger", "thirst", "health" } do
		if meal[stat] ~= nil then
			table.insert(gains, ui.text("gain." .. stat, { meal[stat] }))
		end
	end
	return text .. " " .. table.concat(gains, ", ") .. "."
end

-- Eat (or drink) it; raw meat used on a stone oven cooks instead.
local function eat(name: string)
	return function(player: Handle, _key: string, target: any)
		local key = "wilderness:" .. name
		if COOKED[name] ~= nil and target ~= nil and target.block == "wilderness:stone_oven" then
			if players.take(player, key, 1) == 1 then
				players.give(player, "wilderness:" .. COOKED[name], 1)
				survival.cue(player, "sizzle", target.position)
			end
			return
		end
		survival.consume(player, key, MEALS[name])
	end
end

-- An empty vessel: used at water it fills; a bucket used at a cow is milked.
local function vessel(name: string, full: string)
	return function(player: Handle, _key: string, target: any)
		local key = "wilderness:" .. name
		if name == "bucket" then
			local cow = survival.aimed_creature(player, target and target.look, "wilderness:cow")
			if cow ~= nil then
				local info = entities.get(cow)
				local data = if type(info.data) == "table" then info.data else {}
				local now = time.day() + time.of_day()
				if data.milked ~= nil and now - data.milked < MILK_WAIT then
					players.message(player, ui.text("milk.wait"))
					return
				end
				if players.take(player, key, 1) == 1 then
					players.give(player, "wilderness:milk_bucket", 1)
					data.milked = now
					entities.set_data(cow, data)
					survival.cue(player, "milk", info.position)
				end
				return
			end
		end
		local water = survival.aimed_water(target) or survival.water_near(player, 1)
		if water == nil then
			players.message(player, ui.text("fill.no_water"))
			return
		end
		if players.take(player, key, 1) == 1 then
			players.give(player, "wilderness:" .. full, 1)
			survival.cue(player, "fill", water)
		end
	end
end

-- Every item, in a fixed order. `fallback` is the retail item a world opened
-- without the mod holds (a stick or a compass, which do nothing on their own);
-- `fills` is what an empty vessel becomes at water.
local ITEMS = {
	{ name = "apple", fallback = "stick", stack = 32, icon = "textures/icons/apple" },
	{ name = "berries", fallback = "stick", stack = 32, icon = "textures/icons/berries" },
	{ name = "raw_beef", fallback = "stick", stack = 16, icon = "textures/icons/raw_beef" },
	{ name = "steak", fallback = "stick", stack = 16, icon = "textures/icons/steak" },
	{ name = "raw_pork", fallback = "stick", stack = 16, icon = "textures/icons/raw_pork" },
	{ name = "cooked_pork", fallback = "stick", stack = 16, icon = "textures/icons/cooked_pork" },
	{ name = "raw_chicken", fallback = "stick", stack = 16, icon = "textures/icons/raw_chicken" },
	{ name = "cooked_chicken", fallback = "stick", stack = 16, icon = "textures/icons/cooked_chicken" },
	{ name = "raw_mutton", fallback = "stick", stack = 16, icon = "textures/icons/raw_mutton" },
	{ name = "cooked_mutton", fallback = "stick", stack = 16, icon = "textures/icons/cooked_mutton" },
	{ name = "bowl", fallback = "stick", stack = 16, icon = "textures/icons/bowl" },
	{ name = "stew", fallback = "stick", stack = 1, icon = "textures/icons/stew" },
	{ name = "bucket", fallback = "compass", stack = 1, icon = "textures/icons/bucket", fills = "water_bucket" },
	{ name = "water_bucket", fallback = "compass", stack = 1, icon = "textures/icons/water_bucket" },
	{ name = "milk_bucket", fallback = "compass", stack = 1, icon = "textures/icons/milk_bucket" },
	{ name = "bottle", fallback = "stick", stack = 16, icon = "textures/icons/bottle", fills = "water_bottle" },
	{ name = "water_bottle", fallback = "stick", stack = 16, icon = "textures/icons/water_bottle" },
	{ name = "waterskin", fallback = "compass", stack = 1, icon = "textures/icons/waterskin", fills = "waterskin_full" },
	{ name = "waterskin_full", fallback = "compass", stack = 1, icon = "textures/icons/waterskin_full" },
	{ name = "leather", fallback = "stick", stack = 32, icon = "textures/icons/leather" },
	{ name = "feather", fallback = "stick", stack = 32, icon = "textures/icons/feather" },
	{ name = "wool", fallback = "stick", stack = 32, icon = "textures/icons/wool" },
}
-- How each is held (the hand animations): a meal goes to the mouth (eaten,
-- or tipped up and drunk) for as long as its cooldown; everything else is
-- held out in front, upright, so a bucket or a bottle reads as one.
for _, entry in ITEMS do
	local meal = MEALS[entry.name]
	local use, hold, cooldown = nil, "block", nil
	if entry.fills ~= nil then
		use, cooldown = vessel(entry.name, entry.fills), 0.6
	elseif meal ~= nil then
		use, cooldown = eat(entry.name), 1.2
		hold = if (meal.thirst or 0) > (meal.hunger or 0) then "drink" else "eat"
	end
	items.define(entry.name, {
		name = ui.text("item." .. entry.name), description = describe(entry.name),
		fallback = entry.fallback, icon = entry.icon, stack = entry.stack,
		hold = hold, cooldown = cooldown, use = use,
	})
end

-- A stone oven: raw meat used on it cooks, no coal needed, and it glows.
blocks.define("stone_oven", {
	name = ui.text("item.stone_oven"), fallback = "rock",
	texture = "textures/stone_oven_side",
	faces = { top = "textures/stone_oven_top", bottom = "textures/stone_oven_top" },
	light = 10, drops = "stone_oven",
})
items.define("stone_oven", {
	name = ui.text("item.stone_oven"), description = ui.text("about.stone_oven"),
	fallback = "rock_block", places = "stone_oven", icon = "textures/icons/stone_oven", stack = 8,
})
-- Sheep's wool pressed into blocks for building (snow underneath).
blocks.define("wool_block", { name = ui.text("item.wool_block"), fallback = "snow", texture = "textures/wool_block" })
items.define("wool_block", {
	name = ui.text("item.wool_block"), description = ui.text("about.wool_block"),
	fallback = "snow", places = "wool_block", icon = "textures/icons/wool_block", stack = 64,
})

-- What the animals drop (shared/animals.luau names these tables): meat from
-- the creature's own, and leather, wool or feathers rolled by
-- server/animals.luau.
loot.define("cow", { rolls = 1, entries = { { item = "raw_beef", min = 1, max = 3 } } })
loot.define("pig", { rolls = 1, entries = { { item = "raw_pork", min = 1, max = 3 } } })
loot.define("sheep", { rolls = 1, entries = { { item = "raw_mutton", min = 1, max = 2 } } })
loot.define("chicken", { rolls = 1, entries = { { item = "raw_chicken", min = 1, max = 1 } } })
loot.define("cow_hide", { rolls = 1, entries = { { item = "leather", min = 1, max = 2 } } })
loot.define("sheep_fleece", { rolls = 1, entries = { { item = "wool", min = 1, max = 3 } } })
loot.define("chicken_feathers", { rolls = 1, entries = { { item = "feather", min = 1, max = 3 } } })

-- Leaves now and then hide an apple; spruce boughs, berries (server/forage.luau).
loot.define("leaf_fruit", { rolls = 1, entries = { { item = "apple", weight = 1, min = 1, max = 1 } } })
loot.define("bough_berries", { rolls = 1, entries = { { item = "berries", weight = 1, min = 1, max = 3 } } })

-- Recipes from retail materials, on the crafting screen.
local CATEGORY = "Wilderness"
recipes.add({ result = "bucket", count = 1, ingredients = { { "iron", 3 } }, category = CATEGORY })
recipes.add({ result = "bottle", count = 3, ingredients = { { "sand_block", 2 }, { "coal", 1 } }, category = CATEGORY })
recipes.add({ result = "waterskin", count = 1, ingredients = { { "leather", 2 }, { "stick", 1 } },
	category = CATEGORY })
recipes.add({ result = "bowl", count = 4, ingredients = { { "wood_block", 1 } }, category = CATEGORY })
recipes.add({ result = "stone_oven", count = 1, ingredients = { { "rock_block", 6 }, { "coal", 2 } },
	category = CATEGORY })
recipes.add({ result = "stew", count = 1, ingredients = { { "bowl", 1 }, { "steak", 1 }, { "apple", 1 } },
	category = CATEGORY })
recipes.add({ result = "wool_block", count = 1, ingredients = { { "wool", 4 } }, category = CATEGORY })
for _, raw in RAW do
	recipes.add({ result = COOKED[raw], count = 2, ingredients = { { raw, 2 }, { "coal", 1 } }, category = CATEGORY })
end
