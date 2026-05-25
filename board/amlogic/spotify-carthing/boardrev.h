// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_BOARDREV_H
#define __CARTHING_BOARDREV_H

/*
 * Probe the carthing hardware revision via SARADC channel 1.
 * Returns the revision number (1..12) on success, -1 on read error,
 * 0 if the raw reading is outside the recognized range.
 *
 * Vendor maps an on-board resistor divider to ADC raw values in the
 * 10-bit range; we get 12-bit and divide. See vendor
 * include/spotify/hw_probe.h sp_board_revision_confs[] for the table.
 */
int carthing_probe_board_rev(void);

#endif /* __CARTHING_BOARDREV_H */
