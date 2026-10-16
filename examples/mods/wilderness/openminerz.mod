# The Wilderness example: a whole survival game on top of OpenMinerZ, as one
# server mod (everyone in a session runs it). It replaces the smaller Survival
# and Wildlife examples (two hunger bars, two herds): conflicts= keeps them off
# while it is on. It also replaces the world generator, so it cannot be on
# together with Frontier either; the Add-Ons screen sees that by itself.
id=wilderness
version=1.0.1
scope=server
protocol=both
order=200
name=Wilderness
author=OpenMinerZ examples
description=Tall mountains, meadows and pine forests with lakes, hunger and thirst, farm animals, food, water gathering and a new weapon.
api=1
conflicts=survival, wildlife
