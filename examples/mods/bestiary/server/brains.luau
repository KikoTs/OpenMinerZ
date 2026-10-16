-- Bestiary: two brains. A brain is a state machine the server runs for each
-- creature that names it: every state may have enter, tick, exit, damaged,
-- heard and noticed functions, called with (creature, info); returning a
-- state's name switches to it. Creatures without a brain (the archer, the
-- bat, the leaper, the wolf) run the built-in hunter, which ai.hunt() also runs
-- for one step from any state.

-- Ghoul packs: prowl together, stalk at a distance until the pack has
-- gathered, then rush in and surround; scatter when the pack has lost half
-- its health, and regroup.
brains.define("pack_hunter", {
	initial = "prowl",
	states = {
		prowl = {
			enter = function(ghoul, info)
				ai.wander(ghoul, { radius = 10 })
			end,
			tick = function(ghoul, info)
				if info.target ~= nil then
					return "stalk"
				end
				if info.heard ~= nil then
					ai.move_to(ghoul, info.heard) -- something made a noise: go and look
				elseif info.leader then
					if util.random() < 0.05 then
						ai.wander(ghoul, { radius = 10 })
					end
				else
					ai.chase(ghoul, ai.pack(ghoul)[1], { distance = 3, run = false })
				end
				return nil
			end,
		},
		stalk = {
			tick = function(ghoul, info)
				if info.target == nil then
					return "prowl"
				end
				if info.pack_size < 3 or info.time > 4 or (info.target_distance or 99) < 5 then
					return "hunt"
				end
				ai.keep_distance(ghoul, info.target, { distance = 7, far = 11, run = false })
				return nil
			end,
		},
		hunt = {
			tick = function(ghoul, info)
				if info.target == nil then
					return "prowl"
				end
				if info.pack_size >= 2 and info.pack_hurt > 0.5 then
					return "scatter"
				end
				ai.hunt(ghoul) -- chase, take a place around the prey, claw
				return nil
			end,
		},
		scatter = {
			enter = function(ghoul, info)
				if info.target ~= nil then
					ai.flee(ghoul, info.target, { seconds = 5 })
				end
			end,
			tick = function(ghoul, info)
				if info.time > 5 then
					return "prowl"
				end
				return nil
			end,
		},
	},
})

-- The Hollow King: asleep on his throne until he sees someone, then a duel
-- (slam up close, hexes at range); at 60% health "Wrath" (he calls ghouls),
-- at 25% his "Last Stand" (hexes as fast as they come). His phases switch
-- the state (creatures.define{boss = {phases}}).
local function near(king: Handle, info, distance: number): boolean
	return (info.target_distance or math.huge) < distance
end

brains.define("hollow_king", {
	initial = "slumber",
	states = {
		slumber = {
			enter = function(king, info)
				ai.go_home(king)
			end,
			tick = function(king, info)
				if info.target ~= nil then
					return "duel"
				end
				return nil
			end,
		},
		duel = {
			tick = function(king, info)
				if info.target == nil then
					return "slumber"
				end
				if near(king, info, 4.5) and ai.ready(king, "slam") then
					ai.use(king, "slam")
				else
					ai.hunt(king)
				end
				return nil
			end,
		},
		frenzy = {
			enter = function(king, info)
				ai.use(king, "summon")
			end,
			tick = function(king, info)
				if info.target == nil then
					return "slumber"
				end
				local ghouls = entities.find({ near = info.position, radius = 24, kind = "ghoul" })
				if #ghouls < 4 and ai.ready(king, "summon") then
					ai.use(king, "summon")
				elseif near(king, info, 4.5) and ai.ready(king, "slam") then
					ai.use(king, "slam")
				else
					ai.hunt(king)
				end
				return nil
			end,
		},
		last_stand = {
			tick = function(king, info)
				if info.target == nil then
					return "slumber"
				end
				if not near(king, info, 5) and ai.ready(king, "hex") then
					ai.attack(king, "hex")
				else
					ai.hunt(king)
				end
				return nil
			end,
		},
	},
})
