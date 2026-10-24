<?php
/**
 * Standalone check of the server logic (no HerikaServer needed): php tests/server_policy_test.php
 */

final class ChimInjuryFakeDb
{
    public array $rows = [];
    public array $log = [];

    public function fetchOne($q, array $p = [])
    {
        if (str_contains($q, 'to_regclass')) {
            return ['t' => $p[0] ?? 'x'];
        }
        if (str_contains($q, 'chim_injury_settings')) {
            $this->rows[$p[0]] = (string)$p[1];
            return ['key' => $p[0]];
        }
        if (str_contains($q, 'INSERT INTO plugins.chim_injury_log')) {
            $this->log[] = ['id' => count($this->log) + 1, 'created_at' => 'now', 'source' => $p[0], 'level' => $p[1], 'message' => $p[2]];
            return ['id' => count($this->log)];
        }
        if (str_contains($q, 'DELETE FROM plugins.chim_injury_log')) {
            if (!$p) {
                $this->log = [];
            }
            return [];
        }
        return [];
    }

    public function fetchAll($q)
    {
        if (str_contains($q, 'chim_injury_log')) {
            return array_reverse($this->log);
        }
        $out = [];
        foreach ($this->rows as $k => $v) {
            $out[] = ['key' => $k, 'value' => $v];
        }
        return $out;
    }

    public function escape($s) { return addslashes((string)$s); }
}

$GLOBALS['db'] = new ChimInjuryFakeDb();
$GLOBALS['HERIKA_NAME'] = 'Lydia';
require __DIR__ . '/../lib/chim_injury.php';

$failures = 0;
function check(string $name, bool $ok): void
{
    global $failures;
    echo ($ok ? 'ok   ' : 'FAIL ') . $name . PHP_EOL;
    if (!$ok) {
        $failures++;
    }
}

// --- config text --------------------------------------------------------------------------
$config = chimInjuryConfigText(chimInjuryGetSettings());
check('config starts with plugin id', str_starts_with($config, "plugin=chim_injury_framework\n"));
check('config has every game key', count(array_filter(array_keys(chimInjuryGameKeys()), fn($k) => str_contains($config, "\n{$k}="))) === count(chimInjuryGameKeys()));
check('bool written as 0/1', str_contains($config, "\nshow_overlays=1\n"));
check('default notifications from medium', str_contains($config, "\nnotify_min_severity=2\n"));

// --- commands -----------------------------------------------------------------------------
check('unknown command rejected', !chimInjuryQueueCommand('explode', '14'));
check('heal command queued', chimInjuryQueueCommand('heal', '0001A2b3'));
check('remove command queued', chimInjuryQueueCommand('remove', '14', 'left_arm:slash'));
check('bad key cleaned', chimInjuryQueueCommand('remove', '14', "x\ncmd=99|reset"));
$config = chimInjuryConfigText(chimInjuryGetSettings());
check('command lines in config', str_contains($config, "cmd=1|heal|0001A2B3|\n") && str_contains($config, "cmd=2|remove|14|left_arm:slash\n"));
check('injected key not passed on', str_contains($config, "cmd=3|remove|14|\n") && !str_contains($config, 'cmd=99'));

// --- status with wound state ----------------------------------------------------------------
$state = [
    'game_days' => 12.5,
    'player_name' => 'Martynas',
    'actors' => [
        ['id' => '00000014', 'name' => 'Martynas', 'player' => true, 'female' => false, 'follower' => false, 'loaded' => true,
            'overlays_shown' => 1, 'overlays_wanted' => 1, 'wounds' => [
                ['key' => 'chest:blunt', 'region' => 'chest', 'type' => 'blunt', 'severity' => 2, 'treated' => false, 'arrow' => false,
                    'age_days' => 0.5, 'heals_in_days' => 2.5, 'label' => 'heavy bruising', 'by' => 'Bandit', 'with' => 'Iron Mace'],
            ]],
        ['id' => '000A2C94', 'name' => 'Lydia', 'player' => false, 'female' => true, 'follower' => true, 'loaded' => true,
            'overlays_shown' => 2, 'overlays_wanted' => 2, 'wounds' => [
                ['key' => 'left_arm:slash', 'region' => 'left_arm', 'type' => 'slash', 'severity' => 3, 'treated' => false, 'arrow' => false,
                    'age_days' => 0.01, 'heals_in_days' => -1, 'label' => 'deep gash <b>', 'by' => 'Bandit Chief', 'with' => 'Steel Sword'],
                ['key' => 'right_leg:pierce', 'region' => 'right_leg', 'type' => 'pierce', 'severity' => 1, 'treated' => false, 'arrow' => true,
                    'age_days' => 0.9, 'heals_in_days' => 0.1, 'label' => 'arrow graze', 'by' => 'Bandit', 'with' => 'Hunting Bow'],
            ]],
        ['id' => '000B9982', 'name' => 'Sofia', 'player' => false, 'female' => true, 'follower' => true, 'loaded' => true,
            'overlays_shown' => 0, 'overlays_wanted' => 0, 'wounds' => []],
    ],
];
$body = json_encode(['plugin' => 'chim_injury_framework', 'version' => '0.1.0', 'cmd_done' => 1, 'state' => $state,
    'log_lines' => ['[2026-10-24 12:00:00.000] [info] Lydia wounded: deep gash', '[2026-10-24 12:00:01.000] [warning] slots short']]);
check('status accepted', chimInjuryHandleStatus($body) === null);
check('wrong plugin rejected', chimInjuryHandleStatus('{"plugin":"chim_gore"}') === 'wrong_plugin');
check('invalid json rejected', chimInjuryHandleStatus('{oops') === 'invalid_json');
$settings = chimInjuryGetSettings();
check('executed command pruned', count(chimInjuryCommands($settings)) === 2);
check('game log stored with level', count(array_filter($GLOBALS['db']->log, fn($r) => $r['source'] === 'game' && $r['level'] === 'warn')) === 1);
$stored = chimInjuryGameState($settings, true);
check('state stored', count($stored['actors']) === 3 && $stored['actors'][1]['wounds'][0]['severity'] === 3);
check('stale state ignored in prompts', chimInjuryGameState($settings, true, time() + 3600)['actors'] === []);
// A name from a non-UTF-8 string table (here cp1252 "é") must not sink the whole report.
$latin1 = str_replace('Bandit Chief', "Bandit Chief Andr\xE9", $body);
check('non-UTF-8 name accepted', chimInjuryHandleStatus($latin1) === null);
check('non-UTF-8 name kept readable', str_contains((string)chimInjuryGetSettings()['game_state'], "Bandit Chief Andr\u{FFFD}"));

// --- prompt text ----------------------------------------------------------------------------
check('wound text', chimInjuryWoundText($stored['actors'][1]['wounds'][0]) === 'deep gash <b> on the left arm (untreated, bleeding)');
check('almost healed', chimInjuryStatusText($stored['actors'][1]['wounds'][1]) === 'almost healed');
$parts = chimInjuryBuildPromptParts($settings, $stored, 'Lydia', ['Martynas', 'Lydia', 'Sofia']);
check('self text for the wounded speaker', str_starts_with($parts['self'], 'Your current injuries: deep gash <b> on the left arm (untreated, bleeding); arrow graze on the right leg'));
check('nearby shows the player, not the speaker', str_contains($parts['nearby'], 'Martynas: heavy bruising on the chest') && !str_contains($parts['nearby'], 'Lydia:'));
$parts = chimInjuryBuildPromptParts($settings, $stored, 'Sofia', ['Martynas', 'Sofia']);
check('unwounded speaker gets no self text', $parts['self'] === '');
check('follower out of range not listed', !str_contains($parts['nearby'], 'Lydia'));
$parts = chimInjuryBuildPromptParts($settings, $stored, 'Sofia', null);
check('unknown range: loaded followers listed', str_contains($parts['nearby'], 'Lydia: deep gash'));
$off = $settings;
$off['inject_self'] = '0';
$off['inject_nearby'] = '0';
check('injection can be turned off', chimInjuryBuildPromptParts($off, $stored, 'Lydia', null) === ['self' => '', 'nearby' => '']);
check('player never gets self text', chimInjuryBuildPromptParts($settings, $stored, 'Martynas', null)['self'] === '');

// --- events ---------------------------------------------------------------------------------
$parsed = chimInjuryParseEvent('Martynas: [injury actor="Lydia" event=new] Lydia was wounded: deep gash on the left arm, from Bandit Chief\'s Steel Sword.');
check('event marker parsed', $parsed['values'] === ['actor' => 'Lydia', 'event' => 'new']);
check('event sentence without marker', $parsed['sentence'] === 'Lydia was wounded: deep gash on the left arm, from Bandit Chief\'s Steel Sword.');
check('other text is not an event', chimInjuryParseEvent('Lydia: hello') === null);

// --- packaging rule -------------------------------------------------------------------------
$hookNames = ['globals.php', 'preprocessing.php', 'prerequest.php', 'context_pre.php', 'context.php', 'prompts.php', 'postrequest.php', 'functions.php'];
$bad = [];
foreach (['lib', 'api'] as $dir) {
    foreach (glob(__DIR__ . "/../{$dir}/*.php") as $file) {
        if (in_array(basename($file), $hookNames, true)) {
            $bad[] = $file;
        }
    }
}
check('no hook names in lib/ or api/', $bad === []);

echo $failures === 0 ? "\nAll checks passed.\n" : "\n{$failures} check(s) failed.\n";
exit($failures === 0 ? 0 : 1);
