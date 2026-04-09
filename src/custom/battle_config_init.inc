// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom battle config initializations
 * Place the definition of custom battle configuration initializations in this file.
 *
 * Do not forget to add a comma at the end of the line, if you want to add another config
 *
 * Format:
 *	 { "name", &battle_config.<variable name>, <default value>, <minimum value>, <maximum value> },
 **/

{ "autobattle_default_seconds",    &battle_config.autobattle_default_seconds,    10800, 0, 86400 },
{ "autobattle_vip_bonus_seconds",  &battle_config.autobattle_vip_bonus_seconds,  10800, 0, 86400 },
{ "autobattle_max_seconds",        &battle_config.autobattle_max_seconds,        86400, 0, 604800 },
{ "autobattle_roam_interval",      &battle_config.autobattle_roam_interval,      3000,  1000, 10000 },
{ "autobattle_exp_penalty_base",   &battle_config.autobattle_exp_penalty_base,   0,     0,    100 },
{ "autobattle_exp_penalty_job",    &battle_config.autobattle_exp_penalty_job,    0,     0,    100 },

{ "fakeplayer_enabled",            &battle_config.fakeplayer_enabled,            1,     0,    1      },
{ "fakeplayer_respawn_interval",   &battle_config.fakeplayer_respawn_interval,   60,    10,   3600   },
{ "fakeplayer_online_count",       &battle_config.fakeplayer_online_count,       1,     0,    1      },

