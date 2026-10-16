-- The story: the crypt and the hermit appear with the first player, every
-- player is given the quest, the hermit answers according to the player's
-- progress, the ghouls rise when the quest asks for them, and the end pays.

local crypt = require("./lib/crypt")

local HERMIT = "story_dungeon:hermit"
local GHOUL = "story_dungeon:crypt_ghoul"

-- Every client learns where the crypt is (its dark fog, lights and sound are
-- drawn client side; see client/crypt.luau).
local function publish(info)
	replicated.set("world", "crypt", {
		min = info.min, max = info.max, center = info.center, lights = info.lights,
	})
end

local function ensure_hermit(near: vector)
	if #entities.list({ kind = HERMIT }) > 0 then
		return
	end
	local x, z = near.x + 6, near.z - 2
	local ground = world.surface_y(x, z) or near.y - 1
	entities.spawn(HERMIT, vector.create(x, ground + 1, z), { persistent = true, behaviour = "idle" })
end

-- The first player into the world brings the story with them; everyone
-- (later joiners too) gets the quest unless they finished it.
events.on("player.spawned", function(event)
	local info = crypt.ensure(event.position)
	publish(info)
	ensure_hermit(event.position)
	local status = quests.status(event.player, "crypt")
	if not status.active and not status.completed then
		quests.start(event.player, "crypt")
		ui.toast(event.player, ui.text("toast.quest"), ui.text("toast.journal"), { color = "#c8a0ff" })
	end
end)

-- A world reloaded later: publish what was built before.
events.on("world.loaded", function()
	local info = crypt.current()
	if info ~= nil then
		publish(info)
	end
end)

-- The hermit opens with what fits the player's progress. Returning false
-- stops the default opening (the talk objective still counts first).
events.on("entity.interacted", function(event)
	if event.kind ~= HERMIT then
		return
	end
	local status = quests.status(event.player, "crypt")
	local node = "greet"
	if status.completed then
		node = "after"
	elseif status.active and status.stage == "talk" then
		node = "request"
	elseif status.active and status.stage == "clear" then
		node = "waiting"
	elseif status.active and status.stage == "return" then
		node = "thanks"
	end
	dialogue.open(event.player, "hermit", { node = node, entity = event.entity })
	return false
end)

-- The quest moved on (shared/content.luau passes this on from on_stage).
events.on("story_dungeon:stage", function(event)
	if event.stage ~= "clear" then
		return
	end
	local info = crypt.current()
	if info == nil then
		return
	end
	-- One ghoul per spawn point, unless they already walk.
	if #entities.list({ kind = GHOUL }) >= #info.spawns then
		return
	end
	for _, point in info.spawns do
		entities.spawn(GHOUL, point, { persistent = true, behaviour = "wander" })
	end
end)

events.on("story_dungeon:complete", function(event)
	ui.title(event.player, ui.text("title.complete"), ui.text("quest.title"), { stay = 3 })
end)
