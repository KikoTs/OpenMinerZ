-- The animals' server side: the second thing each one drops, and the calls
-- they make (each player's client plays the ones near its player).

local survival = require("../shared/lib/survival")

-- Leather from cows, wool from sheep, feathers from chickens; the meat comes
-- from the creature's own loot table.
local EXTRA = {
	["wilderness:cow"] = { "cow_hide", 0.7 },
	["wilderness:sheep"] = { "sheep_fleece", 1 },
	["wilderness:chicken"] = { "chicken_feathers", 0.8 },
}
events.on("entity.died", function(event)
	local extra = EXTRA[event.kind]
	if extra ~= nil and util.random() < extra[2] then
		loot.drop(extra[1], event.position)
	end
end)

-- What each animal says.
local CALLS = {
	["wilderness:cow"] = "moo",
	["wilderness:sheep"] = "baa",
	["wilderness:pig"] = "oink",
	["wilderness:chicken"] = "cluck",
}

-- Herds appear only on open ground, never on the crown of a tree (the top of
-- a forest is "the surface" to a spawn rule).
local GROUND = {
	grass = true, dirt = true, sand = true, ["wilderness:needle_litter"] = true, ["wilderness:gravel"] = true,
}
events.on("entity.spawning", function(event)
	if CALLS[event.kind] ~= nil and not GROUND[world.get_block(event.position - vector.create(0, 1, 0))] then
		return false
	end
	return nil
end)

-- A hurt animal cries out to everyone near it.
events.on("entity.damaged", function(event)
	local info = entities.get(event.entity)
	local sound = CALLS[info.kind]
	if sound == nil then
		return
	end
	for _, player in players.list() do
		if vector.magnitude(players.position(player) - info.position) < 32 then
			survival.cue(player, sound, info.position)
		end
	end
end)

-- Now and then one animal of the herds near a player calls out.
timer.every(3, function()
	for _, player in players.list() do
		local here = players.position(player)
		for _, animal in entities.list({ near = here, radius = 24 }) do
			local info = entities.get(animal)
			local sound = CALLS[info.kind]
			if sound ~= nil and util.random() < 0.08 then
				survival.cue(player, sound, info.position)
			end
		end
	end
end)
