-- The Restless Crypt: blocks, the reward, the two creatures, the hermit's
-- dialogue and the quest. Declared for both sides; the functions inside run on
-- the server only (dialogue conditions, quest callbacks, item use).

local HERMIT = ui.text("hermit.name")

blocks.define("crypt_brick", { name = "Crypt Brick", fallback = "rock", texture = "textures/crypt_brick" })
blocks.define("mossy_brick", { name = "Mossy Crypt Brick", fallback = "rock", texture = "textures/mossy_brick" })

-- Using the charm mends you for a while (the built-in "regen" effect).
items.define("hermit_charm", {
	name = "Hermit's Charm", description = "Use it to mend: +1 health a second for 20 seconds.",
	fallback = "copper", icon = "textures/icons/hermit_charm", stack = 1, cooldown = 60,
	use = function(player)
		effects.apply(player, "regen", { duration = 20, strength = 1 })
	end,
})

loot.define("ghoul_remains", {
	rolls = { 1, 2 },
	entries = { { item = "coal", weight = 3, min = 1, max = 2 }, { item = "iron", weight = 1 } },
})

creatures.define("hermit", {
	name = HERMIT, model = "models/hermit", behaviour = "npc", dialogue = "hermit",
	health = 50, speed = 1.2, size = { 0.6, 1.8 }, show_name = true,
})
creatures.define("crypt_ghoul", {
	name = "Crypt Ghoul", model = "models/ghoul", behaviour = "hostile",
	health = 6, damage = 8, speed = 2.0, run_speed = 3.6, view_range = 14,
	attack_range = 1.7, attack_cooldown = 1.2, size = { 0.6, 1.9 }, drops = "ghoul_remains",
})

-- server/quest.luau decides which node the hermit opens with.
dialogue.define("hermit", {
	title = HERMIT,
	start = "greet",
	nodes = {
		greet = { speaker = HERMIT, text = ui.text("hermit.greet"), choices = {
			{ text = ui.text("choice.ask"), next = "request" },
			{ text = ui.text("choice.bye") },
		} },
		request = { speaker = HERMIT, text = ui.text("hermit.request"), choices = {
			{ text = ui.text("choice.yes"), next = "accept" },
			{ text = ui.text("choice.no") },
		} },
		accept = { speaker = HERMIT, text = ui.text("hermit.accept") },
		waiting = { speaker = HERMIT, text = ui.text("hermit.waiting") },
		thanks = { speaker = HERMIT, text = ui.text("hermit.thanks") },
		after = { speaker = HERMIT, text = ui.text("hermit.after") },
	},
})

-- Talk, clear, return. The callbacks pass the moment on as this mod's own
-- events, which server/quest.luau handles (shared code cannot require
-- server code, because shared files also load on the client).
quests.define("crypt", {
	title = ui.text("quest.title"),
	description = ui.text("quest.description"),
	stages = {
		{ id = "talk", text = ui.text("quest.talk"), objectives = {
			{ id = "met", type = "talk", target = "story_dungeon:hermit", text = ui.text("quest.talk") },
		} },
		{ id = "clear", text = ui.text("quest.clear"), objectives = {
			{ id = "ghouls", type = "kill", target = "story_dungeon:crypt_ghoul", count = 4, text = ui.text("quest.clear") },
		} },
		{ id = "return", text = ui.text("quest.return"), objectives = {
			{ id = "report", type = "talk", target = "story_dungeon:hermit", text = ui.text("quest.return") },
		} },
	},
	rewards = { items = { { "story_dungeon:hermit_charm", 1 }, { "diamond", 2 } } },
	on_stage = function(player, _quest, stage)
		events.emit("story_dungeon:stage", { player = player, stage = stage })
	end,
	on_complete = function(player, _quest)
		events.emit("story_dungeon:complete", { player = player })
	end,
})
