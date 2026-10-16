-- This player's side of survival: the sounds the server asks for, warnings
-- as hunger and thirst run low, the drink key, and the glow of stone ovens.
-- Presentation only; the server decides everything that counts.

-- Sounds of what this player does (eating, drinking, filling, milking,
-- cooking) play in their ears; the animals' calls come from where they stand.
local SOUNDS = {
	eat = { file = "sounds/eat", volume = 0.8, own = true },
	drink = { file = "sounds/drink", volume = 0.8, own = true },
	fill = { file = "sounds/fill", volume = 0.7, own = true },
	milk = { file = "sounds/milk", volume = 0.7, own = true },
	sizzle = { file = "sounds/sizzle", volume = 0.6, own = true },
	moo = { file = "sounds/moo", volume = 0.9 },
	baa = { file = "sounds/baa", volume = 0.8 },
	oink = { file = "sounds/oink", volume = 0.8 },
	cluck = { file = "sounds/cluck", volume = 0.7 },
}
net.on("cue", function(payload)
	local cue = SOUNDS[payload.sound]
	if cue == nil then
		return
	end
	if cue.own then
		sound.play(cue.file, { volume = cue.volume })
	elseif payload.at ~= nil then
		sound.play(cue.file, { position = payload.at, min_distance = 3, max_distance = 28, volume = cue.volume })
	end
end)

net.on("no_water", function(_payload)
	ui.toast(ui.text("drink.no_water"), ui.text("drink.hint"), { color = "#e0a040", seconds = 3 })
end)

-- Drinking with bare hands: look at the water (or stand in it) and press H.
input.bind("drink", "H", function()
	local me = players.me()
	if me ~= nil then
		net.send("drink", { look = me.look })
	end
end, { label = ui.text("key.drink") })

-- A warning when a stat drops below a fifth; the picture washes out as thirst
-- runs dry.
events.on("stat.changed", function(event)
	if event.stat ~= "hunger" and event.stat ~= "thirst" then
		return
	end
	if event.value <= 20 and event.previous > 20 then
		ui.toast(ui.text("toast.low_" .. event.stat), ui.text("toast.low_hint"), { color = "#e0a040", seconds = 4 })
	end
	if event.stat == "thirst" then
		local parched = math.clamp((25 - event.value) / 25, 0, 1)
		camera.fade("#c8b48c", parched * 0.22, 1.5)
	end
end)

-- Stone ovens glow: a warm flickering light at the fire mouth on each side
-- of every oven near the player (a light inside the solid block itself
-- would light nothing).
local REACH = 20
local MOUTHS = {
	vector.create(0.5, 0.35, -0.45), vector.create(0.5, 0.35, 1.45),
	vector.create(-0.45, 0.35, 0.5), vector.create(1.45, 0.35, 0.5),
}
local glowing = {} -- "x,y,z" -> its light handles
timer.every(2, function()
	local wanted = {}
	local me = players.me()
	if me ~= nil then
		local here = vector.floor(me.position)
		for _, cell in world.find_blocks(here - vector.create(REACH, 8, REACH), here + vector.create(REACH, 8, REACH),
			"wilderness:stone_oven", 16) do
			wanted[string.format("%d,%d,%d", cell.x, cell.y, cell.z)] = cell
		end
	end
	for key, lights in glowing do
		if wanted[key] == nil then
			for _, handle in lights do
				light.remove(handle)
			end
			glowing[key] = nil
		end
	end
	for key, cell in wanted do
		if glowing[key] == nil then
			local lights = {}
			for _, mouth in MOUTHS do
				table.insert(lights, light.create({
					position = cell + mouth,
					color = "#ff8a3c", intensity = 1.3, radius = 5, flicker = 0.45, flicker_speed = 7,
				}))
			end
			glowing[key] = lights
		end
	end
end)
