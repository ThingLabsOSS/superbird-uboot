# Upstream u-boot for the Spotify Car Thing

Mainline u-boot ported to the Spotify Car Thing (Amlogic G12A / S905D2,
512 MiB DDR, 480x800 ST7701S panel, ~5 sec from cold-boot to splash).
The original `u-boot.git` README is preserved as
[`README.orig`](README.orig).

This tree is the BL33 half of a complete Spotify-free boot chain. Stock
BL2 is kept (we can't replace it — mask-ROM RSA-verifies it against a
fused key hash), but everything from BL30/BL31/BL32/BL33 onward is
ours, signed with the production key Spotify accidentally open-sourced
as `aml-user-key.sig` in their `spsgsb/uboot` repo. The whole user area
(3.6 GiB) is freed for repartitioning.

> Sister tooling lives outside this tree:
> - [`superbird-tool`](https://github.com/ThingLabsOSS/superbird-tool) — pyamlboot wrapper used for
>   mask-ROM USB chainload, vendor burn-mode, RAM-loading our signed FIP.
> - [`superbird-fip-tools`](https://github.com/ThingLabsOSS/superbird-fip-tools) — `fip-rebuild.sh`
>   (wraps `aml_encrypt_g12a` to produce the signed FIP),
>   `flash_boot_partition.py` (builds info_sector + hybrid + flashes
>   boot0/boot1), `aml_decrypt.py` (inverse)..

## What works

  - Cold boot in ~5 sec, panel splash visible (Sitronix ST7701S DSI
    panel fully driven from u-boot — see
    `superbird-docs/uboot/spotify-carthing-display-notes.md`).
  - eMMC (8-bit DDR52, 42 MB/s sustained), fastboot, UMS, env-in-FAT.
  - On-panel **boot menu** (preset1/preset4 + wheel + back) — fastboot,
    UMS, brightness, charger info, hardware inventory, hwid QR.
  - On-panel auto-detected modes — when chainloaded via mask-ROM USB the
    device drops straight into fastboot with a clear "FASTBOOT" splash;
    `fastboot reboot bootloader` round-trips back into fastboot via an
    AO-domain reboot-reason latch.
  - Mainline TF-A 2.14 as BL31 (replaces vendor 2019 blob).
  - Custom efuse-derived serial number (matches what stock adb reports).
  - I2C-driven hardware probe: MAX14656 charger, TMD2772 prox/ALS,
    TLSC6X touch+panel-variant detect, Apple MFi 3.0.

## End-to-end install

The "go from a stock Car Thing all the way to power-on-boots-our-u-boot,
no host required after this" recipe:

1. **Hold buttons 1+4, hit reset** → SoC mask-ROM USB Mode
   (`lsusb` shows `1b8e:c003 GX-CHIP`).
2. **`superbird-tool --burn_mode <signed-fip>`** — mask-ROM loads stock
   BL2, BL2 streams our signed FIP body in over USB, our u-boot comes
   up in DRAM. The signed FIP is
   `superbird-fip-tools/out/u-boot.bin.spotify.encrypt`, produced from
   the `u-boot.bin` you just built here:
   ```bash
   cd ../superbird-fip-tools
   ./fip-rebuild.sh -b ../superbird-uboot/u-boot.bin
   ```
3. **Auto-fastboot fires** — our u-boot detects it was RAM-loaded
   (boot device = USB) and drops straight into fastboot with the
   `FASTBOOT` on-panel splash. Host enumerates as `18d1:fada`.
4. **Build the boot-partition image** on the host:
   ```bash
   cd ../superbird-fip-tools
   ./flash_boot_partition.py ours \
       --signed-fip out/u-boot.bin.spotify.encrypt \
       --stock-bootloader bootloader.dump \
       --dry-run -o /tmp/boot.bin
   ```
   That assembles info_sector (LBA 0) + stock-BL2[:0x10000] +
   signed-FIP[0x10000:], padded to 2 MiB (4096 sectors).
5. **Stage + write to boot0 and boot1** via fastboot:
   ```bash
   sudo fastboot stage /tmp/boot.bin
   sudo fastboot oem console "mmc dev 0 1; mmc write 0x6000000 0 0x1000"
   sudo fastboot oem console "mmc dev 0 2; mmc write 0x6000000 0 0x1000"
   sudo fastboot oem console "mmc dev 0 0"
   ```
6. **Wipe the user area** so BL2's fallback chain (MPT → user-area
   fip_a/b → boot0 → boot1) can't find an older mirror to fall back to:
   ```bash
   sudo fastboot oem console "mmc dev 0 0; mmc erase 0 0x80000"
   ```
   256 MiB from LBA 0 — kills MPT, both vendor FIP mirrors, and any
   stale env-in-FAT from prior sessions.
7. **Reset** — cold boot now goes mask-ROM → stock-BL2-from-boot0 →
   our-FIP-from-boot0 → our u-boot. No host. No Spotify firmware.
   Mask-ROM USB (step 1) always remains as a recovery path.

## Build

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
make spotify_carthing_defconfig
make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu-
```

Produces `u-boot.bin` at the tree root. Hand that to
`superbird-fip-tools/fip-rebuild.sh -b u-boot.bin` to get a flashable
signed FIP.

## Layout

```
arch/arm/dts/meson-g12a-spotify-carthing.dts   board DTS
board/amlogic/spotify-carthing/                board file + bootmenu + commands
configs/spotify_carthing_defconfig             config
```

## License

u-boot is GPL-2.0+. Our additions are GPL-2.0+ unless individually
marked otherwise.
