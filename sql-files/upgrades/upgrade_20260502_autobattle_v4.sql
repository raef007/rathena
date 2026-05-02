-- Auto-Battle v4: Persist full per-character config across logins
-- Run AFTER upgrade_20260401_autobattle_v3.sql
--
-- NOTE: This script is NOT idempotent. Running it twice will error with
-- "Duplicate column" — that's harmless, just means the columns already exist.
-- If you need to re-run after partial application, drop the columns first:
--   ALTER TABLE `char_autobattle_config`
--     DROP COLUMN `autopot_hp_id`, DROP COLUMN `autopot_hp_threshold`, ... ;

ALTER TABLE `char_autobattle_config`
    ADD COLUMN `autopot_hp_id`           INT UNSIGNED     DEFAULT 0  AFTER `support_target_name`,
    ADD COLUMN `autopot_hp_threshold`    TINYINT UNSIGNED DEFAULT 90 AFTER `autopot_hp_id`,
    ADD COLUMN `autopot_sp_id`           INT UNSIGNED     DEFAULT 0  AFTER `autopot_hp_threshold`,
    ADD COLUMN `autopot_sp_threshold`    TINYINT UNSIGNED DEFAULT 90 AFTER `autopot_sp_id`,
    ADD COLUMN `gohome_no_pots`          TINYINT UNSIGNED DEFAULT 0  AFTER `autopot_sp_threshold`,
    ADD COLUMN `support_skill_triggers`  VARCHAR(255)     DEFAULT '' AFTER `support_skill_scopes`,
    ADD COLUMN `support_item_count`      TINYINT UNSIGNED DEFAULT 0  AFTER `support_skill_triggers`,
    ADD COLUMN `support_item_ids`        VARCHAR(255)     DEFAULT '' AFTER `support_item_count`,
    ADD COLUMN `support_item_status_ids` VARCHAR(255)     DEFAULT '' AFTER `support_item_ids`;
