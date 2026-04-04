// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef CONFIG_RENEWAL_HPP
#define CONFIG_RENEWAL_HPP

//quick option to disable all renewal option, used by ./configure
//#define PRERE
#ifndef PRERE
/**
 * rAthena configuration file (http://rathena.org)
 * For detailed guidance on these check http://rathena.org/wiki/SRC/config/
 **/


/**
 * @INFO: This file holds general-purpose renewal settings, for class-specific ones check /src/config/classes folder
 **/

/// Game renewal server mode
/// (disable by commenting the line)
///
/// Leave this line to enable renewal specific support such as renewal formulas
#define RENEWAL

/// Renewal cast time
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: cast time uses single DEX-based reduction
/// (instacast at DEX 150 via castrate_dex_scale). No fixed cast time floor.
//#define RENEWAL_CAST

/// Renewal drop rate algorithms
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: no level-factor drop penalty when
/// farming monsters weaker than the player.
//#define RENEWAL_DROP

/// Renewal exp rate algorithms
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: no level-factor EXP penalty.
//#define RENEWAL_EXP

/// Renewal level modifier on damage
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: no base level modifier on skill damage.
//#define RENEWAL_LVDMG

/// Renewal ASPD [malufett]
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: Pre-RE ASPD formula, no shield penalty,
/// AGI-based bonuses, max ASPD 190 for all jobs.
//#define RENEWAL_ASPD

/// Renewal stat calculations
/// (disable by commenting the line)
///
/// Disabled for classic Pre-RE feel: uniform stat point cost table (Pre-RE).
//#define RENEWAL_STAT

#endif

#endif /* CONFIG_RENEWAL_HPP */
