-- Frontier's own blocks. Each names a retail fallback: what the save, stock
-- messages and a world opened without the mod hold. In generated terrain every
-- peer works out which cells are these blocks from the generator itself.

blocks.define("redwood_bark", {
	name = "Redwood Bark", fallback = "log",
	texture = "textures/redwood_bark", faces = { top = "textures/redwood_rings", bottom = "textures/redwood_rings" },
})
blocks.define("redwood_needles", { name = "Redwood Needles", fallback = "leaves", texture = "textures/redwood_needles" })
blocks.define("salt_crust", { name = "Salt Crust", fallback = "sand", texture = "textures/salt_crust" })
blocks.define("rock_salt", { name = "Rock Salt", fallback = "rock", texture = "textures/rock_salt" })
blocks.define("ash", { name = "Ash", fallback = "sand", texture = "textures/ash" })
blocks.define("cinder_stone", { name = "Cinder Stone", fallback = "rock", texture = "textures/cinder_stone" })
blocks.define("basalt", {
	name = "Basalt", fallback = "rock",
	texture = "textures/basalt_side", faces = { top = "textures/basalt_top", bottom = "textures/basalt_top" },
})
blocks.define("ember_ore", { name = "Ember Ore", fallback = "coal_ore", texture = "textures/ember_ore", drops = "ember" })

items.define("ember", {
	name = "Ember", description = "A shard that stays warm. Burns like coal.",
	fallback = "coal", icon = "textures/icons/ember", stack = 64,
})
