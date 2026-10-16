-- Wary animals: deer and rabbits bolt when a player comes close, and a deer
-- that bolts startles the rest of its herd. Checked twice a second.

local SHY = { ["wildlife:deer"] = 7, ["wildlife:rabbit"] = 5 } -- kind -> how close is too close
local HERD = 10

local function bolt(animal: Handle, from: Handle)
	entities.set_behaviour(animal, "flee", { target = from, distance = 16, duration = 4 })
end

timer.every(0.5, function()
	for _, player in players.list() do
		local here = players.position(player)
		for _, animal in entities.list({ near = here, radius = 8 }) do
			local state = entities.get(animal)
			local reach = SHY[state.kind]
			if reach ~= nil and state.behaviour ~= "flee" and vector.magnitude(state.position - here) < reach then
				bolt(animal, player)
				if state.kind == "wildlife:deer" then
					for _, other in entities.list({ kind = "wildlife:deer", near = state.position, radius = HERD }) do
						if other ~= animal then
							bolt(other, player)
						end
					end
				end
			end
		end
	end
end)

-- Say how many animals are about, for testing: /wildlife
commands.register("wildlife", "Count the animals near you", function(player)
	local here = players.position(player)
	local counts = { deer = 0, rabbit = 0, boar = 0 }
	for _, animal in entities.list({ near = here, radius = 64 }) do
		local kind = string.match(entities.get(animal).kind, "^wildlife:(.+)$")
		if kind ~= nil and counts[kind] ~= nil then
			counts[kind] += 1
		end
	end
	return string.format("Within 64 blocks: %d deer, %d rabbits, %d boars.", counts.deer, counts.rabbit, counts.boar)
end)
