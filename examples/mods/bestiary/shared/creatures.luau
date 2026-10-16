-- Bestiary: the creatures and projectiles. Definitions go in shared/ so both
-- sides know them; everything they do runs on the server (server/).
--
-- `health` is in retail enemy units (a pistol round is about 1), `damage` in
-- player health (100 = full). An attack's `windup` is the time from its
-- animation starting to the hit: the clip's strike frame lands with it.

-- An arrow arcs (gravity); the Hollow King's hex homes in and bursts. Their
-- trails and the hex's burst are effects of client/fx.luau (the Enhanced
-- presentation draws them; the Original one draws the models alone).
projectiles.define("bone_arrow", {
	speed = 28, gravity = 9.8, damage = 14, radius = 0.12, lifetime = 5, model = "models/bone_arrow", scale = 1.2,
	fx = "arrow_streak",
})
projectiles.define("hex_bolt", {
	speed = 11, homing = 150, damage = 12, radius = 0.3, lifetime = 8, model = "models/hex_bolt",
	fx = "hex_trail", impact_fx = "hex_burst",
	-- Blocks break only where the session lets monsters break them (Hard,
	-- Hardcore, Endurance), never in spawn protection. (breaks_blocks = true
	-- would break them whatever the difficulty, still never in spawn protection.)
	explosion = { radius = 2.5, damage = 16, blocks = true },
})

-- The zombie's replacement: packs that stalk, surround and scatter (the
-- pack_hunter brain, server/brains.luau). They wear the retail zombie from
-- the player's own game when it is installed, and this mod's ghoul otherwise.
creatures.define("ghoul", {
	name = "Ghoul", model = "retail:zombie|models/ghoul", behaviour = "hostile", brain = "pack_hunter",
	team = "undead", hostile_to = { "players", "beasts" },
	health = 14, speed = 1.6, run_speed = 4.4, size = { 0.6, 1.85 },
	senses = { sight = 22, fov = 130, hearing = 28, memory = 8 },
	attacks = { claw = { reach = 1.7, damage = 18, creature_damage = 3, cooldown = 1.1, windup = 0.45 } },
	drops = "ghoul",
	spawn = { time = "night", surface = true, chance = 0.05, max = 6, group_min = 3, group_max = 5 },
})

-- Archers keep their distance and shoot arcing arrows; the built-in hunter
-- (no brain named) does all of that from the attack's range and min_range.
creatures.define("bone_archer", {
	name = "Bone Archer", model = "models/bone_archer", behaviour = "hostile",
	team = "undead", hostile_to = { "players", "beasts" },
	health = 9, speed = 1.8, run_speed = 3.6, size = { 0.5, 1.85 },
	senses = { sight = 30, fov = 140, hearing = 20, memory = 10 },
	attacks = { shoot = { projectile = "bone_arrow", range = 26, min_range = 4, cooldown = 2.2, windup = 0.5,
		spread = 1.5 } },
	drops = "stick",
	spawn = { time = "night", surface = true, chance = 0.03, max = 3 },
})

-- Flyers cruise `height` above the ground and dive at what they hunt.
creatures.define("gloom_bat", {
	name = "Gloom Bat", model = "models/gloom_bat", behaviour = "hostile",
	team = "undead", hostile_to = { "players" },
	health = 3, speed = 3, run_speed = 7, size = { 0.5, 0.45 },
	movement = { mode = "fly", height = 6 },
	senses = { sight = 24, fov = 360, hearing = 32, memory = 6 },
	attacks = { bite = { reach = 1.4, damage = 6, cooldown = 0.9, windup = 0.2 } },
	spawn = { time = "night", surface = true, chance = 0.03, max = 4, group_min = 2, group_max = 3 },
})

-- Walkers can be tuned: leapers spring up three blocks where a ghoul climbs
-- one, and drop ten, so a wall or a ledge does not keep them off you. They
-- hunt in the dark below ground.
creatures.define("grave_leaper", {
	name = "Grave Leaper", model = "models/grave_leaper", behaviour = "hostile",
	team = "undead", hostile_to = { "players" },
	health = 6, speed = 2, run_speed = 5.2, size = { 0.6, 1.2 },
	movement = { mode = "ground", jump = 3, fall = 10 },
	senses = { sight = 18, fov = 160, hearing = 26, memory = 6 },
	attacks = { claws = { reach = 1.5, damage = 10, cooldown = 1.0, windup = 0.3 } },
	spawn = { surface = false, y_max = 0, light_max = 0.3, chance = 0.04, max = 4, group_min = 1, group_max = 2 },
})

-- Wolves hunt the dead and leave players alone unless hurt (teams in
-- server/rules.luau): an ally in the night, if you let them be.
creatures.define("grey_wolf", {
	name = "Grey Wolf", model = "models/grey_wolf", behaviour = "neutral",
	team = "beasts", hostile_to = { "undead" },
	health = 10, speed = 3, run_speed = 6.5, size = { 0.6, 0.9 },
	senses = { sight = 26, fov = 220, hearing = 30, memory = 8 },
	attacks = { bite = { reach = 1.5, damage = 9, creature_damage = 4, cooldown = 0.8, windup = 0.25 } },
	spawn = { biomes = { "classic", "mountain", "arctic" }, surface = true, chance = 0.03, max = 4,
		group_min = 2, group_max = 3 },
})

-- The boss: two phases at 60% and 25% health, a bar on every client nearby,
-- music, an arena it will not be dragged out of, and a brain of its own.
creatures.define("hollow_king", {
	name = "The Hollow King", model = "models/hollow_king", behaviour = "hostile", brain = "hollow_king",
	team = "undead", hostile_to = { "players", "beasts" },
	health = 160, speed = 1.6, run_speed = 3.4, size = { 1.2, 2.8 },
	senses = { sight = 32, fov = 360, hearing = 40, memory = 30 },
	attacks = {
		swipe = { reach = 2.6, damage = 22, cooldown = 1.6, windup = 0.55, arc = 140 },
		hex = { projectile = "hex_bolt", range = 28, min_range = 5, cooldown = 4, windup = 0.6,
			animation = "cast" },
	},
	abilities = {
		slam = { cooldown = 8, range = 5, windup = 0.8, animation = "slam", run = function(king, target)
			local at = entities.get(king).position
			for _, player in players.list() do
				local distance = vector.magnitude(players.position(player) - at)
				if distance < 5 then
					players.damage(player, 28 * (1 - distance / 6))
				end
			end
			ai.noise(at, 40, king)
		end },
		summon = { cooldown = 14, windup = 1, animation = "cast", run = function(king, target)
			local at = entities.get(king).position
			for i = 1, 2 do
				local angle = util.random() * math.pi * 2
				local ghoul = entities.spawn("ghoul", at + vector.create(math.cos(angle) * 3, 0, math.sin(angle) * 3),
					{ pack = king })
				if ghoul ~= nil and target ~= nil then
					ai.set_target(ghoul, target)
				end
			end
		end },
	},
	boss = {
		title = "The Hollow King", color = "#8a4fd0", music = "music/hollow_king", arena = 28,
		phases = {
			{ below = 0.6, name = "Wrath", state = "frenzy", speed = 1.3, damage = 1.4 },
			{ below = 0.25, name = "Last Stand", state = "last_stand", speed = 1.6, damage = 2,
				music = "music/last_stand" },
		},
	},
	drops = "crown",
})

loot.define("ghoul", { rolls = 1, entries = { { item = "coal", min = 0, max = 2 } } })
loot.define("crown", { rolls = 3, entries = { { item = "gold", min = 2, max = 5 }, { item = "diamond", min = 1, max = 2 } } })
