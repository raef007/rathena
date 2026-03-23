-- Auto-Battle Daily Time Cap System [rAthena]
-- Adds daily usage tracking and admin-configurable limits
-- Run this AFTER upgrade_20260323_autobattle.sql

-- =============================================
-- 1) Global settings table (edit in DBeaver to change limits)
-- =============================================
CREATE TABLE IF NOT EXISTS `autobattle_settings` (
	`setting_name` VARCHAR(50) NOT NULL,
	`setting_value` INT NOT NULL DEFAULT 0,
	`description` VARCHAR(255) NOT NULL DEFAULT '',
	PRIMARY KEY (`setting_name`)
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

-- Default limits (change these anytime via DBeaver, takes effect on next player login)
INSERT INTO `autobattle_settings` VALUES
	('daily_limit_seconds',     10800,  'Daily auto-battle limit for normal players (10800 = 3 hours)'),
	('vip_daily_limit_seconds', 21600,  'Daily auto-battle limit for VIP players (21600 = 6 hours)'),
	('max_bonus_seconds',       86400,  'Maximum bonus seconds from addtime/purchases (86400 = 24 hours)')
ON DUPLICATE KEY UPDATE `setting_name` = `setting_name`;

-- =============================================
-- 2) Per-character daily usage tracking columns
-- =============================================
ALTER TABLE `char_autobattle_config`
	ADD COLUMN `daily_seconds_used` INT NOT NULL DEFAULT 0 AFTER `loot_rarity_filter`,
	ADD COLUMN `bonus_seconds` INT NOT NULL DEFAULT 0 AFTER `daily_seconds_used`,
	ADD COLUMN `last_reset_date` DATE DEFAULT NULL AFTER `bonus_seconds`;
