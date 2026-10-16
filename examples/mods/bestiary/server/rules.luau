-- Bestiary: the session's rules. Every retail enemy is switched off for
-- everyone in the session (each player's game spawns its own enemies, and
-- each one follows these rules); this mod's creatures take their place.
-- clear_existing sends the retail enemies already about away too (retail's
-- own give-up: no drops), on every player's game. Server mods only run in
-- sessions no player of the original game can join, so nobody plays by
-- other rules.
enemies.rules({ enabled = false, clear_existing = true })

-- Finer control is a table away, for an overhaul that keeps some of them:
--   enemies.rules({
--       zombie = false, skeleton = false, dragon = false,   -- families
--       distance = { scale = 0.12, max = 3400 },            -- tiers in a 25,000-block world
--       regions = { { biome = "frontier:ashlands", enabled = false } },
--   })

-- Teams: the dead hunt players and beasts; wolves hunt the dead and only
-- fight players who hurt them (the creature definitions list whom each
-- attacks on sight; this makes the players' side of it explicit).
teams.set_relation("beasts", "players", "neutral")
teams.set_relation("undead", "beasts", "hostile", { one_way = true })
