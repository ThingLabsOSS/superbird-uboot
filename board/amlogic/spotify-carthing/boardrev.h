// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_BOARDREV_H
#define __CARTHING_BOARDREV_H

/*
 * Probe the hardware revision via SARADC channel 1. Returns 1..12, -1 on read
 * error, 0 if the reading is outside the recognized range.
 *
 * Vendor's table (include/spotify/hw_probe.h, sp_board_revision_confs[]) maps
 * the resistor divider to 10-bit ADC values; we read 12-bit and divide.
 */
int carthing_probe_board_rev(void);

#endif /* __CARTHING_BOARDREV_H */
