<?php
/**
 * CHIM Injury Framework - plugin page (CHIM web UI -> Server Plugins -> CHIM Injury Framework -> Plugin Page).
 */

$enginePath = dirname(__DIR__, 2) . DIRECTORY_SEPARATOR;
require_once $enginePath . 'lib' . DIRECTORY_SEPARATOR . 'runtime_bootstrap.php';
chimRuntimeBootstrap($enginePath, [
    'load_general_settings' => true,
    'load_player_name' => false,
    'load_narrator' => false,
]);
$GLOBALS['db'] = $GLOBALS['db'] ?? new sql();

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_injury.php';

function ciH($value): string
{
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

function ciChecked(array $settings, string $key): string
{
    return ($settings[$key] ?? '0') === '1' ? 'checked' : '';
}

function ciDef(string $key, string $unit = ''): string
{
    return ' <span class="def">default: ' . ciH(chimInjuryDefaults()[$key] ?? '') . ciH($unit) . '</span>';
}

function ciDefOnOff(string $key): string
{
    return ' <span class="def">default: ' . ((chimInjuryDefaults()[$key] ?? '0') === '1' ? 'on' : 'off') . '</span>';
}

function ciHealsIn(array $w): string
{
    $days = (float)($w['heals_in_days'] ?? -1);
    if ($days < 0) {
        return !empty($w['treated']) ? 'after sleeping in a bed' : 'needs healing magic or a potion';
    }
    if ($days < 1) {
        return 'in about ' . max(1, (int)round($days * 24)) . ' h';
    }
    return 'in about ' . number_format($days, 1) . ' days';
}

const CI_BOOLS = ['enabled', 'debug', 'track_player', 'track_followers', 'show_overlays', 'show_on_player', 'animations', 'notifications', 'log_to_chim',
    'inject_self', 'inject_nearby'];
// key => [min, max, integer]
const CI_NUMBERS = [
    'notify_min_severity' => [1, 3, true],
    'log_min_severity' => [1, 3, true],
    'min_damage_pct' => [0, 100, false],
    'medium_damage_pct' => [0, 100, false],
    'severe_damage_pct' => [0, 100, false],
    'max_wounds' => [1, 12, true],
    'light_heal_days' => [0.05, 60, false],
    'medium_heal_days' => [0.05, 60, false],
    'heal_min_amount' => [0, 1000, false],
    'min_sleep_hours' => [0, 24, false],
    'overlay_strength' => [10, 100, true],
    'anim_min_severity' => [1, 3, true],
];
const CI_TEXTS = ['self_template', 'nearby_template'];

if (isset($_GET['download'])) {
    header('Content-Type: text/plain; charset=utf-8');
    header('Content-Disposition: attachment; filename="chim-injury-diagnostics-' . gmdate('Ymd-His') . '.txt"');
    header('Cache-Control: no-store');
    echo chimInjuryDiagnosticReport();
    exit;
}

$message = '';
$error = '';
$dbReady = chimInjuryDbReady();

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? 'save');
    if (!$dbReady) {
        $error = 'The plugin tables are missing. Reinstall the plugin package so its migrations run.';
    } elseif (in_array($action, chimInjuryCommandActions(), true)) {
        if (chimInjuryQueueCommand($action, (string)($_POST['actor'] ?? ''), (string)($_POST['key'] ?? ''))) {
            $message = 'Sent to the game. It is applied within about 15 seconds while the game is running.';
        } else {
            $error = 'Could not queue the command.';
        }
    } elseif ($action === 'restore_defaults') {
        $defaults = chimInjuryDefaults();
        $ok = true;
        foreach (array_merge(CI_BOOLS, array_keys(CI_NUMBERS), CI_TEXTS) as $key) {
            $ok = chimInjurySetValue($key, $defaults[$key]) && $ok;
        }
        $ok ? $message = 'All settings restored to their defaults.' : $error = 'Some settings could not be restored.';
    } elseif ($action === 'clear_log') {
        chimInjuryLogClear() ? $message = 'Diagnostics log cleared.' : $error = 'Could not clear the log.';
    } else {
        $values = [];
        foreach (CI_BOOLS as $key) {
            $values[$key] = isset($_POST[$key]) ? '1' : '0';
        }
        foreach (CI_NUMBERS as $key => [$min, $max, $int]) {
            $raw = $_POST[$key] ?? null;
            $num = is_numeric($raw) ? max($min, min($max, (float)$raw)) : (float)chimInjuryDefaults()[$key];
            $values[$key] = $int ? (string)(int)round($num) : (string)$num;
        }
        $values['medium_damage_pct'] = (string)max((float)$values['min_damage_pct'], (float)$values['medium_damage_pct']);
        $values['severe_damage_pct'] = (string)max((float)$values['medium_damage_pct'], (float)$values['severe_damage_pct']);
        foreach (CI_TEXTS as $key) {
            $values[$key] = trim(mb_substr((string)($_POST[$key] ?? ''), 0, 600));
        }
        $ok = true;
        foreach ($values as $key => $value) {
            $ok = chimInjurySetValue($key, $value) && $ok;
        }
        if ($ok) {
            $message = 'Settings saved. The game picks them up within 15 seconds.';
            chimInjuryLog('info', 'Settings changed on the plugin page (debug=' . $values['debug'] . ')');
        } else {
            $error = 'Some settings could not be saved.';
        }
    }
}

$settings = chimInjuryGetSettings();
$status = chimInjuryGameStatus($settings);
$state = chimInjuryGameState($settings);
$statusTs = (int)$settings['game_status_ts'];
$statusAge = $statusTs > 0 ? time() - $statusTs : null;
$gameOnline = $statusAge !== null && $statusAge < 60;
$pending = count(chimInjuryCommands($settings));
$logSource = in_array($_GET['source'] ?? '', ['game', 'server'], true) ? $_GET['source'] : null;
$logRows = chimInjuryLogFetch(150, $logSource);
$actors = $state['actors'] ?? [];
usort($actors, static fn($a, $b) => [empty($a['player']), -count($a['wounds'] ?? [])] <=> [empty($b['player']), -count($b['wounds'] ?? [])]);
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CHIM Injury Framework</title>
<style>
  :root { color-scheme: dark; --bg:#121011; --panel:#1c1718; --line:#3a2d2e; --text:#ece4dc; --muted:#aa9a8f; --accent:#a8322b; --ok:#6f9a5a; --warn:#d8a446; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:15px/1.5 system-ui, sans-serif; }
  main { max-width: 960px; margin: 0 auto; padding: 0 16px 48px; }
  .hero { position:relative; margin: 0 -16px 18px; max-height: 300px; overflow:hidden; border-bottom:1px solid var(--line); }
  .hero img { width:100%; display:block; object-fit:cover; max-height:300px; }
  .hero .title { position:absolute; left:0; right:0; bottom:0; padding: 40px 24px 14px; background:linear-gradient(transparent, rgba(10,8,8,.92)); }
  h1 { margin:0; font-size: 28px; letter-spacing:.5px; }
  h1 span { color: #e0574c; }
  .sub { color: var(--muted); margin: 2px 0 0; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:18px; margin-bottom:16px; }
  h2 { font-size:16px; margin:0 0 12px; }
  h3 { font-size:15px; margin:18px 0 6px; }
  label { display:block; margin: 12px 0 4px; font-weight:600; }
  label.check { font-weight:500; }
  .hint { color:var(--muted); font-size:13px; margin:2px 0 0; }
  .def { color:var(--muted); font-size:12px; font-weight:normal; white-space:nowrap; }
  input[type=number] { width: 110px; }
  input, textarea, select { background:#0d0b0b; color:var(--text); border:1px solid var(--line); border-radius:6px; padding:8px; font:inherit; }
  textarea { width:100%; min-height:70px; }
  .row { display:grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 8px 16px; }
  button, .button { background:var(--accent); color:#fff; border:0; border-radius:6px; padding:8px 14px; font:inherit; cursor:pointer; text-decoration:none; display:inline-block; }
  button.secondary, .button.secondary { background:transparent; border:1px solid var(--line); color:var(--text); }
  button.small { padding:3px 9px; font-size:13px; }
  .msg { padding:10px 12px; border-radius:6px; margin-bottom:16px; }
  .ok { background:#1f2a1a; border:1px solid var(--ok); }
  .err { background:#2a1716; border:1px solid var(--accent); }
  table { border-collapse: collapse; width:100%; }
  td, th { padding:6px 4px; border-bottom:1px solid var(--line); vertical-align: top; text-align:left; }
  th { color:var(--muted); font-weight:600; font-size:13px; }
  table.kv td:first-child { color:var(--muted); width:42%; }
  code { background:#0d0b0b; padding:1px 5px; border-radius:4px; }
  .inline { display:flex; gap:8px; flex-wrap:wrap; margin-top:12px; align-items:center; }
  .good { color:#8fc27a; } .bad { color:#e0705f; } .muted { color:var(--muted); } .warnc { color: var(--warn); }
  .sev1 { color:#d9c27a; } .sev2 { color:#e39a52; } .sev3 { color:#ef5f55; font-weight:600; }
  .log { background:#0d0b0b; border:1px solid var(--line); border-radius:6px; padding:8px; max-height:420px; overflow:auto; font:12px/1.45 ui-monospace, Consolas, monospace; white-space:pre-wrap; word-break:break-word; }
  .log .warn { color:#e3b65c; } .log .error { color:#e0705f; } .log .debug { color:var(--muted); }
  .tag { display:inline-block; min-width:52px; color:var(--muted); }
  .sticky { position: sticky; bottom: 0; background: var(--bg); padding: 10px 0; }
  form.inline-form { display:inline; }
  .scroll { overflow-x:auto; }
</style>
</head>
<body>
<main>
  <div class="hero">
    <img src="images/banner.jpg" alt="">
    <div class="title">
      <h1>CHIM <span>Injury</span> Framework</h1>
      <p class="sub">Wounds for the player and followers: they show on the body, heal with time, magic and rest, and NPCs know about them. Server plugin v<?= ciH(CHIM_INJURY_VERSION) ?>.</p>
    </div>
  </div>

  <?php if ($message): ?><div class="msg ok"><?= ciH($message) ?></div><?php endif; ?>
  <?php if ($error): ?><div class="msg err"><?= ciH($error) ?></div><?php endif; ?>
  <?php if (!$dbReady): ?><div class="msg err">Plugin database tables are missing. Defaults are used and nothing can be saved until the plugin package is reinstalled.</div><?php endif; ?>

  <section>
    <h2>Status</h2>
    <table class="kv">
      <tr><td>Game plugin (CHIMInjury.dll)</td><td>
        <?php if ($statusTs === 0): ?><span class="bad">no report yet</span> <span class="muted">start the game and load a save</span>
        <?php elseif ($gameOnline): ?><span class="good">connected</span> <span class="muted">v<?= ciH($status['version'] ?? '?') ?>, Skyrim <?= ciH($status['game_version'] ?? '?') ?>, last report <?= (int)$statusAge ?> s ago</span>
        <?php else: ?><span class="muted">offline, last report <?= ciH(date('Y-m-d H:i', $statusTs)) ?></span><?php endif; ?>
      </td></tr>
      <tr><td>RaceMenu (wounds on the body)</td><td>
        <?php if ($statusTs === 0): ?><span class="muted">unknown</span>
        <?php elseif (!empty($status['racemenu_loaded'])): ?><span class="good">found</span> <span class="muted"><?= ciH($status['racemenu_version'] ?? '') ?>, <?= (int)($status['overlay_slots'] ?? 0) ?> body overlay slots</span>
          <?php if ((int)($status['overlay_slots_short'] ?? 0) > 0): ?> <span class="warnc">(sometimes not enough free slots, see the hint below)</span><?php endif; ?>
        <?php else: ?><span class="bad">not found</span> <span class="muted">wounds work, but are not drawn on the body</span><?php endif; ?>
      </td></tr>
      <tr><td>Wound animations</td><td>
        <?php if ($statusTs === 0): ?><span class="muted">unknown</span>
        <?php elseif (!empty($status['anim_plugin'])): ?><span class="good">animation pack found</span> <span class="muted">(CHIMInjury_Animations.esp, <?= (int)($status['anim_actors'] ?? 0) ?> wounded people use it now)</span>
        <?php else: ?><span class="muted">no animation pack (optional)</span><?php endif; ?>
      </td></tr>
      <tr><td>This session: hits checked / wounds / healed (magic, rest, time)</td><td><?= (int)($status['hits_seen'] ?? 0) ?> / <?= (int)($status['wounds_made'] ?? 0) ?> / <?= (int)($status['healed_magic'] ?? 0) ?>, <?= (int)($status['healed_rest'] ?? 0) ?>, <?= (int)($status['healed_time'] ?? 0) ?></td></tr>
      <tr><td>Events saved to CHIM memory / prompts with injuries</td><td><?= ciH($settings['stat_events']) ?> / <?= ciH($settings['stat_injected']) ?></td></tr>
      <?php if ($pending > 0): ?><tr><td>Commands waiting for the game</td><td><?= $pending ?></td></tr><?php endif; ?>
      <?php if (!empty($status['last_error'])): ?><tr><td>Last error in game</td><td class="bad"><?= ciH($status['last_error']) ?></td></tr><?php endif; ?>
    </table>
  </section>

  <section>
    <h2>Wounds</h2>
    <?php if (!$actors): ?>
      <p class="muted">No report from the game yet. Load a save; the player and followers appear here.</p>
    <?php endif; ?>
    <?php foreach ($actors as $a): ?>
      <h3><?= ciH($a['name']) ?> <span class="muted" style="font-weight:normal"><?= !empty($a['player']) ? 'player' : (!empty($a['follower']) ? 'follower' : 'not following now') ?><?= !empty($a['wounds']) && (int)$a['overlays_wanted'] > 0 ? ', ' . (int)$a['overlays_shown'] . '/' . (int)$a['overlays_wanted'] . ' shown on the body' : '' ?></span></h3>
      <?php if (empty($a['wounds'])): ?>
        <p class="muted">No wounds.</p>
      <?php else: ?>
        <div class="scroll"><table>
          <tr><th>Wound</th><th>Where</th><th>Severity</th><th>Status</th><th>Heals</th><th>From</th><th></th></tr>
          <?php foreach ($a['wounds'] as $w): ?>
          <tr>
            <td><?= ciH($w['label']) ?></td>
            <td><?= ciH(chimInjuryRegionLabel($w['region'])) ?></td>
            <td class="sev<?= (int)$w['severity'] ?>"><?= ciH(chimInjurySeverityLabel((int)$w['severity'])) ?></td>
            <td><?= ciH(chimInjuryStatusText($w)) ?></td>
            <td><?= ciH(ciHealsIn($w)) ?></td>
            <td class="muted"><?= ciH(trim($w['by'] . ($w['with'] !== '' ? ' (' . $w['with'] . ')' : ''))) ?></td>
            <td><form method="post" class="inline-form"><input type="hidden" name="action" value="remove"><input type="hidden" name="actor" value="<?= ciH($a['id']) ?>"><input type="hidden" name="key" value="<?= ciH($w['key']) ?>"><button class="small secondary" type="submit">Remove</button></form></td>
          </tr>
          <?php endforeach; ?>
        </table></div>
        <div class="inline">
          <form method="post" class="inline-form"><input type="hidden" name="action" value="treat"><input type="hidden" name="actor" value="<?= ciH($a['id']) ?>"><button class="small" type="submit">Heal like a potion</button></form>
          <form method="post" class="inline-form"><input type="hidden" name="action" value="heal"><input type="hidden" name="actor" value="<?= ciH($a['id']) ?>"><button class="small secondary" type="submit">Heal everything</button></form>
        </div>
      <?php endif; ?>
    <?php endforeach; ?>
    <?php if ($actors): ?>
      <div class="inline">
        <form method="post" class="inline-form"><button type="submit" name="action" value="heal_all" class="secondary">Heal everyone</button></form>
        <form method="post" class="inline-form"><button type="submit" name="action" value="reset" class="secondary" onclick="return confirm('Remove all wounds and wound overlays? Use this before uninstalling.');">Remove all wounds and overlays</button></form>
      </div>
      <p class="hint">Changes reach the game within about 15 seconds. Before uninstalling, press <em>Remove all wounds and overlays</em> with your followers nearby, then save.</p>
    <?php endif; ?>
  </section>

  <form method="post">
    <section>
      <h2>General</h2>
      <label class="check"><input type="checkbox" name="enabled" value="1" <?= ciChecked($settings, 'enabled') ?>> CHIM Injury Framework enabled<?= ciDefOnOff('enabled') ?></label>
      <label class="check"><input type="checkbox" name="track_player" value="1" <?= ciChecked($settings, 'track_player') ?>> Track the player's wounds<?= ciDefOnOff('track_player') ?></label>
      <label class="check"><input type="checkbox" name="track_followers" value="1" <?= ciChecked($settings, 'track_followers') ?>> Track followers' wounds (found automatically, no NFF needed)<?= ciDefOnOff('track_followers') ?></label>
      <label class="check"><input type="checkbox" name="notifications" value="1" <?= ciChecked($settings, 'notifications') ?>> On-screen messages about wounds and healing<?= ciDefOnOff('notifications') ?></label>
      <div class="row">
        <div><label>Message from severity <span class="def">default: medium</span></label><select name="notify_min_severity"><?php foreach ([1 => 'light', 2 => 'medium', 3 => 'severe'] as $v => $l): ?><option value="<?= $v ?>" <?= (int)$settings['notify_min_severity'] === $v ? 'selected' : '' ?>><?= $l ?></option><?php endforeach; ?></select></div>
      </div>
      <label class="check"><input type="checkbox" name="debug" value="1" <?= ciChecked($settings, 'debug') ?>> Debug mode<?= ciDefOnOff('debug') ?></label>
      <p class="hint">Writes every hit (health before and after) and the overlay slots to the log. <code>CHIM-injury: activated</code> is always shown after loading a save.</p>
    </section>

    <section>
      <h2>On the body (RaceMenu, animations)</h2>
      <label class="check"><input type="checkbox" name="show_overlays" value="1" <?= ciChecked($settings, 'show_overlays') ?>> Draw wounds on the body<?= ciDefOnOff('show_overlays') ?></label>
      <label class="check"><input type="checkbox" name="show_on_player" value="1" <?= ciChecked($settings, 'show_on_player') ?>> Also on the player<?= ciDefOnOff('show_on_player') ?></label>
      <div class="row">
        <div><label>Strength (%)<?= ciDef('overlay_strength', ' %') ?></label><input type="number" name="overlay_strength" min="10" max="100" value="<?= ciH($settings['overlay_strength']) ?>"></div>
      </div>
      <label class="check"><input type="checkbox" name="animations" value="1" <?= ciChecked($settings, 'animations') ?>> Wounded movement and idle animations (needs an animation pack made for CHIM Injury Framework)<?= ciDefOnOff('animations') ?></label>
      <div class="row">
        <div><label>Animations from severity <span class="def">default: medium</span></label><select name="anim_min_severity"><?php foreach ([1 => 'light', 2 => 'medium', 3 => 'severe'] as $v => $l): ?><option value="<?= $v ?>" <?= (int)$settings['anim_min_severity'] === $v ? 'selected' : '' ?>><?= $l ?></option><?php endforeach; ?></select></div>
      </div>
      <p class="hint">Uses free RaceMenu body overlay slots (<code>Body [Ovl#]</code>); body paints you placed yourself are never touched. If wounds are missing on the body, raise <code>iNumOverlays</code> under <code>[Overlays/Body]</code> in <code>skee64.ini</code>. The head has no wound overlays yet: head wounds are only described.</p>
    </section>

    <section>
      <h2>Wounds</h2>
      <div class="row">
        <div><label>Smallest hit that wounds (% of max health)<?= ciDef('min_damage_pct', ' %') ?></label><input type="number" step="0.5" name="min_damage_pct" min="0" max="100" value="<?= ciH($settings['min_damage_pct']) ?>"><p class="hint">Power and sneak attacks always wound.</p></div>
        <div><label>Medium from (%)<?= ciDef('medium_damage_pct', ' %') ?></label><input type="number" step="0.5" name="medium_damage_pct" min="0" max="100" value="<?= ciH($settings['medium_damage_pct']) ?>"></div>
        <div><label>Severe from (%)<?= ciDef('severe_damage_pct', ' %') ?></label><input type="number" step="0.5" name="severe_damage_pct" min="0" max="100" value="<?= ciH($settings['severe_damage_pct']) ?>"><p class="hint">Power and sneak attacks and hits below 20% health are one step worse.</p></div>
        <div><label>Max wounds per person<?= ciDef('max_wounds') ?></label><input type="number" name="max_wounds" min="1" max="12" value="<?= ciH($settings['max_wounds']) ?>"></div>
      </div>
    </section>

    <section>
      <h2>Healing</h2>
      <div class="row">
        <div><label>Light wounds heal after (game days)<?= ciDef('light_heal_days') ?></label><input type="number" step="0.25" name="light_heal_days" min="0.05" max="60" value="<?= ciH($settings['light_heal_days']) ?>"></div>
        <div><label>Medium wounds heal after (game days)<?= ciDef('medium_heal_days') ?></label><input type="number" step="0.25" name="medium_heal_days" min="0.05" max="60" value="<?= ciH($settings['medium_heal_days']) ?>"></div>
        <div><label>Smallest potion or spell that heals<?= ciDef('heal_min_amount', ' HP') ?></label><input type="number" name="heal_min_amount" min="0" max="1000" value="<?= ciH($settings['heal_min_amount']) ?>"><p class="hint">Concentration healing spells (Healing, Healing Hands) always count. Food and regeneration enchantments never do.</p></div>
        <div><label>Sleep needed (hours in a bed)<?= ciDef('min_sleep_hours', ' h') ?></label><input type="number" step="0.5" name="min_sleep_hours" min="0" max="24" value="<?= ciH($settings['min_sleep_hours']) ?>"></div>
      </div>
      <p class="hint">Healing magic and potions close light and medium wounds. Severe wounds only get treated; a treated severe wound heals fully after sleeping in a bed. Followers rest when you sleep.</p>
    </section>

    <section>
      <h2>CHIM (what NPCs know)</h2>
      <label class="check"><input type="checkbox" name="log_to_chim" value="1" <?= ciChecked($settings, 'log_to_chim') ?>> Save wounds and healing to CHIM's memory (nearby NPCs remember)<?= ciDefOnOff('log_to_chim') ?></label>
      <div class="row">
        <div><label>Save from severity <span class="def">default: medium</span></label><select name="log_min_severity"><?php foreach ([1 => 'light', 2 => 'medium', 3 => 'severe'] as $v => $l): ?><option value="<?= $v ?>" <?= (int)$settings['log_min_severity'] === $v ? 'selected' : '' ?>><?= $l ?></option><?php endforeach; ?></select></div>
      </div>
      <label class="check"><input type="checkbox" name="inject_self" value="1" <?= ciChecked($settings, 'inject_self') ?>> A wounded follower knows their own wounds when they speak<?= ciDefOnOff('inject_self') ?></label>
      <label for="self_template">Text for the speaker's own wounds</label>
      <textarea id="self_template" name="self_template"><?= ciH($settings['self_template']) ?></textarea>
      <label class="check"><input type="checkbox" name="inject_nearby" value="1" <?= ciChecked($settings, 'inject_nearby') ?>> NPCs see the wounds of the player and followers nearby<?= ciDefOnOff('inject_nearby') ?></label>
      <label for="nearby_template">Text for visible wounds of others</label>
      <textarea id="nearby_template" name="nearby_template"><?= ciH($settings['nearby_template']) ?></textarea>
      <p class="hint"><code>{INJURIES}</code> = the wound list, for example <em>deep gash on the left arm (untreated, bleeding)</em>. Keep these short: they are added to every prompt while someone is wounded.</p>
    </section>

    <div class="inline sticky">
      <button type="submit" name="action" value="save">Save settings</button>
      <button type="submit" name="action" value="restore_defaults" class="secondary" onclick="return confirm('Restore all settings to their defaults?');">Restore all defaults</button>
    </div>
  </form>

  <section>
    <h2>Diagnostics</h2>
    <p class="hint">If something does not work, create a diagnostic file and send it with your report. It contains versions, settings, wounds and the log below (no API keys or dialogue).</p>
    <div class="inline">
      <a class="button" href="?download=1">Create diagnostic file</a>
      <form method="post" class="inline-form"><button type="submit" name="action" value="clear_log" class="secondary">Clear log</button></form>
      <span class="muted">Show:</span>
      <a class="button secondary" href="?">all</a>
      <a class="button secondary" href="?source=game">game</a>
      <a class="button secondary" href="?source=server">server</a>
    </div>
    <p class="hint">The complete game log is on the gaming PC: <code>Documents\My Games\Skyrim Special Edition\SKSE\chim-injury.log</code>.</p>
    <div class="log"><?php if (!$logRows): ?><span class="muted">No log entries yet.</span><?php endif; ?><?php foreach ($logRows as $row): ?><div class="<?= ciH($row['level']) ?>"><span class="tag"><?= ciH($row['source']) ?></span> <?= ciH($row['source'] === 'game' ? $row['message'] : ($row['created_at'] . ' ' . $row['message'])) ?></div><?php endforeach; ?></div>
  </section>
  <p class="hint">Wound textures: <em>Wounds Overlays For RaceMenu</em> by JaySerpa. Made by Marcius3d.</p>
</main>
</body>
</html>
