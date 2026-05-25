// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_PANEL_PROBE_H
#define __CARTHING_PANEL_PROBE_H

#include <linux/types.h>

struct carthing_panel_info {
	bool valid;
	uint8_t vendor_id;	/* parsed from touch chip config NVM */
	uint16_t hw_id;		/* chip_code in vendor terms */
	uint8_t conf_ver;
	uint8_t mccode;
	const char *variant;	/* "BOE" / "Wily" / "Holitech" / "unknown" */
};

/* Probe the TLSC6X touch controller at I2C 0x2e (bus i2c0, GPIOZ
 * pinmux) and parse the panel-variant tag from its config NVM.
 * Returns 0 on success, negative on failure. */
int carthing_panel_probe(struct carthing_panel_info *info);

#endif /* __CARTHING_PANEL_PROBE_H */
