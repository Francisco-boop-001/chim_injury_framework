<?php
/**
 * CHIM Injury Framework - HerikaServer hook (loaded from main.php before dispatch).
 *
 * "info_injury" events from CHIMInjury.dll: the [injury ...] marker is removed and the event is
 * stored for everyone nearby (CHIM would otherwise limit it to the names in the text).
 * Everything else is left untouched.
 */

if (($gameRequest[0] ?? '') !== 'info_injury') {
    return;
}

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_injury.php';

if (chimInjuryHandleEvent($gameRequest)) {
    terminate();
}
