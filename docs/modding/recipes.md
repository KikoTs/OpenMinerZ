# Tested scripting recipes

These snippets run in the documentation test harness. Put each block in the
folder named by its fence (`shared`, `server`, `client`); `client-mod` means
`client/` in a client-scoped pack. Server/client pairs belong to the same
server-scoped add-on. The examples use `mymod` as the add-on ID.

## Items and commands

Use a built-in fallback while developing your own art:

```luau shared
items.define("field_canteen", {name = "Field Canteen", fallback = "compass", stack = 1})
```

Give items only on the authoritative side:

```luau server
commands.register("kit", "Give a small starter kit", function(player)
    players.give(player, "iron_pickaxe", 1)
    players.give(player, "torch", 8)
    players.give(player, "mymod:field_canteen", 1)
    return "Starter kit added."
end)
```

## Persistent stats

Define the stat on both sides so the client knows how to display it:

```luau shared
stats.define("energy", {
    min = 0, max = 100, initial = 100, persist = true, decay = 0.1,
    hud = {label = "Energy", color = "#80c050"},
})
```

React on the server. Persist progression through `storage`, not a Lua global:

```luau server
events.on("stat.depleted", function(event)
    if event.stat == "energy" then
        ui.toast(event.player, "Tired", "Your energy is empty.")
    end
end)
events.on("player.joined", function(event)
    local visits = (storage.player.get(event.player, "visits") or 0) + 1
    storage.player.set(event.player, "visits", visits)
end)
```

## World edits

Use the world service so edits participate in saves and replication. Large
fills are scheduled over ticks. This changes the test world at the given cells:

```luau server
world.set_block(vector.create(10, 40, 10), "rock")
world.fill(vector.create(0, 30, 0), vector.create(3, 30, 3), "wood", function(_job, changed)
    log.info("placed", changed, "cells")
end)
local hit = world.raycast(vector.create(0, 60, 0), vector.create(0, -1, 0), 128)
if hit ~= nil then log.info("ground", hit.block, hit.position) end
```

For an entire terrain generator use `terrain.define` and the complete
`frontier` example. Keep generation deterministic for a seed and coordinate;
do not read player state or wall-clock time from a terrain expression.

## Client/server requests

Declare the payload schema on the receiving server. The server derives the
player identity from the connection, never from a player ID in the payload:

```luau server
net.on("greet", function(player, payload)
    net.send_to(player, "greeting", {text = "Hello, " .. players.get(player).name .. payload.suffix})
end, {suffix = {type = "string", max = 8}})
```

The paired client script sends a request and displays the response:

```luau client
net.on("greeting", function(payload)
    ui.toast("Greeting", payload.text)
end)
input.bind("greet", "B", function()
    net.send("greet", {suffix = "!"})
end, {label = "Greet"})
```

For purchases, damage or interactions, additionally validate prices, ownership,
distance, cooldowns and available inventory on the server. A schema checks the
shape of a request; it does not authorize its gameplay effect.

## Replicated state

Write shared state on the server:

```luau server
replicated.set("world", "objective", {name = "Explore the hills", stage = 1})
```

Observe it on the client and tolerate absent values while joining:

```luau client
replicated.on_change("world", "objective", function(value, _old)
    if value ~= nil then ui.title(value.name, "Stage " .. value.stage) end
end)
events.on("net.ready", function()
    log.info("current objective", replicated.get("world", "objective"))
end)
```

## A client HUD widget

UI handles belong to the client that created them. Update a widget instead of
recreating it every frame:

```luau client-mod
local panel = ui.add_hud({
    kind = "panel", anchor = "top_right", x = -16, y = 16, padding = 8,
    children = {{kind = "label", id = "day", text = "", wrap = false}},
})
local label = ui.find(panel, "day")
timer.every(1, function()
    ui.set(label, {text = string.format("Day %d", time.day())})
end)
```

## A client light

This affects presentation only. The host still owns actual gameplay light and
world rules:

```luau client-mod
local lamp = light.create({
    position = vector.create(12.5, 41.2, -3.5),
    color = "#ff9040", intensity = 1.4, radius = 8,
    flicker = 0.4, flicker_speed = 8,
})
light.set(lamp, {intensity = 0.8})
```

For creature AI, quests, structures, audio and effects, use the complete example
folders and [API reference](api-reference.md). They include their required media
and interconnected definitions rather than leaving you to guess dependencies.
