// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2016 The Android Open Source Project
 */

#include <blk.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fastboot.h>
#include <fastboot-internal.h>
#include <fb_block.h>
#include <fb_mmc.h>
#include <fb_nand.h>
#include <fb_spi_flash.h>
#include <part.h>
#include <stdlib.h>
#include <vsprintf.h>
#include <linux/printk.h>
#include <dm/uclass-id.h>

/**
 * image_size - final fastboot image size
 */
static u32 image_size;

/**
 * fastboot_bytes_received - number of bytes received in the current download
 */
static u32 fastboot_bytes_received;

/**
 * fastboot_bytes_expected - number of bytes expected in the current download
 */
static u32 fastboot_bytes_expected;

static void okay(char *, char *);
static void getvar(char *, char *);
static void download(char *, char *);
static void flash(char *, char *);
static void erase(char *, char *);
static void reboot_bootloader(char *, char *);
static void reboot_fastbootd(char *, char *);
static void reboot_recovery(char *, char *);
static void oem_format(char *, char *);
static void oem_partconf(char *, char *);
static void oem_bootbus(char *, char *);
static void oem_console(char *, char *);
static void oem_maskrom(char *, char *);
static void oem_board(char *, char *);
static void oem_help(char *, char *);
static void fetch(char *, char *);
static void run_ucmd(char *, char *);
static void run_acmd(char *, char *);

static const struct {
	const char *command;
	void (*dispatch)(char *cmd_parameter, char *response);
} commands[FASTBOOT_COMMAND_COUNT] = {
	[FASTBOOT_COMMAND_GETVAR] = {
		.command = "getvar",
		.dispatch = getvar
	},
	[FASTBOOT_COMMAND_DOWNLOAD] = {
		.command = "download",
		.dispatch = download
	},
	[FASTBOOT_COMMAND_FLASH] =  {
		.command = "flash",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_FLASH, (flash), (NULL))
	},
	[FASTBOOT_COMMAND_ERASE] =  {
		.command = "erase",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_FLASH, (erase), (NULL))
	},
	[FASTBOOT_COMMAND_BOOT] =  {
		.command = "boot",
		.dispatch = okay
	},
	[FASTBOOT_COMMAND_CONTINUE] =  {
		.command = "continue",
		.dispatch = okay
	},
	[FASTBOOT_COMMAND_REBOOT] =  {
		.command = "reboot",
		.dispatch = okay
	},
	[FASTBOOT_COMMAND_REBOOT_BOOTLOADER] =  {
		.command = "reboot-bootloader",
		.dispatch = reboot_bootloader
	},
	[FASTBOOT_COMMAND_REBOOT_FASTBOOTD] =  {
		.command = "reboot-fastboot",
		.dispatch = reboot_fastbootd
	},
	[FASTBOOT_COMMAND_REBOOT_RECOVERY] =  {
		.command = "reboot-recovery",
		.dispatch = reboot_recovery
	},
	[FASTBOOT_COMMAND_SET_ACTIVE] =  {
		.command = "set_active",
		.dispatch = okay
	},
	[FASTBOOT_COMMAND_OEM_FORMAT] = {
		.command = "oem format",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_FORMAT, (oem_format), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_PARTCONF] = {
		.command = "oem partconf",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_PARTCONF, (oem_partconf), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_BOOTBUS] = {
		.command = "oem bootbus",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_BOOTBUS, (oem_bootbus), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_RUN] = {
		.command = "oem run",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_OEM_RUN, (run_ucmd), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_CONSOLE] = {
		.command = "oem console",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_CONSOLE, (oem_console), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_MASKROM] = {
		.command = "oem maskrom",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_MASKROM, (oem_maskrom), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_BOARD] = {
		.command = "oem board",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_OEM_BOARD, (oem_board), (NULL))
	},
	[FASTBOOT_COMMAND_OEM_HELP] = {
		.command = "oem help",
		.dispatch = oem_help
	},
	[FASTBOOT_COMMAND_FETCH] = {
		.command = "fetch",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_CMD_FETCH, (fetch), (NULL))
	},
	[FASTBOOT_COMMAND_UCMD] = {
		.command = "UCmd",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_UUU_SUPPORT, (run_ucmd), (NULL))
	},
	[FASTBOOT_COMMAND_ACMD] = {
		.command = "ACmd",
		.dispatch = CONFIG_IS_ENABLED(FASTBOOT_UUU_SUPPORT, (run_acmd), (NULL))
	},
};

/**
 * fastboot_handle_command - Handle fastboot command
 *
 * @cmd_string: Pointer to command string
 * @response: Pointer to fastboot response buffer
 *
 * Return: Executed command, or -1 if not recognized
 */
int fastboot_handle_command(char *cmd_string, char *response)
{
	int i;
	char *cmd_parameter;
	size_t cmd_len;

	/*
	 * Match each registered command against cmd_string. Three valid forms
	 * (per the android fastboot protocol):
	 *   - "<cmd>"           bare command, no params
	 *   - "<cmd>:<param>"   colon separator (used by getvar, download,
	 *                       flash, etc.)
	 *   - "<cmd> <param>"   space separator (used by `oem <subcmd> <args>`
	 *                       — the android client sends these with spaces,
	 *                       never colons).
	 * The previous implementation did `strsep(&cmd_parameter, ":")` up
	 * front, which NUL-terminated cmd_string at the *first* colon. That
	 * destroyed any embedded colon in space-form parameters — e.g.
	 * `oem console env set foo bar:baz` would arrive at the dispatcher
	 * as `env set foo bar`, silently dropping `:baz` and everything
	 * after. Detect the separator per-command instead so embedded colons
	 * in params survive.
	 */
	for (i = 0; i < FASTBOOT_COMMAND_COUNT; i++) {
		cmd_len = strlen(commands[i].command);

		if (strncmp(commands[i].command, cmd_string, cmd_len) != 0)
			continue;

		switch (cmd_string[cmd_len]) {
		case '\0':
			cmd_parameter = NULL;
			break;
		case ':':
		case ' ':
			cmd_parameter = cmd_string + cmd_len + 1;
			break;
		default:
			/* prefix match but neither separator — keep searching
			 * (e.g. "reboot" vs "reboot-bootloader"). */
			continue;
		}

		if (commands[i].dispatch) {
			commands[i].dispatch(cmd_parameter, response);
			return i;
		}
		pr_err("command %s not supported.\n", cmd_string);
		fastboot_fail("Unsupported command", response);
		return -1;
	}

	pr_err("command %s not recognized.\n", cmd_string);
	fastboot_fail("unrecognized command", response);
	return -1;
}

void fastboot_multiresponse(int cmd, char *response)
{
	switch (cmd) {
	case FASTBOOT_COMMAND_GETVAR:
		fastboot_getvar_all(response);
		break;
	case FASTBOOT_COMMAND_OEM_HELP:
		/* oem help fills the console buffer the same way an oem
		 * console invocation does, so reuse the OEM_CONSOLE drain. */
		fallthrough;
	case FASTBOOT_COMMAND_OEM_CONSOLE:
		if (CONFIG_IS_ENABLED(FASTBOOT_CMD_OEM_CONSOLE) ||
		    cmd == FASTBOOT_COMMAND_OEM_HELP) {
			char buf[FASTBOOT_RESPONSE_LEN] = { 0 };

			if (console_record_isempty()) {
				console_record_reset();
				fastboot_okay(NULL, response);
			} else {
				int ret = console_record_readline(buf, sizeof(buf) - 5);

				if (ret < 0)
					fastboot_fail("Error reading console", response);
				else
					fastboot_response("INFO", response, "%s", buf);
			}
			break;
		}
		fallthrough;
	default:
		fastboot_fail("Unknown multiresponse command", response);
		break;
	}
}

/**
 * okay() - Send bare OKAY response
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 *
 * Send a bare OKAY fastboot response. This is used where the command is
 * valid, but all the work is done after the response has been sent (e.g.
 * boot, reboot etc.)
 */
static void okay(char *cmd_parameter, char *response)
{
	fastboot_okay(NULL, response);
}

/**
 * getvar() - Read a config/version variable
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void getvar(char *cmd_parameter, char *response)
{
	fastboot_getvar(cmd_parameter, response);
}

/**
 * fastboot_download() - Start a download transfer from the client
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void download(char *cmd_parameter, char *response)
{
	char *tmp;

	if (!cmd_parameter) {
		fastboot_fail("Expected command parameter", response);
		return;
	}
	fastboot_bytes_received = 0;
	fastboot_bytes_expected = hextoul(cmd_parameter, &tmp);
	if (fastboot_bytes_expected == 0) {
		fastboot_fail("Expected nonzero image size", response);
		return;
	}
	/*
	 * Nothing to download yet. Response is of the form:
	 * [DATA|FAIL]$cmd_parameter
	 *
	 * where cmd_parameter is an 8 digit hexadecimal number
	 */
	if (fastboot_bytes_expected > fastboot_buf_size) {
		fastboot_fail(cmd_parameter, response);
	} else {
		printf("Starting download of %d bytes\n",
		       fastboot_bytes_expected);
		fastboot_response("DATA", response, "%s", cmd_parameter);
	}
}

/**
 * fastboot_data_remaining() - return bytes remaining in current transfer
 *
 * Return: Number of bytes left in the current download
 */
u32 fastboot_data_remaining(void)
{
	return fastboot_bytes_expected - fastboot_bytes_received;
}

/**
 * fastboot_data_download() - Copy image data to fastboot_buf_addr.
 *
 * @fastboot_data: Pointer to received fastboot data
 * @fastboot_data_len: Length of received fastboot data
 * @response: Pointer to fastboot response buffer
 *
 * Copies image data from fastboot_data to fastboot_buf_addr. Writes to
 * response. fastboot_bytes_received is updated to indicate the number
 * of bytes that have been transferred.
 *
 * On completion sets image_size and ${filesize} to the total size of the
 * downloaded image.
 */
void fastboot_data_download(const void *fastboot_data,
			    unsigned int fastboot_data_len,
			    char *response)
{
#define BYTES_PER_DOT	0x20000
	u32 pre_dot_num, now_dot_num;

	if (fastboot_data_len == 0 ||
	    (fastboot_bytes_received + fastboot_data_len) >
	    fastboot_bytes_expected) {
		fastboot_fail("Received invalid data length",
			      response);
		return;
	}
	/* Download data to fastboot_buf_addr */
	memcpy(fastboot_buf_addr + fastboot_bytes_received,
	       fastboot_data, fastboot_data_len);

	pre_dot_num = fastboot_bytes_received / BYTES_PER_DOT;
	fastboot_bytes_received += fastboot_data_len;
	now_dot_num = fastboot_bytes_received / BYTES_PER_DOT;

	if (pre_dot_num != now_dot_num) {
		putc('.');
		if (!(now_dot_num % 74))
			putc('\n');
	}
	*response = '\0';
}

/**
 * fastboot_data_complete() - Mark current transfer complete
 *
 * @response: Pointer to fastboot response buffer
 *
 * Set image_size and ${filesize} to the total size of the downloaded image.
 */
void fastboot_data_complete(char *response)
{
	/* Download complete. Respond with "OKAY" */
	fastboot_okay(NULL, response);
	printf("\ndownloading of %d bytes finished\n", fastboot_bytes_received);
	image_size = fastboot_bytes_received;
	env_set_hex("filesize", image_size);
	fastboot_bytes_expected = 0;
	fastboot_bytes_received = 0;
}

/**
 * flash() - write the downloaded image to the indicated partition.
 *
 * @cmd_parameter: Pointer to partition name
 * @response: Pointer to fastboot response buffer
 *
 * Writes the previously downloaded image to the partition indicated by
 * cmd_parameter. Writes to response.
 */
static void __maybe_unused flash(char *cmd_parameter, char *response)
{
	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_BLOCK))
		fastboot_block_flash_write(cmd_parameter, fastboot_buf_addr,
					   image_size, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_MMC))
		fastboot_mmc_flash_write(cmd_parameter, fastboot_buf_addr,
					 image_size, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_NAND))
		fastboot_nand_flash_write(cmd_parameter, fastboot_buf_addr,
					  image_size, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_SPI))
		fastboot_spi_flash_write(cmd_parameter, fastboot_buf_addr,
					 image_size, response);
}

/**
 * erase() - erase the indicated partition.
 *
 * @cmd_parameter: Pointer to partition name
 * @response: Pointer to fastboot response buffer
 *
 * Erases the partition indicated by cmd_parameter (clear to 0x00s). Writes
 * to response.
 */
static void __maybe_unused erase(char *cmd_parameter, char *response)
{
	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_BLOCK))
		fastboot_block_erase(cmd_parameter, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_MMC))
		fastboot_mmc_erase(cmd_parameter, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_NAND))
		fastboot_nand_erase(cmd_parameter, response);

	if (IS_ENABLED(CONFIG_FASTBOOT_FLASH_SPI))
		fastboot_spi_flash_erase(cmd_parameter, response);
}

/**
 * run_ucmd() - Execute the UCmd command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused run_ucmd(char *cmd_parameter, char *response)
{
	if (!cmd_parameter) {
		pr_err("missing slot suffix\n");
		fastboot_fail("missing command", response);
		return;
	}

	if (run_command(cmd_parameter, 0))
		fastboot_fail("", response);
	else
		fastboot_okay(NULL, response);
}

static char g_a_cmd_buff[64];

void fastboot_acmd_complete(void)
{
	run_command(g_a_cmd_buff, 0);
}

/**
 * run_acmd() - Execute the ACmd command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused run_acmd(char *cmd_parameter, char *response)
{
	if (!cmd_parameter) {
		pr_err("missing slot suffix\n");
		fastboot_fail("missing command", response);
		return;
	}

	if (strlen(cmd_parameter) >= sizeof(g_a_cmd_buff)) {
		pr_err("too long command\n");
		fastboot_fail("too long command", response);
		return;
	}

	strcpy(g_a_cmd_buff, cmd_parameter);
	fastboot_okay(NULL, response);
}

/**
 * reboot_bootloader() - Sets reboot bootloader flag.
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void reboot_bootloader(char *cmd_parameter, char *response)
{
	if (fastboot_set_reboot_flag(FASTBOOT_REBOOT_REASON_BOOTLOADER))
		fastboot_fail("Cannot set reboot flag", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * reboot_fastbootd() - Sets reboot fastboot flag.
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void reboot_fastbootd(char *cmd_parameter, char *response)
{
	if (fastboot_set_reboot_flag(FASTBOOT_REBOOT_REASON_FASTBOOTD))
		fastboot_fail("Cannot set fastboot flag", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * reboot_recovery() - Sets reboot recovery flag.
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void reboot_recovery(char *cmd_parameter, char *response)
{
	if (fastboot_set_reboot_flag(FASTBOOT_REBOOT_REASON_RECOVERY))
		fastboot_fail("Cannot set recovery flag", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * oem_format() - Execute the OEM format command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_format(char *cmd_parameter, char *response)
{
	char cmdbuf[32];
	const int mmc_dev = config_opt_enabled(CONFIG_FASTBOOT_FLASH_MMC,
					       CONFIG_FASTBOOT_FLASH_MMC_DEV, -1);

	if (!env_get("partitions")) {
		fastboot_fail("partitions not set", response);
	} else {
		sprintf(cmdbuf, "gpt write mmc %x $partitions", mmc_dev);
		if (run_command(cmdbuf, 0))
			fastboot_fail("", response);
		else
			fastboot_okay(NULL, response);
	}
}

/**
 * oem_partconf() - Execute the OEM partconf command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_partconf(char *cmd_parameter, char *response)
{
	char cmdbuf[32];
	const int mmc_dev = config_opt_enabled(CONFIG_FASTBOOT_FLASH_MMC,
					       CONFIG_FASTBOOT_FLASH_MMC_DEV, -1);

	if (!cmd_parameter) {
		fastboot_fail("Expected command parameter", response);
		return;
	}

	/* execute 'mmc partconfg' command with cmd_parameter arguments*/
	snprintf(cmdbuf, sizeof(cmdbuf), "mmc partconf %x %s 0", mmc_dev, cmd_parameter);
	printf("Execute: %s\n", cmdbuf);
	if (run_command(cmdbuf, 0))
		fastboot_fail("Cannot set oem partconf", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * oem_bootbus() - Execute the OEM bootbus command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_bootbus(char *cmd_parameter, char *response)
{
	char cmdbuf[32];
	const int mmc_dev = config_opt_enabled(CONFIG_FASTBOOT_FLASH_MMC,
					       CONFIG_FASTBOOT_FLASH_MMC_DEV, -1);

	if (!cmd_parameter) {
		fastboot_fail("Expected command parameter", response);
		return;
	}

	/* execute 'mmc bootbus' command with cmd_parameter arguments*/
	snprintf(cmdbuf, sizeof(cmdbuf), "mmc bootbus %x %s", mmc_dev, cmd_parameter);
	printf("Execute: %s\n", cmdbuf);
	if (run_command(cmdbuf, 0))
		fastboot_fail("Cannot set oem bootbus", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * oem_console() - Execute the OEM console command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_console(char *cmd_parameter, char *response)
{
	/* Two modes:
	 *  1. No parameter: drain whatever's accumulated in the console
	 *     ring buffer (preserves the original behaviour — capture
	 *     state from before `fastboot 0` was entered).
	 *  2. With a parameter: treat it as a u-boot CLI command. Reset
	 *     the ring buffer so the host gets a clean slate, then
	 *     run_command() — its output is captured by the console
	 *     framework and drained back to the host through the same
	 *     multi-response loop. Effective "run a u-boot command and
	 *     get its output back" over fastboot.
	 *
	 * Note this is *not* real-time streaming — the command runs to
	 * completion first, then the ring buffer is replayed. Commands
	 * that print more than CONFIG_CONSOLE_RECORD_OUT_SIZE before
	 * exiting will get their early output overwritten. For
	 * long-running stuff (`wheel watch`, etc) use `oem run` and tail
	 * the UART instead.
	 */
	if (cmd_parameter && *cmd_parameter) {
		console_record_reset();
		run_command(cmd_parameter, 0);
	}

	if (console_record_isempty())
		fastboot_okay(NULL, response);
	else
		fastboot_response(FASTBOOT_MULTIRESPONSE_START, response, NULL);
}

/**
 * oem_maskrom() - Reboot into mask-ROM USB download mode (1b8e:c003).
 *
 * Sets the MASKROM reboot reason (board's fastboot_set_reboot_flag override
 * stashes it in PREG_STICKY_REG3) and acks OKAY. The actual reset is deferred
 * to compl_do_reset() in f_fastboot.c — keyed on FASTBOOT_COMMAND_OEM_MASKROM
 * — so the host sees OKAY *before* the device drops off the bus, same as the
 * reboot-* commands. BL31 then reads the reason during the PSCI reset and asks
 * the SCP to enter USB download. Host: `fastboot oem maskrom`.
 *
 * @cmd_parameter: ignored
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_maskrom(char *cmd_parameter, char *response)
{
	if (fastboot_set_reboot_flag(FASTBOOT_REBOOT_REASON_MASKROM))
		fastboot_fail("Cannot set maskrom reboot flag", response);
	else
		fastboot_okay(NULL, response);
}

/**
 * oem_help() - List the fastboot oem subcommands that this build
 * actually has compiled in. Walks the dispatch table for entries
 * whose name starts with "oem " and whose dispatch is non-NULL,
 * pairs each with a one-line description, and pushes the lot
 * through the OEM_CONSOLE multi-response drain (so it shows up on
 * the host as a bunch of (bootloader) INFO lines, no extra protocol
 * machinery needed).
 *
 * @cmd_parameter: ignored
 * @response: fastboot response buffer
 */
static void oem_help(char *cmd_parameter, char *response)
{
	struct help_entry {
		const char *name;
		const char *blurb;
	};
	static const struct help_entry help[] = {
		{ "oem format",   "gpt write mmc 0 $partitions (lays GPT from env)" },
		{ "oem partconf", "set EXT_CSD PARTITION_CONFIG (boot-part select)" },
		{ "oem bootbus",  "set EXT_CSD BOOT_BUS_CONDITIONS" },
		{ "oem run",      "<arg> = run any u-boot CLI command" },
		{ "oem console",  "[arg] = drain console buffer; with arg, run it first" },
		{ "oem maskrom",  "reboot into mask-ROM USB download mode (1b8e:c003)" },
		{ "oem board",    "board-specific hook (override fastboot_oem_board)" },
		{ "oem help",     "this list" },
	};
	int i;

	console_record_reset();
	printf("Available fastboot oem commands (this build):\n");
	for (i = 0; i < ARRAY_SIZE(help); i++) {
		int j;
		bool compiled_in = false;

		for (j = 0; j < FASTBOOT_COMMAND_COUNT; j++) {
			if (commands[j].command &&
			    !strcmp(commands[j].command, help[i].name) &&
			    commands[j].dispatch) {
				compiled_in = true;
				break;
			}
		}
		if (compiled_in)
			printf("  %-14s %s\n", help[i].name, help[i].blurb);
	}

	if (console_record_isempty())
		fastboot_okay(NULL, response);
	else
		fastboot_response(FASTBOOT_MULTIRESPONSE_START, response, NULL);
}

/* ---- fetch (upload partition data to host) ---- */
static u32 fastboot_upload_total;
static u32 fastboot_upload_sent;

u32 fastboot_upload_remaining(void)
{
	return fastboot_upload_total - fastboot_upload_sent;
}

const void *fastboot_upload_get_chunk(unsigned int max_len, unsigned int *out_len)
{
	u32 remaining = fastboot_upload_remaining();

	*out_len = (max_len < remaining) ? max_len : remaining;
	return (const u8 *)fastboot_buf_addr + fastboot_upload_sent;
}

void fastboot_upload_consume(unsigned int len)
{
	fastboot_upload_sent += len;
	if (fastboot_upload_sent >= fastboot_upload_total) {
		printf("fastboot fetch: %u bytes uploaded\n",
		       fastboot_upload_total);
		fastboot_upload_total = 0;
		fastboot_upload_sent = 0;
	}
}

/**
 * fastboot_fetch_virtual() - let a board serve a fetch target that
 * isn't a partition.
 *
 * Called before the partition lookup. Copy up to @bufsz bytes into
 * @buf, set *@out_len, and return 0 to serve the request; return
 * -ENOENT to fall through to the normal partition path.
 *
 * Exists so a diagnostic image can hand a report back over the same
 * `fastboot fetch` the host already has, without inventing a transport
 * or writing anything to the eMMC it is diagnosing.
 */
__weak int fastboot_fetch_virtual(const char *name, void *buf, u32 bufsz,
				  u32 *out_len)
{
	return -ENOENT;
}

/**
 * fetch() - Implement the AOSP `fastboot fetch` command. Reads bytes
 * from a partition into the fastboot RAM buffer and replies DATA<size>;
 * the gadget glue then streams the buffer back to the host over the
 * bulk-IN endpoint, mirroring `download` in reverse.
 *
 * Syntax accepted (matches the host fastboot client):
 *   fetch:<partition>
 *   fetch:<partition>:<offset_hex>:<size_hex>
 *
 * Partition lookup tries the standard partition tables first, then the
 * `fastboot_raw_partition_<name>` env shorthand (same as flash).
 *
 * The fetched range must fit in CONFIG_FASTBOOT_BUF_SIZE — this is a
 * single-shot transfer, not a streamed read. For dumps larger than the
 * buffer, slice by repeating with explicit offset+size args.
 */
static void __maybe_unused fetch(char *cmd_parameter, char *response)
{
	struct disk_partition info;
	struct blk_desc *dev_desc;
	char part_name[64];
	char *off_str = NULL, *sz_str = NULL;
	u64 offset_b = 0, size_b = 0;
	u64 part_byte_size;
	lbaint_t start_lba, blk_count;
	int dev_index = CONFIG_FASTBOOT_FLASH_MMC_DEV;
	int ret;

	if (!cmd_parameter || !*cmd_parameter) {
		fastboot_fail("Expected partition name", response);
		return;
	}

	/* Copy + tokenize "part[:off[:size]]". */
	strlcpy(part_name, cmd_parameter, sizeof(part_name));

	off_str = strchr(part_name, ':');
	if (off_str) {
		*off_str++ = '\0';
		sz_str = strchr(off_str, ':');
		if (sz_str) {
			*sz_str++ = '\0';
			size_b = simple_strtoull(sz_str, NULL, 16);
		}
		offset_b = simple_strtoull(off_str, NULL, 16);
	}

	/*
	 * Board-served virtual targets take precedence over partitions.
	 * Has to be after tokenizing: the host sends
	 * "fetch:<name>:<off>:<size>", so matching before the split would
	 * compare the name against the whole parameter.
	 */
	{
		u32 vlen = 0;

		if (!fastboot_fetch_virtual(part_name, fastboot_buf_addr,
					    fastboot_buf_size, &vlen)) {
			if (offset_b >= vlen) {
				fastboot_fail("Offset past end of data",
					      response);
				return;
			}
			if (offset_b) {
				vlen -= (u32)offset_b;
				memmove(fastboot_buf_addr,
					(u8 *)fastboot_buf_addr + offset_b,
					vlen);
			}
			if (size_b && size_b < vlen)
				vlen = (u32)size_b;

			printf("fastboot fetch: virtual '%s' (%u bytes)\n",
			       part_name, vlen);
			fastboot_upload_total = vlen;
			fastboot_upload_sent = 0;
			fastboot_response("DATA", response, "%08x", vlen);
			return;
		}
	}

	dev_desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, dev_index);
	if (!dev_desc) {
		fastboot_fail("MMC device not found", response);
		return;
	}

	ret = part_get_info_by_name(dev_desc, part_name, &info);
	if (ret < 0) {
		/* No GPT/MBR entry — fall back to the raw_partition shorthand
		 * (same env used by `fastboot flash` for raw LBA ranges). */
		char env_key[80];
		char *raw, *tok, *parse;

		snprintf(env_key, sizeof(env_key),
			 "fastboot_raw_partition_%s", part_name);
		raw = env_get(env_key);
		if (!raw) {
			fastboot_fail("Unknown partition", response);
			return;
		}
		/* "<lba_start_hex> <num_sectors_hex>", space-separated. */
		raw = strdup(raw);
		if (!raw) {
			fastboot_fail("OOM", response);
			return;
		}
		parse = raw;
		info.blksz = dev_desc->blksz;
		tok = strsep(&parse, " ");
		if (!tok) {
			free(raw);
			fastboot_fail("Bad raw partition env", response);
			return;
		}
		info.start = simple_strtoull(tok, NULL, 16);
		tok = strsep(&parse, " ");
		if (!tok) {
			free(raw);
			fastboot_fail("Bad raw partition env", response);
			return;
		}
		info.size = simple_strtoull(tok, NULL, 16);
		free(raw);
	}

	part_byte_size = (u64)info.size * info.blksz;
	if (offset_b >= part_byte_size) {
		fastboot_fail("Offset past end of partition", response);
		return;
	}
	if (size_b == 0)
		size_b = part_byte_size - offset_b;
	if (offset_b + size_b > part_byte_size)
		size_b = part_byte_size - offset_b;
	if (size_b > fastboot_buf_size) {
		fastboot_fail("Range exceeds fastboot buffer", response);
		return;
	}
	if (offset_b % info.blksz) {
		fastboot_fail("Offset must be sector-aligned", response);
		return;
	}

	start_lba = info.start + (offset_b / info.blksz);
	blk_count = (size_b + info.blksz - 1) / info.blksz;
	printf("fastboot fetch: %s LBA %lu+%lu (%llu bytes)\n",
	       part_name, (ulong)start_lba, (ulong)blk_count, size_b);
	if (blk_dread(dev_desc, start_lba, blk_count,
		      fastboot_buf_addr) != blk_count) {
		fastboot_fail("Block read failed", response);
		return;
	}

	fastboot_upload_total = (u32)size_b;
	fastboot_upload_sent = 0;
	fastboot_response("DATA", response, "%08x", (u32)size_b);
}

/**
 * fastboot_oem_board() - Execute the OEM board command. This is default
 * weak implementation, which may be overwritten in board/ files.
 *
 * @cmd_parameter: Pointer to command parameter
 * @data: Pointer to fastboot input buffer
 * @size: Size of the fastboot input buffer
 * @response: Pointer to fastboot response buffer
 */
void __weak fastboot_oem_board(char *cmd_parameter, void *data, u32 size, char *response)
{
	fastboot_fail("oem board function not defined", response);
}

/**
 * oem_board() - Execute the OEM board command
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 */
static void __maybe_unused oem_board(char *cmd_parameter, char *response)
{
	fastboot_oem_board(cmd_parameter, (void *)fastboot_buf_addr, image_size, response);
}
