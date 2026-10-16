-- One landmark per Frontier biome, placed as chunks are generated near
-- players. They are ordinary world edits made by the host, so joined players
-- receive them as chunk data like any other edit (and the save keeps them).

structures.define("ranger_hut", {
	palette = { ["#"] = "wood", L = "frontier:redwood_bark", g = "glass_basic", ["."] = "air", t = "torch" },
	origin = { x = 2, y = 0, z = 2 },
	layers = {
		{ "L###L", "#####", "#####", "#####", "L###L" },
		{ "L#g#L", "#...#", "g...g", "#...#", "L#.#L" },
		{ "L###L", "#..t#", "#...#", "#...#", "L#.#L" },
		{ "LLLLL", "L###L", "L###L", "L###L", "LLLLL" },
		{ "     ", " ### ", " ### ", " ### ", "     " },
	},
})

structures.define("salt_cairn", {
	palette = { s = "frontier:rock_salt", l = "lantern" },
	origin = { x = 1, y = 0, z = 1 },
	layers = {
		{ "sss", "sss", "sss" },
		{ " s ", "sss", " s " },
		{ "   ", " s ", "   " },
		{ "   ", " s ", "   " },
		{ "   ", " l ", "   " },
	},
})

structures.define("ember_shrine", {
	palette = { c = "frontier:cinder_stone", b = "frontier:basalt", e = "frontier:ember_ore", l = "lantern", ["."] = "air" },
	origin = { x = 2, y = 0, z = 2 },
	layers = {
		{ "bbbbb", "bcccb", "bcecb", "bcccb", "bbbbb" },
		{ "b...b", ".....", "..l..", ".....", "b...b" },
		{ "b...b", ".....", ".....", ".....", "b...b" },
		{ "bcccb", "c...c", "c...c", "c...c", "bcccb" },
	},
})

-- Which landmark a biome gets, and how often (per chunk).
local LANDMARKS = {
	["frontier:redwood"] = { structure = "ranger_hut", chance = 0.012 },
	["frontier:salt_flats"] = { structure = "salt_cairn", chance = 0.02 },
	["frontier:ashlands"] = { structure = "ember_shrine", chance = 0.012 },
}

worldgen.enable({ radius = 10 })

events.on("worldgen.chunk", function(chunk)
	local landmark = LANDMARKS[chunk.biome]
	if landmark == nil or worldgen.random(chunk.cx, chunk.cz, 7) > landmark.chance then
		return
	end
	local x = chunk.x + 4 + math.floor(worldgen.random(chunk.cx, chunk.cz, 8) * 8)
	local z = chunk.z + 4 + math.floor(worldgen.random(chunk.cx, chunk.cz, 9) * 8)
	local ground = world.surface_y(x, z)
	if ground == nil then
		return
	end
	-- Not in a lava pool or on the frozen brine.
	local top = world.get_block(vector.create(x, ground, z))
	if top == "ice" or top == "surface_lava" then
		return
	end
	local turn = 90 * math.floor(worldgen.random(chunk.cx, chunk.cz, 10) * 4)
	structures.place(landmark.structure, vector.create(x, ground + 1, z), { rotation = turn })
	log.debug("landmark", landmark.structure, "at", x, ground + 1, z)
end)
