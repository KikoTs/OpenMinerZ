-- Ember: the game's interface in warm amber on dark plum. ui.theme only
-- changes how things look on this computer, which is why a client mod may do
-- it on any server.

ui.theme({
	-- The game's own inks, by name (ui.colors() lists them). "#RRGGBBAA" has alpha.
	colors = {
		["menu.row"] = "#d8c8a8",
		["menu.focus"] = "#ffb040",
		["menu.heading"] = "#ffd890",
		["menu.text"] = "#e8dcc4",
		["menu.quiet"] = "#a89878",
		["menu.disabled"] = "#6a6050",
		["menu.accent"] = "#ff9030",
		["menu.warning"] = "#ff6040",
		["hud.plate"] = "#1a1420c0",
		["hud.panel"] = "#221a28e8",
		["hud.well"] = "#140f18",
		["hud.slot"] = "#2a2230",
		["hud.slot_full"] = "#3a2e3c",
		["hud.band"] = "#ff9030",
		["hud.line"] = "#806040",
		["hud.health"] = "#e04a3a",
		["hud.stamina"] = "#f0b040",
	},
	-- Headings and menu rows in a pixel face drawn for this example.
	fonts = { display = "assets/fonts/EmberPixel.ttf" },
	-- Nine-slice images: `slice` is the border width in image pixels; the
	-- corners keep their size and the edges stretch.
	images = {
		panel = { path = "assets/ui/panel.png", slice = 8 },
		dialog = { path = "assets/ui/panel.png", slice = 8 },
		button = { path = "assets/ui/button.png", slice = 6 },
		button_focus = { path = "assets/ui/button_focus.png", slice = 6 },
		slot = { path = "assets/ui/slot.png", slice = 4 },
	},
	cursor = { path = "assets/ui/cursor.png", hotspot = { 1, 1 } },
	sounds = { click = "assets/ui/click.wav", popup = "assets/ui/popup.wav" },
	-- The game's own words, by the English text it shows.
	strings = { ["Play Offline"] = "Venture Forth" },
	-- Style-specific lines win over the ones above in that style.
	classic = { colors = { ["menu.focus"] = "#ffc860" } },
})
