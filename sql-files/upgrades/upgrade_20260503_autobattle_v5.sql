-- Auto-Battle v5: widen `mode` column from TINYINT to SMALLINT UNSIGNED
-- Run AFTER upgrade_20260502_autobattle_v4.sql
--
-- Background: the original `char_autobattle_config` schema declared `mode`
-- as TINYINT (signed: -128..127). The mode bitmask grew over time:
--   AUTOBATTLE_AUTOSIT   = 0x80 = 128  → already overflows signed TINYINT
--   AUTOBATTLE_TELESKILL = 0x100 = 256 → exceeds even unsigned TINYINT (max 255)
--
-- With the column too narrow, every UPSERT in autobattle_save_config_db
-- silently returned "Out of range value for column 'mode'" via
-- Sql_ShowDebug — the save spammed the error log but never persisted.
-- Players saw their config not load on next session, and the error spam
-- was suspected of contributing to a map-server instability.
--
-- Widening to SMALLINT UNSIGNED gives 0..65535 — plenty of headroom for
-- all current mode bits and any future ones short of 16 flags.

ALTER TABLE `char_autobattle_config`
    MODIFY COLUMN `mode` SMALLINT UNSIGNED NOT NULL DEFAULT 0;
