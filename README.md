# CHIM Injury Framework

![CHIM Injury Framework](images/banner.jpg)

**Wounds that stay, show and get talked about.**

CHIM Injury Framework is an add-on for [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) (AI NPCs for Skyrim). It gives the player and the followers real wounds: a deep gash on the left arm, cracked ribs, a burn. The wounds show on the body, heal with time, healing magic and rest, and every NPC nearby knows about them.

> "Hold still. That cut on your arm is still bleeding, and I'm not carrying you back to Whiterun."

> **Status: 0.3.0, test version.** Works in game on Nolvus (Skyrim SE 1.5.97) with RaceMenu; testing continues. Please report results (see [Feedback](#feedback)).

## ⬇ Download

**[Download the latest CHIM Injury Framework for MO2 / Vortex (CHIM-Injury-Framework.zip)](https://github.com/Marcius3d/chim_injury_framework/releases/latest/download/CHIM-Injury-Framework.zip)**

This link always points to the newest release. Older versions are on the [Releases](https://github.com/Marcius3d/chim_injury_framework/releases) page; use the `CHIM-Injury-Framework-<version>.zip` file, not *Source code*.

---

## What it does

| When | What happens |
|---|---|
| A hit lands | If the player or a follower loses at least 5% of their maximum health in one hit, or takes a power or sneak attack, they get a wound. The weapon or spell decides the type, the health lost decides the severity, and the body region is picked at random (back if the hit came from behind). |
| During and after the fight | Medium and severe wounds are saved to CHIM's memory for everyone nearby, for example *"Lydia was wounded: deep gash on the left arm, from Bandit Chief's Steel Sword."* A short message shows in the corner of the screen. |
| When someone speaks | A wounded follower knows their own wounds, and NPCs see the wounds of the player and followers around them. This is added to the prompt only while someone is wounded. |
| On the body | With RaceMenu installed, cuts and bruises appear on the chest, back, arms and legs. Burns, frostbite and shock burns are tinted bruises. |
| Healing | See below. |

### Wound types

| Attack | Wound |
|---|---|
| Sword, axe, whip, claw weapon | cut (shallow cut, cut, deep gash) |
| Dagger, spear, rapier | stab wound |
| Bow, crossbow | arrow wound |
| Mace, warhammer, staff, fists, shield bash, giants | bruise, heavy bruising, then cracked ribs, a possible fracture or a concussion |
| Animals and monsters without weapons | claw scratches, bite wound, deep mauling |
| Fire, frost, shock magic and breath | burn, frostbite, shock burn |

Severity: light below 12% of maximum health in one hit, medium below 25%, severe above. Power and sneak attacks and hits that leave someone below 20% health are one step worse. Each person has at most 6 wounds; a new hit on the same wound makes it worse instead of adding a second one.

### Healing

| Wound | Heals by itself | Healing magic or potion | Sleeping in a bed (6 h) |
|---|---|---|---|
| Light | after 1 game day | closes | – |
| Medium | after 3 game days | closes | – |
| Severe | never | only **treated** (closes partly) | a **treated** severe wound heals fully |

- Healing spells count when cast on the wounded person (Healing, Healing Hands, Heal Other, ...). Potions count when they restore at least 15 health. Food and regeneration enchantments never heal wounds.
- Followers rest together with the player: when you sleep in a bed for at least 6 hours, their treated severe wounds heal too.
- Wounds are stored in the save game. Loading an older save brings back the wounds of that moment.

## Requirements

| Required | Notes |
|---|---|
| Skyrim SE/AE + [SKSE64](https://skse.silverlock.org/) | any runtime supported by Address Library |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | |
| [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) + DwemerDistro server | CHIM / HerikaServer 3.4.2 or newer (prompt injection) |

| Optional | What for |
|---|---|
| [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080) | Wounds drawn on the body. Without it everything else works. |
| A wound animation pack made for CHIM Injury Framework | Limping, holding a wound, splinted arms. None is public yet; see [For animation pack authors](#for-animation-pack-authors). |
| [Animation Queue Fix](https://www.nexusmods.com/skyrimspecialedition/mods/82395) (Ersh) | Recommended with an animation pack: prevents T-poses when many animations load at once. Nolvus already includes it. |

No NFF, no other wound or gore mod, no ESP/ESL: CHIM Injury Framework takes no plugin slot. Followers are found automatically (`IsPlayerTeammate` or the vanilla follower faction). Summons and thralls are ignored.

Body textures: women use the CBBE UV (CBBE, 3BA), men the vanilla male UV (vanilla, SOS, HIMBO). Beast races use the same body UVs.

## Installation

1. Download **[CHIM-Injury-Framework.zip](https://github.com/Marcius3d/chim_injury_framework/releases/latest/download/CHIM-Injury-Framework.zip)**. Do not use the *Source code* archives.
2. Install it with Mod Organizer 2 or Vortex like any other mod. Load order does not matter.
3. Start the game with the CHIM server running. CHIM finds the embedded server package (`CHIM/server-plugins/chim_injury_framework/`) and installs it on the server automatically.
4. A few seconds after the save loads you should see `CHIM-injury: activated`.
5. Optional: open the CHIM web UI → **Server Plugins** → *CHIM Injury Framework* → **Plugin Page**.

The server part can also be installed or updated from CHIM's **Plugin Manager** (GitHub topic `chim-plugin`). The game part still comes from the zip above, because the server cannot install `CHIMInjury.dll`.

**Updating:** install the new zip over the old one. The server part updates on the next game start.

**Uninstalling:** on the plugin page press **Remove all wounds and overlays** with your followers nearby, wait 15 seconds, save, then remove the mod. This removes the RaceMenu overlays the mod placed, so no empty overlay stays in your save.

## Plugin page

CHIM web UI → **Server Plugins** → *CHIM Injury Framework* → **Plugin Page**. Changes reach the game within about 15 seconds.

| Section | What is there |
|---|---|
| Status | game plugin connected, RaceMenu and its overlay slots, counters, last error |
| Wounds | every tracked person with their wounds: type, region, severity, status, when it heals, who caused it. Buttons: *Remove* a wound, *Heal like a potion*, *Heal everything*, *Heal everyone*, *Remove all wounds and overlays* |
| General | enable, track player, track followers, on-screen messages (from which severity), debug mode |
| On the body | draw wounds, also on the player, strength |
| Wounds | damage thresholds for a wound and for medium and severe, max wounds per person |
| Healing | days for light and medium wounds, smallest potion or spell that heals, hours of sleep |
| CHIM | save wounds to CHIM's memory (from which severity), speaker's own wounds, visible wounds of others, both texts |
| Diagnostics | **Create diagnostic file** (one .txt for bug reports), game and server log, clear log |

`Data/SKSE/Plugins/CHIMInjury.ini` holds the same game options. They are used only while the server plugin cannot be reached.

### Body overlay slots

RaceMenu has 6 body overlay slots by default (`iNumOverlays` under `[Overlays/Body]` in `skee64.ini`). CHIM Injury Framework uses only slots that are empty or its own, highest numbers first, and never touches body paints you placed yourself. One slot is needed per wounded region (chest, back, each arm, each leg). If you use many body paints, raise `iNumOverlays` (for example to 10). The plugin page shows when wounds could not be drawn for lack of slots.

### Logs

- Game: `Documents\My Games\Skyrim Special Edition\SKSE\chim-injury.log`. Recent lines are also sent to the server and shown on the plugin page.
- Server: the Diagnostics section of the plugin page (last 2000 entries).

## How it works

```text
 Skyrim                                          DwemerDistro / HerikaServer
 ────────────────────────────────────────────    ─────────────────────────────────────────
 TESHitEvent on the player or a follower
   └─ health before and 0.35 s after the hit
      wound? type, region, severity
      ├─ saved in the SKSE co-save (with the game save)
      ├─ RaceMenu: NiOverride body overlay slot
      └─ AIAgentFunctions.logMessage ──────────▶ ext/chim_injury_framework/prerequest.php
                                                   eventlog "info_injury" for everyone nearby
 potion (TESEquipEvent), healing spell
 (TESMagicEffectApplyEvent), sleep (TESSleep*)
   └─ heal / treat
 every 15 s and after each change ── status + wounds ─▶ api/status.php
                                  ◀── settings + page commands ── api/config.php
                                                 context_pre.php: wounds added to the prompt
                                                   of the NPC who speaks
```

- **`CHIMInjury.dll`** is an SKSE plugin built with CommonLibSSE-NG. It talks to CHIM only through `AIAgentFunctions`, the API CHIM's own scripts use, and to RaceMenu only through the public `NiOverride` Papyrus functions. It works without the server (settings from the INI), and without RaceMenu (no visuals).
- **`ext/chim_injury_framework/`** is a HerikaServer plugin with two hooks (`prerequest.php` for `info_injury` events, `context_pre.php` for the prompt), a settings page, two API endpoints (`config`, `status`) and two tables, `plugins.chim_injury_settings` and `plugins.chim_injury_log`. Prompt text uses CHIM's `chimRegisterPromptInjection` (`character_bottom` and `prompt_bottom`).
- Nothing changes in combat: no slowdown, no limping, no extra animations.

## For animation pack authors

CHIM Injury Framework does not ship animations, but it tells OAR/DAR what state a wounded person is in. Ship `CHIMInjury_Animations.esp` with your pack (make it with `python tools/make_anim_plugin.py`, ESL-flagged, keywords only) and use conditions like `HasKeyword("CHIMInjury_Animations.esp"|0x000808)`. The DLL adds the keywords to the wounded person's base form and removes them when the wounds heal (player and followers, from the severity set on the plugin page). By default the keywords come and go in episodes (on 5–10 s, off 20–25 s), so the animations play now and then.

| Form ID | Keyword | Set when |
|---|---|---|
| 0x800 | `CHIMInjury_Anim_Concussion` | head: medium blunt wound, or severe untreated cut |
| 0x801 / 0x802 | `..._SkullBroken` / `..._SkullSevere` | head: severe blunt wound / and not yet treated |
| 0x803 / 0x804 | `..._TorsoCut` / `..._TorsoCutTreated` | chest or back: open cut, stab, bite or burn / treated severe one |
| 0x805 / 0x806 / 0x807 | `..._RibsSoft` / `..._RibsMiddle` / `..._RibsSevere` | chest or back blunt: medium / severe treated / severe untreated |
| 0x808–0x80B | `..._ArmLCut`, `..._ArmLCutTreated`, `..._ArmLBroken`, `..._ArmLSplinted` | left arm: open cut / treated severe cut / severe blunt / treated severe blunt |
| 0x80C–0x80F | same for `ArmR` | right arm |
| 0x810–0x813 | same for `LegL` | left leg |
| 0x814–0x817 | same for `LegR` | right leg |

## Feedback

Please open an [issue](https://github.com/Marcius3d/chim_injury_framework/issues) and attach the diagnostic file from the plugin page (**Create diagnostic file**). It contains versions, settings, the current wounds and the recent game and server log, but no API keys or dialogue.

## Credits and licence

- Code: MIT licence (see `LICENSE`), by Marcius3d. And Santa's little helper Claude.
- Wound textures: **[Wounds Overlays For RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/38547)** by **JaySerpa**, used and modified with the author's permission; wound photo from the [bmastock stock image](https://www.deviantart.com/bmastock/art/Wound-01-458522921). The male versions were converted from them to the male UV. The textures are **not** under the MIT licence: do not reuse them elsewhere, sell them or use them for Donation Points.
- Inspired by [Injury Engine](https://github.com/GoodProvider/Injury_Engine) by GoodProvider (SkyrimNet, MIT): the idea of wound type, body region and severity from the hit. No code or assets are taken from it.
- [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) by Dwemer Dynamics, [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080) by expired6978, CommonLibSSE-NG.
