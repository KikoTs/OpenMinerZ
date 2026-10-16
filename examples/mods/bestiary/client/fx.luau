-- Bestiary: what each player's game draws of its projectiles. Effects are
-- presentation, so they are defined on the client VM; the projectiles name
-- them (fx = a trail, impact_fx = where one lands). The Enhanced presentation
-- draws them; the Original one has no effects layer and shows the models.

-- A pale streak behind each bone arrow: short-lived glow left along its path.
fx.define("arrow_streak", {
	emitters = { {
		material = "glow", rate = 320, loop = true, life = { 0.12, 0.2 }, speed = { 0, 0.15 }, spread = 0.05,
		size = { 0.05, 0.07 }, size_over_life = { from = 1, to = 0.3 }, alpha = { from = 0.7, to = 0 },
		color = "#fff4dc", emissive = 1.5,
	} },
})

-- The hex leaves a violet wake.
fx.define("hex_trail", {
	emitters = { {
		material = "glow", rate = 60, loop = true, life = { 0.3, 0.45 }, speed = { 0, 0.3 }, spread = 0.2,
		size = { 0.1, 0.14 }, size_over_life = { from = 1, to = 0.2 }, alpha = { from = 0.8, to = 0 },
		color = "#b070ff", emissive = 2,
	} },
})

-- Where a hex bursts: a violet flash and sparks and a wisp of smoke, small
-- enough that the Hollow King stays in sight behind it. It stands for the
-- game's own blast, which is sized for a dragon's fireball.
fx.define("hex_burst", {
	emitters = {
		{ material = "glow", burst = 6, life = { 0.2, 0.35 }, speed = { 0, 0.5 }, size = { 0.5, 0.8 },
		  size_over_life = { from = 0.6, to = 1.6 }, alpha = { from = 0.9, to = 0 }, color = "#c080ff", emissive = 3 },
		{ material = "spark", burst = 28, life = { 0.3, 0.6 }, speed = { 3, 7 }, spread = 1, size = { 0.03, 0.06 },
		  color = "#e0b0ff", gravity = 4, emissive = 2 },
		{ material = "smoke", burst = 3, life = { 0.8, 1.2 }, speed = { 0.3, 0.8 }, spread = 1, size = { 0.25, 0.35 },
		  size_over_life = { from = 1, to = 2.5 }, alpha = { from = 0.35, to = 0 }, color = "#3a2a4a" },
	},
	lights = { { color = "#a060ff", radius = 5, intensity = 2, duration = 0.4 } },
})
