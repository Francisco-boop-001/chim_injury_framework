<?php
/**
 * CHIM Injury Framework - shared server helpers.
 *
 * Note: HerikaServer loads hook files (preprocessing.php, context.php, ...) recursively from
 * every folder under ext/. Do not give files in this folder or in api/ a hook name.
 */

if (defined('CHIM_INJURY_VERSION')) {
    return;
}

define('CHIM_INJURY_VERSION', '0.3.0');
define('CHIM_INJURY_PLUGIN', 'chim_injury_framework');
define('CHIM_INJURY_TABLE', 'plugins.chim_injury_settings');
define('CHIM_INJURY_LOG_TABLE', 'plugins.chim_injury_log');
define('CHIM_INJURY_LOG_KEEP', 2000);
define('CHIM_INJURY_STATE_MAX_AGE', 600);  // seconds: older wound reports are not used in prompts

/** Marker CHIMInjury.dll puts in front of each info_injury event: [injury actor="Name" event=new] */
define('CHIM_INJURY_EVENT_MARKER_REGEX', '/\[injury\b([^\]]*)\]\s*/i');

/** Settings that CHIMInjury.dll reads from api/config.php (key => type). Keep in sync with ApplyServerConfig(). */
function chimInjuryGameKeys(): array
{
    return [
        'enabled' => 'bool',
        'debug' => 'bool',
        'track_player' => 'bool',
        'track_followers' => 'bool',
        'show_overlays' => 'bool',
        'show_on_player' => 'bool',
        'notifications' => 'bool',
        'notify_min_severity' => 'int',
        'log_to_chim' => 'bool',
        'log_min_severity' => 'int',
        'min_damage_pct' => 'float',
        'medium_damage_pct' => 'float',
        'severe_damage_pct' => 'float',
        'max_wounds' => 'int',
        'light_heal_days' => 'float',
        'medium_heal_days' => 'float',
        'heal_min_amount' => 'float',
        'min_sleep_hours' => 'float',
        'overlay_strength' => 'int',
        'animations' => 'bool',
        'anim_min_severity' => 'int',
        'anim_episodes' => 'bool',
        'anim_on_min_seconds' => 'float',
        'anim_on_max_seconds' => 'float',
        'anim_off_min_seconds' => 'float',
        'anim_off_max_seconds' => 'float',
    ];
}

function chimInjuryDefaults(): array
{
    return [
        // Game behaviour (sent to the DLL)
        'enabled' => '1',
        'debug' => '0',
        'track_player' => '1',
        'track_followers' => '1',
        'show_overlays' => '1',
        'show_on_player' => '1',
        'notifications' => '1',
        'notify_min_severity' => '2',
        'log_to_chim' => '1',
        'log_min_severity' => '2',
        'min_damage_pct' => '5',
        'medium_damage_pct' => '12',
        'severe_damage_pct' => '25',
        'max_wounds' => '6',
        'light_heal_days' => '1',
        'medium_heal_days' => '3',
        'heal_min_amount' => '15',
        'min_sleep_hours' => '6',
        'overlay_strength' => '100',
        'animations' => '1',
        'anim_min_severity' => '2',
        'anim_episodes' => '1',
        'anim_on_min_seconds' => '5',
        'anim_on_max_seconds' => '10',
        'anim_off_min_seconds' => '20',
        'anim_off_max_seconds' => '25',
        // Prompt (server only)
        'inject_self' => '1',
        'inject_nearby' => '1',
        'self_template' => 'Your current injuries: {INJURIES}. They hurt; let them show in how you speak and act when it fits, without listing them.',
        'nearby_template' => 'Visible injuries of people here: {INJURIES}.',
        // State
        'game_status' => '',
        'game_status_ts' => '0',
        'game_state' => '',
        'game_state_ts' => '0',
        'commands' => '[]',
        'command_seq' => '0',
        'stat_injected' => '0',
        'stat_events' => '0',
    ];
}

function chimInjuryDb()
{
    return $GLOBALS['db'] ?? null;
}

function chimInjuryTableExists(string $table): bool
{
    static $cache = [];
    if (array_key_exists($table, $cache)) {
        return $cache[$table];
    }
    $db = chimInjuryDb();
    if (!$db) {
        return $cache[$table] = false;
    }
    try {
        $row = $db->fetchOne('SELECT to_regclass($1) AS t', [$table]);
        return $cache[$table] = !empty($row['t']);
    } catch (Throwable $e) {
        return $cache[$table] = false;
    }
}

function chimInjuryDbReady(): bool
{
    return chimInjuryTableExists(CHIM_INJURY_TABLE);
}

function chimInjuryGetSettings(): array
{
    $settings = chimInjuryDefaults();
    if (!chimInjuryDbReady()) {
        return $settings;
    }
    try {
        $rows = chimInjuryDb()->fetchAll('SELECT key, value FROM ' . CHIM_INJURY_TABLE);
        foreach ((array)$rows as $row) {
            if (isset($row['key']) && array_key_exists($row['key'], $settings)) {
                $settings[$row['key']] = (string)$row['value'];
            }
        }
    } catch (Throwable $e) {
        // Defaults keep the plugin usable while the table is unavailable.
    }
    foreach (['self_template', 'nearby_template'] as $key) {
        if (trim($settings[$key]) === '') {
            $settings[$key] = chimInjuryDefaults()[$key];
        }
    }
    return $settings;
}

function chimInjurySetValue(string $key, string $value): bool
{
    if (!chimInjuryDbReady() || !array_key_exists($key, chimInjuryDefaults())) {
        return false;
    }
    try {
        $row = chimInjuryDb()->fetchOne(
            'INSERT INTO ' . CHIM_INJURY_TABLE . ' (key, value, updated_at) VALUES ($1, $2, CURRENT_TIMESTAMP) '
            . 'ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = CURRENT_TIMESTAMP RETURNING key',
            [$key, $value]
        );
        return !empty($row);
    } catch (Throwable $e) {
        return false;
    }
}

function chimInjuryIncrement(string $key): void
{
    $settings = chimInjuryGetSettings();
    chimInjurySetValue($key, (string)((int)($settings[$key] ?? 0) + 1));
}

// ---------------------------------------------------------------------------------------
// Diagnostics log (plugins.chim_injury_log). source: server | game
// ---------------------------------------------------------------------------------------

function chimInjuryLog(string $level, string $message, string $source = 'server'): void
{
    $line = '[chim_injury] ' . $message;
    if (class_exists('Logger') && method_exists('Logger', $level)) {
        Logger::$level($line);
    }
    chimInjuryLogStore($source, $level, $message);
}

function chimInjuryLogStore(string $source, string $level, string $message): void
{
    if (!chimInjuryTableExists(CHIM_INJURY_LOG_TABLE)) {
        return;
    }
    try {
        chimInjuryDb()->fetchOne(
            'INSERT INTO ' . CHIM_INJURY_LOG_TABLE . ' (source, level, message) VALUES ($1, $2, $3) RETURNING id',
            [substr($source, 0, 16), substr($level, 0, 16), mb_substr($message, 0, 1000)]
        );
        if (random_int(1, 50) === 1) {
            chimInjuryDb()->fetchOne(
                'DELETE FROM ' . CHIM_INJURY_LOG_TABLE . ' WHERE id <= (SELECT COALESCE(MAX(id), 0) - $1 FROM '
                . CHIM_INJURY_LOG_TABLE . ') RETURNING 1',
                [CHIM_INJURY_LOG_KEEP]
            );
        }
    } catch (Throwable $e) {
        // Logging must never break a game request.
    }
}

function chimInjuryLogFetch(int $limit = 200, ?string $source = null): array
{
    if (!chimInjuryTableExists(CHIM_INJURY_LOG_TABLE)) {
        return [];
    }
    $limit = max(1, min(CHIM_INJURY_LOG_KEEP, $limit));
    try {
        $where = $source !== null ? " WHERE source = '" . chimInjuryDb()->escape($source) . "'" : '';
        $rows = chimInjuryDb()->fetchAll(
            'SELECT id, created_at, source, level, message FROM ' . CHIM_INJURY_LOG_TABLE . $where . ' ORDER BY id DESC LIMIT ' . $limit
        );
        return array_reverse((array)$rows);
    } catch (Throwable $e) {
        return [];
    }
}

function chimInjuryLogClear(): bool
{
    if (!chimInjuryTableExists(CHIM_INJURY_LOG_TABLE)) {
        return false;
    }
    try {
        chimInjuryDb()->fetchOne('DELETE FROM ' . CHIM_INJURY_LOG_TABLE . ' RETURNING 1');
        return true;
    } catch (Throwable $e) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------
// Page commands for the game (heal, remove wound, ...), executed once by the DLL.
// ---------------------------------------------------------------------------------------

function chimInjuryCommandActions(): array
{
    return ['heal', 'treat', 'remove', 'heal_all', 'reset'];
}

function chimInjuryCommands(array $settings): array
{
    $list = json_decode((string)$settings['commands'], true);
    return is_array($list) ? array_values(array_filter($list, 'is_array')) : [];
}

function chimInjuryQueueCommand(string $action, string $actor = '', string $key = ''): bool
{
    if (!in_array($action, chimInjuryCommandActions(), true)) {
        return false;
    }
    $actor = preg_match('/^[0-9A-Fa-f]{1,8}$/', $actor) ? strtoupper($actor) : '';
    $key = preg_match('/^[a-z_]{1,20}:[a-z]{1,12}$/', $key) ? $key : '';
    $settings = chimInjuryGetSettings();
    $id = (int)$settings['command_seq'] + 1;
    $list = chimInjuryCommands($settings);
    $list[] = ['id' => $id, 'action' => $action, 'actor' => $actor, 'key' => $key, 'ts' => time()];
    $list = array_slice($list, -50);
    $ok = chimInjurySetValue('command_seq', (string)$id);
    $ok = chimInjurySetValue('commands', json_encode($list)) && $ok;
    if ($ok) {
        chimInjuryLog('info', "Command queued for the game: {$action} {$actor} {$key}");
    }
    return $ok;
}

/** Drops commands the game has executed (and stale ones). */
function chimInjuryPruneCommands(int $done, ?int $now = null): void
{
    $now = $now ?? time();
    $settings = chimInjuryGetSettings();
    $list = chimInjuryCommands($settings);
    $kept = array_values(array_filter($list, static fn($c) => (int)($c['id'] ?? 0) > $done && $now - (int)($c['ts'] ?? 0) < 86400));
    if (count($kept) !== count($list)) {
        chimInjurySetValue('commands', json_encode($kept));
    }
}

// ---------------------------------------------------------------------------------------
// Game <-> server
// ---------------------------------------------------------------------------------------

/** Plain key=value text for CHIMInjury.dll (no JSON parser needed in the game plugin). */
function chimInjuryConfigText(array $settings): string
{
    $lines = ['plugin=' . CHIM_INJURY_PLUGIN, 'version=' . CHIM_INJURY_VERSION];
    foreach (chimInjuryGameKeys() as $key => $type) {
        $value = (string)($settings[$key] ?? '');
        if ($type === 'bool') {
            $value = $value === '1' ? '1' : '0';
        } elseif ($type === 'int') {
            $value = (string)(int)$value;
        } else {
            $value = (string)(float)$value;
        }
        $lines[] = "{$key}={$value}";
    }
    foreach (chimInjuryCommands($settings) as $c) {
        $lines[] = sprintf('cmd=%d|%s|%s|%s', (int)($c['id'] ?? 0), (string)($c['action'] ?? ''), (string)($c['actor'] ?? ''), (string)($c['key'] ?? ''));
    }
    return implode("\n", $lines) . "\n";
}

/** Stores a status report (with the wound state) from CHIMInjury.dll. Returns an error string or null. */
function chimInjuryHandleStatus(string $body): ?string
{
    if (strlen($body) > 524288) {
        return 'too_large';
    }
    try {
        $data = json_decode($body, true, 32, JSON_THROW_ON_ERROR);
    } catch (Throwable $e) {
        return 'invalid_json';
    }
    if (!is_array($data) || ($data['plugin'] ?? '') !== CHIM_INJURY_PLUGIN) {
        return 'wrong_plugin';
    }
    $lines = is_array($data['log_lines'] ?? null) ? array_slice($data['log_lines'], -300) : [];
    $state = $data['state'] ?? null;
    unset($data['log_lines'], $data['state']);
    $status = [];
    foreach ($data as $key => $value) {
        if (is_string($key) && (is_scalar($value) || $value === null)) {
            $status[substr($key, 0, 64)] = is_string($value) ? mb_substr($value, 0, 300) : $value;
        }
    }
    chimInjurySetValue('game_status', json_encode($status, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
    chimInjurySetValue('game_status_ts', (string)time());
    if (is_array($state)) {
        chimInjurySetValue('game_state', json_encode(chimInjuryCleanState($state), JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
        chimInjurySetValue('game_state_ts', (string)time());
    }
    if (isset($status['cmd_done'])) {
        chimInjuryPruneCommands((int)$status['cmd_done']);
    }
    foreach ($lines as $line) {
        if (!is_string($line) || trim($line) === '') {
            continue;
        }
        $level = 'info';
        if (preg_match('/\]\s*\[(error|warning|warn|info|debug|critical)\]/i', $line, $m)) {
            $level = strtolower($m[1]) === 'warning' ? 'warn' : strtolower($m[1]);
        }
        chimInjuryLogStore('game', $level, $line);
    }
    return null;
}

/** Keeps only known fields of the wound report (it ends up in prompts). */
function chimInjuryCleanState(array $state): array
{
    $str = static fn($v, int $len = 80): string => mb_substr(trim((string)$v), 0, $len);
    $actors = [];
    foreach (array_slice(is_array($state['actors'] ?? null) ? $state['actors'] : [], 0, 40) as $a) {
        if (!is_array($a) || $str($a['name'] ?? '') === '') {
            continue;
        }
        $wounds = [];
        foreach (array_slice(is_array($a['wounds'] ?? null) ? $a['wounds'] : [], 0, 12) as $w) {
            if (!is_array($w)) {
                continue;
            }
            $wounds[] = [
                'key' => $str($w['key'] ?? '', 40),
                'region' => $str($w['region'] ?? '', 20),
                'type' => $str($w['type'] ?? '', 20),
                'severity' => max(1, min(3, (int)($w['severity'] ?? 1))),
                'treated' => !empty($w['treated']),
                'arrow' => !empty($w['arrow']),
                'age_days' => (float)($w['age_days'] ?? 0),
                'heals_in_days' => (float)($w['heals_in_days'] ?? -1),
                'label' => $str($w['label'] ?? 'wound', 40),
                'by' => $str($w['by'] ?? ''),
                'with' => $str($w['with'] ?? ''),
            ];
        }
        $actors[] = [
            'id' => preg_match('/^[0-9A-Fa-f]{1,8}$/', (string)($a['id'] ?? '')) ? strtoupper((string)$a['id']) : '',
            'name' => $str($a['name']),
            'player' => !empty($a['player']),
            'female' => !empty($a['female']),
            'follower' => !empty($a['follower']),
            'loaded' => !empty($a['loaded']),
            'overlays_shown' => (int)($a['overlays_shown'] ?? 0),
            'overlays_wanted' => (int)($a['overlays_wanted'] ?? 0),
            'wounds' => $wounds,
        ];
    }
    return [
        'game_days' => (float)($state['game_days'] ?? 0),
        'player_name' => $str($state['player_name'] ?? ''),
        'actors' => $actors,
    ];
}

function chimInjuryGameStatus(array $settings): array
{
    $status = json_decode((string)$settings['game_status'], true);
    return is_array($status) ? $status : [];
}

function chimInjuryGameState(array $settings, bool $freshOnly = false, ?int $now = null): array
{
    $now = $now ?? time();
    if ($freshOnly && $now - (int)$settings['game_state_ts'] > CHIM_INJURY_STATE_MAX_AGE) {
        return ['actors' => []];
    }
    $state = json_decode((string)$settings['game_state'], true);
    return is_array($state) && is_array($state['actors'] ?? null) ? $state : ['actors' => []];
}

// ---------------------------------------------------------------------------------------
// Wound text for prompts and the page
// ---------------------------------------------------------------------------------------

function chimInjuryRegionLabel(string $region): string
{
    return [
        'head' => 'head', 'chest' => 'chest', 'back' => 'back', 'left_arm' => 'left arm', 'right_arm' => 'right arm',
        'left_leg' => 'left leg', 'right_leg' => 'right leg',
    ][$region] ?? str_replace('_', ' ', $region);
}

function chimInjurySeverityLabel(int $severity): string
{
    return [1 => 'light', 2 => 'medium', 3 => 'severe'][$severity] ?? 'light';
}

function chimInjuryStatusText(array $w): string
{
    $cut = in_array($w['type'] ?? '', ['slash', 'pierce', 'maul'], true);
    if ((int)$w['severity'] >= 3) {
        if (!empty($w['treated'])) {
            return 'treated, needs rest';
        }
        return $cut ? 'untreated, bleeding' : 'untreated';
    }
    if ((float)($w['age_days'] ?? 0) < 0.1) {
        return 'fresh';
    }
    if ((float)($w['heals_in_days'] ?? -1) >= 0 && (float)$w['heals_in_days'] < 0.25) {
        return 'almost healed';
    }
    return 'healing';
}

/** "deep gash on the left arm (untreated, bleeding)" */
function chimInjuryWoundText(array $w): string
{
    return sprintf('%s on the %s (%s)', $w['label'] ?? 'wound', chimInjuryRegionLabel((string)($w['region'] ?? '')), chimInjuryStatusText($w));
}

/** Worst wounds first, at most $max. */
function chimInjuryWoundList(array $wounds, int $max = 3): string
{
    usort($wounds, static fn($a, $b) => [(int)$b['severity'], empty($b['treated'])] <=> [(int)$a['severity'], empty($a['treated'])]);
    $parts = array_map('chimInjuryWoundText', array_slice($wounds, 0, $max));
    $more = count($wounds) - count($parts);
    return implode('; ', $parts) . ($more > 0 ? "; and {$more} lighter wound" . ($more > 1 ? 's' : '') : '');
}

function chimInjuryFindActor(array $state, string $name): ?array
{
    $name = mb_strtolower(trim($name));
    if ($name === '') {
        return null;
    }
    foreach ($state['actors'] ?? [] as $a) {
        if (mb_strtolower((string)$a['name']) === $name) {
            return $a;
        }
    }
    return null;
}

/**
 * Builds the prompt additions for the NPC who is about to speak.
 * $nearbyNames: names CHIM considers nearby (null = unknown, then the player and loaded followers count).
 * Returns ['self' => string, 'nearby' => string].
 */
function chimInjuryBuildPromptParts(array $settings, array $state, string $speaker, ?array $nearbyNames): array
{
    $out = ['self' => '', 'nearby' => ''];
    if ($settings['enabled'] !== '1') {
        return $out;
    }
    $speakerLower = mb_strtolower(trim($speaker));
    if ($settings['inject_self'] === '1') {
        $me = chimInjuryFindActor($state, $speaker);
        if ($me && !empty($me['wounds']) && empty($me['player'])) {
            $out['self'] = strtr((string)$settings['self_template'], ['{INJURIES}' => chimInjuryWoundList($me['wounds'])]);
        }
    }
    if ($settings['inject_nearby'] === '1') {
        $near = $nearbyNames === null ? null : array_map(static fn($n) => mb_strtolower(trim((string)$n)), $nearbyNames);
        $parts = [];
        foreach ($state['actors'] ?? [] as $a) {
            if (empty($a['wounds']) || mb_strtolower((string)$a['name']) === $speakerLower) {
                continue;
            }
            $here = !empty($a['player']) || ($near === null ? !empty($a['loaded']) : in_array(mb_strtolower((string)$a['name']), $near, true));
            if (!$here) {
                continue;
            }
            $parts[] = $a['name'] . ': ' . chimInjuryWoundList($a['wounds'], 2);
            if (count($parts) >= 4) {
                break;
            }
        }
        if ($parts) {
            $out['nearby'] = strtr((string)$settings['nearby_template'], ['{INJURIES}' => implode(' | ', $parts)]);
        }
    }
    return $out;
}

/** context_pre.php: register the prompt additions with CHIM. */
function chimInjuryInjectPrompt(): void
{
    if (!function_exists('chimRegisterPromptInjection')) {
        return;
    }
    $settings = chimInjuryGetSettings();
    $state = chimInjuryGameState($settings, true);
    if (empty($state['actors'])) {
        return;
    }
    $nearby = null;
    if (function_exists('DataBeingsInRange')) {
        try {
            // CHIM format: "|Player|Lydia|Sofia|"
            $names = array_filter(array_map('trim', explode('|', str_replace(',', '|', (string)DataBeingsInRange()))));
            $nearby = $names ? array_values($names) : null;
        } catch (Throwable $e) {
            $nearby = null;
        }
    }
    $parts = chimInjuryBuildPromptParts($settings, $state, (string)($GLOBALS['HERIKA_NAME'] ?? ''), $nearby);
    if ($parts['self'] !== '') {
        chimRegisterPromptInjection('character_bottom', 'chim_injury_self', "\n" . $parts['self'], 150);
    }
    if ($parts['nearby'] !== '') {
        chimRegisterPromptInjection('prompt_bottom', 'chim_injury_nearby', "\n\n<injuries>\n" . $parts['nearby'] . "\n</injuries>", 150);
    }
    if ($parts['self'] !== '' || $parts['nearby'] !== '') {
        chimInjuryIncrement('stat_injected');
    }
}

// ---------------------------------------------------------------------------------------
// info_injury events from the game
// ---------------------------------------------------------------------------------------

function chimInjuryParseEvent(string $data): ?array
{
    if (!preg_match(CHIM_INJURY_EVENT_MARKER_REGEX, $data, $m, PREG_OFFSET_CAPTURE)) {
        return null;
    }
    $values = ['actor' => '', 'event' => ''];
    if (preg_match('/actor="([^"]*)"/i', $m[1][0], $am)) {
        $values['actor'] = $am[1];
    }
    if (preg_match('/event=([a-z_]+)/i', $m[1][0], $em)) {
        $values['event'] = strtolower($em[1]);
    }
    $sentence = trim(substr($data, $m[0][1] + strlen($m[0][0])));
    return ['values' => $values, 'sentence' => $sentence];
}

/**
 * prerequest.php for "info_injury": remove the marker and store the event so everyone nearby knows.
 * Returns true when the request has been handled completely.
 */
function chimInjuryHandleEvent(array &$gameRequest): bool
{
    $parsed = chimInjuryParseEvent((string)($gameRequest[3] ?? ''));
    if ($parsed === null || $parsed['sentence'] === '') {
        return false;
    }
    $gameRequest[3] = $parsed['sentence'];
    chimInjuryIncrement('stat_events');
    chimInjuryLog('info', 'Event: ' . $parsed['sentence']);
    if (!function_exists('logEvent')) {
        return false;
    }
    $people = function_exists('DataBeingsInRange') ? (string)DataBeingsInRange() : '';
    logEvent($gameRequest, $people);
    return true;
}

// ---------------------------------------------------------------------------------------
// Diagnostic file
// ---------------------------------------------------------------------------------------

function chimInjuryDiagnosticReport(): string
{
    $settings = chimInjuryGetSettings();
    $status = chimInjuryGameStatus($settings);
    $state = chimInjuryGameState($settings);
    $out = [];
    $out[] = 'CHIM Injury Framework diagnostic report';
    $out[] = 'Created: ' . gmdate('Y-m-d H:i:s') . ' UTC';
    $out[] = 'Server plugin version: ' . CHIM_INJURY_VERSION;
    $version = @file_get_contents(dirname(__DIR__, 3) . '/.version.txt');
    $out[] = 'HerikaServer version: ' . trim((string)$version);
    $out[] = 'PHP: ' . PHP_VERSION;
    $out[] = 'Prompt injection API: ' . (function_exists('chimRegisterPromptInjection') ? 'yes' : 'unknown here (checked in game requests)');
    $out[] = '';
    $out[] = '== Game plugin (last report ' . ((int)$settings['game_status_ts'] > 0 ? gmdate('Y-m-d H:i:s', (int)$settings['game_status_ts']) . ' UTC' : 'never') . ') ==';
    foreach ($status as $key => $value) {
        $out[] = sprintf('%-22s %s', $key . ':', is_bool($value) ? ($value ? 'true' : 'false') : (string)$value);
    }
    $out[] = '';
    $out[] = '== Wounds (last report) ==';
    foreach ($state['actors'] ?? [] as $a) {
        $out[] = sprintf('%s [%s]%s%s: %s', $a['name'], $a['id'], !empty($a['player']) ? ' player' : '', !empty($a['follower']) ? ' follower' : '',
            empty($a['wounds']) ? 'no wounds' : chimInjuryWoundList($a['wounds'], 12));
    }
    $out[] = '';
    $out[] = '== Settings ==';
    foreach ($settings as $key => $value) {
        if (in_array($key, ['game_status', 'game_state'], true)) {
            continue;
        }
        $out[] = sprintf('%-24s %s', $key . ':', str_replace("\n", ' ', (string)$value));
    }
    $out[] = '';
    $out[] = '== Log (server and game, oldest first) ==';
    foreach (chimInjuryLogFetch(CHIM_INJURY_LOG_KEEP) as $row) {
        $out[] = sprintf('%s [%s/%s] %s', $row['created_at'] ?? '', $row['source'] ?? '', $row['level'] ?? '', $row['message'] ?? '');
    }
    $out[] = '';
    $out[] = 'Full game log: Documents\\My Games\\Skyrim Special Edition\\SKSE\\chim-injury.log';
    return implode("\n", $out) . "\n";
}
