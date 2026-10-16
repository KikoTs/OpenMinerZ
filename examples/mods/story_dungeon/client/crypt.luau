-- This player's view of the crypt: dark fog and cold light inside it,
-- flickering torchlight, a low drone, and a key for the quest journal. All
-- presentation, driven by what the server replicates.

local layer = nil
local lights = {}
local drone = nil

local function clear()
	if layer ~= nil then
		atmosphere.remove(layer, true)
		layer = nil
	end
	for _, handle in lights do
		light.remove(handle)
	end
	lights = {}
	if drone ~= nil then
		sound.stop(drone, 0.5)
		drone = nil
	end
end

local function build(info)
	clear()
	-- Fades in over a block or three as the player walks down the stairs.
	layer = atmosphere.push({
		fog_color = "#120e18", fog_start = 1, fog_end = 20,
		ambient = "#40365a", ambient_intensity = 0.3, sun_intensity = 0.05,
		saturation = 0.75, contrast = 1.1, vignette = 0.4, temperature = -0.2,
		region = { min = info.min, max = info.max, feather = 3 },
		priority = 10, fade_in = 1.5, fade_out = 1.5,
	})
	for _, position in info.lights do
		local handle = light.create({
			position = position, color = "#ff8a3a", intensity = 2.4, radius = 7,
			flicker = 0.35, flicker_speed = 7,
		})
		if handle ~= nil then
			table.insert(lights, handle)
		end
	end
	drone = sound.play("sounds/crypt_drone", {
		loop = true, position = info.center, min_distance = 4, max_distance = 22, bus = "ambience", volume = 0.7,
	})
	log.info("crypt presentation ready:", #lights, "lights, sound", drone ~= nil)
end

replicated.on_change("world", "crypt", function(value)
	if value == nil then
		clear()
	else
		build(value)
	end
end)

input.bind("journal", "J", function()
	ui.open_journal()
end, { label = ui.text("key.journal") })
