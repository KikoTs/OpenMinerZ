-- A day clock on the HUD, a key to hide it, a setting, and a pause-menu row
-- that opens a dialog.

mod.config.define("show_clock", {
	type = "boolean", default = true,
	label = ui.text("setting.clock"), description = ui.text("setting.clock_hint"),
})

-- Units are the 1280x720 reference canvas; the HUD scales with the window,
-- Interface scale and Screen margins like the game's own.
local clock = ui.add_hud({
	kind = "panel", anchor = "top_right", x = -16, y = 16, padding = 8, background = "panel",
	children = {
		{ kind = "label", id = "time", text = "", face = "display", text_size = 13, color = "menu.heading", wrap = false },
	},
})
local label = ui.find(clock, "time")
local hidden = false

local function refresh()
	local hours = time.of_day() * 24
	local minutes = math.floor((hours - math.floor(hours)) * 60)
	ui.set(label, { text = ui.text("clock", { day = time.day(), time = string.format("%02d:%02d", math.floor(hours), minutes) }) })
	ui.set(clock, { visible = mod.config.get("show_clock") and not hidden })
end
refresh()
timer.every(0.5, refresh)

input.bind("toggle_clock", "F8", function()
	hidden = not hidden
	refresh()
end, { label = ui.text("key.clock") })

ui.add_menu_entry({
	menu = "pause",
	label = ui.text("menu.about"),
	description = ui.text("menu.about_hint"),
	on_select = function()
		ui.open_dialog({
			title = ui.text("about.title"),
			text = ui.text("about.text"),
			buttons = { { text = ui.text("about.close"), close = true } },
		})
	end,
})
