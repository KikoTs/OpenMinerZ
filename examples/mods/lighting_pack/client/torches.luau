-- Lighting props: a warm, flickering light at every torch and a steady glow at
-- every lantern around the player, kept in step as blocks change and as the
-- player moves. players.me() is this machine's own view, so a client mod may
-- read it on any server.

mod.config.define("torches", {
	type = "boolean", default = true,
	label = ui.text("setting.torches"), description = ui.text("setting.torches_hint"),
})

local REACH = 24 -- blocks around the player, each way
local KINDS = {
	torch = { color = "#ffa050", intensity = 0.9, radius = 6, flicker = 0.4 },
	torch_pos_x = "torch", torch_neg_x = "torch", torch_pos_z = "torch", torch_neg_z = "torch",
	torch_pos_y = "torch", torch_neg_y = "torch",
	lantern = { color = "#ffd890", intensity = 1.1, radius = 9, flicker = 0.05 },
	fixed_lantern = "lantern", lantern_fancy = "lantern",
}

local placed = {} -- "x,y,z" -> light handle
local missing = {} -- blocks this game's rules do not have (fancy lanterns are Modern only)

local function scan()
	local wanted = {}
	local me = players.me()
	if mod.config.get("torches") and me ~= nil then
		local here = vector.floor(me.position)
		local from = here - vector.create(REACH, 16, REACH)
		local to = here + vector.create(REACH, 16, REACH)
		for block, look in KINDS do
			local style = type(look) == "string" and KINDS[look] or look
			local ok, cells = false, {}
			if not missing[block] then
				ok, cells = pcall(world.find_blocks, from, to, block, 256)
				missing[block] = not ok
			end
			for _, cell in (ok and cells or {}) do
				local key = string.format("%d,%d,%d", cell.x, cell.y, cell.z)
				wanted[key] = true
				if placed[key] == nil then
					placed[key] = light.create({
						position = cell + vector.create(0.5, 0.7, 0.5),
						color = style.color, intensity = style.intensity, radius = style.radius,
						flicker = style.flicker, flicker_speed = 8,
					})
				end
			end
		end
	end
	for key, handle in placed do
		if not wanted[key] then
			light.remove(handle)
			placed[key] = nil
		end
	end
end

events.on("world.loaded", scan)
timer.every(3, scan)
