# Changelog

## 0.1.3

- Support for wound animation packs: when `CHIMInjury_Animations.esp` (keywords only) is installed, wounded people get state keywords (concussion, broken skull, torso cut, broken ribs, cut / broken / splinted arms and legs) that OAR/DAR conditions can use. Works for the player and followers. Settings: on/off and from which severity (default medium).
- `tools/make_anim_plugin.py` writes that keyword plugin.

## 0.1.2

- Fixed: bruises, burns, frostbite and shock burns on the back were drawn on the front and side of the torso. They now use the back texture, tinted.

## 0.1.1

- Wound textures included (Wounds Overlays For RaceMenu by JaySerpa, used with permission; male versions converted to the male UV). Wounds are now drawn on the body when RaceMenu is installed.

## 0.1.0 (first test version)

- Wounds for the player and followers (followers are found automatically, no NFF needed; summons are ignored).
- A hit makes a wound when it takes at least 5% of the maximum health, or is a power or sneak attack.
  - Type from the weapon or spell: cut, stab, arrow, blunt, mauling (creatures), burn, frostbite, shock burn.
  - Body region: head, chest, back (hit from behind), arms, legs.
  - Severity from the health lost in the hit: light, medium, severe. Power and sneak attacks and hits below 20% health are one step worse.
  - At most 6 wounds per person; the same wound in the same place gets worse instead of doubling.
- Healing:
  - light wounds heal in 1 game day, medium ones in 3;
  - healing spells and potions close light and medium wounds; severe wounds only get treated;
  - a treated severe wound heals after sleeping in a bed (6 h); followers rest when the player sleeps.
- Wounds are kept in the save game, so loading an older save brings back the wounds of that moment.
- RaceMenu body overlays (optional): cuts and bruises on the chest, back, arms and legs for women (CBBE/3BA UV) and men (HIMBO/vanilla UV). Only free overlay slots are used; your own body paints are left alone.
- CHIM:
  - medium and severe wounds, treatment and rest are saved to CHIM's memory for everyone nearby;
  - a wounded follower knows their own wounds when they speak; NPCs see the wounds of the player and followers nearby (prompt injection, needs CHIM 3.4.2 or newer).
- Plugin page with banner: wounds of everyone (heal, treat, remove), all settings, diagnostics file and log.
