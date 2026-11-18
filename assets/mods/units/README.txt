Put your mods here.
===================

One folder per mod, exactly like Rusted Warfare's own mods folder:

    assets/mods/units/YOURMOD/
        mod-info.txt        <- optional, but this is where the title / description come from
        your_unit.ini
        your_unit.png

    [mod]
    title: My Mod
    description: What this mod changes.
    minVersion: v1.15p7
    thumbnail: mod-thumbnail.png

A single .ini file dropped straight into this folder also counts as a mod.

The Mods screen in game (Main Menu -> Mods) lists everything it finds under:

    assets/units                    core data, always loaded, not toggleable
    assets/mods/units               this folder - enabled by default
    assets/builtin_mods             optional built-in mods - disabled by default
    assets/builtin_mods_enabled     built-in mods that ship enabled

Toggle a mod, press Apply, and every custom unit is reloaded. The choice is stored in
assets/mods_enabled.txt, so it survives a restart.

You can also drag a mod folder onto the Mods screen to copy it into this directory.

Do NOT add modded units under assets/units: that folder is the core data set, and a unit
defined there overrides the shipped one for everyone.
