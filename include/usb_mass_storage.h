/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2011 Samsung Electrnoics
 * Lukasz Majewski <l.majewski@samsung.com>
 */

#ifndef __USB_MASS_STORAGE_H__
#define __USB_MASS_STORAGE_H__

#include <part.h>
#include <linux/usb/composite.h>

/* Wait at maximum 60 seconds for cable connection */
#define UMS_CABLE_READY_TIMEOUT	60

struct ums {
	int (*read_sector)(struct ums *ums_dev,
			   ulong start, lbaint_t blkcnt, void *buf);
	int (*write_sector)(struct ums *ums_dev,
			    ulong start, lbaint_t blkcnt, const void *buf);
	unsigned int start_sector;
	unsigned int num_sectors;
	const char *name;
	struct blk_desc block_dev;
	int hwpart;
};

int fsg_init(struct ums *ums_devs, int count, struct udevice *udc);
void fsg_cleanup(void);
int fsg_main_thread(void *);
int fsg_add(struct usb_configuration *c);

/*
 * Board hook for exiting the UMS loop without UART access. Default is
 * a no-op (in cmd/usb_mass_storage.c); boards can override to poll a
 * physical button and return non-zero to request shutdown. Polled from
 * both the outer ums command loop AND the inner sleep_thread, so it
 * stays responsive even when the gadget is idle waiting for host I/O.
 */
int ums_board_abort_check(void);
#endif /* __USB_MASS_STORAGE_H__ */
