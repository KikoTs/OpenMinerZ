-- Wilderness: the land.
--
-- Retail terrain everywhere except retail's mountain ring (about 1,600 to
-- 2,400 blocks out, and its mirror past the fold at 4,400), which becomes a
-- range in the style of Minecraft's mountains: knife-edge ridges and pointed
-- peaks near the top of the world, stone cliffs with overhangs and bands of
-- slate, snow above a ragged snow line, spruce forests and alpine meadows on
-- the lower slopes and lakes of water in the valleys. Enemies keep retail's
-- difficulty by distance, and the spawn is retail's.
--
-- The whole range shares ONE height field; its five biomes are altitude zones
-- of it (lake, meadow or forest, stony slopes, peaks), so a blend between
-- them never bends the ground, only changes what it is made of. This file
-- only declares data: every peer evaluates it natively and generates the same
-- land from the world seed.

-- Blocks of the range. Each names a retail fallback: what saves and stock
-- messages hold, and what a world opened without the mod shows.
blocks.define("water", {
	name = "Water", fallback = "ice", texture = "textures/terrain/water", drops = "none", liquid = true,
})
blocks.define("spruce_log", {
	name = "Spruce Log", fallback = "log", texture = "textures/terrain/spruce_bark",
	faces = { top = "textures/terrain/spruce_rings", bottom = "textures/terrain/spruce_rings" },
})
blocks.define("spruce_needles", { name = "Spruce Needles", fallback = "leaves", texture = "textures/terrain/spruce_needles" })
blocks.define("snowy_needles", {
	name = "Snowy Spruce Needles", fallback = "leaves", texture = "textures/terrain/snowy_needles_side",
	faces = { top = "textures/terrain/snowy_needles_top" },
})
blocks.define("needle_litter", {
	name = "Needle Litter", fallback = "dirt", texture = "textures/terrain/needle_litter_side",
	faces = { top = "textures/terrain/needle_litter", bottom = "textures/terrain/forest_dirt" },
})
blocks.define("gravel", { name = "Gravel", fallback = "sand", texture = "textures/terrain/gravel" })
blocks.define("slate", { name = "Slate", fallback = "rock", texture = "textures/terrain/slate" })

local SEA = 1

-- Band edges wander by up to 120 blocks instead of tracing circles. The
-- layout and the height field use the same warp, so the mountains rise
-- exactly where the band begins.
local warp = noise.mul(noise.fbm({ scale = 700, octaves = 2, seed = 501 }), 120)

-- 1 inside the mountain ring, 0 outside, ramping over 240 blocks at each
-- edge, and mirrored past 4,400 like retail's fold: the ring comes back every
-- 8,800 blocks (a curve takes 64 points: out to 64,000 blocks; further out
-- the band is rolling lowland).
local ring_points = {}
for fold = 0, 7 do
	local spans = if fold == 0 then { { 1560, 2460 } }
		else { { 8800 * fold - 2460, 8800 * fold - 1560 }, { 8800 * fold + 1560, 8800 * fold + 2460 } }
	for _, span in spans do
		table.insert(ring_points, { span[1], 0 })
		table.insert(ring_points, { span[1] + 240, 1 })
		table.insert(ring_points, { span[2] - 240, 1 })
		table.insert(ring_points, { span[2], 0 })
	end
end
local inside = noise.curve(noise.add(noise.distance(), warp), ring_points)

-- The height field. (Every reference to an expression evaluates it again,
-- so each field below is used once.)
-- Massifs and broad valleys: how tall the mountains may grow here.
local massif = noise.curve(noise.fbm({ scale = 750, octaves = 3, seed = 502 }),
	{ { -0.35, 0.15 }, { -0.1, 0.55 }, { 0.12, 0.92 }, { 0.3, 1 } }, true)
-- Ridgelines: two crossing networks of crests (so ridges branch and meet in
-- peaks instead of closing into rings), with spurs and jagged tops riding on
-- them, each finer crest only where the coarser one stands:
-- ridges * (0.58 + spurs * (0.27 + teeth * 0.15)).
local ridges = noise.max(
	noise.ridged({ scale = 130, octaves = 1, seed = 503 }),
	noise.mul(noise.ridged({ scale = 165, octaves = 1, seed = 504 }), 0.92)
)
local spurs = noise.ridged({ scale = 64, octaves = 1, seed = 505 })
local teeth = noise.ridged({ scale = 26, octaves = 1, seed = 506 })
local crest = noise.mul(ridges, noise.add(0.58, noise.mul(spurs, noise.add(0.27, noise.mul(teeth, 0.15)))))
-- Broad valleys, steep upper slopes and summits that end in a point (the
-- curve keeps rising to the very top instead of rounding off).
local profile = noise.curve(crest,
	{ { 0.1, 0 }, { 0.35, 0.06 }, { 0.55, 0.24 }, { 0.72, 0.5 }, { 0.86, 0.76 }, { 1, 1 } }, true)
-- Ridges rise and fall along their length: saddles, passes and summits.
local summits = noise.add(56, noise.mul(noise.fbm({ scale = 150, octaves = 2, seed = 507 }), 22))
-- A massif lifts its high valleys too, so its ridges stand on a plinth.
local mountains = noise.mul(massif, noise.add(8, noise.mul(profile, summits)))
-- Basins under the sea level that fill with lakes (tarns in the high
-- valleys, bigger lakes in the broad ones).
local basins = noise.curve(noise.fbm({ scale = 200, octaves = 2, seed = 509 }), { { 0.04, 0 }, { 0.26, -13 } }, true)
-- Valley floors, and a little roughness everywhere.
local floor = noise.add(3, noise.mul(noise.fbm({ scale = 160, octaves = 3, seed = 508 }), 5))
local rough = noise.mul(noise.fbm({ scale = 22, octaves = 2, seed = 510 }), 1.5)
-- The last blocks squeezed so summits (with their overhangs) stay under the
-- top of the world at 63.
local height = noise.max(
	noise.curve(noise.add(floor, rough, noise.mul(inside, noise.add(mountains, basins))),
		{ { -40, -40 }, { 52, 52 }, { 64, 58 }, { 90, 60 } }, true),
	-- No hollows under the sea level where the ring fades into retail
	-- terrain (a lake there would be half retail ice).
	noise.lerp(SEA + 0.5, -40, inside)
)

-- Cliffs: the ground moves by a 3D field near the surface (overhangs,
-- ledges, the odd arch).
local crags = noise.fbm({ scale = 14, vertical_scale = 9, octaves = 2, three_d = true, seed = 511 })

local ORES = {
	{ block = "coal_ore", chance = 0.45, size = 2, y = { -50, 60 } },
	{ block = "iron_ore", chance = 0.32, size = 1, y = { -60, 45 } },
	{ block = "copper_ore", chance = 0.3, size = 2, y = { -40, 40 } },
	{ block = "gold_ore", chance = 0.14, size = 1, y = { -62, 12 } },
	{ block = "diamond_ore", chance = 0.05, size = 1, y = { -62, -30 } },
	-- Bands of slate in the cliffs, and gravel pockets.
	{ block = "slate", chance = 0.75, size = 5, spacing = 14, y = { 15, 17 }, seed = 1 },
	{ block = "slate", chance = 0.75, size = 5, spacing = 14, y = { 28, 29 }, seed = 2 },
	{ block = "slate", chance = 0.75, size = 5, spacing = 14, y = { 41, 43 }, seed = 3 },
	{ block = "gravel", chance = 0.22, size = 2, y = { -40, 62 } },
}

local function with(list, extra)
	local out = table.clone(list)
	for _, entry in extra do
		table.insert(out, 1, entry)
	end
	return out
end

local SPRUCE = { trunk = "spruce_log", leaves = "spruce_needles" }

terrain.biome("jagged_peaks", {
	name = "Jagged Peaks",
	like = "mountain",
	height = height,
	density = crags,
	overhang = 3,
	-- One cell of snow: every step of the slope shows its stone face, so the
	-- shape of a peak reads from far away instead of melting into white.
	surface = { { block = "snow" } },
	underwater = { { block = "gravel", depth = 1 } },
	fluid = "water",
	ores = ORES,
	atmosphere = {
		fog_color = "#c6d4e4", sky_horizon = "#d2e0ee", sky_zenith = "#4f86cc", ambient = "#dce6f2",
		saturation = 0.95, temperature = -0.12, exposure = 0.08, fade_in = 4, fade_out = 4,
	},
})

terrain.biome("stony_slopes", {
	name = "Stony Slopes",
	like = "mountain",
	height = height,
	density = crags,
	overhang = 5,
	surface = { { block = "rock" } },
	underwater = { { block = "gravel", depth = 1 } },
	fluid = "water",
	ores = with(ORES, { { block = "gravel", chance = 0.5, size = 2, spacing = 8, y = { 10, 62 }, seed = 4 } }),
	trees = {
		{ shape = "cone", chance = 0.002, height = { 5, 8 }, radius = 2, trunk = "spruce_log", leaves = "snowy_needles", on = { "rock" } },
	},
	atmosphere = {
		fog_color = "#b4c0cc", sky_horizon = "#c6d4e2", ambient = "#d4dce6",
		saturation = 0.97, temperature = -0.08, fade_in = 4, fade_out = 4,
	},
})

terrain.biome("pine_forest", {
	name = "Pine Forest",
	like = "classic",
	height = height,
	surface = { { block = "grass" }, { block = "dirt", depth = 3 } },
	underwater = { { block = "gravel", depth = 1 }, { block = "dirt", depth = 2 } },
	fluid = "water",
	ores = with(ORES, { { block = "needle_litter", inside = "grass", chance = 0.6, size = 3, spacing = 9 } }),
	trees = {
		{ shape = "cone", chance = 0.022, height = { 7, 11 }, radius = 2, trunk = SPRUCE.trunk, leaves = SPRUCE.leaves },
		{ shape = "cone", chance = 0.006, height = { 12, 17 }, radius = 3, trunk = SPRUCE.trunk, leaves = SPRUCE.leaves },
		{ shape = "cone", chance = 0.001, height = { 18, 24 }, radius = 4, thick = true, trunk = SPRUCE.trunk, leaves = SPRUCE.leaves },
		{ shape = "boulder", chance = 0.0012, radius = 2, trunk = "rock" },
	},
	atmosphere = {
		fog_color = "#7f9a88", sky_horizon = "#a6c2b0", ambient = "#c8dcc6",
		saturation = 1.05, temperature = -0.05, fade_in = 4, fade_out = 4,
	},
})

terrain.biome("alpine_meadow", {
	name = "Alpine Meadow",
	like = "classic",
	height = height,
	surface = { { block = "grass" }, { block = "dirt", depth = 3 } },
	underwater = { { block = "gravel", depth = 1 }, { block = "dirt", depth = 2 } },
	fluid = "water",
	ores = ORES,
	trees = {
		{ shape = "cone", chance = 0.0025, height = { 8, 13 }, radius = 2, trunk = SPRUCE.trunk, leaves = SPRUCE.leaves },
		-- Bushes: one log in a mound of leaves.
		{ shape = "round", chance = 0.004, height = { 1, 1 }, radius = 2, trunk = SPRUCE.trunk, leaves = "leaves" },
		{ shape = "boulder", chance = 0.0015, radius = 2, trunk = "rock" },
		{ shape = "boulder", chance = 0.001, radius = 1, trunk = "slate" },
	},
	atmosphere = {
		fog_color = "#a6c6d8", sky_horizon = "#bedcea", ambient = "#d8e8d0",
		saturation = 1.12, temperature = 0.02, exposure = 0.04, fade_in = 4, fade_out = 4,
	},
})

terrain.biome("lake_valley", {
	name = "Lake Valley",
	like = "classic",
	height = height,
	surface = { { block = "sand", depth = 2 }, { block = "dirt", depth = 2 } },
	underwater = { { block = "sand", depth = 2 }, { block = "dirt", depth = 1 } },
	fluid = "water",
	ores = with(ORES, { { block = "gravel", inside = "sand", chance = 0.55, size = 2, spacing = 7 } }),
	trees = {
		{ shape = "boulder", chance = 0.002, radius = 1, trunk = "rock" },
	},
	atmosphere = {
		fog_color = "#98b8cc", sky_horizon = "#b2d0e4", ambient = "#d0e2ea",
		saturation = 1.08, temperature = -0.04, fade_in = 4, fade_out = 4,
	},
})

-- The range: altitude zones of the height field, with a ragged edge between
-- them (the snow line wanders by a few blocks, and rises and falls by up to
-- six more across a mountainside, so it never reads as a level line), and
-- forest or meadow by a climate field on the lower slopes.
local altitude = noise.add(height,
	noise.mul(noise.fbm({ scale = 30, octaves = 2, seed = 512 }), 2.5),
	noise.mul(noise.fbm({ scale = 110, octaves = 2, seed = 514 }), 6))
local range = {
	by = altitude,
	bands = {
		-- Shores: a beach only just above the water (every zone fills its
		-- hollows with water, so a lake is a lake whatever its shore).
		{ to = SEA + 0.75, biome = "lake_valley" },
		{ to = 22, blend = 1, layout = {
			-- Forest or meadow by a climate field, and meadow (open foothills)
			-- towards the edges of the ring.
			by = noise.add(noise.fbm({ scale = 320, octaves = 3, seed = 513 }), noise.mul(noise.sub(inside, 1), 0.5)),
			bands = {
				{ to = -0.1, biome = "alpine_meadow" },
				{ biome = "pine_forest", blend = 0.1 },
			},
		} },
		{ to = 35, biome = "stony_slopes", blend = 2.5 },
		{ biome = "jagged_peaks", blend = 2 },
	},
}

terrain.define({
	sea_level = SEA,
	-- Retail's own start, beside the tower.
	spawn = { x = 8, z = -8 },
	layout = {
		warp = warp,
		-- Past 4,400 the rings run backwards, exactly like retail's fold.
		beyond = "mirror",
		bands = {
			-- Retail's classic plains, lagoons and desert.
			{ to = 1300, biome = "retail" },
			-- Retail's desert (alone, so retail's own mountains never show
			-- through where the band edge wanders).
			{ to = 1560, biome = "retail:desert" },
			{ to = 2460, blend = 260, layout = range },
			{ to = 2800, biome = "retail:arctic", blend = 260 },
			-- Retail's arctic and the descent.
			{ to = 4400, biome = "retail" },
		},
	},
})
