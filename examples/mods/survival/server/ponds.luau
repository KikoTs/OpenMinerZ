-- Fresh-water ponds: one next to where players first appear, and more in new
-- terrain as it generates. Everything here is an ordinary block edit, so every
-- player (vanilla ones too) receives it as chunk data.

-- A round pond, two blocks deep, its surface level with the ground at `center`.
local function dig_pond(center: vector, radius: number)
	for dx = -radius, radius do
		for dz = -radius, radius do
			if dx * dx + dz * dz <= radius * radius + 1 then
				local top = center + vector.create(dx, 0, dz)
				world.set_block(top, "survival:fresh_water")
				if dx * dx + dz * dz < radius * radius then
					world.set_block(top - vector.create(0, 1, 0), "survival:fresh_water")
				end
			end
		end
	end
end

-- The first pond, near where the first player appears in this world.
events.on("player.spawned", function(event)
	if storage.world.get("spawn_pond") then
		return
	end
	local at = event.position + vector.create(4, 0, 3)
	local ground = world.surface_y(at.x, at.z)
	if ground == nil then
		return
	end
	local center = vector.create(math.floor(at.x), ground, math.floor(at.z))
	dig_pond(center, 2)
	storage.world.set("spawn_pond", center)
	log.info("spawn pond dug at", center)
end)

-- About one new chunk in six (not in deserts or snow) gets a pond.
worldgen.enable({ radius = 10 })
events.on("worldgen.chunk", function(chunk)
	if chunk.biome == "desert" or chunk.biome == "arctic" then
		return
	end
	if worldgen.random(chunk.cx, chunk.cz, 1) > 0.17 then
		return
	end
	local x = chunk.x + 3 + math.floor(worldgen.random(chunk.cx, chunk.cz, 2) * 10)
	local z = chunk.z + 3 + math.floor(worldgen.random(chunk.cx, chunk.cz, 3) * 10)
	local ground = world.surface_y(x, z)
	if ground == nil or ground < 2 then
		return
	end
	dig_pond(vector.create(x, ground, z), 1 + math.floor(worldgen.random(chunk.cx, chunk.cz, 4) * 3))
end)
