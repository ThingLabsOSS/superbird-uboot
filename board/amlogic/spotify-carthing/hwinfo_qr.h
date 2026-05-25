// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_HWINFO_QR_H
#define __CARTHING_HWINFO_QR_H

#include <linux/types.h>

#define HWINFO_QR_JSON_MAX	320

/* Build a compact JSON describing this carthing's probed hardware.
 * Writes a NUL-terminated string into buf. Returns the byte count
 * (excluding NUL) or 0 on error. */
size_t carthing_hwinfo_build_json(char *buf, size_t buflen);

#endif /* __CARTHING_HWINFO_QR_H */
