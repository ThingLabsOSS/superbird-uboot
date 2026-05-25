// SPDX-License-Identifier: GPL-2.0+
/*
 * `bootmenu` u-boot command — Android-style on-panel menu for the
 * Car Thing.
 *
 * Renders directly via the vidconsole uclass `position_cursor` API
 * (NOT ANSI escape codes — those don't survive the rotated console
 * driver) and uses a ">" cursor marker for selection (also avoids
 * needing reverse-video ANSI).
 *
 * Navigation: preset1 (up) / preset4 (down), or spin the rotary wheel.
 * Selection:  the wheel-press "select" button.
 * Cancel:     "back".
 *
 * Adding new items: append to `items[]`. Each item's `action_cmd` is
 * passed to `run_command()` when selected.
 */

#include <command.h>
#include <console.h>
#include <button.h>
#include <env.h>
#include <linux/delay.h>
#include <time.h>
#include <linux/string.h>
#include <dm.h>
#include <dm/uclass.h>
#include <video.h>
#include <video_console.h>

#include "wheel.h"
#include "charger.h"
#include "boardrev.h"
#include "mfi.h"
#include "cert_parse.h"
#include "hwinfo_qr.h"
#include "qrcodegen.h"
#include "panel_probe.h"

#include <asm/arch/boot.h>
#include <asm/global_data.h>
#include <adc.h>
#include <i2c.h>
#include <mmc.h>
#include <u-boot/sha256.h>

/* Brightness presets — selecting the menu item cycles through these.
 * Index matches the order: Low -> Medium -> High -> Low. The short
 * names double as values for the `brightness` env var, so we can
 * round-trip cycle selection across reboots. */
static const char * const brightness_names[] = { "Low", "Medium", "High" };
static const char * const brightness_short[] = { "low", "med", "high" };
static const char * const brightness_cmds[]  = {
	"setbright low", "setbright med", "setbright high",
};
#define NBRIGHTNESS	((int)(sizeof(brightness_names) / sizeof(brightness_names[0])))

/* Persist across menu invocations so the cycle remembers where it left off. */
static int brightness_idx = 1;	/* default to "Medium" */

static int brightness_idx_from_env(void)
{
	const char *v = env_get("brightness");
	int i;

	if (!v)
		return 1;
	for (i = 0; i < NBRIGHTNESS; i++)
		if (!strcmp(v, brightness_short[i]))
			return i;
	return 1;
}

enum item_kind {
	K_CMD,		/* run action_cmd, optionally reset_after */
	K_SETTINGS,	/* open the settings sub-menu (brightness, display-init) */
	K_CHARGER,	/* paint a panel screen with charger status, wait for back */
	K_HWINFO,	/* paint a panel screen with full hardware inventory */
};

struct menu_item {
	enum item_kind kind;
	const char *label;
	const char *action_cmd;
	/* If non-NULL: while action_cmd runs, paint this string centred on
	 * the panel + a right-aligned "Press to exit ->" hint, so the user
	 * sees which mode the device is in (these commands otherwise sit
	 * blocked on the USB gadget with a blank screen). */
	const char *panel_title;
	/* If true: reset the device after the action returns. Needed for
	 * USB gadget modes — u-boot's gadget stack doesn't fully tear
	 * down between sessions, so a second `ums`/`fastboot` after the
	 * first wedges the controller. Resetting is the pragmatic fix
	 * until upstream cleans up the gadget shutdown path. */
	int reset_after;
};

static const struct menu_item items[] = {
	{ K_CMD,        "Fastboot",         "fastboot 0",  "FASTBOOT",         1 },
	{ K_CMD,        "Target Disk Mode", "ums 0 mmc 0", "TARGET DISK MODE", 1 },
	{ K_SETTINGS,   "Settings",         NULL,          NULL,               0 },
	{ K_CHARGER,    "Charger Info",     NULL,          NULL,               0 },
	{ K_HWINFO,     "Hardware Info",    NULL,          NULL,               0 },
};

#define NITEMS	((int)(sizeof(items) / sizeof(items[0])))

static const char *item_label(int i)
{
	return items[i].label;
}

/* Logical (post-rotation) panel size with 16x32 font:
 *   800 / 16 = 50 cols
 *   480 / 32 = 15 rows */
#define VC_COLS	50
#define VC_ROWS	15

/* Vidconsole device handle, looked up once at command start. */
static struct udevice *vc;

/* Write directly to the vidconsole device, bypassing the stdout
 * multiplexer — so menu drawing never depends on env stdout containing
 * "vidconsole", and conversely general u-boot output (command echo,
 * busy spinners, etc) never bleeds onto the panel even while the menu
 * is active. */
static void vc_puts(const char *s)
{
	if (vc)
		vidconsole_put_string(vc, s);
}

static void vc_at(int row, int col)
{
	if (vc)
		vidconsole_position_cursor(vc, col, row);
}

static int center_col(int width)
{
	int c = (VC_COLS - width) / 2;
	return c < 0 ? 0 : c;
}

static void draw(int sel)
{
	const char *title = "BOOT MENU";
	const char *sep   = "----------------";
	const char *hint  = "1/4 or knob to nav  knob-press=enter  back=exit";
	int i;

	if (vc)
		vidconsole_clear_and_reset(vc);

	/* Title centered on row 2; separator on row 3. */
	vc_at(2, center_col(strlen(title)));
	vc_puts(title);
	vc_at(3, center_col(strlen(sep)));
	vc_puts(sep);

	/* Items one-row-spaced starting at row 5. Cursor marker "> " on
	 * selected line. */
	for (i = 0; i < NITEMS; i++) {
		const char *label = item_label(i);
		int row = 5 + i;
		int col = center_col(strlen(label) + 4);
		vc_at(row, col);
		vc_puts((i == sel) ? "> " : "  ");
		vc_puts(label);
	}

	/* Hint on the last visible row. */
	vc_at(VC_ROWS - 1, center_col(strlen(hint)));
	vc_puts(hint);
}

/*
 * Paint a one-shot "mask-ROM USB attempt failed" screen.
 *
 * Triggered when spotify-carthing.c's detect_maskrom_failed() sees the
 * SoC's POC field set to USB-intent (= buttons 1+4 held at strapping)
 * but the actual boot device is eMMC (= mask-ROM tried USB, host
 * didn't enumerate, fell through to eMMC). The most common cause is a
 * charge-only USB cable. Print the diagnosis + suggestion, wait for
 * any button press, then carry on into the normal bootmenu.
 */
static void draw_maskrom_failed_screen(void)
{
	if (!vc)
		return;

	vidconsole_clear_and_reset(vc);

	{
		const char *title = "MASK-ROM USB RECOVERY FAILED";
		vc_at(1, center_col(strlen(title)));
		vc_puts(title);
	}
	{
		const char *sep = "============================";
		vc_at(2, center_col(strlen(sep)));
		vc_puts(sep);
	}

	vc_at(4, 1);
	vc_puts("You tried to enter recovery mode by holding");
	vc_at(5, 1);
	vc_puts("buttons 1 and 4 at reset, but the SoC could");
	vc_at(6, 1);
	vc_puts("not enumerate over USB.");

	vc_at(8, 1);
	vc_puts("Common causes: a charge-only or damaged USB");
	vc_at(9, 1);
	vc_puts("cable, or a faulty USB port on the host.");

	vc_at(11, 1);
	vc_puts("Try a different cable AND/OR port.");

	vc_at(VC_ROWS - 1, VC_COLS - 16);
	vc_puts("Press to dismiss");

	video_sync(dev_get_parent(vc), false);
}

/*
 * Paint the in-mode screen: big title in the vertical centre, plus a
 * right-aligned "Press to exit ->" hint 3 rows up from the bottom —
 * which physically lines up with the back button on the carthing's
 * landscape orientation.
 *
 * Explicit video_sync() at the end is load-bearing: vidconsole only
 * flushes the framebuffer on newlines or via the cyclic idle worker,
 * and the fastboot/UMS poll loops we're about to enter don't yield
 * long enough for the cyclic to fire. Without the sync, the offscreen
 * writes are invisible until the user exits and the next vidconsole
 * activity happens to flush.
 */
static void draw_mode(const char *title)
{
	const char *exit_hint = "Press to exit ->";

	if (!vc)
		return;

	vidconsole_clear_and_reset(vc);

	vc_at(VC_ROWS / 2 - 1, center_col(strlen(title)));
	vc_puts(title);

	vc_at(VC_ROWS - 3, VC_COLS - strlen(exit_hint));
	vc_puts(exit_hint);

	video_sync(dev_get_parent(vc), false);
}

/*
 * Settings sub-menu.
 *
 * Currently has two items:
 *   - Brightness  — cycles through brightness_names[] (Low/Medium/High);
 *     persisted in env "brightness", consumed by misc_init_r's
 *     apply_saved_brightness().
 *   - Init display at boot  — toggles whether misc_init_r's eager
 *     UCLASS_VIDEO probe runs. Default Yes (= panel comes up during
 *     boot, splash visible, OS adopts an initialized framebuffer).
 *     No saves ~700 ms reset→autoboot but leaves the panel
 *     uninitialized — fine for OSes that init the display themselves
 *     (mainline Linux DRM), broken for OSes that just adopt u-boot's
 *     state (e.g. the stock Spotify rootfs). Persisted in env
 *     "quick_boot" (kept the legacy name to not break anyone who
 *     already set it); polarity is inverted (display-init=Yes means
 *     quick_boot unset or 0).
 *
 * Same input model as the other sub-screens (charger, hwinfo): own
 * input loop, knob/buttons-1+4 to navigate, knob-press to change,
 * back to return to the main menu.
 */
/* Edge-triggered button tracker — shared by the main menu loop and
 * sub-screens (settings, charger, hwinfo). */
struct btn_edge {
	const char *label;
	struct udevice *dev;
	int prev;
};

static int btn_init(struct btn_edge *b, const char *label)
{
	int ret;
	b->label = label;
	b->prev = 0;
	ret = button_get_by_label(label, &b->dev);
	if (ret) {
		printf("bootmenu: button '%s' not found (err=%d)\n", label, ret);
		return ret;
	}
	return 0;
}

static int btn_edge(struct btn_edge *b)
{
	int now = (button_get_state(b->dev) == BUTTON_ON);
	int edge = now && !b->prev;
	b->prev = now;
	return edge;
}

enum settings_item {
	SET_BRIGHTNESS,
	SET_DISPLAY_INIT,
};
#define NSETTINGS 2

/* True when u-boot will initialize the panel during boot. The
 * underlying env var (`quick_boot`) keeps its name for backward
 * compat with anyone who already set it — semantics unchanged
 * (quick_boot=1 means "skip the eager init"). */
static bool display_init_enabled(void)
{
	return env_get_yesno("quick_boot") != 1;
}

static void draw_settings(int sel)
{
	const char *title = "SETTINGS";
	const char *sep   = "----------------";
	const char *hint  = "knob to nav  press=change  back=exit";
	char buf[VC_COLS + 1];

	if (!vc)
		return;
	vidconsole_clear_and_reset(vc);

	vc_at(2, center_col(strlen(title)));
	vc_puts(title);
	vc_at(3, center_col(strlen(sep)));
	vc_puts(sep);

	snprintf(buf, sizeof(buf), "Brightness: %s",
		 brightness_names[brightness_idx]);
	vc_at(6, center_col(strlen(buf) + 4));
	vc_puts(sel == SET_BRIGHTNESS ? "> " : "  ");
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "Init display at boot: %s",
		 display_init_enabled() ? "Yes" : "No");
	vc_at(7, center_col(strlen(buf) + 4));
	vc_puts(sel == SET_DISPLAY_INIT ? "> " : "  ");
	vc_puts(buf);

	/* Help row explains the non-obvious "No" consequence. Some OSes
	 * (notably the stock Spotify rootfs) don't re-init the panel
	 * themselves — they just adopt whatever state u-boot left it in.
	 * Skipping the u-boot init shaves ~700 ms but leaves those OSes
	 * with a dark / partially-initialized screen. */
	{
		const char *help1 = "\"No\" boots ~700 ms faster but the OS";
		const char *help2 = "that follows must init the display.";
		vc_at(10, center_col(strlen(help1)));
		vc_puts(help1);
		vc_at(11, center_col(strlen(help2)));
		vc_puts(help2);
	}

	vc_at(VC_ROWS - 1, center_col(strlen(hint)));
	vc_puts(hint);

	video_sync(dev_get_parent(vc), false);
}

static void cycle_brightness(void)
{
	brightness_idx = (brightness_idx + 1) % NBRIGHTNESS;
	run_command(brightness_cmds[brightness_idx], 0);
	env_set("brightness", brightness_short[brightness_idx]);
	env_save();
}

static void toggle_display_init(void)
{
	env_set("quick_boot", display_init_enabled() ? "1" : "0");
	env_save();
}

static void run_settings_menu(struct btn_edge *up, struct btn_edge *dn,
			      struct btn_edge *sel_btn, struct btn_edge *back)
{
	int sel = 0;

	/* Drain any held buttons inherited from the outer menu. */
	(void)btn_edge(up);
	(void)btn_edge(dn);
	(void)btn_edge(sel_btn);
	(void)btn_edge(back);

	draw_settings(sel);

	while (1) {
		int dt;

		mdelay(5);

		dt = wheel_poll_detents();
		if (dt != 0) {
			sel = ((sel + dt) % NSETTINGS + NSETTINGS) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(up)) {
			sel = (sel - 1 + NSETTINGS) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(dn)) {
			sel = (sel + 1) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(sel_btn)) {
			if (sel == SET_BRIGHTNESS)
				cycle_brightness();
			else
				toggle_display_init();
			draw_settings(sel);
			continue;
		}
		if (btn_edge(back) || ctrlc())
			return;
	}
}

/*
 * Render a multi-line "Charger Info" screen: read the BC1.2 detector
 * over I2C and paint a few decoded fields. Same right-aligned exit
 * hint as draw_mode() so the layout feels consistent. video_sync at
 * the end for the same reason as draw_mode — vidconsole only flushes
 * on newlines or the cyclic worker, and we're about to spin waiting
 * for a button press.
 */
static void draw_charger_info(void)
{
	const char *exit_hint = "Press to exit ->";
	struct carthing_charger_info info;
	char buf[64];
	const char *title = "CHARGER";
	int row = 2;

	if (!vc)
		return;

	vidconsole_clear_and_reset(vc);

	vc_at(row++, center_col(strlen(title)));
	vc_puts(title);
	row++;

	if (carthing_charger_read(&info) || !info.valid) {
		const char *err = "I2C read failed";
		vc_at(row++, center_col(strlen(err)));
		vc_puts(err);
	} else {
		const char *type = carthing_charger_type_str(info.status);

		vc_at(row++, center_col(strlen(type)));
		vc_puts(type);
		row++;

		snprintf(buf, sizeof(buf), "MAX14656 rev %d  status=0x%02x",
			 info.regs[0x00] & 0xf, info.status);
		vc_at(row++, center_col(strlen(buf)));
		vc_puts(buf);

		snprintf(buf, sizeof(buf), "VB=%d  OVP=%d  detecting=%d",
			 (info.status >> 4) & 1,
			 (info.status >> 5) & 1,
			 (info.status >> 6) & 1);
		vc_at(row++, center_col(strlen(buf)));
		vc_puts(buf);
	}

	{
		const char *knob_hint = "Knob press: force redetect";

		vc_at(VC_ROWS - 5, center_col(strlen(knob_hint)));
		vc_puts(knob_hint);
	}

	vc_at(VC_ROWS - 3, VC_COLS - strlen(exit_hint));
	vc_puts(exit_hint);

	video_sync(dev_get_parent(vc), false);
}

/* Forward decl from cmd_tsensor.c — read on-die temp sensors. */
int carthing_tsensor_read_pll(void);
int carthing_tsensor_read_ddr(void);


/*
 * Inventory screen. Two pages; knob press cycles between them and
 * re-reads any live values. Page 1 is the at-a-glance overview;
 * page 2 has finer hardware details. Title row has a "Page N/2"
 * indicator on the right, followed by a separator line that spans
 * the screen. Hints sit at the very bottom.
 */
static int hwinfo_page;	/* 0..HWINFO_NPAGES-1; persists across re-entry */

#define HWINFO_NPAGES	6

/* QR page cache. Filled lazily on first visit. Holds the JSON
 * payload + the encoded QR matrix. Re-encoded on knob press. */
static struct {
	bool valid;
	char json[HWINFO_QR_JSON_MAX];
	size_t json_len;
	/* qrcodegen output buffers (sized per nayuki's BUFFER_LEN_MAX for
	 * the max version we permit). */
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
	uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
	int qr_size;
} hwinfo_qr_cache;

/* Cert hex scroll modal — entered by knob press on the MFi info page
 * (page 2). Knob rotate scrolls 16 bytes at a time, knob press
 * re-reads the cert, back exits back to the MFi info page. */
static int cert_scroll_lines;	/* current top-of-window in 16-B lines */
#define CERT_HEX_VISIBLE_ROWS	11
#define CERT_HEX_BYTES_PER_ROW	16

/* SARADC dump page (page 4). Lazy single-shot read of all 8
 * channels with human-readable interpretation where known. */
static struct {
	bool valid;
	uint raw[8];
} hwinfo_adc;

/* Panel info cache — populated once at hwinfo entry. */
static struct carthing_panel_info hwinfo_panel_cache;

/* MFi page cache. Filled the first time the user enters the MFi
 * page (or refreshes it). Reading the cert involves ~40 ms of cold-
 * wake retries + 10 ms settle + the 608-byte payload itself, so we
 * skip re-reading on every redraw. */
static struct {
	bool valid;
	uint8_t version;
	uint8_t serial[MFI_SERIAL_SIZE];
	uint16_t cert_len;
	uint8_t cert_sha256[SHA256_SUM_LEN];
	uint8_t cert[MFI_CERT_MAX_SIZE];
	struct carthing_cert_info parsed;
} hwinfo_mfi_cache;

/* MFi challenge-response cache. Lazily filled on first entry to that
 * page; knob-press re-runs with an incremented counter for fresh
 * signatures. */
static struct {
	bool valid;
	uint32_t counter;
	uint8_t challenge[MFI_CHALLENGE_SIZE];
	uint8_t response[MFI_RESPONSE_SIZE];
	uint32_t elapsed_ms;
} hwinfo_chr_cache;

static int refresh_mfi_cache(void)
{
	int ret;

	hwinfo_mfi_cache.valid = false;

	ret = carthing_mfi_read_version(&hwinfo_mfi_cache.version);
	if (ret < 0)
		return ret;
	ret = carthing_mfi_read_serial(hwinfo_mfi_cache.serial);
	if (ret < 0)
		return ret;
	ret = carthing_mfi_read_cert_len(&hwinfo_mfi_cache.cert_len);
	if (ret < 0)
		return ret;
	if (hwinfo_mfi_cache.cert_len == 0 ||
	    hwinfo_mfi_cache.cert_len > MFI_CERT_MAX_SIZE)
		return -EINVAL;
	ret = carthing_mfi_read_cert(hwinfo_mfi_cache.cert,
				     hwinfo_mfi_cache.cert_len);
	if (ret < 0)
		return ret;
	sha256_csum_wd(hwinfo_mfi_cache.cert, hwinfo_mfi_cache.cert_len,
		       hwinfo_mfi_cache.cert_sha256, CHUNKSZ_SHA256);
	(void)carthing_cert_parse(hwinfo_mfi_cache.cert,
				  hwinfo_mfi_cache.cert_len,
				  &hwinfo_mfi_cache.parsed);
	hwinfo_mfi_cache.valid = true;
	return 0;
}

static void draw_hwinfo_chrome(int page)
{
	char title[VC_COLS + 1];
	char sep[VC_COLS + 1];
	int i, tlen;

	if (!vc)
		return;

	/* Title and separator span up to VC_COLS-1 chars (NEVER touch
	 * the very last column — vidconsole appears to wrap after that
	 * char which can propagate into a screen scroll). */
	{
		int max_w = VC_COLS - 1;

		snprintf(title, sizeof(title), "HARDWARE INFO");
		tlen = strlen(title);
		for (i = tlen; i < max_w - 8; i++)
			title[i] = ' ';
		title[max_w - 8] = '\0';
		snprintf(title + strlen(title), sizeof(title) - strlen(title),
			 "Page %d/%d", page + 1, HWINFO_NPAGES);

		for (i = 0; i < max_w; i++)
			sep[i] = '-';
		sep[max_w] = '\0';
	}

	vc_at(0, 0);
	vc_puts(title);
	vc_at(1, 0);
	vc_puts(sep);
}

/* Decode JEDEC eMMC manufacturer ID to a short name. Spotify's
 * carthing comes with eMMC from a couple different vendors across
 * builds, so this list covers the plausible candidates. */
static const char *mmc_mid_name(uint8_t mid)
{
	switch (mid) {
	case 0x02: return "SanDisk";
	case 0x11: return "Toshiba";
	case 0x13: return "Micron";
	case 0x15: return "Samsung";
	case 0x45: return "SanDisk";
	case 0x70: return "Kingston";
	case 0x88: return "Foresee";
	case 0x90: return "SK Hynix";
	case 0x9b: return "YMTC";
	case 0xfe: return "Numonyx";
	default:   return NULL;
	}
}

static void draw_hwinfo_page0(void)
{
	struct carthing_charger_info ch;
	char buf[64];
	u32 socinfo;
	int rev, t_pll, t_ddr, row = 2;
	bool charger_ok;
	DECLARE_GLOBAL_DATA_PTR;

	rev = carthing_probe_board_rev();
	if (rev > 0)
		snprintf(buf, sizeof(buf), "Board    : Rev %d", rev);
	else
		snprintf(buf, sizeof(buf), "Board    : unknown");
	vc_at(row++, 0);
	vc_puts(buf);

	socinfo = meson_get_socinfo();
	snprintf(buf, sizeof(buf), "SoC      : G12A S905D2 %x:%x",
		 (socinfo >> 24) & 0xff, (socinfo >> 8) & 0xff);
	vc_at(row++, 0);
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "DRAM     : %llu MiB",
		 (unsigned long long)gd->ram_size / (1024 * 1024));
	vc_at(row++, 0);
	vc_puts(buf);

	if (hwinfo_panel_cache.valid)
		snprintf(buf, sizeof(buf),
			 "Panel    : %s ST7701S (vid=%02x hw=%03x)",
			 hwinfo_panel_cache.variant,
			 hwinfo_panel_cache.vendor_id,
			 hwinfo_panel_cache.hw_id);
	else
		snprintf(buf, sizeof(buf),
			 "Panel    : ST7701S (probe failed)");
	vc_at(row++, 0);
	vc_puts(buf);

	charger_ok = (carthing_charger_read(&ch) == 0 && ch.valid);
	if (charger_ok)
		snprintf(buf, sizeof(buf), "Charger  : %s",
			 carthing_charger_type_str(ch.status));
	else
		snprintf(buf, sizeof(buf), "Charger  : (no response)");
	vc_at(row++, 0);
	vc_puts(buf);

	t_pll = carthing_tsensor_read_pll();
	t_ddr = carthing_tsensor_read_ddr();
	if (t_pll >= 0 && t_ddr >= 0)
		snprintf(buf, sizeof(buf), "Temp     : PLL %dC  DDR %dC",
			 t_pll, t_ddr);
	else
		snprintf(buf, sizeof(buf), "Temp     : read failed");
	vc_at(row++, 0);
	vc_puts(buf);

	{
		const char *bv = env_get("brightness");
		if (!bv) bv = "(unset)";
		snprintf(buf, sizeof(buf), "Backlight: %s", bv);
		vc_at(row++, 0);
		vc_puts(buf);
	}

	{
		struct mmc *mmc = find_mmc_device(0);

		if (mmc) {
			const char *mfg = mmc_mid_name(mmc->cid[0] >> 24);
			char name[7] = {0};
			unsigned long mib;

			name[0] = mmc->cid[0] & 0xff;
			name[1] = (mmc->cid[1] >> 24) & 0xff;
			name[2] = (mmc->cid[1] >> 16) & 0xff;
			name[3] = (mmc->cid[1] >> 8)  & 0xff;
			name[4] =  mmc->cid[1]        & 0xff;
			name[5] = (mmc->cid[2] >> 24) & 0xff;
			mib = (unsigned long)(mmc->capacity_user / (1024 * 1024));

			if (mfg)
				snprintf(buf, sizeof(buf),
					 "eMMC     : %s %s, %lu MiB",
					 mfg, name, mib);
			else
				snprintf(buf, sizeof(buf),
					 "eMMC     : %s (MID 0x%02x), %lu MiB",
					 name, mmc->cid[0] >> 24, mib);
			vc_at(row++, 0);
			vc_puts(buf);
		}
	}

	/* eFuse-derived serials (set at misc_init_r via the secure-monitor
	 * SMC). serial# is the per-device usid Spotify burns at factory
	 * (same as adb get-serialno); f_serial is the date-coded factory
	 * trace serial in the eFuse user area after the usid. */
	{
		const char *s = env_get("serial#");
		if (s) {
			snprintf(buf, sizeof(buf), "Serial   : %s", s);
			vc_at(row++, 0);
			vc_puts(buf);
		}
		s = env_get("f_serial");
		if (s) {
			snprintf(buf, sizeof(buf), "F-serial : %s", s);
			vc_at(row++, 0);
			vc_puts(buf);
		}
	}
}

static void draw_hwinfo_page1(void)
{
	struct carthing_charger_info ch;
	char buf[64];
	int row = 2;
	bool charger_ok;

	vc_at(row++, 0);
	vc_puts("I2C@bus2 (EE):");
	charger_ok = (carthing_charger_read(&ch) == 0 && ch.valid);
	if (charger_ok)
		snprintf(buf, sizeof(buf),
			 "  0x35 chrg MAX14656 rev %x",
			 ch.regs[0] & 0xf);
	else
		snprintf(buf, sizeof(buf), "  0x35 chrg MAX14656 (no resp)");
	vc_at(row++, 0);
	vc_puts(buf);
	vc_at(row++, 0);
	vc_puts("  0x39 prox/ALS TMD2772");

	vc_at(row++, 0);
	vc_puts("I2C@bus0 : 0x2e TLSC6X touch");
	vc_at(row++, 0);
	vc_puts("I2C@bus3 : 0x10 Apple MFi (idle)");

	row++;
	vc_at(row++, 0);
	vc_puts("Undriven : BT (BCM, SDIO+UART_AO)");
	vc_at(row++, 0);
	vc_puts("           PDM mics (4-mic array)");
}

/* Derive a 32-byte challenge from a counter by SHA-256 hashing. Each
 * counter value gives a distinct, deterministic challenge — handy
 * for live demos where the user can rotate-press to "sign a fresh
 * challenge" and see the resulting 64-byte ECDSA signature. */
static void challenge_from_counter(uint32_t counter, uint8_t out[MFI_CHALLENGE_SIZE])
{
	uint8_t seed[4] = {
		counter >> 24, counter >> 16, counter >> 8, counter
	};
	sha256_csum_wd(seed, sizeof(seed), out, CHUNKSZ_SHA256);
}

static int refresh_chr_cache(void)
{
	uint32_t t0;
	int ret;

	hwinfo_chr_cache.valid = false;
	challenge_from_counter(hwinfo_chr_cache.counter, hwinfo_chr_cache.challenge);

	t0 = get_timer(0);
	ret = carthing_mfi_challenge_response(hwinfo_chr_cache.challenge,
					      hwinfo_chr_cache.response);
	hwinfo_chr_cache.elapsed_ms = get_timer(t0);
	if (ret < 0)
		return ret;
	hwinfo_chr_cache.valid = true;
	hwinfo_chr_cache.counter++;
	return 0;
}

static void hex_byte(char *out, uint8_t b)
{
	static const char *hex = "0123456789abcdef";

	out[0] = hex[b >> 4];
	out[1] = hex[b & 0xf];
}

/* Format an ASN.1 time string ("YYMMDDHHMMSSZ" or "YYYYMMDDHHMMSSZ")
 * into "YYYY-MM-DD". Returns pointer into the static buffer or "?". */
static const char *fmt_time(const char *t, char buf[16])
{
	int yy, mm, dd, year;
	size_t len = strlen(t);

	if (len < 12)
		return "?";
	if (len == 13 || len == 12) {
		yy = (t[0] - '0') * 10 + (t[1] - '0');
		mm = (t[2] - '0') * 10 + (t[3] - '0');
		dd = (t[4] - '0') * 10 + (t[5] - '0');
		year = (yy < 50) ? 2000 + yy : 1900 + yy;
	} else if (len >= 14) {
		year = (t[0] - '0') * 1000 + (t[1] - '0') * 100 +
		       (t[2] - '0') * 10 + (t[3] - '0');
		mm = (t[4] - '0') * 10 + (t[5] - '0');
		dd = (t[6] - '0') * 10 + (t[7] - '0');
	} else {
		return "?";
	}
	snprintf(buf, 16, "%04d-%02d-%02d", year, mm, dd);
	return buf;
}

/* Truncate the parsed Apple-CA issuer name to fit in our column
 * budget. Most carthing certs come back as "Apple Accessories
 * Certification Authority - 00000002" (52 chars) which overflows.
 * Abbreviated form is human-recognisable and fits. */
static const char *abbrev_issuer(const char *full)
{
	static char buf[48];
	const char *suffix;

	if (!full || !full[0])
		return "(parse failed)";
	suffix = strstr(full, " - ");
	if (strstr(full, "Apple Accessories")) {
		if (suffix)
			snprintf(buf, sizeof(buf),
				 "Apple Accessories CA%s", suffix);
		else
			snprintf(buf, sizeof(buf),
				 "Apple Accessories CA");
		return buf;
	}
	return full;
}

static void draw_hwinfo_page2(void)
{
	char buf[64], tbuf1[16], tbuf2[16], serial[MFI_SERIAL_SIZE + 1];
	int row = 2;

	vc_at(row++, 0);
	vc_puts("MFi 3.0 / CP3.0 @ i2c3 0x10");

	if (!hwinfo_mfi_cache.valid) {
		vc_at(row++, 0);
		vc_puts("(read failed)");
		return;
	}

	snprintf(buf, sizeof(buf), "Ver: 0x%02x  Cert: %u B",
		 hwinfo_mfi_cache.version, hwinfo_mfi_cache.cert_len);
	vc_at(row++, 0);
	vc_puts(buf);

	/* Chip serial register (32 ASCII chars = device UID hex). */
	memcpy(serial, hwinfo_mfi_cache.serial, MFI_SERIAL_SIZE);
	serial[MFI_SERIAL_SIZE] = '\0';
	vc_at(row++, 0);
	vc_puts("Serial:");
	vc_at(row++, 0);
	vc_puts("  ");
	vc_at(row - 1, 2);
	vc_puts(serial);

	vc_at(row++, 0);
	vc_puts("Subject:");
	snprintf(buf, sizeof(buf), "  %s",
		 hwinfo_mfi_cache.parsed.subject_cn[0]
			? hwinfo_mfi_cache.parsed.subject_cn
			: "(parse failed)");
	vc_at(row++, 0);
	vc_puts(buf);

	vc_at(row++, 0);
	vc_puts("Issuer:");
	snprintf(buf, sizeof(buf), "  %s",
		 abbrev_issuer(hwinfo_mfi_cache.parsed.issuer_cn));
	vc_at(row++, 0);
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "Valid: %s -> %s",
		 fmt_time(hwinfo_mfi_cache.parsed.not_before, tbuf1),
		 fmt_time(hwinfo_mfi_cache.parsed.not_after, tbuf2));
	vc_at(row++, 0);
	vc_puts(buf);
}

static int max_cert_scroll(void)
{
	int total_lines;

	if (!hwinfo_mfi_cache.valid)
		return 0;
	total_lines = (hwinfo_mfi_cache.cert_len + CERT_HEX_BYTES_PER_ROW - 1)
		      / CERT_HEX_BYTES_PER_ROW;
	if (total_lines <= CERT_HEX_VISIBLE_ROWS)
		return 0;
	return total_lines - CERT_HEX_VISIBLE_ROWS;
}

/*
 * Cert hex modal — its own full screen, NOT one of the rotate-pages.
 * Layout:
 *   row 0: "MFi Cert hex     [0xNNN..0xNNN/0xNNN]"
 *   row 1: separator
 *   rows 2-12: 11 rows of 16 packed hex bytes each
 *   row 14: hints (rotate=scroll, press=refresh, back=back to MFi)
 */
static void draw_cert_hex(void)
{
	char buf[64];
	char *p;
	const char *sep = "--------------------------------------------------";
	int start_byte, end_byte, max, i, j;

	if (!vc || !hwinfo_mfi_cache.valid)
		return;

	vidconsole_clear_and_reset(vc);

	start_byte = cert_scroll_lines * CERT_HEX_BYTES_PER_ROW;
	end_byte = start_byte + CERT_HEX_VISIBLE_ROWS * CERT_HEX_BYTES_PER_ROW;
	if (end_byte > hwinfo_mfi_cache.cert_len)
		end_byte = hwinfo_mfi_cache.cert_len;
	max = hwinfo_mfi_cache.cert_len;

	snprintf(buf, sizeof(buf),
		 "MFi Cert hex      0x%03x-0x%03x/0x%03x",
		 start_byte, end_byte, max);
	vc_at(0, 0);
	vc_puts(buf);
	vc_at(1, 0);
	vc_puts(sep);

	for (i = 0; i < CERT_HEX_VISIBLE_ROWS; i++) {
		int off = start_byte + i * CERT_HEX_BYTES_PER_ROW;

		if (off >= max)
			break;
		p = buf;
		p += snprintf(p, sizeof(buf), "%04x ", off);
		for (j = 0; j < CERT_HEX_BYTES_PER_ROW; j++) {
			if (off + j >= max)
				break;
			hex_byte(p, hwinfo_mfi_cache.cert[off + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(2 + i, 0);
		vc_puts(buf);
	}

	{
		const char *exit_hint = "Back: MFi";
		const char *knob_hint = "Rotate: scroll  Press: refresh";
		char line[VC_COLS + 1];
		int gap = (VC_COLS - 1) - (int)strlen(knob_hint) -
			  (int)strlen(exit_hint);

		if (gap < 2)
			gap = 2;
		snprintf(line, sizeof(line), "%s%*s%s",
			 knob_hint, gap, "", exit_hint);
		vc_at(VC_ROWS - 1, 0);
		vc_puts(line);
	}

	video_sync(dev_get_parent(vc), false);
}

static int refresh_qr_cache(void)
{
	hwinfo_qr_cache.valid = false;
	hwinfo_qr_cache.json_len = carthing_hwinfo_build_json(
		hwinfo_qr_cache.json, sizeof(hwinfo_qr_cache.json));
	if (!hwinfo_qr_cache.json_len)
		return -1;
	if (!qrcodegen_encodeText(hwinfo_qr_cache.json,
				  hwinfo_qr_cache.tmp,
				  hwinfo_qr_cache.qr,
				  qrcodegen_Ecc_MEDIUM,
				  qrcodegen_VERSION_MIN,
				  15,
				  qrcodegen_Mask_AUTO,
				  true))
		return -1;
	hwinfo_qr_cache.qr_size = qrcodegen_getSize(hwinfo_qr_cache.qr);
	hwinfo_qr_cache.valid = true;
	return 0;
}

/* Direct-FB QR renderer. Bypasses vidconsole and pokes RGB565 pixels
 * straight into the panel's framebuffer. Each QR module is rendered
 * as an N×N pixel block where N is chosen to fit within the panel's
 * shorter dimension with a quiet-zone (4 modules) on each side.
 *
 * Renders a 1-bit white-on-black QR. Includes a printed text caption
 * below the code via vidconsole.
 */
static void draw_hwinfo_page5(void)
{
	struct video_priv *vid_priv;
	struct udevice *vdev;
	int total, px_per_mod, side_px, ox, oy;
	int x, y, ymod, xmod;
	uint32_t *fb;
	uint32_t white, black;

	if (!hwinfo_qr_cache.valid) {
		vc_at(2, 0);
		vc_puts("(QR build failed)");
		return;
	}

	if (uclass_first_device_err(UCLASS_VIDEO, &vdev))
		return;
	vid_priv = dev_get_uclass_priv(vdev);
	if (!vid_priv || !vid_priv->fb)
		return;

	/* Side length in modules including quiet zone. */
	total = hwinfo_qr_cache.qr_size + 8;

	/* Native FB orientation is 480 × 800 (portrait). The carthing
	 * runs landscape via console rotation but the FB itself is
	 * portrait, so we use the SHORTER native dimension (480) as
	 * our QR side budget. Each module gets floor(480 / total) px. */
	{
		int short_side = vid_priv->xsize < vid_priv->ysize
				? vid_priv->xsize : vid_priv->ysize;
		px_per_mod = short_side / total;
		if (px_per_mod < 1)
			px_per_mod = 1;
	}
	side_px = total * px_per_mod;

	/* Center within the FB. */
	ox = (vid_priv->xsize - side_px) / 2;
	oy = (vid_priv->ysize - side_px) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;

	/* Fill the QR region (background = white, modules = black).
	 * The carthing's FB is 32 BPP (pitch 1920 / xsize 480 = 4 B/px,
	 * ARGB8888-ish). Alpha = 0xff so the pixel is fully opaque. */
	fb = (uint32_t *)vid_priv->fb;
	white = 0xffffffff;
	black = 0xff000000;
	{
		int pixel_pitch = vid_priv->line_length / 4;	/* 4 B/px */

		for (y = 0; y < side_px; y++) {
			uint32_t *row = fb + (oy + y) * pixel_pitch + ox;

			for (x = 0; x < side_px; x++)
				row[x] = white;
		}

		for (ymod = 0; ymod < hwinfo_qr_cache.qr_size; ymod++) {
			for (xmod = 0; xmod < hwinfo_qr_cache.qr_size; xmod++) {
				if (!qrcodegen_getModule(hwinfo_qr_cache.qr,
							 xmod, ymod))
					continue;
				int px_x = ox + (xmod + 4) * px_per_mod;
				int px_y = oy + (ymod + 4) * px_per_mod;
				int j;

				for (j = 0; j < px_per_mod; j++) {
					uint32_t *row = fb +
						(px_y + j) * pixel_pitch
						+ px_x;
					int k;

					for (k = 0; k < px_per_mod; k++)
						row[k] = black;
				}
			}
		}
	}

	/* Mark damage region so video_sync flushes properly. */
	if (vid_priv->damage.xend == 0)
		vid_priv->damage.xend = vid_priv->xsize;
	if (vid_priv->damage.yend == 0)
		vid_priv->damage.yend = vid_priv->ysize;

}

static int refresh_adc_cache(void)
{
	struct udevice *adcdev;
	int i;

	hwinfo_adc.valid = false;
	if (uclass_get_device_by_name(UCLASS_ADC, "adc@9000", &adcdev))
		return -1;
	for (i = 0; i < 8; i++) {
		if (adc_channel_single_shot("adc@9000", i, &hwinfo_adc.raw[i]))
			return -1;
	}
	hwinfo_adc.valid = true;
	return 0;
}

/* Vref ~ 1.8 V, 12-bit. Convert raw → millivolts. */
static int adc_raw_to_mv(uint raw)
{
	return (int)((raw * 1800) / 4095);
}

/*
 * NTC thermistor lookup table from the vendor kernel DT (rt1/rt2/rt3
 * `temperature-lookup-table`). Mapping is vendor 10-bit ADC raw -> °C.
 * Higher raw == hotter (NTC wired as upper divider). 26 entries from
 * -40 to +85 °C in 5 °C steps. We linearly interpolate between
 * adjacent points.
 */
static const struct { int raw; int c; } ntc_lut[] = {
	{ 894,  85 }, { 878,  80 }, { 860,  75 }, { 839,  70 },
	{ 815,  65 }, { 789,  60 }, { 759,  55 }, { 726,  50 },
	{ 690,  45 }, { 650,  40 }, { 607,  35 }, { 560,  30 },
	{ 512,  25 }, { 462,  20 }, { 411,  15 }, { 360,  10 },
	{ 311,   5 }, { 265,   0 }, { 221,  -5 }, { 182, -10 },
	{ 147, -15 }, { 117, -20 }, {  92, -25 }, {  70, -30 },
	{  53, -35 }, {  40, -40 },
};

/* Decode a 12-bit SARADC raw value to °C. Returns 0 and the temp via
 * `*c_out` on success, -1 if out of range. */
static int ntc_raw_to_c(uint raw12, int *c_out)
{
	int raw10 = (int)(raw12 >> 2);
	int n = (int)(sizeof(ntc_lut) / sizeof(ntc_lut[0]));
	int i;

	if (raw10 >= ntc_lut[0].raw) {
		*c_out = ntc_lut[0].c;
		return 0;
	}
	if (raw10 <= ntc_lut[n - 1].raw) {
		*c_out = ntc_lut[n - 1].c;
		return 0;
	}
	for (i = 0; i < n - 1; i++) {
		int a = ntc_lut[i].raw, b = ntc_lut[i + 1].raw;

		if (raw10 <= a && raw10 >= b) {
			int span = a - b;	/* positive */
			int frac = a - raw10;	/* 0..span */
			int c_a = ntc_lut[i].c;
			int c_b = ntc_lut[i + 1].c;

			*c_out = c_a + ((c_b - c_a) * frac) / span;
			return 0;
		}
	}
	return -1;
}

/* Per-channel meaning, from vendor kernel DT (superbird_evt_512.dts).
 * Channels 4-7 aren't referenced — they float at ESD-clamp / leakage
 * voltage. */
struct adc_chan_meta {
	const char *label;	/* short label that fits on the row */
	bool is_ntc;		/* if true, treat raw as NTC thermistor */
};

static const struct adc_chan_meta adc_channels[8] = {
	[0] = { "PCB   ", true  },	/* rt3 = pcb_thermal */
	[1] = { "rev   ", false },	/* board-revision resistor divider */
	[2] = { "BT    ", true  },	/* rt1 = bluetooth_thermal */
	[3] = { "DRAM  ", true  },	/* rt2 = dram_thermal */
	[4] = { "unused", false },
	[5] = { "unused", false },
	[6] = { "unused", false },
	[7] = { "unused", false },
};

static void draw_hwinfo_page4(void)
{
	char buf[64];
	int row = 2, i;

	vc_at(row++, 0);
	vc_puts("SARADC channels (Vref 1.8 V, 12-bit)");

	if (!hwinfo_adc.valid) {
		vc_at(row++, 0);
		vc_puts("(read failed)");
		return;
	}

	/* Only show channels with a known purpose (0..3). ch4-7 aren't
	 * referenced in the vendor DT — they float at ESD-clamp /
	 * leakage voltage and just clutter the page. */
	for (i = 0; i < 4; i++) {
		const struct adc_chan_meta *m = &adc_channels[i];
		char note[16] = "";
		int mv = adc_raw_to_mv(hwinfo_adc.raw[i]);

		if (i == 1) {
			int rev = carthing_probe_board_rev();

			if (rev > 0)
				snprintf(note, sizeof(note), " REV %d", rev);
			else
				snprintf(note, sizeof(note), " REV ?");
		} else if (m->is_ntc) {
			int c;

			if (ntc_raw_to_c(hwinfo_adc.raw[i], &c) == 0)
				snprintf(note, sizeof(note), " ~%d C", c);
		}
		snprintf(buf, sizeof(buf), "ch%d %s: %4u raw %4d mV%s",
			 i, m->label, hwinfo_adc.raw[i], mv, note);
		vc_at(row++, 0);
		vc_puts(buf);
	}
}

/* Page 3: MFi challenge / response demo. Signs a deterministic 32-byte
 * challenge derived from a counter, shows the full 64-byte ECDSA P-256
 * signature + signing wall-clock time. */
static void draw_hwinfo_page3(void)
{
	char buf[64];
	int row = 2, i;

	vc_at(row++, 0);
	vc_puts("MFi Challenge / Response");
	row++;

	if (!hwinfo_chr_cache.valid) {
		vc_at(row++, 0);
		vc_puts("(signing failed or not run)");
		return;
	}

	vc_at(row++, 0);
	vc_puts("Challenge (32B):");
	for (i = 0; i < 2; i++) {
		int j;
		char *p = buf;

		*p++ = ' ';
		*p++ = ' ';
		for (j = 0; j < 16; j++) {
			hex_byte(p, hwinfo_chr_cache.challenge[i * 16 + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(row++, 0);
		vc_puts(buf);
	}

	vc_at(row++, 0);
	vc_puts("Signature (64B, ECDSA P-256):");
	for (i = 0; i < 4; i++) {
		int j;
		char *p = buf;

		*p++ = ' ';
		*p++ = ' ';
		for (j = 0; j < 16; j++) {
			hex_byte(p, hwinfo_chr_cache.response[i * 16 + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(row++, 0);
		vc_puts(buf);
	}

	snprintf(buf, sizeof(buf), "Signed in %u ms",
		 hwinfo_chr_cache.elapsed_ms);
	vc_at(row++, 0);
	vc_puts(buf);
}

static void draw_hwinfo(void)
{
	const char *exit_hint = "Back: exit";
	const char *knob_hint;

	if (!vc)
		return;

	vidconsole_clear_and_reset(vc);
	draw_hwinfo_chrome(hwinfo_page);

	switch (hwinfo_page) {
	case 0: draw_hwinfo_page0(); break;
	case 1: draw_hwinfo_page1(); break;
	case 2: draw_hwinfo_page2(); break;
	case 3: draw_hwinfo_page3(); break;
	case 4: draw_hwinfo_page4(); break;
	case 5: draw_hwinfo_page5(); break;
	}

	switch (hwinfo_page) {
	case 2: knob_hint = "Rotate: page   Press: full cert"; break;
	case 3: knob_hint = "Rotate: page   Press: re-sign";   break;
	default: knob_hint = "Rotate: page   Press: refresh";  break;
	}

	/* Build the bottom hint as a SINGLE string with the knob hint
	 * on the left and the exit hint right-justified. Cap total
	 * length at VC_COLS-1 so we never write a char into the very
	 * last column — vidconsole appears to implicitly wrap after
	 * column VC_COLS, which on the bottom row would scroll the
	 * whole screen up by one line. */
	{
		char line[VC_COLS + 1];
		int gap = (VC_COLS - 1) - (int)strlen(knob_hint) -
			  (int)strlen(exit_hint);

		if (gap < 2)
			gap = 2;
		snprintf(line, sizeof(line), "%s%*s%s",
			 knob_hint, gap, "", exit_hint);
		vc_at(VC_ROWS - 1, 0);
		vc_puts(line);
	}

	video_sync(dev_get_parent(vc), false);
}
static int do_bootmenu(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct udevice *vdev;
	struct btn_edge up, dn, sel_btn, back;
	int sel = 0;
	int ret;

	/* If misc_init_r's eager UCLASS_VIDEO probe was skipped (env
	 * `quick_boot=1`), the panel hasn't been brought up yet. Probe
	 * UCLASS_VIDEO directly here — this is where we'd typically pay
	 * the ~700 ms ST7701S init cost, but bootmenu can't draw without
	 * a working panel anyway. (If misc_init_r already ran the probe,
	 * this is a cheap idempotent re-probe.) */
	(void)uclass_first_device_err(UCLASS_VIDEO, &vdev);

	/* Look up the vidconsole device so we can call position_cursor.
	 * If not found, fall back to plain printf without positioning. */
	if (uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &vc))
		vc = NULL;

	/* Sync the cycle index to whatever brightness env is in. */
	brightness_idx = brightness_idx_from_env();

	if (btn_init(&up,      "preset1") ||
	    btn_init(&dn,      "preset4") ||
	    btn_init(&sel_btn, "select")  ||
	    btn_init(&back,    "back"))
		return CMD_RET_FAILURE;

	/* Drain any held buttons. */
	(void)btn_edge(&up);
	(void)btn_edge(&dn);
	(void)btn_edge(&sel_btn);
	(void)btn_edge(&back);

	/* If the SoC reports the user tried mask-ROM but ended up here on
	 * the eMMC fallback (set by spotify-carthing.c's detect_maskrom_
	 * failed at misc_init_r), show the dedicated help screen first.
	 * Wait for any button press to dismiss and fall through to the
	 * normal menu. */
	if (env_get_yesno("maskrom_failed") == 1) {
		draw_maskrom_failed_screen();
		while (1) {
			mdelay(5);
			if (btn_edge(&up) || btn_edge(&dn) ||
			    btn_edge(&sel_btn) || btn_edge(&back) || ctrlc())
				break;
		}
	}

	draw(sel);

	while (1) {
		int dt;

		/* 5ms = 200 Hz polling. Fast enough to never miss a real
		 * encoder transition at human-spin rates, and tight enough
		 * that button-press latency stays well under "snappy". */
		mdelay(5);

		dt = wheel_poll_detents();
		if (dt != 0) {
			/* CW (positive detent) moves selection down. The +
			 * NITEMS keeps modulo positive when wrapping past 0
			 * upward; sized 100x for safety against bigger dt. */
			sel = ((sel + dt) % NITEMS + NITEMS) % NITEMS;
			draw(sel);
			continue;
		}

		if (btn_edge(&up)) {
			sel = (sel - 1 + NITEMS) % NITEMS;
			draw(sel);
		} else if (btn_edge(&dn)) {
			sel = (sel + 1) % NITEMS;
			draw(sel);
		} else if (btn_edge(&sel_btn)) {
			if (items[sel].kind == K_SETTINGS) {
				/* Enter settings sub-menu (brightness, display-init).
				 * Sub-menu owns its own input loop and exits
				 * on back; redraw the main menu when it
				 * returns. */
				run_settings_menu(&up, &dn, &sel_btn, &back);
				draw(sel);
				(void)btn_edge(&up);
				(void)btn_edge(&dn);
				(void)btn_edge(&sel_btn);
				continue;
			}
			if (items[sel].kind == K_CHARGER) {
				/* Paint info screen. Knob-press triggers a
				 * manual redetect (writes CHG_TYP_MAN, waits
				 * ~250 ms, repaints). Back exits.
				 */
				draw_charger_info();
				(void)btn_edge(&back);
				(void)btn_edge(&sel_btn);
				while (1) {
					mdelay(5);
					if (btn_edge(&back) || ctrlc())
						break;
					if (btn_edge(&sel_btn)) {
						(void)carthing_charger_redetect();
						mdelay(CARTHING_CHARGER_REDETECT_DELAY_MS);
						draw_charger_info();
					}
				}
				draw(sel);
				(void)btn_edge(&up);
				(void)btn_edge(&dn);
				(void)btn_edge(&sel_btn);
				continue;
			}
			if (items[sel].kind == K_HWINFO) {
				/* Six-page inventory:
				 *  0  overview (board / SoC / charger / temp)
				 *  1  i2c bus + undriven peripherals
				 *  2  MFi cert info (parsed issuer/subject/
				 *     validity)  — knob press here opens the
				 *     cert-hex modal
				 *  3  MFi challenge/response (live ECDSA sign)
				 *  4  SARADC channels with mV conversions
				 *  5  QR code of the hwinfo JSON payload
				 *
				 * Rotate knob cycles pages, knob press
				 * refreshes the current page (or enters
				 * the cert-hex modal from page 2). Back
				 * exits. */
				bool in_cert_modal = false;

				(void)refresh_mfi_cache();
				(void)refresh_adc_cache();
				(void)carthing_panel_probe(&hwinfo_panel_cache);
				draw_hwinfo();
				(void)btn_edge(&back);
				(void)btn_edge(&sel_btn);

				while (1) {
					int dt;

					mdelay(5);

					if (btn_edge(&back) || ctrlc()) {
						if (in_cert_modal) {
							in_cert_modal = false;
							draw_hwinfo();
							continue;
						}
						break;
					}

					dt = wheel_poll_detents();
					if (dt != 0) {
						if (in_cert_modal) {
							int max = max_cert_scroll();
							cert_scroll_lines += dt;
							if (cert_scroll_lines < 0)
								cert_scroll_lines = 0;
							if (cert_scroll_lines > max)
								cert_scroll_lines = max;
							draw_cert_hex();
						} else {
							hwinfo_page =
							  ((hwinfo_page + dt) % HWINFO_NPAGES
							   + HWINFO_NPAGES) % HWINFO_NPAGES;
							if (hwinfo_page == 3 &&
							    !hwinfo_chr_cache.valid)
								(void)refresh_chr_cache();
							if (hwinfo_page == 5 &&
							    !hwinfo_qr_cache.valid)
								(void)refresh_qr_cache();
							draw_hwinfo();
						}
					}

					if (btn_edge(&sel_btn)) {
						if (in_cert_modal) {
							(void)refresh_mfi_cache();
							draw_cert_hex();
						} else if (hwinfo_page == 2) {
							/* Enter cert hex modal. */
							in_cert_modal = true;
							cert_scroll_lines = 0;
							draw_cert_hex();
						} else if (hwinfo_page == 3) {
							(void)refresh_chr_cache();
							draw_hwinfo();
						} else if (hwinfo_page == 4) {
							(void)refresh_adc_cache();
							draw_hwinfo();
						} else if (hwinfo_page == 5) {
							(void)refresh_qr_cache();
							draw_hwinfo();
						} else {
							draw_hwinfo();
						}
					}
				}
				draw(sel);
				(void)btn_edge(&up);
				(void)btn_edge(&dn);
				(void)btn_edge(&sel_btn);
				continue;
			}
			if (items[sel].panel_title)
				draw_mode(items[sel].panel_title);
			else if (vc)
				vidconsole_clear_and_reset(vc);
			printf("\nRunning: %s\n", items[sel].action_cmd);
			ret = run_command(items[sel].action_cmd, 0);
			if (ret)
				printf("\n(command returned %d)\n", ret);
			if (items[sel].reset_after) {
				printf("\nResetting in 1s "
				       "(USB gadget cleanup)...\n");
				mdelay(1000);
				run_command("reset", 0);
				/* not reached */
			}
			draw(sel);
			(void)btn_edge(&up);
			(void)btn_edge(&dn);
			(void)btn_edge(&sel_btn);
			(void)btn_edge(&back);
		} else if (btn_edge(&back)) {
			if (vc)
				vidconsole_clear_and_reset(vc);
			return 0;
		}

		if (ctrlc()) {
			if (vc)
				vidconsole_clear_and_reset(vc);
			return 0;
		}
	}
}

U_BOOT_CMD(
	bootmenu, 1, 1, do_bootmenu,
	"Car Thing on-panel boot menu",
	"\n"
	"  Renders a scrollable menu on the panel. Navigate with\n"
	"  preset1 (up) / preset4 (down), select with the wheel press,\n"
	"  cancel with back."
);

/*
 * Standalone "auto-fastboot" entry: draws the same centred FASTBOOT
 * splash + "Press to exit ->" hint that the bootmenu's Fastboot item
 * paints, then runs `fastboot 0`, then resets — mirroring the K_CMD
 * dispatch path (panel_title -> run_command -> reset_after) without
 * the menu wrapper.
 *
 * Used by spotify-carthing.c's set_boot_source() when the SoC reports
 * BOOT_DEVICE_USB (= we were RAM-loaded via superbird-tool / mask-ROM
 * USB), so a dev-iteration chainload drops straight into fastboot with
 * a clear on-panel indication of what mode the device is in.
 */
static int do_fastboot_with_screen(struct cmd_tbl *cmdtp, int flag,
				   int argc, char *const argv[])
{
	struct udevice *vdev;
	int ret;

	/* Same panel bring-up the bootmenu does — handles the case where
	 * misc_init_r's eager video probe was skipped (quick_boot=1). */
	(void)uclass_first_device_err(UCLASS_VIDEO, &vdev);
	if (uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &vc))
		vc = NULL;

	draw_mode("FASTBOOT");

	printf("\nRunning: fastboot 0\n");
	ret = run_command("fastboot 0", 0);
	if (ret)
		printf("\n(fastboot returned %d)\n", ret);

	printf("\nResetting in 1s (USB gadget cleanup)...\n");
	mdelay(1000);
	run_command("reset", 0);
	return 0; /* not reached */
}

U_BOOT_CMD(
	fastboot_with_screen, 1, 1, do_fastboot_with_screen,
	"Enter fastboot with the on-panel FASTBOOT splash",
	"\n"
	"  Paints the same centred FASTBOOT title the bootmenu uses,\n"
	"  runs `fastboot 0`, then resets the device on exit."
);
