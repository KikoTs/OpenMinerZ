-- The Wilderness weapons: an iron broadsword and a crude stone club, with
-- their own low-poly models (assets/models/weapons/, drawn by
-- tools/ModSdk/make_wilderness_weapons.py) and melee stats. Declared once for
-- both sides: every client draws them in the hand and on the ground, the
-- server judges every swing by these numbers (a client cannot claim more).
--
-- `weapon` stats: `damage` to enemies and creatures in retail enemy units (a
-- knife deals 0.5, a gold knife 1, a diamond knife 2; a zombie has 1 to 18
-- health), `player_damage` in health points when PVP allows it (every retail
-- melee item deals 21), `reach` in blocks from the eye (retail: 5) and
-- `cooldown` in seconds between swings. The `fallback` is what vanilla
-- inventories and the save hold: a weapon's must be an item that strikes.

items.define("iron_broadsword", {
	name = ui.text("item.iron_broadsword"),
	description = ui.text("about.iron_broadsword"),
	fallback = "knife",
	icon = "textures/icons/iron_broadsword",
	model = "models/weapons/iron_broadsword", -- clips: idle, equip, use
	hold = "swing",
	stack = 1,
	weapon = { damage = 1.5, player_damage = 26, reach = 5, cooldown = 0.55 },
})

items.define("stone_club", {
	name = ui.text("item.stone_club"),
	description = ui.text("about.stone_club"),
	fallback = "stick",
	icon = "textures/icons/stone_club",
	model = "models/weapons/stone_club",
	hold = "swing",
	stack = 1,
	weapon = { damage = 0.8, player_damage = 18, reach = 4.5, cooldown = 0.75 },
})

-- Three iron bars and a stick; a stick, a log and two rocks.
recipes.add({
	result = "iron_broadsword", count = 1, category = "Wilderness",
	ingredients = { { "iron", 3 }, { "stick", 1 } },
})
recipes.add({
	result = "stone_club", count = 1, category = "Wilderness",
	ingredients = { { "stick", 1 }, { "log_block", 1 }, { "rock_block", 2 } },
})
