-- The rules of hunger and thirst: health lost while either is empty, slower
-- legs while either runs low, warnings, and a half-full start after dying.

local LOW = 15 -- below this a player is weak

-- Once a second: damage at zero, and a weak player walks slower (the speed
-- effect lasts a little longer than a second, so it holds while they stay low).
local elapsed = 0
events.on("tick", function(event)
	elapsed += event.dt
	if elapsed < 1 then
		return
	end
	elapsed -= 1
	local damage = mod.config.get("empty_damage")
	for _, player in players.list() do
		local hunger = stats.get(player, "hunger")
		local thirst = stats.get(player, "thirst")
		if damage > 0 and (hunger <= 0 or thirst <= 0) then
			players.damage(player, damage)
		end
		if hunger < LOW or thirst < LOW then
			effects.apply(player, "speed", { duration = 1.5, strength = 0.8 })
		end
	end
end)

-- Said once, when a stat runs out.
events.on("stat.depleted", function(event)
	if event.stat == "hunger" then
		ui.toast(event.player, ui.text("toast.starving"), ui.text("toast.starving_hint"), { color = "#e05030" })
	elseif event.stat == "thirst" then
		ui.toast(event.player, ui.text("toast.parched"), ui.text("toast.parched_hint"), { color = "#e05030" })
	end
end)

-- Back from death half full, so a respawn is not an instant second death.
events.on("player.respawned", function(event)
	stats.set(event.player, "hunger", 50)
	stats.set(event.player, "thirst", 50)
end)
