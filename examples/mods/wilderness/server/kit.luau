-- Getting started: new players get a small kit and a hint, and /wilderness
-- shows how fed they are, hands out the kit again and brings animals over (the
-- last two only for the host, so testing is quick and guests cannot cheat).

-- What a new player starts with, and what /wilderness kit gives.
local KIT = {
	{ "wilderness:bucket", 1 }, { "wilderness:bottle", 2 }, { "wilderness:apple", 4 },
	{ "wilderness:raw_beef", 2 }, { "wilderness:stone_oven", 1 },
}

local function give_kit(player: Handle)
	for _, entry in KIT do
		players.give(player, entry[1], entry[2])
	end
	-- The pack's weapon, when its items are defined (shared/weapons.luau).
	if table.find(items.list(), "wilderness:iron_broadsword") then
		players.give(player, "wilderness:iron_broadsword", 1)
	end
end

events.on("player.joined", function(event)
	if storage.player.get(event.player, "welcomed") then
		return
	end
	storage.player.set(event.player, "welcomed", true)
	if mod.config.get("starter_kit") then
		give_kit(event.player)
	end
	players.message(event.player, ui.text("welcome"))
end)

-- A few animals of one kind, a few blocks in front of the player.
local function bring(player: Handle, kind: string, count: number): number
	local me = players.get(player)
	local ahead = vector.create(math.sin(me.yaw), 0, -math.cos(me.yaw))
	local first: Handle? = nil
	local brought = 0
	for index = 1, count do
		local row, column = math.floor((index - 1) / 3), (index - 1) % 3
		local spot = me.position + ahead * 7 + vector.create(column * 1.6 - 1.6, 0, row * 1.8)
		local ground = world.surface_y(math.floor(spot.x), math.floor(spot.z))
		if ground ~= nil then
			local at = vector.create(math.floor(spot.x) + 0.5, ground + 1, math.floor(spot.z) + 0.5)
			local animal = entities.spawn(kind, at, if first ~= nil then { pack = first } else nil)
			first = first or animal
			brought += 1
		end
	end
	return brought
end

local ANIMALS = { cow = true, pig = true, sheep = true, chicken = true }

commands.register("wilderness", ui.text("command.wilderness"), function(player, args)
	local what = args[1] or "help"
	if what == "needs" then
		local hunger, thirst = stats.get(player, "hunger"), stats.get(player, "thirst")
		return ui.text("needs", { math.floor(hunger + 0.5), math.floor(thirst + 0.5) })
	end
	if what == "kit" or what == "herd" then
		if not players.get(player)["local"] then
			return ui.text("host_only")
		end
		if what == "kit" then
			give_kit(player)
			return ui.text("kit.given")
		end
		local kind = args[2] or "cow"
		if not ANIMALS[kind] then
			return ui.text("herd.unknown", { kind })
		end
		local count = math.clamp(tonumber(args[3]) or 4, 1, 8)
		return ui.text("herd.brought", { bring(player, "wilderness:" .. kind, count), ui.text("animal." .. kind) })
	end
	return ui.text("help")
end)
