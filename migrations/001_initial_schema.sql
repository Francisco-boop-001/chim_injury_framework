-- CHIM Injury Framework: plugin-owned settings, wound report and diagnostics log.
CREATE SCHEMA IF NOT EXISTS plugins;

CREATE TABLE IF NOT EXISTS plugins.chim_injury_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS plugins.chim_injury_log (
    id BIGSERIAL PRIMARY KEY,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    source TEXT NOT NULL DEFAULT 'server',
    level TEXT NOT NULL DEFAULT 'info',
    message TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_chim_injury_log_source ON plugins.chim_injury_log (source, id);
