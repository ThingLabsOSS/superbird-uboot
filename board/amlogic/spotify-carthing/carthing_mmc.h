/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __CARTHING_MMC_H
#define __CARTHING_MMC_H

#include <mmc.h>

#define CARTHING_MMC_DEV	0

/*
 * Pin the eMMC to a single bus mode, or let it negotiate freely.
 *
 * @mode:   a value from enum bus_mode, or MMC_MODES_END to un-pin and
 *          restore full negotiation ("fast").
 * @reinit: re-initialise the card now. Pass false before the card has
 *          ever been initialised (the setting is picked up by the first
 *          init); true to switch speed on a live card.
 *
 * Returns 0, or negative on error.
 */
int carthing_mmc_set_mode(enum bus_mode mode, bool reinit);

/*
 * Switch to full speed for a host-attached session (fastboot, UMS).
 * Falls back to the pinned boot mode if the fast re-init fails, so a
 * marginal unit ends up slow-but-working rather than wedged. No-op if
 * the boot mode isn't pinned in the first place.
 */
void carthing_mmc_fast_for_host(void);

/* Name of the mode the card actually negotiated, for the debug report. */
const char *carthing_mmc_current_mode(void);

#endif /* __CARTHING_MMC_H */
