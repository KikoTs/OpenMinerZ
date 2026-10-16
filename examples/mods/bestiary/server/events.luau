-- Bestiary: what players see of it, and commands to try it.

-- The king's phases and fall, for everyone near him.
events.on("entity.phase_changed", function(event)
	if entities.get(event.entity).kind ~= "bestiary:hollow_king" then
		return
	end
	local at = entities.get(event.entity).position
	for _, player in players.list() do
		if vector.magnitude(players.position(player) - at) < 64 then
			ui.title(player, event.name, "The Hollow King grows desperate", { stay = 2 })
		end
	end
end)

events.on("entity.died", function(event)
	if event.kind == "bestiary:hollow_king" then
		players.broadcast("The Hollow King has fallen.")
		if event.killer ~= nil then
			ui.toast(event.killer, "Kingslayer", "The crown is yours.")
		end
	end
end)

local KINDS = { "ghoul", "bone_archer", "gloom_bat", "grave_leaper", "grey_wolf", "hollow_king" }

-- /bestiary: what is about. /summon <kind> [count]: creatures 10 blocks ahead.
commands.register("bestiary", "Count the Bestiary's creatures within 64 blocks", function(player)
	local here = players.position(player)
	local parts = {}
	for _, kind in KINDS do
		local count = #entities.find({ near = here, radius = 64, kind = kind })
		if count > 0 then
			table.insert(parts, string.format("%d %s", count, kind))
		end
	end
	return #parts == 0 and "Nothing from the Bestiary within 64 blocks." or table.concat(parts, ", ")
end)

commands.register("summon", "Summon Bestiary creatures: /summon <kind> [count]", function(player, args)
	local kind = args[1]
	if kind == nil or not table.find(KINDS, kind) then
		return "Kinds: " .. table.concat(KINDS, ", ")
	end
	local me = players.get(player)
	-- The way the player faces (yaw 0 looks along -Z).
	local at = me.position + vector.create(math.sin(me.yaw), 0, -math.cos(me.yaw)) * 10
	local count = math.clamp(tonumber(args[2]) or 1, 1, 12)
	local first = nil
	for i = 1, count do
		local spot = at + vector.create((i % 3) - 1, 0, math.floor(i / 3))
		local ground = world.surface_y(math.floor(spot.x), math.floor(spot.z))
		if ground ~= nil and kind ~= "gloom_bat" then
			spot = vector.create(spot.x, ground + 1, spot.z)
		end
		local made = entities.spawn(kind, spot, first ~= nil and { pack = first } or nil)
		first = first or made
	end
	return string.format("Summoned %d %s.", count, kind)
end)
