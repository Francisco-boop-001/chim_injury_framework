<?php
/**
 * CHIM Injury Framework - HerikaServer hook (loaded from main.php before the system prompt is built).
 * Tells the speaking NPC about their own wounds and the visible wounds of the people around.
 */

require_once __DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'chim_injury.php';

try {
    chimInjuryInjectPrompt();
} catch (Throwable $e) {
    if (class_exists('Logger')) {
        Logger::warn('[chim_injury] prompt injection failed: ' . $e->getMessage());
    }
}
