-- This player's side of Survival: sounds and warnings. Presentation only; the
-- bars themselves come from stats.define's `hud` and need no code.

-- The server tells us when we ate or drank.
net.on("consumed", function(_info)
	sound.play("sounds/gulp", { volume = 0.8 })
end)

-- A warning when a stat drops below a fifth, and the picture washes out a
-- little as thirst runs dry.
events.on("stat.changed", function(event)
	if event.stat ~= "hunger" and event.stat ~= "thirst" then
		return
	end
	if event.value <= 20 and event.previous > 20 then
		ui.toast(ui.text("toast.low_" .. event.stat), ui.text("toast.low_hint"), { color = "#e0a040", seconds = 4 })
	end
	if event.stat == "thirst" then
		local parched = math.clamp((25 - event.value) / 25, 0, 1)
		camera.fade("#c8b48c", parched * 0.22, 1.5)
	end
end)
