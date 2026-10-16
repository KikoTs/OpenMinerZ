-- What eating and drinking do. A module (it lives in a subfolder, so it only
-- runs when a script requires it). Item `use` callbacks call these on the
-- server; the client never runs them.

local needs = {}

-- Fresh water within two blocks of the player's feet.
function needs.near_water(player: Handle): boolean
	local feet = players.position(player)
	local cells = world.find_blocks(feet - vector.create(2, 2, 2), feet + vector.create(2, 1, 2), "survival:fresh_water", 1)
	return #cells > 0
end

-- Takes one `item` from the player and adds to their stats. Returns false
-- (and changes nothing) when the player has none.
function needs.consume(player: Handle, item: string, hunger: number, thirst: number): boolean
	if players.take(player, item, 1) ~= 1 then
		return false
	end
	if hunger ~= 0 then
		stats.add(player, "hunger", hunger)
	end
	if thirst ~= 0 then
		stats.add(player, "thirst", thirst)
	end
	-- The client half of this mod plays the sound (presentation runs there).
	net.send_to(player, "consumed", { item = item })
	return true
end

return needs
