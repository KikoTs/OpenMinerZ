-- Hunger and thirst on the HUD, in the look of the game's own health in each
-- interface style:
--   Classic: rows of ten drumsticks and ten water drops over the right half of
--   the hotbar, beside the row of hearts;
--   Remastered: a row of icon, number and bar each, beside the health bar;
--   Modern (1.9.9.8): segmented gauges either side of the health gauge at the
--   top of the screen.
-- Positions are units of the 1280x720 HUD canvas, measured from the game's own
-- HUD at the default interface scale. The Classic and Modern HUDs ignore the
-- screen margins, so those rows use frame = "game" (the whole window); the
-- Remastered HUD keeps inside the margins, and so do its rows. The Remastered
-- bars follow the replicated stats by themselves (`stat = "hunger"`); the
-- icons, numbers and segments, and the pulse of a stat running low, are kept
-- up here.

local BARS = {
	{ stat = "hunger", icon = "assets/ui/hunger.png", half = "assets/ui/hunger_half.png",
		empty = "assets/ui/hunger_empty.png", color = "#e2892a", dim = "#e2892a3c" },
	{ stat = "thirst", icon = "assets/ui/thirst.png", half = "assets/ui/thirst_half.png",
		empty = "assets/ui/thirst_empty.png", color = "#3d9cf2", dim = "#3d9cf23c" },
}
local LOW = 20 -- below this the row pulses
local ICONS = 10

-- Classic: the row of hearts is 21 units tall, its middle 90 units above the
-- bottom of the window; the tray ends 238 right of centre.
local CLASSIC = { size = 21, pitch = 22.5, above = 90, right = 238, gap = 4 }
-- Remastered: the health row's middle is 79 units above the bottom of the
-- HUD's area (inside the margins) and the tray ends 234 right of centre.
local REMASTERED = { above = 79, right = 234, height = 16, gap = 6, bar = 148 }
-- Modern: the health gauge (176 wide) sits at the top centre.
local MODERN = { width = 170, offset = 202, top = 6 }

local root: Handle? = nil
local built_style: string? = nil
local parts = {} -- stat -> { row, value, percent, icons = {...}, segments = {...} }
local shown = {} -- stat -> the rounded value drawn

-- The x, y of a bottom-anchored element whose right edge is `right` units
-- right of the centre and whose lowest row's middle is `above` units above
-- the bottom (rows `row` tall).
local function bottom_right(width: number, right: number, above: number, row: number)
	return right - width / 2, -(above - row / 2)
end

local function classic_row(bar)
	local icons = {}
	for index = 1, ICONS do
		table.insert(icons, { kind = "image", id = bar.stat .. ".icon" .. index, image = bar.icon,
			width = CLASSIC.size, height = CLASSIC.size, padding = 0 })
	end
	return { kind = "stack", id = bar.stat, direction = "horizontal", gap = CLASSIC.pitch - CLASSIC.size,
		children = icons }
end

local function remastered_row(bar)
	return {
		kind = "stack", id = bar.stat, direction = "horizontal", align = "center", gap = 0,
		height = REMASTERED.height,
		children = {
			{ kind = "image", image = bar.icon, width = 18, height = 18, padding = 0 },
			{ kind = "spacer", width = 7 },
			{ kind = "label", id = bar.stat .. ".value", text = "100", outline = true, wrap = false,
				width = 25, text_size = 10, color = "#ffffff" },
			{ kind = "spacer", width = 6 },
			{ kind = "progress", stat = bar.stat, width = REMASTERED.bar, height = 10, color = bar.color },
		},
	}
end

local function modern_gauge(bar, x: number)
	local segments = {}
	for index = 1, ICONS do
		table.insert(segments, { kind = "panel", id = bar.stat .. ".seg" .. index, width = 6, height = 24,
			background = bar.color })
	end
	return {
		kind = "stack", id = bar.stat, anchor = "top", x = x, y = MODERN.top, direction = "horizontal",
		align = "end", gap = 10, width = MODERN.width,
		children = {
			{ kind = "image", image = bar.icon, width = 30, height = 30, padding = 0 },
			{ kind = "stack", direction = "vertical", gap = 2, children = {
				{ kind = "label", id = bar.stat .. ".percent", text = "100%", outline = true, wrap = false,
					text_size = 13, color = bar.color },
				{ kind = "stack", direction = "horizontal", gap = 6.4, children = segments },
			} },
		},
	}
end

local function build()
	if root ~= nil then
		ui.close(root)
	end
	parts = {}
	shown = {}
	built_style = ui.style()
	local hud: Handle
	if built_style == "modern" then
		-- An invisible strip across the top: hunger left of the health
		-- gauge, thirst right of it.
		hud = ui.add_hud({ kind = "panel", frame = "game", anchor = "top", x = 0, y = 0, width = 640, height = 70,
			children = { modern_gauge(BARS[1], -MODERN.offset), modern_gauge(BARS[2], MODERN.offset) } })
	elseif built_style == "classic" then
		local width = CLASSIC.pitch * (ICONS - 1) + CLASSIC.size
		local x, y = bottom_right(width, CLASSIC.right, CLASSIC.above, CLASSIC.size)
		hud = ui.add_hud({ kind = "stack", frame = "game", anchor = "bottom", direction = "vertical",
			gap = CLASSIC.gap, x = x, y = y, children = { classic_row(BARS[2]), classic_row(BARS[1]) } })
	else
		local x, y = bottom_right(18 + 7 + 25 + 6 + REMASTERED.bar, REMASTERED.right, REMASTERED.above,
			REMASTERED.height)
		hud = ui.add_hud({ kind = "stack", anchor = "bottom", direction = "vertical", gap = REMASTERED.gap,
			x = x, y = y, children = { remastered_row(BARS[2]), remastered_row(BARS[1]) } })
	end
	root = hud
	for _, bar in BARS do
		local found = {
			row = ui.find(hud, bar.stat),
			value = ui.find(hud, bar.stat .. ".value"),
			percent = ui.find(hud, bar.stat .. ".percent"),
			icons = {},
			segments = {},
		}
		for index = 1, ICONS do
			found.icons[index] = ui.find(hud, bar.stat .. ".icon" .. index)
			found.segments[index] = ui.find(hud, bar.stat .. ".seg" .. index)
		end
		parts[bar.stat] = found
	end
end

local function show(bar, value: number)
	local found = parts[bar.stat]
	local rounded = math.clamp(math.floor(value + 0.5), 0, 100)
	if found == nil or shown[bar.stat] == rounded then
		return
	end
	shown[bar.stat] = rounded
	if found.value ~= nil then
		ui.set(found.value, { text = tostring(rounded) })
	end
	if found.percent ~= nil then
		ui.set(found.percent, { text = rounded .. "%" })
	end
	-- Each icon or segment stands for ten points; an icon is half full from five.
	local step = 100 / ICONS
	for index = 1, ICONS do
		if found.icons[index] ~= nil then
			local image = bar.empty
			if rounded >= index * step then
				image = bar.icon
			elseif rounded >= index * step - step / 2 then
				image = bar.half
			end
			ui.set(found.icons[index], { image = image })
		end
		if found.segments[index] ~= nil then
			ui.set(found.segments[index], { background = if rounded > (index - 1) * step then bar.color else bar.dim })
		end
	end
end

-- Twice a second: follow the interface style, update, and pulse a stat that
-- is running low.
local pulse = false
local function refresh()
	if built_style ~= ui.style() then
		build()
	end
	pulse = not pulse
	for _, bar in BARS do
		local value = stats.mine(bar.stat)
		local found = parts[bar.stat]
		if value ~= nil and found ~= nil then
			show(bar, value)
			ui.set(found.row, { opacity = if value < LOW and pulse then 0.45 else 1 })
		end
	end
end

build()
refresh()
timer.every(0.5, refresh)
events.on("stat.changed", function(event)
	for _, bar in BARS do
		if bar.stat == event.stat then
			show(bar, event.value)
		end
	end
end)
