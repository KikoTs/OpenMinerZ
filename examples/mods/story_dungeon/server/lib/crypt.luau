-- The crypt: its layout (made by code, as a structure), where it was built in
-- this world, and the points the quest and the client need. A module: the
-- server files require it, so it runs once.

local crypt = {}

-- Cells of the structure: x across (0..14), y up (0 = hall floor), z away from
-- the entrance (0..25). The ground the stairs start from is layer GROUND.
local WIDTH, HEIGHT, LENGTH = 15, 12, 26
local GROUND = 7
local ORIGIN = vector.create(7, GROUND, 0) -- the ground cell at the top of the stairs

local function layout(): { [string]: any }
	local grid = {}
	for y = 0, HEIGHT - 1 do
		grid[y] = {}
		for z = 0, LENGTH - 1 do
			local row = table.create(WIDTH, " ")
			if y < GROUND then
				-- Solid below the ground; a few bricks are mossy.
				for x = 1, WIDTH do
					row[x] = ((x * 7 + y * 13 + z * 5) % 11 == 0) and "m" or "#"
				end
			end
			grid[y][z] = row
		end
	end
	local function carve(x0, x1, y0, y1, z0, z1, char)
		for y = y0, y1 do
			for z = z0, z1 do
				for x = x0, x1 do
					grid[y][z][x + 1] = char
				end
			end
		end
	end
	-- Stairs down from the ground, one step per block.
	for step = 0, GROUND - 1 do
		carve(6, 8, GROUND - step, GROUND - step + 2, step, step, ".")
	end
	carve(6, 8, 1, 3, GROUND, 9, ".") -- corridor
	carve(1, 13, 1, 4, 10, 24, ".") -- the hall
	for _, z in { 13, 17, 21 } do -- pillars
		carve(4, 4, 1, 4, z, z, "#")
		carve(10, 10, 1, 4, z, z, "#")
	end
	-- The broken arch over the entrance.
	carve(5, 5, GROUND + 1, GROUND + 3, 0, 0, "#")
	carve(9, 9, GROUND + 1, GROUND + 2, 0, 0, "#")
	carve(5, 8, GROUND + 3, GROUND + 3, 0, 0, "m")

	local layers = {}
	for y = 0, HEIGHT - 1 do
		local rows = {}
		for z = 0, LENGTH - 1 do
			rows[z + 1] = table.concat(grid[y][z])
		end
		layers[y + 1] = rows
	end
	return {
		palette = { ["#"] = "crypt_brick", m = "mossy_brick", ["."] = "air" },
		layers = layers,
		origin = { x = ORIGIN.x, y = ORIGIN.y, z = ORIGIN.z },
	}
end

structures.define("crypt", layout())

-- A cell of the structure, in the world, for a crypt built at `anchor`.
local function at(anchor: vector, x: number, y: number, z: number): vector
	return anchor + vector.create(x, y, z) - ORIGIN
end

-- What a built crypt looks like to the rest of the mod.
function crypt.describe(anchor: vector)
	local lights = {}
	for _, z in { 11, 15, 19, 23 } do
		table.insert(lights, at(anchor, 1, 3, z) + vector.create(0.6, 0.5, 0.5))
		table.insert(lights, at(anchor, 13, 3, z) + vector.create(0.4, 0.5, 0.5))
	end
	table.insert(lights, at(anchor, 7, 3, 8) + vector.create(0.5, 0.5, 0.5))
	return {
		anchor = anchor,
		-- The whole crypt, stairs included, for the atmosphere layer.
		min = at(anchor, 0, 0, 0),
		max = at(anchor, WIDTH, GROUND + 1, LENGTH),
		center = at(anchor, 7, 2, 17),
		lights = lights,
		spawns = {
			at(anchor, 3, 1, 15) + vector.create(0.5, 0, 0.5),
			at(anchor, 11, 1, 15) + vector.create(0.5, 0, 0.5),
			at(anchor, 7, 1, 20) + vector.create(0.5, 0, 0.5),
			at(anchor, 7, 1, 23) + vector.create(0.5, 0, 0.5),
		},
	}
end

-- The crypt of this world, building it the first time (east of `near`).
function crypt.ensure(near: vector)
	local saved = storage.world.get("crypt")
	if saved ~= nil then
		return crypt.describe(saved)
	end
	local x, z = math.floor(near.x) + 24, math.floor(near.z) - 6
	local ground = world.surface_y(x, z) or math.floor(near.y)
	local anchor = vector.create(x, ground, z)
	structures.place("crypt", anchor, {
		on_done = function(_job, changed)
			log.info("the crypt is built:", changed, "cells changed")
		end,
	})
	storage.world.set("crypt", anchor)
	log.info("the crypt stands at", anchor)
	return crypt.describe(anchor)
end

-- The crypt if this world has one yet.
function crypt.current()
	local saved = storage.world.get("crypt")
	return saved ~= nil and crypt.describe(saved) or nil
end

return crypt
