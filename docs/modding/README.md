# Add-ons

An add-on is a folder containing `openminerz.mod`, Luau scripts and optional
assets. Start by installing a complete example, then change one feature at a time.

| Guide | Use it for |
| --- | --- |
| [Authoring](authoring.md) | Installation, manifest, client/server scope, SDK, testing and publishing |
| [Recipes](recipes.md) | Small runnable scripts for gameplay, UI and networking |
| [Animation and Rig Lab](animation.md) | Player clips, weapon parts, live grip editing and resets |
| [API reference](api-reference.md) | Exact registered functions, arguments and events |
| [Development map](../development.md) | Extending the native engine or adding an API |

Install folders under `%LOCALAPPDATA%\OpenMinerZ\Mods\`, or `addons/` beside the
executable. **Add-Ons > Open Mods Folder** opens the per-user directory. Installed
add-ons start on; the bundled examples in `examples/mods/` start off. Enable or
disable them in the main menu before opening a world.

Client add-ons affect presentation and can be used on stock servers. Server
add-ons run gameplay logic on the host and require matching OpenMinerZ clients.
The host's server-mod set controls joining; a player's local toggles do not
override it. World-generator add-ons are recorded as world dependencies.

For a working starting point, copy a folder from [examples/mods](../../examples/mods/)
into your Mods directory. Edit the copy: a user mod replaces the bundled version
with the same ID. `wilderness` replaces `survival` and `wildlife` and cannot run
alongside another world generator such as `frontier`.

Script errors appear on the add-on row and in terminal output. `/mods` shows
loaded mods; `/mods reload` reloads local scripts for development. Restart hosting
before testing changed scripts with new clients so their files and digests match.
