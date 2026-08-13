# Upstream u-boot for the Spotify Car Thing

This is a **fresh mainline u-boot clone** (`source.denx.de/u-boot/u-boot.git`,
partial-blob clone). Goal: build a custom u-boot that runs on the Car Thing
hardware, eventually paired with a custom mainline kernel + custom partitioning.
We're entirely off the Spotify boot chain.

If you need to look at the Spotify vendor u-boot 2015.01 source for hardware
details (panel init, DDR timings, GPIO map), it's on github at
`spsgsb/uboot` — this folder used to contain a fork of it but was wiped to
start clean. The notes I'd extracted from it before wiping are summarized
below.

## status

  - **Phase 1 — chain-load mechanism: DONE.** `../superbird-tool/` has
    a `--chainload BINFILE` command. Used during phase-1 bring-up; not
    the iteration primitive anymore (see `## Workflow loop` —
    `--burn_mode CUSTOM_FIP` is now the path).
  - **Phase 2 — chain-loaded upstream u-boot with display + backlight:
    DONE 2026-05-12.** ST7701S panel lights up + renders the u-boot
    splash + vidconsole output. See `superbird-docs/display/display-bringup.md`
    for the deep dive; 7 upstream u-boot bugs found+fixed along the way.
      - **Panel-quality pass (2026-06): DONE.** Replaced the ST7701S init
        table in `drivers/video/sitronix-st7701s-carthing.c` with the
        verified *shipping* table (RE'd from stock BL33, cross-checked vs 4
        sources) — fixes ghosting/retention (VCOM → 0.96 V) and horizontal
        lines (RTNI 0x07, GIP `9a a0`). Backlight PWM left at stock 30 kHz
        (a 30k→1k sweep proved min-brightness white is gamma-bound, not
        PWM-bound). Boot-splash smear killed in `spotify-carthing.c`: panel
        syncs onto a black FB (set `hide_logo` before the video probe) under
        a dim glow, then the logo is painted + the backlight ramps once it
        locks. Added the `blramp` cmd (`cmd_setbright.c`) for live ramp
        tuning. The B0/B1 gamma toe-lift A/B was tried and reverted (washed
        the darks, no shadow gain).
  - **Phase 3 — sign+flash our u-boot as BL33, boot at power-on with
    NO vendor BL33: DONE 2026-05-13.** Spotify open-sourced their FIP
    signing key (`aml-user-key.sig` in `spsgsb/uboot`). Combined with
    stock BL2 (unchanged first 64 KiB) + a vendor-style info_sector at
    LBA 0, this flashes to boot0/boot1 and boots in ~5 seconds at
    power-on, no Spotify components ever run. The entire user area
    (3.6 GiB) is free to repartition. See `## end-to-end install` below
    and the `carthing_secure_boot` / `carthing_fip_replacement` /
    `carthing_emmc_boot_layout` memory entries for the full mechanism.
  - **Phase 4 — terraform user area to GPT + mainline kernel: GPT done,
    kernel now lives in a mature external BSP.** The user-area terraform is
    proven (GPT: `env + boot_a + root_a + boot_b + root_b + bandaid + data`).
    The kernel/userspace side is **`JoeyEamigh/yocto-superbird`** (GitHub) —
    a Yocto stack running mainline **Linux 7.0.2** (`linux-7.0.y` + a small
    panel/BT/touch/rotary patch stack) under *this* u-boot + signed FIP. It
    builds a bare BSP, a chromium-kiosk fork-template, and the `bridgething`
    product, all sharing that GPT layout and an **A/B libswupdate OTA**
    pipeline (delta/zchunk over the USB link, 3-strike bootloader rollback
    via `slot_active` in our env). Mainline TF-A 2.14 is BL31. The early
    joey-4.9.113 + stock-rootfs path (first kernel boot 2026-05-15) is
    abandoned; notes archived in `memory/MEMORY_archive.md` for the GPT
    layout + durable u-boot bugs, not the kernel choice. See
    `superbird-docs/uboot-port/roadmap.md` for the broader roadmap.
      - **Coupling to this repo:** yocto's `superbird-uboot_git.bb` pins
        `ThingLabsOSS/superbird-uboot.git;branch=master` at a SRCREV (as of
        2026-06 = our HEAD, the LCD-dither-disable commit). **After pushing
        u-boot changes (e.g. these panel fixes), bump that SRCREV** for them
        to reach yocto images. Device interaction there mirrors our rig:
        mask-rom via `just reboot-to-maskrom`, UART agent holds FT232 RTS
        deasserted (same reset-pin wiring), host flasher is `flashthing-cli`.

## end-to-end install (mask-ROM → flashed boot0/1)

The full "take a stock Car Thing all the way to power-on-boots-our-u-boot,
no host required" recipe. Each step's wiring lives in memory entries
listed at the top of this file; this is just the high-level dance.

1. **Hold buttons 1+4, hit reset** → SoC mask-ROM USB Mode
   (`1b8e:c003 GX-CHIP`).
2. **`superbird-tool --burn_mode <signed-fip>`** → mask-ROM accepts stock
   BL2, BL2 streams our signed FIP body in via AMLC, our u-boot comes
   up in DRAM. (Signed FIP is `superbird-fip-tools/out/
   u-boot.bin.spotify.encrypt`, produced by `fip-tool sign` — pure Go.)
3. **Auto-fastboot fires** because `set_boot_source()` sees
   `BOOT_DEVICE_USB` and overrides bootcmd to `fastboot_with_screen` —
   panel shows the FASTBOOT splash, host sees `18d1:fada`.
4. **Build the boot-partition image** on the host with
   `flash_boot_partition.py ours --dry-run -o /tmp/boot.bin
   --signed-fip ... --stock-bootloader bootloader.dump` (info_sector +
   stock-BL2[:0x10000] + signed-FIP[0x10000:], padded to 2 MiB).
5. **Stage + write to boot0/boot1** via fastboot:
   ```
   fastboot stage /tmp/boot.bin
   fastboot oem console "mmc dev 0 1; mmc write 0x6000000 0 0x1000"
   fastboot oem console "mmc dev 0 2; mmc write 0x6000000 0 0x1000"
   fastboot oem console "mmc dev 0 0"
   ```
6. **Wipe user area** to neutralise BL2's earlier fallback paths
   (MPT → user-area fip_a/fip_b mirrors come before boot0/boot1 in
   BL2's search order):
   ```
   fastboot oem console "mmc dev 0 0; mmc erase 0 0x80000"
   ```
   256 MiB from LBA 0 — kills MPT, both FIP mirrors, our own env-in-FAT
   if it was there from a prior session.
7. **Reset** → power-on now goes mask-ROM → stock-BL2-from-boot0 → our
   signed-FIP-from-boot0 → our u-boot. No host, no Spotify components.

## Important hardware/workflow notes

  - **UART is a hardware mod, not stock.** Most users don't have UART
    access — the test pads need soldering. Do NOT add anything to
    `superbird-tool` that assumes a serial console exists.
  - **The user's dev rig has a recovery trick wired up:** UART adapter's
    RTS pin is hooked to the SoC reset line. Combine with holding (or
    briefly taping) preset buttons 1+4 to land in mask-ROM USB Mode on
    reset. Buttons are NOT permanently taped — user holds them only
    when they want a maskrom entry; without them, RTS-reset boots
    normally from boot0. Don't bake any of this into `superbird-tool`
    — it's user-specific.
  - **Mask-ROM-signed BL2 is your only entry point.** The shipped
    `../superbird-tool/images/superbird.bl2.encrypted.bin` is the only
    BL2 the ROM will accept (secure-boot fuses are blown). That gets
    you to streaming a custom FIP via BL2's AMLC protocol, which is
    how `--burn_mode CUSTOM_FIP` works.

## Hardware facts

The DTS (`dts/upstream/src/arm64/amlogic/meson-g12a-spotify-carthing.dts`
— the file the build actually consumes; `arch/arm/dts/` only holds the
`meson-g12a-spotify-carthing-u-boot.dtsi` overlay) is authoritative for
buttons, panel, GPIO, regulators, eMMC. Things NOT in DTS or otherwise
discoverable from the tree:

  - **SoC**: G12A family. Exact SKU never confirmed via cpuinfo —
    `sei510_defconfig` (S905X2) was the right starting point.
  - **eMMC stock layout**: 18-partition vendor layout —
    `../superbird-tool/superbird_partitions.py`. We've stopped using it
    (terraformed to GPT); kept as reference for `mmc erase` ranges when
    nuking BL2's stock fallback paths.
  - **USB IDs**: `0x1b8e:0xc003` in mask-ROM USB and vendor burn mode;
    our u-boot's gadget enumerates as `0x18d1:0xfada` for fastboot.
  - **Upstream u-boot bugs found+fixed during the port**: see
    `[carthing_upstream_patches]` memory.

## Folder layout

```
carthing-stuff/
├── superbird-tool/         # python tool, mask-ROM USB primitive
├── superbird-uboot/        # THIS FOLDER — mainline u-boot + mainline TF-A
├── superbird-fip-tools/    # fip-tool/ (Go, primary: ramboot/decrypt/flash/sign)
│                           # + python/ (legacy fip-rebuild.sh etc.)
└── fip_unpack/             # per-device captures (bootloader.dump etc.)
                            # — inputs for fip-tools, not tracked
```

Kernel + userspace work lives outside this layout in the
**`JoeyEamigh/yocto-superbird`** Yocto BSP (Linux 7.0.2 under this
u-boot; multi-image with A/B OTA — see Phase 4 above).

## Things to know about `../superbird-tool`

  - **`--burn_mode CUSTOM_FIP`** is the iteration primitive. Mask-ROM
    accepts stock BL2, BL2 streams the given signed FIP body in via
    AMLC, and your u-boot comes up in DRAM. Pair with `fip-tool sign`
    to produce a fresh signed FIP after every `make`.
  - **`--bulkcmd 'cmd'` / `--bulkcmd_shell`** talk to *vendor* burn-mode
    u-boot — only useful if you `--burn_mode` with no arg, which is
    rare now.
  - **`--find_device`** reports current mode without locking the device
    (no USB open).
  - **`--chainload BINFILE [--load_addr 0x...]`** is the legacy phase-1
    iteration primitive — loads to `0x01080000` by default. Superseded
    by `--burn_mode CUSTOM_FIP` for normal dev; only useful for
    RAM-loading things that aren't a full FIP (e.g.
    `chainload-test/hello.bin`).

## Workflow loop

```bash
# 1. modify u-boot source
cd ../superbird-uboot
vim board/amlogic/spotify-carthing/cmd_bootmenu.c   # or whatever

# 2. rebuild u-boot
make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu-

# 3. re-sign as a Spotify-valid FIP — pure Go, no vendor toolchain/clone/shell.
#    (custom TF-A: add --bl31 ../path/to/bl31.bin)
cd ../superbird-fip-tools
./fip-tool/fip-tool sign ../superbird-uboot/u-boot.bin

# 4. reset device into mask-ROM USB Mode (hold buttons 1+4, RTS-reset
#    via UART adapter; without buttons held, RTS-reset normal-boots
#    from boot0 instead)

# 5. RAM-load via mask-ROM → BL2 → custom FIP (fip-tool ramboot is pure Go;
#    superbird-tool --burn_mode also works)
./fip-tool/fip-tool ramboot ./out/u-boot.bin.spotify.encrypt
# (UART tail in another terminal; fastboot gadget auto-fires. To make
#  the change stick across reset, follow steps 4-6 of
#  `## end-to-end install`.)
```

Don't bother adding UART or reset helpers to `superbird-tool` — UART
access is a hardware mod that 99% of users won't have, and the reset
trick is dev-rig-specific.
