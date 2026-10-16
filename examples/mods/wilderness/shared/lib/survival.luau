-- What eating, drinking, filling a vessel and milking have in common. A module
-- (it lives in a subfolder, so it runs only when a script requires it): the
-- items' `use` callbacks and the server's rules call these, on the server.

local survival = {}

survival.WATER = "wilderness:water"
local EYE = 1.4 -- the player's eye above the feet
local REACH = 5 -- how far a player reaches with what they hold

type Meal = { hunger: number?, thirst: number?, health: number?, back: string? }

-- Takes one `item` from the player and adds to their hunger and thirst (and
-- health); `back` is what they keep (the empty bowl, bucket or bottle).
-- False, and nothing changes, when the player has none.
function survival.consume(player: Handle, item: string, meal: Meal): boolean
	if players.take(player, item, 1) ~= 1 then
		return false
	end
	local hunger = meal.hunger or 0
	local thirst = meal.thirst or 0
	if hunger ~= 0 then
		stats.add(player, "hunger", hunger)
	end
	if thirst ~= 0 then
		stats.add(player, "thirst", thirst)
	end
	if meal.health ~= nil and meal.health > 0 then
		players.heal(player, meal.health)
	end
	if meal.back ~= nil then
		players.give(player, meal.back, 1)
	end
	-- The client half of the mod plays the sound (presentation runs there).
	survival.cue(player, if thirst > hunger then "drink" else "eat")
	return true
end

-- A sound for one player, played by client/feedback.luau.
function survival.cue(player: Handle, sound: string, at: vector?)
	net.send_to(player, "cue", { sound = sound, at = at })
end

-- The water an item was used at: the first liquid cell its look ray met
-- (`target.liquid`), or the block under the crosshair (in a game that still
-- draws water as a solid block). nil when it was not water.
function survival.aimed_water(target: any): vector?
	if target == nil then
		return nil
	end
	if target.liquid ~= nil and target.liquid.block == survival.WATER then
		return target.liquid.position
	end
	if target.block == survival.WATER then
		return target.position
	end
	return nil
end

-- Water along a look direction from the player's eyes, within reach, before
-- anything solid. Used when the client only sends where it looks.
function survival.water_along(player: Handle, look: vector): vector?
	if vector.magnitude(look) < 0.01 then
		return nil
	end
	local eye = players.position(player) + vector.create(0, EYE, 0)
	local direction = vector.normalize(look)
	for step = 1, REACH * 4 do
		local cell = vector.floor(eye + direction * (step * 0.25))
		local block = world.get_block(cell)
		if block == survival.WATER then
			return cell
		end
		if block ~= nil and block ~= "air" then
			return nil
		end
	end
	return nil
end

-- Water at the player's feet or right beside them (wading, or kneeling on the
-- shore).
function survival.water_near(player: Handle, radius: number): vector?
	local feet = players.position(player)
	local cells = world.find_blocks(feet - vector.create(radius, 2, radius), feet + vector.create(radius, 1, radius),
		survival.WATER, 1)
	return cells[1]
end

-- The nearest creature of `kind` the player's look passes through, within
-- reach (for milking a cow with a bucket).
function survival.aimed_creature(player: Handle, look: vector?, kind: string): Handle?
	if look == nil or vector.magnitude(look) < 0.01 then
		return nil
	end
	local eye = players.position(player) + vector.create(0, EYE, 0)
	local direction = vector.normalize(look)
	local best: Handle? = nil
	local nearest = math.huge
	for _, creature in entities.list({ kind = kind, near = eye, radius = REACH + 2 }) do
		local info = entities.get(creature)
		local middle = info.position + vector.create(0, 0.8, 0)
		local along = vector.dot(middle - eye, direction)
		if along > 0 and along < REACH + 0.6 and along < nearest then
			local miss = vector.magnitude(middle - (eye + direction * along))
			if miss < 0.95 then
				best, nearest = creature, along
			end
		end
	end
	return best
end

return survival
