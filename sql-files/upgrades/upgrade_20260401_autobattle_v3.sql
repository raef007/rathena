-- Auto-Battle v3: Phase 22-25 (Auto-Target, Auto-Skill, Auto-Sit, Auto-Support Enhancement)
-- Run: mysql -u root -p ragnarok < sql-files/upgrades/upgrade_20260401_autobattle_v3.sql

ALTER TABLE `char_autobattle_config`
    ADD COLUMN IF NOT EXISTS `target_mob_ids` VARCHAR(255) DEFAULT '' AFTER `bonus_seconds`,
    ADD COLUMN IF NOT EXISTS `attack_skill_id` SMALLINT UNSIGNED DEFAULT 0 AFTER `target_mob_ids`,
    ADD COLUMN IF NOT EXISTS `attack_skill_lv` TINYINT UNSIGNED DEFAULT 0 AFTER `attack_skill_id`,
    ADD COLUMN IF NOT EXISTS `autosit_hp_threshold` TINYINT UNSIGNED DEFAULT 0 AFTER `attack_skill_lv`,
    ADD COLUMN IF NOT EXISTS `autosit_sp_threshold` TINYINT UNSIGNED DEFAULT 0 AFTER `autosit_hp_threshold`,
    ADD COLUMN IF NOT EXISTS `support_target_mode` TINYINT UNSIGNED DEFAULT 0 AFTER `autosit_sp_threshold`,
    ADD COLUMN IF NOT EXISTS `support_target_name` VARCHAR(24) DEFAULT '' AFTER `support_target_mode`;
