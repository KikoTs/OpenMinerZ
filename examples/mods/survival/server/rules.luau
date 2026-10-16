-- The server's rules: damage while empty, a gentler start after dying,
-- berries from leaves, the /drink command and warnings.

local needs = require("../shared/lib/needs")

-- Once a second, everyone at zero hunger or thirst loses health.
local elapsed = 0
events.on("tick", function(event)
	elapsed += event.dt
	if elapsed < 1 then
		return
	end
	elapsed -= 1
	local damage = mod.config.get("empty_damage")
	if damage <= 0 then
		return
	end
	for _, player in players.list() do
		if stats.get(player, "hunger") <= 0 or stats.get(player, "thirst") <= 0 then
			players.damage(player, damage)
		end
	end
end)

-- Said once, when a stat runs out.
events.on("stat.depleted", function(event)
	if event.stat == "hunger" then
		ui.toast(event.player, ui.text("toast.starving"),
			ui.text("toast.empty_hint", { action = ui.text("action.eat") }), { color = "#e05030" })
	elseif event.stat == "thirst" then
		ui.toast(event.player, ui.text("toast.parched"),
			ui.text("toast.empty_hint", { action = ui.text("action.drink") }), { color = "#e05030" })
	end
end)

-- Coming back from death half full, so a respawn is not an instant re-death.
events.on("player.respawned", function(event)
	stats.set(event.player, "hunger", 60)
	stats.set(event.player, "thirst", 60)
end)

-- A quarter of the leaves a player breaks drop berries.
events.on("block.broken", function(event)
	if event.block == "leaves" and not event.explosion and util.random() < 0.25 then
		loot.drop("leaf_litter", event.position)
	end
end)

-- /drink: a mouthful straight from a pond, no canteen needed.
commands.register("drink", "Drink from fresh water next to you", function(player)
	if not needs.near_water(player) then
		return ui.text("drink.no_water")
	end
	stats.add(player, "thirst", 15)
	net.send_to(player, "consumed", { item = "water" })
	return ui.text("drink.ok")
end)

-- New players start with an empty canteen and a hint.
events.on("player.joined", function(event)
	if storage.player.get(event.player, "welcomed") then
		return
	end
	storage.player.set(event.player, "welcomed", true)
	players.give(event.player, "survival:canteen", 1)
	players.give(event.player, "survival:berries", 4)
	players.message(event.player, ui.text("welcome"))
end)
