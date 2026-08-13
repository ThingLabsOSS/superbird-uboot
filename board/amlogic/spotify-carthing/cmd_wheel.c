// SPDX-License-Identifier: GPL-2.0+
/*
 * Quadrature decoder for the rotary encoder. Board-local because u-boot has
 * no rotary-encoder uclass upstream.
 *
 * GPIOZ_8 = A, GPIOZ_9 = B, confirmed by register-level probe. NOTE this
 * differs from Spotify's open-sourced kernel DT, which describes the *1g*
 * revision (Z_9 + Z_10); this 512MB revision wires A/B one bit lower. The
 * press switch is Z_7 on both.
 *
 * Detents are the (0,0) and (1,1) states, so wheel_poll_detents() only counts
 * transitions landing on one — 1 physical click = exactly +/-1.
 * Transition table: superbird-docs/uboot/knob_decoder.md.
 */

#include <command.h>
#include <console.h>
#include <dm.h>
#include <dm/uclass.h>
#include <env.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/string.h>
#include <asm/gpio.h>
#include <time.h>

#include "wheel.h"

/* Z-bank input register (covers Z_0..Z_15 at bit positions 0..15) — read
 * directly via readl() for the raw `wheel scope` diagnostic so we bypass
 * the dm_gpio uclass and the quadrature decoder entirely. */
#define Z_INPUT_REG_ADDR	0xff634478

/* dm_gpio_lookup_name() matches "<bank_name><offset>". Meson G12A
 * periphs bank registers as "periphs-banks"; offsets are bindings'
 * GPIOZ_N indices (so GPIOZ_8 = 8, GPIOZ_9 = 9). */
#define WHEEL_A_PAD "periphs-banks8"
#define WHEEL_B_PAD "periphs-banks9"

static struct gpio_desc wheel_a, wheel_b;
static int wheel_inited;
static u8 wheel_prev_state;
static int wheel_accum;		/* signed half-step count, CLI-facing */

static int wheel_init(void)
{
	struct udevice *btn;
	int ret;

	if (wheel_inited)
		return 0;

	/* Force the gpio-keys "button-select" device to probe — that's the
	 * node carrying our `carthing_wheel_pins` pinctrl-0, and
	 * pinctrl-default only fires on probe. Without this, Z_7/Z_8/Z_9
	 * stay in whatever alt-function BL2/burn-mode-uboot left behind
	 * and dm_gpio_get_value() returns garbage. Iterate all
	 * UCLASS_BUTTON devices so we don't depend on `select` being
	 * first. */
	for (uclass_first_device(UCLASS_BUTTON, &btn); btn;
	     uclass_next_device(&btn))
		;

	ret = dm_gpio_lookup_name(WHEEL_A_PAD, &wheel_a);
	if (ret) {
		printf("wheel: %s lookup: %d\n", WHEEL_A_PAD, ret);
		return ret;
	}
	ret = dm_gpio_lookup_name(WHEEL_B_PAD, &wheel_b);
	if (ret) {
		printf("wheel: %s lookup: %d\n", WHEEL_B_PAD, ret);
		return ret;
	}
	ret = dm_gpio_request(&wheel_a, "wheel_a");
	if (ret && ret != -EBUSY) {
		printf("wheel: %s request: %d\n", WHEEL_A_PAD, ret);
		return ret;
	}
	ret = dm_gpio_request(&wheel_b, "wheel_b");
	if (ret && ret != -EBUSY) {
		printf("wheel: %s request: %d\n", WHEEL_B_PAD, ret);
		return ret;
	}
	/* Bias is left to the DTS pinctrl (bias-disable on Z_7/Z_8/Z_9 —
	 * external pull-ups on the rotary lines). */
	dm_gpio_set_dir_flags(&wheel_a, GPIOD_IS_IN);
	dm_gpio_set_dir_flags(&wheel_b, GPIOD_IS_IN);

	/* State encoding: bit 0 = A, bit 1 = B (matches knob_decoder.md
	 * convention). */
	wheel_prev_state = dm_gpio_get_value(&wheel_a) |
			   (dm_gpio_get_value(&wheel_b) << 1);
	wheel_inited = 1;
	return 0;
}

/*
 * One quadrature decoder step. Reads both pins, returns -1/0/+1 per
 * the cur_A XOR prev_B rule from knob_decoder.md:
 *
 *     CW transition  -> (cur_A XOR prev_B) == 1
 *     CCW transition -> (cur_A XOR prev_B) == 0
 *     no transition  -> 0 (no change since last poll)
 */
static int wheel_step(void)
{
	int a = dm_gpio_get_value(&wheel_a);
	int b = dm_gpio_get_value(&wheel_b);
	u8 state = a | (b << 1);
	int cur_a, prev_b, cw, delta;

	if (state == wheel_prev_state)
		return 0;

	cur_a  = state & 1;
	prev_b = (wheel_prev_state >> 1) & 1;
	cw     = cur_a ^ prev_b;
	delta  = cw ? +1 : -1;

	wheel_prev_state = state;
	return delta;
}

/*
 * wheel_poll_detents — read pins once, return net *detents* since the
 * last call. Detents are the notch states (A,B) = (0,0) or (1,1); the
 * mid states (0,1) and (1,0) sit between physical clicks. We only emit
 * a count when a transition *lands on* a notch — so a single click
 * produces exactly +/-1 even though the underlying quadrature stream
 * passes through a mid state on the way.
 */
int wheel_poll_detents(void)
{
	int d;
	u8 state_after;

	if (wheel_init())
		return 0;

	d = wheel_step();
	if (d == 0)
		return 0;

	state_after = wheel_prev_state;
	wheel_accum += d;	/* keep the half-step total for `wheel poll` */

	/* Emit only when we've arrived at a notch (both pins equal). */
	if (state_after == 0b00 || state_after == 0b11)
		return d;
	return 0;
}

static int do_wheel(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	const char *sub = (argc >= 2) ? argv[1] : "poll";

	if (wheel_init())
		return CMD_RET_FAILURE;

	if (!strcmp(sub, "poll")) {
		int d = wheel_step();
		wheel_accum += d;
		printf("%d %d\n", d, wheel_accum);
		env_set_ulong("wheel_delta", (long)wheel_accum);
		return 0;
	}
	if (!strcmp(sub, "watch")) {
		int total = 0;
		printf("wheel watch - turn the knob; Ctrl+C to exit\n");
		while (1) {
			int d = wheel_step();
			if (d != 0) {
				total += d;
				printf("step %+d  total %d  state=%d\n",
				       d, total, wheel_prev_state);
			}
			udelay(500);
			if (ctrlc())
				break;
		}
		return 0;
	}
	if (!strcmp(sub, "detents")) {
		int total = 0;
		printf("wheel detents - turn the knob; Ctrl+C to exit\n");
		while (1) {
			int d = wheel_poll_detents();
			if (d != 0) {
				total += d;
				printf("detent %+d  total %d\n", d, total);
			}
			udelay(500);
			if (ctrlc())
				break;
		}
		return 0;
	}
	if (!strcmp(sub, "scope")) {
		/* Raw-register scope. Polls the Z input register at max speed
		 * for N seconds and reports independent edge counts on bits
		 * 8 (Z_8 = A) and 9 (Z_9 = B). No dm_gpio, no qdec, no
		 * anything — answers "does the silicon actually see these
		 * pins toggling?" */
		ulong secs = (argc >= 3) ? simple_strtoul(argv[2], NULL, 10) : 5;
		ulong end = get_timer(0) + secs * 1000;
		ulong polls = 0, a_edges = 0, b_edges = 0;
		u32 prev = readl((void *)Z_INPUT_REG_ADDR);
		int prev_a = (prev >> 8) & 1;
		int prev_b = (prev >> 9) & 1;

		printf("wheel scope - polling 0x%08x for %lu s; spin the knob!\n",
		       Z_INPUT_REG_ADDR, secs);
		printf("  start: raw=0x%08x  A=%d  B=%d\n",
		       prev, prev_a, prev_b);
		while (get_timer(0) < end) {
			u32 v = readl((void *)Z_INPUT_REG_ADDR);
			int a = (v >> 8) & 1;
			int b = (v >> 9) & 1;
			if (a != prev_a) {
				a_edges++;
				prev_a = a;
			}
			if (b != prev_b) {
				b_edges++;
				prev_b = b;
			}
			prev = v;
			polls++;
		}
		printf("  end:   raw=0x%08x  A=%d  B=%d\n",
		       prev, prev_a, prev_b);
		printf("  polls: %lu (%lu Hz),  A edges: %lu,  B edges: %lu\n",
		       polls, polls / secs, a_edges, b_edges);
		return 0;
	}
	if (!strcmp(sub, "raw")) {
		int a = dm_gpio_get_value(&wheel_a);
		int b = dm_gpio_get_value(&wheel_b);
		printf("A=%d B=%d  state=%d  prev=%d  accum=%d\n",
		       a, b, a | (b << 1), wheel_prev_state, wheel_accum);
		return 0;
	}
	if (!strcmp(sub, "reset")) {
		wheel_accum = 0;
		env_set("wheel_delta", "0");
		return 0;
	}
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	wheel, 3, 1, do_wheel,
	"rotary wheel quadrature decoder",
	"[poll|watch|detents|scope <secs>|reset|raw]\n"
	"  poll     - read once, accumulate half-step delta, print '<step> <total>'\n"
	"             also sets $wheel_delta to running total\n"
	"  watch    - poll at 2 kHz, print every half-step; Ctrl+C to exit\n"
	"  detents  - poll at 2 kHz, print only completed detents; Ctrl+C to exit\n"
	"  scope    - raw register-level edge count for both channels (default 5s)\n"
	"  reset    - zero the accumulator and $wheel_delta\n"
	"  raw      - print current GPIO state without decoding"
);
