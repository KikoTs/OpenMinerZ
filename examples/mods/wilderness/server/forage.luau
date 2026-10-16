-- Living off the land: fruit from the trees, and a mouthful of water straight
-- from a lake or river (the drink key of client/feedback.luau, or /drink).

local survival = require("../shared/lib/survival")

-- Broken leaves sometimes drop an apple, spruce boughs a handful of berries.
local FRUIT = {
	leaves = { "leaf_fruit", 0.08 },
	["wilderness:spruce_needles"] = { "bough_berries", 0.12 },
	["wilderness:snowy_needles"] = { "bough_berries", 0.06 },
}
events.on("block.broken", function(event)
	local fruit = FRUIT[event.block]
	if fruit ~= nil and not event.explosion and util.random() < fruit[2] then
		loot.drop(fruit[1], event.position)
	end
end)

-- Drinking from water with bare hands: a little at a time, twice a second at
-- most. The water must be within reach along where the player looks, or at
-- their feet.
local SIP = 8
local last_sip: { [Handle]: number } = {}

local function sip(player: Handle, look: vector?): boolean
	local now = time.day() + time.of_day()
	if last_sip[player] ~= nil and now - last_sip[player] < 0.5 / 960 then
		return true
	end
	local water = (look ~= nil and survival.water_along(player, look)) or survival.water_near(player, 1.5)
	if water == nil then
		return false
	end
	last_sip[player] = now
	stats.add(player, "thirst", SIP)
	survival.cue(player, "drink", water)
	return true
end

net.on("drink", function(player, payload)
	if not sip(player, payload.look) then
		net.send_to(player, "no_water", {})
	end
end, { look = "vector" })

commands.register("drink", ui.text("command.drink"), function(player)
	return if sip(player, nil) then ui.text("drink.ok") else ui.text("drink.no_water")
end)

events.on("player.left", function(event)
	last_sip[event.player] = nil
end)
