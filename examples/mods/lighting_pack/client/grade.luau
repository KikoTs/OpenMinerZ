-- Time-of-day grading: golden light around dawn and dusk, cooler moonlit
-- nights. Two atmosphere layers over the game's own sky and light, their
-- strength following the clock.

mod.config.define("grade", {
	type = "boolean", default = true,
	label = ui.text("setting.grade"), description = ui.text("setting.grade_hint"),
})

local GOLDEN = {
	sun_color = "#ffb070", ambient = "#ffd2a8", fog_color = "#d8a070", sky_horizon = "#f0a868",
	saturation = 1.1, temperature = 0.3, bloom = 1.25,
	lut = "luts/warm_evening", lut_strength = 0.6,
	priority = -10, fade_in = 0, fade_out = 0,
}
local MOONLIT = {
	ambient = "#7080b0", ambient_intensity = 1.25, fog_color = "#1c2434", sky_zenith = "#0c1020",
	saturation = 0.85, temperature = -0.35,
	priority = -11, fade_in = 0, fade_out = 0,
}

-- A smooth bump of width `width` (days) around `center`, 0..1.
local function bump(t: number, center: number, width: number): number
	local d = math.abs(t - center)
	d = math.min(d, 1 - d) -- the clock wraps at midnight
	return math.clamp(1 - d / width, 0, 1) ^ 2
end

local function with_strength(values, strength: number)
	local layer = table.clone(values)
	layer.strength = strength
	return layer
end

local golden = atmosphere.push(with_strength(GOLDEN, 0))
local moonlit = atmosphere.push(with_strength(MOONLIT, 0))

local function update()
	local on = mod.config.get("grade")
	local t = time.of_day()
	-- 06:30 and 19:30 golden hours; full night from 22:00 to 04:00.
	local gold = on and math.max(bump(t, 0.27, 0.06), bump(t, 0.81, 0.07)) or 0
	local night = on and math.clamp(1 - bump(t, 0.5, 0.33) * 1.6, 0, 1) or 0
	atmosphere.set(golden, with_strength(GOLDEN, gold))
	atmosphere.set(moonlit, with_strength(MOONLIT, night))
end
update()
timer.every(1, update)
