-- Wilderness: getting around the land to try it out (the host's commands).
--
--   /biome            where you are: the biome under your feet and the ground
--   /goto <biome>     the nearest dry column of that biome, and a landing on it
--   /goto <x> <z>     the ground at a place
--
-- The search asks the world generator itself (terrain.info) ring by ring
-- around the player, a few thousand columns a tick, so nothing has to be
-- loaded or generated to find a place.

local SEA = 1
local STEP = 24 -- blocks between samples on a ring, and between rings
local REACH = 7000 -- the mountain ring is 1,600 to 2,400 blocks from the centre
local PER_TICK = 2500

local KEYS = {
	"wilderness:jagged_peaks", "wilderness:stony_slopes", "wilderness:pine_forest",
	"wilderness:alpine_meadow", "wilderness:lake_valley",
	"classic", "lagoon", "desert", "arctic", "descent",
}
local ALIASES = {
	peaks = "wilderness:jagged_peaks", peak = "wilderness:jagged_peaks",
	mountain = "wilderness:jagged_peaks", mountains = "wilderness:jagged_peaks",
	slopes = "wilderness:stony_slopes",
	forest = "wilderness:pine_forest", pines = "wilderness:pine_forest", taiga = "wilderness:pine_forest",
	meadow = "wilderness:alpine_meadow", foothills = "wilderness:alpine_meadow",
	lake = "wilderness:lake_valley", lakes = "wilderness:lake_valley", valley = "wilderness:lake_valley",
	plains = "classic", hell = "descent",
}

-- "Jagged Peaks", "jagged_peaks", "wilderness:jagged_peaks", "peaks" -> the key.
local function biome_key(words)
	local name = string.lower(table.concat(words, "_"))
	name = string.gsub(name, "[%s%-]", "_")
	if ALIASES[name] then
		return ALIASES[name]
	end
	for _, key in KEYS do
		if key == name or key == "wilderness:" .. name then
			return key
		end
	end
	return nil
end

local function biome_name(key)
	return (string.gsub(string.gsub(key, "^wilderness:", ""), "_", " "))
end

local function allowed(player)
	-- In a session the host decides who travels: only its own player may.
	return #players.list() <= 1 or players.get(player)["local"] == true
end

local function land(player, x, z, height)
	players.teleport(player, vector.create(x + 0.5, height + 1, z + 0.5))
end

commands.register("biome", ui.text("command.biome"), function(player)
	local here = players.get(player).position
	local info = terrain.info(here.x, here.z)
	if info == nil then
		return ui.text("terrain.retail")
	end
	local key = world.biome(here.x, here.z)
	local distance = math.floor(math.sqrt(here.x * here.x + here.z * here.z) + 0.5)
	return ui.text("biome.here", { biome_name(key), key, math.floor(info.height), distance })
end)

-- One search per player at a time.
local searches = {}

local function search(player, key)
	local origin = players.get(player).position
	local ox, oz = math.floor(origin.x), math.floor(origin.z)
	local ring, index, count = 0, 0, 1
	local handle
	handle = timer.every(0.05, function()
		for _ = 1, PER_TICK do
			local radius = ring * STEP
			local angle = (index / count) * math.pi * 2
			local x = ox + math.floor(math.cos(angle) * radius)
			local z = oz + math.floor(math.sin(angle) * radius)
			-- world.biome names retail's biomes where the layout reuses retail
			-- terrain ("lagoon"), and the Wilderness's own by key.
			if world.biome(x, z) == key then
				local info = terrain.info(x, z)
				-- Firmly inside the biome, on dry ground under the top of the world.
				if info ~= nil and info.height >= SEA + 1 and info.height <= 60 and
					info.weights[1] ~= nil and info.weights[1][2] >= 0.7 then
					timer.cancel(handle)
					searches[player] = nil
					land(player, x, z, info.height)
					log.info("goto", key, "->", x, info.height + 1, z)
					players.message(player, ui.text("goto.found", { biome_name(key), radius, x, z }))
					return
				end
			end
			index += 1
			if index >= count then
				ring += 1
				index = 0
				count = math.max(1, math.ceil(2 * math.pi * ring))
				if ring * STEP > REACH then
					timer.cancel(handle)
					searches[player] = nil
					players.message(player, ui.text("goto.none", { biome_name(key), REACH }))
					return
				end
			end
		end
	end)
	searches[player] = handle
end

commands.register("goto", ui.text("command.goto"), function(player, args)
	if not allowed(player) then
		return ui.text("goto.host_only")
	end
	if #args == 0 then
		return ui.text("goto.usage")
	end
	-- Coordinates.
	local x, z = tonumber(args[1]), tonumber(args[2])
	if x ~= nil and z ~= nil and #args == 2 then
		x, z = math.floor(x), math.floor(z)
		local info = terrain.info(x, z)
		if info == nil then
			return ui.text("terrain.retail")
		end
		land(player, x, z, math.max(info.height, SEA))
		return ui.text("goto.place", { x, z, math.floor(info.height) })
	end
	local key = biome_key(args)
	if key == nil then
		return ui.text("goto.unknown", { table.concat(args, " ") })
	end
	if terrain.info(0, 0) == nil then
		return ui.text("terrain.retail")
	end
	if searches[player] ~= nil then
		timer.cancel(searches[player])
	end
	search(player, key)
	return ui.text("goto.searching", { biome_name(key) })
end)

events.on("player.left", function(event)
	if searches[event.player] ~= nil then
		timer.cancel(searches[event.player])
		searches[event.player] = nil
	end
end)
