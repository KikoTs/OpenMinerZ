-- Farm animals: cows, pigs, sheep and chickens. They graze in herds by day on
-- grassy land (retail's own and Wilderness's meadows and pine forests), never
-- attack, and bolt - the whole herd with them - when one of them is hurt.
-- Their spawn rules are all it takes for them to appear around players; the
-- server rolls them by biome and time of day.

-- Hurt: the animal runs from whatever hurt it, and its herd runs with it.
local function bolt(animal: Handle, info: any)
	local from = info.attacker or info.position
	for _, member in ai.pack(animal) do
		if member ~= animal then
			ai.set_state(member, "bolt")
			ai.flee(member, from, { seconds = 5 })
		end
	end
	ai.flee(animal, from, { seconds = 5 })
	return "bolt"
end

-- The animal its herd follows (the first of its pack), or nil for a leader
-- or a loner.
local function herd_leader(animal: Handle, info: any): Handle?
	if info.leader or info.pack_size < 2 then
		return nil
	end
	local leader = ai.pack(animal)[1]
	return if leader ~= animal and entities.exists(leader) then leader else nil
end

-- Grazing animals share one brain: stand and graze (the idle clip lowers the
-- head to the grass), now and then stroll a few blocks, keep near the herd,
-- and bolt when hurt.
brains.define("grazer", {
	initial = "graze",
	states = {
		graze = {
			enter = function(animal, _info)
				ai.stop(animal)
			end,
			tick = function(animal, info)
				-- Strayed from the herd: back to its leader.
				local leader = herd_leader(animal, info)
				if leader ~= nil and vector.magnitude(entities.get(leader).position - info.position) > 9 then
					return "regroup"
				end
				if info.time > 3 and util.random() < 0.05 then
					return "stroll"
				end
				return nil
			end,
			damaged = bolt,
		},
		stroll = {
			enter = function(animal, _info)
				ai.wander(animal, { radius = 6 })
			end,
			tick = function(_animal, info)
				return if info.time > 4 + util.random() * 3 then "graze" else nil
			end,
			damaged = bolt,
		},
		regroup = {
			enter = function(animal, info)
				local leader = herd_leader(animal, info)
				if leader ~= nil then
					ai.move_to(animal, entities.get(leader).position)
				end
			end,
			tick = function(animal, info)
				local leader = herd_leader(animal, info)
				if leader == nil or info.time > 10 then
					return "graze"
				end
				return if vector.magnitude(entities.get(leader).position - info.position) < 4 then "graze" else nil
			end,
			damaged = bolt,
		},
		bolt = {
			tick = function(_animal, info)
				return if info.time > 5 then "graze" else nil
			end,
		},
	},
})

-- Grassy land: retail's own, Wilderness's alpine meadows and pine forests,
-- and the lake shores.
local PASTURE = { "classic", "wilderness:alpine_meadow", "wilderness:lake_valley" }
local WOODLAND = { "classic", "wilderness:pine_forest", "wilderness:alpine_meadow" }

-- `health` is in retail enemy units (a pistol round is about 1).
creatures.define("cow", {
	name = ui.text("animal.cow"), model = "models/cow", behaviour = "passive", brain = "grazer",
	health = 8, speed = 1.1, run_speed = 4.2, size = { 0.95, 1.5 }, drops = "cow",
	spawn = { biomes = PASTURE, time = "day", surface = true, chance = 0.08, max = 6, group_min = 3, group_max = 5 },
})
creatures.define("sheep", {
	name = ui.text("animal.sheep"), model = "models/sheep", behaviour = "passive", brain = "grazer",
	health = 6, speed = 1.2, run_speed = 4.4, size = { 0.9, 1.3 }, drops = "sheep",
	spawn = { biomes = PASTURE, time = "day", surface = true, chance = 0.07, max = 5, group_min = 2, group_max = 4 },
})
creatures.define("pig", {
	name = ui.text("animal.pig"), model = "models/pig", behaviour = "passive", brain = "grazer",
	health = 6, speed = 1.3, run_speed = 4.6, size = { 0.75, 0.9 }, drops = "pig",
	spawn = { biomes = WOODLAND, time = "day", surface = true, chance = 0.07, max = 4, group_min = 2, group_max = 3 },
})
creatures.define("chicken", {
	name = ui.text("animal.chicken"), model = "models/chicken", behaviour = "passive", brain = "grazer",
	health = 2, speed = 1.4, run_speed = 4.2, size = { 0.45, 0.75 }, drops = "chicken",
	spawn = { biomes = WOODLAND, time = "day", surface = true, chance = 0.06, max = 5, group_min = 2, group_max = 4 },
})
