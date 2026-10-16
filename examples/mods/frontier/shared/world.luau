-- The Frontier world: retail terrain around the spawn, then three new biomes
-- in rings out to 25,000 blocks, mirrored beyond that like retail's own fold.
--
-- This file only declares data. The game evaluates it natively on every peer
-- (host, joined players, single player) and on the chunk workers, so each
-- player generates the same land from the world seed and only edits travel.
-- Heights are world Y: the bottom of the world is -64, the top 63, and the
-- sea level (where the fluids of the salt flats and ashlands stop) is 1.

-- Redwood forest: rolling hills under giant two-wide redwoods.
terrain.biome("redwood", {
	name = "Redwood Forest",
	like = "classic",
	height = noise.add(
		18,
		noise.mul(noise.fbm({ scale = 320, octaves = 5, seed = 11 }), 14),
		noise.mul(noise.fbm({ scale = 48, octaves = 3, seed = 12 }), 3)
	),
	surface = { { block = "grass" }, { block = "dirt", depth = 3 } },
	underwater = { { block = "sand", depth = 2 } },
	trees = {
		{ shape = "cone", chance = 0.006, height = { 20, 30 }, radius = 4, thick = true,
		  trunk = "redwood_bark", leaves = "redwood_needles" },
		{ shape = "cone", chance = 0.018, height = { 10, 15 }, radius = 3,
		  trunk = "redwood_bark", leaves = "redwood_needles" },
		{ shape = "retail", chance = 0.004 },
		{ shape = "boulder", chance = 0.002, radius = 2, trunk = "rock" },
	},
	ores = {
		{ block = "coal_ore", chance = 0.35, size = 2 },
		{ block = "iron_ore", chance = 0.22, size = 1, y = { -60, 10 } },
		{ block = "gold_ore", chance = 0.08, size = 1, y = { -60, -20 } },
	},
	atmosphere = {
		fog_color = "#6f8a6a", sky_horizon = "#a3bfa2", ambient = "#d0e0c0",
		saturation = 1.05, temperature = -0.08, fade_in = 3, fade_out = 3,
	},
	music = { track = "music/redwood", volume = 0.55 },
})

-- Salt flats: a white plain cracked into plates, frozen brine in the hollows
-- and pillars of rock salt.
terrain.biome("salt_flats", {
	name = "Salt Flats",
	like = "desert",
	height = noise.add(
		2.3,
		noise.mul(noise.fbm({ scale = 520, octaves = 3, seed = 21 }), 2.6),
		-- One block lower along the cell edges: the cracked plates.
		noise.select(noise.cellular({ scale = 9, mode = "edge", seed = 22 }), -1, 0, 0.04, 0.09)
	),
	surface = { { block = "salt_crust" }, { block = "rock_salt", depth = 3 } },
	underwater = { { block = "rock_salt", depth = 2 } },
	fluid = "ice",
	caves = { scale = 64, threshold = -0.62, y = { -58, -10 } },
	trees = {
		{ shape = "column", chance = 0.0015, height = { 3, 7 }, trunk = "rock_salt", on = "salt_crust" },
		{ shape = "boulder", chance = 0.001, radius = 2, trunk = "rock_salt", on = "salt_crust" },
	},
	ores = {
		{ block = "rock_salt", inside = "rock", chance = 0.5, size = 3 },
		{ block = "copper_ore", chance = 0.3, size = 2 },
		{ block = "iron_ore", chance = 0.15, size = 1 },
	},
	atmosphere = {
		fog_color = "#e2d8c6", sky_horizon = "#f0e6d4", sky_zenith = "#7aa0d0",
		exposure = 0.2, saturation = 0.85, temperature = 0.15, bloom = 1.2, fade_in = 3, fade_out = 3,
	},
	music = { track = "music/salt_flats", volume = 0.5 },
})

-- Ashlands: jagged cinder ridges with overhangs, lava in the low ground,
-- dead trees and basalt boulders, ember ore.
terrain.biome("ashlands", {
	name = "Ashlands",
	like = "descent",
	height = noise.add(
		6,
		noise.mul(noise.ridged({ scale = 240, octaves = 4, seed = 31 }), 34),
		noise.mul(noise.fbm({ scale = 60, octaves = 3, seed = 32 }), 5)
	),
	-- Arches and overhangs on the ridges.
	density = noise.fbm({ scale = 26, octaves = 2, three_d = true, seed = 33 }),
	overhang = 7,
	surface = { { block = "ash", depth = 2 }, { block = "cinder_stone", depth = 6 } },
	underwater = { { block = "basalt", depth = 2 } },
	fluid = "surface_lava",
	caves = { scale = 40, threshold = -0.5, width = 0.08 },
	trees = {
		{ shape = "dead", chance = 0.004, height = { 4, 7 }, trunk = "log", on = "ash" },
		{ shape = "boulder", chance = 0.003, radius = 2, trunk = "basalt", on = "ash" },
	},
	ores = {
		{ block = "ember_ore", inside = { "cinder_stone", "rock" }, chance = 0.4, size = 2 },
		{ block = "diamond_ore", chance = 0.04, size = 1, y = { -62, -30 } },
	},
	atmosphere = {
		fog_color = "#3c2622", sky_zenith = "#2a1a1c", sky_horizon = "#7a4030", ambient = "#b07a64",
		saturation = 0.8, contrast = 1.1, temperature = 0.35, vignette = 0.2,
		lut = "luts/ashlands", lut_strength = 0.5, fade_in = 3, fade_out = 3,
	},
	music = { track = "music/ashlands", volume = 0.6 },
})

terrain.define({
	sea_level = 1,
	-- Retail's own start, beside the tower.
	spawn = { x = 8, z = -8 },
	layout = {
		-- Band edges wander by a few hundred blocks instead of tracing circles.
		warp = noise.mul(noise.fbm({ scale = 1100, octaves = 3, seed = 40 }), 380),
		-- Past 25,000 the rings run backwards (25,100 looks like 24,900).
		beyond = "mirror",
		bands = {
			{ to = 700, biome = "retail" },
			{ to = 6500, biome = "redwood", blend = 260 },
			{ to = 13000, biome = "salt_flats", blend = 320 },
			{ to = 19000, biome = "ashlands", blend = 320 },
			-- The far frontier: all three, by a climate field.
			{ to = 25000, blend = 400, layout = {
				by = noise.fbm({ scale = 1400, octaves = 2, seed = 41 }),
				bands = {
					{ to = -0.15, biome = "ashlands" },
					{ to = 0.2, biome = "salt_flats", blend = 0.12 },
					{ biome = "redwood", blend = 0.12 },
				},
			} },
		},
	},
})
