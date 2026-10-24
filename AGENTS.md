# CHIM Injury Framework: agent notes

This repository is the source of a CHIM add-on. It has two parts.

| Part | Path | Installs to |
|---|---|---|
| SKSE plugin (C++, CommonLibSSE-NG) | `SkyrimPlugin/` | `Data/SKSE/Plugins/CHIMInjury.dll` + `.ini` |
| HerikaServer plugin (PHP) | repository root (`prerequest.php`, `context_pre.php`, `index.php`, `lib/`, `api/`, `migrations/`, `images/`, `manifest.json`) | `HerikaServer/ext/chim_injury_framework/` |
| Overlay textures (optional, only with the texture author's permission) | `Textures/actors/character/CHIMInjury/{Female,Male}/*.dds` | `Data/textures/actors/character/CHIMInjury/...` |

## Contracts to preserve

- **Game state.** The DLL owns the wounds. They live in the SKSE co-save (`'CHIJ'`, record `'WNDS'` version 1), so loading a save restores its wounds. The server only mirrors the last report. Change the record version when the layout changes and keep reading the old one.
- **Game → CHIM.** `AIAgentFunctions.logMessage(text, "info_injury")`. The text starts with `[injury actor="Name" event=new|worse|treated|rested]`. `prerequest.php` removes the marker, stores the event for everyone nearby (`DataBeingsInRange()`) and terminates the request.
- **Game → RaceMenu.** Only the public `NiOverride` Papyrus functions: `GetNodeOverrideString`, `AddNodeOverrideString/Int/Float`, `RemoveAllNodeNameOverrides`, `AddOverlays`. Nodes `Body [Ovl#]`, keys 9 (texture, index 0), 7 (tint, index -1), 8 (alpha, index -1), persisted. A slot is ours when its texture path contains `CHIMInjury`; only empty/`default.dds` slots or ours are used. The slot count comes from `skee64.ini` `[Overlays/Body] iNumOverlays`.
- **Game ↔ server HTTP.** The DLL resolves the server like CHIM does (`CHIMInjury.ini [Server]` → `AIAgent.ini` → `127.0.0.1:7135/discover` → `127.0.0.1:8081`).
  - `GET api/config.php` returns plain `key=value` lines (keys in `chimInjuryGameKeys()`, unknown keys ignored) plus `cmd=<id>|<action>|<actor hex>|<wound key>` lines. Actions: `heal`, `treat`, `remove`, `heal_all`, `reset`. The DLL runs each id once.
  - `POST api/status.php` takes flat JSON status, `cmd_done` (last executed command), `state` (wounds of everyone tracked) and `log_lines` (up to 300). Executed commands are pruned.
  - Keys shared by both sides: regions `head chest back left_arm right_arm left_leg right_leg`, types `slash pierce blunt maul burn frost shock`, wound key `<region>:<type>`. Keep `kRegionKeys`/`kTypeKeys` (main.cpp) and `chimInjuryRegionLabel()` (lib) in sync.
- **Prompt.** `context_pre.php` calls `chimRegisterPromptInjection`: `character_bottom` (speaker's own wounds) and `prompt_bottom` (visible wounds of the player and followers nearby). Only reports younger than 10 minutes are used. Keep the texts short: they are sent with every prompt while someone is wounded.
- **Server hooks.** Never give files in `lib/`, `api/` or `images/` a hook name (HerikaServer loads hook names recursively); `scripts/build_packages.py` enforces this.
- **Database.** `plugins.chim_injury_settings` (key/value, also holds the last report and the command queue) and `plugins.chim_injury_log` (diagnostics, trimmed to 2000 rows). Migrations are append-only once released.
- **Notifications.** `RE::DebugNotification` with the `CHIM-injury: ` prefix. `activated` is always shown; wound messages follow the page settings.

## Versioning

Bump the same version in:

- `manifest.json`
- `dwemer-package.json`
- `SkyrimPlugin/CMakeLists.txt`
- `SkyrimPlugin/vcpkg.json`
- `SkyrimPlugin/src/main.cpp` (`kPluginVersion`)
- `lib/chim_injury.php`

`python scripts/build_packages.py --check-only` verifies they match. Add a `CHANGELOG.md` entry.

## Validation

- `php -l` on changed PHP files.
- `php tests/server_policy_test.php`.
- CI must build the DLL.
- Report in-game testing separately from build results. A successful build does not prove that wounds appear in Skyrim.
- Never commit API keys, saves, logs or textures without the texture author's permission.
