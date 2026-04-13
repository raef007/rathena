-- ==========================================================================
-- Pre-Registration Reward Support
-- Adds created_at column to login table for date-based eligibility checks.
-- Run: mysql -u root -p ragnarok < sql-files/upgrades/upgrade_20260413_prereg.sql
-- ==========================================================================
--
-- IMPORTANT: For existing accounts, created_at will be set to the moment
-- this migration runs. If accounts were created during April 1-18, 2026,
-- backfill those account IDs manually:
--
--   UPDATE login SET created_at = '2026-04-01 00:00:00'
--   WHERE account_id IN (list of pre-reg account IDs);
--
-- ==========================================================================

ALTER TABLE `login`
  ADD COLUMN IF NOT EXISTS `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
  AFTER `web_auth_token_enabled`;
