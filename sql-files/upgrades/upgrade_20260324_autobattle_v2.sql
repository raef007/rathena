-- Auto-Battle v2: Unlimited time, EXP penalty, rental expiry settings
-- Run AFTER upgrade_20260323b_autobattle_daily.sql
-- Adds new rows to the autobattle_settings admin table.
-- To enable unlimited time, set daily_limit_seconds = 0 in the existing row.

INSERT INTO `autobattle_settings` VALUES
    ('exp_penalty_base',    0,       'Base EXP penalty % during auto-battle (0=none, 50=half, 100=zero)'),
    ('exp_penalty_job',     0,       'Job EXP penalty % during auto-battle (0=none, 50=half, 100=zero)'),
    ('item_expiry_seconds', 2592000, 'Auto-Battle Pass rental duration in seconds (2592000=30 days, 0=permanent)')
ON DUPLICATE KEY UPDATE `setting_name` = `setting_name`;
