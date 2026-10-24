<?php
/**
 * GET: settings and pending page commands for CHIMInjury.dll as plain key=value lines.
 */
require __DIR__ . DIRECTORY_SEPARATOR . 'bootstrap_inc.php';

header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');
echo chimInjuryConfigText(chimInjuryGetSettings());
