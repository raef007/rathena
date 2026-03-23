-- Auto-Battle System upgrade [rAthena]
-- Adds support for persistent auto-battle configurations

-- Create table for auto-battle character settings
CREATE TABLE IF NOT EXISTS `char_autobattle_config` (
	`char_id` INT NOT NULL,
	`mode` TINYINT NOT NULL DEFAULT 0,
	`range` TINYINT NOT NULL DEFAULT 9,
	`target_priority` TINYINT NOT NULL DEFAULT 2,
	`support_skill_count` TINYINT NOT NULL DEFAULT 0,
	`support_skill_ids` VARCHAR(255) NOT NULL DEFAULT '',
	`support_skill_lvs` VARCHAR(255) NOT NULL DEFAULT '',
	`support_skill_thresholds` VARCHAR(255) NOT NULL DEFAULT '',
	`support_skill_scopes` VARCHAR(255) NOT NULL DEFAULT '',
	`loot_range` SMALLINT NOT NULL DEFAULT 9,
	`loot_rarity_filter` TINYINT NOT NULL DEFAULT 0,
	`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
	`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	PRIMARY KEY (`char_id`),
	FOREIGN KEY (`char_id`) REFERENCES `char`(`char_id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET=utf8mb4;

-- Create index for faster lookups
CREATE INDEX `ix_autobattle_updated` ON `char_autobattle_config`(`updated_at`);
