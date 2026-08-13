.. SPDX-License-Identifier: GPL-2.0+

Car Thing: diagnosing a bootloop with the debug console build
=============================================================

The problem
-----------

A Car Thing that bootloops shows nothing useful. There is no UART unless
the owner has soldered to the test pads, the interesting output scrolls
past in the fraction of a second before the reset, and the reset wipes
the panel. So the failure destroys its own evidence.

``spotify_carthing_debug_defconfig`` builds a u-boot that mirrors the
entire boot log to the LCD and refuses to auto-reset, so the last thing
that happened stays on screen to be read or photographed.

Two variants
------------

**Both variants halt on a failed boot** rather than resetting. That is
the whole point: a reset wipes the panel a second after the failure, so
there is never a screen to photograph. Halting is into fastboot, so the
image stays frozen for the camera while a host can still attach and run
``debugreport``.

What differs is whether the image may write to the unit.

``spotify_carthing_debug_defconfig`` — **transient**, for RAM-loading
or chainloading. It runs once and leaves, so it must not touch the
unit: ``env_save()`` is suppressed, try counters keep their values
(they are the evidence) and the panel console never reaches
``uboot.env``.

``spotify_carthing_debug_flash_defconfig`` — **flashed**, for writing
to boot0/boot1. This image *is* the bootloader, so the A/B state
machine keeps real books: tries decrement, slots fail over, and each
step is saved *before* the halt. The saved env is still kept clean of
the panel console — the original values are restored around the save —
so flashing back to a normal build leaves no trace.

So a flashed unit that can't hand off stops on the failure with the log
up, and **advances one power cycle at a time**: power on, watch it
fail, photograph it, power-cycle to see the next attempt with one fewer
try, and eventually the failover to the other slot. Rollback still
reaches slot b; it just asks for a power button instead of resetting
itself. Holding the menu button still opens the bootmenu on any cycle.

Building
--------

.. code-block:: bash

   make spotify_carthing_debug_defconfig
   make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu-

This produces two useful artifacts from the same ``u-boot.bin``:

* ``u-boot.bin`` itself, which carries an arm64 Linux Image header and
  can be chainloaded over fastboot (see below);
* a signed FIP for mask-ROM RAM-loading, via
  ``superbird-fip-tools/fip-tool sign u-boot.bin``.

Getting it onto a device
------------------------

Nothing here writes to the eMMC. Both routes are RAM-only, so a power
cycle returns the unit to whatever it was doing before.

The build also suppresses ab_boot's ``env_save()``, so running it does
not burn a try against the slot or otherwise edit ``uboot.env``. That
matters twice over: the try counters are evidence we came to read, and
without it the on-screen console setting would persist and the unit
would keep printing to the panel after going back to a normal build.

Route 1: over fastboot, no disassembly (preferred)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This works on a unit running an **unmodified, already-installed**
u-boot — it needs no command that isn't already in the shipped build.
A bootlooping unit can still reach fastboot, because the boot router
runs before ``bootcmd`` and honours a held menu button: power on with
the menu button held to get the bootmenu, then pick fastboot.

.. code-block:: bash

   fastboot stage u-boot-debug.bin
   fastboot oem console 'cp.b 0x6000000 0x1000000 ${filesize}'
   fastboot oem console 'booti 0x1000000 - ${fdtcontroladdr}'

Note the single quotes: ``${filesize}`` and ``${fdtcontroladdr}`` are
meant to be expanded by u-boot, not by the host shell. ``filesize`` is
set by the fastboot download itself, so the recipe carries no
image-specific constants.

Why ``booti`` and not ``go``:

``go`` calls the entry point directly (``do_go_exec()`` is a bare
function call). The incoming u-boot then starts its own cache and MMU
setup while the outgoing one's MMU is still on and its page tables are
still live. Confirmed on hardware — the SoC drops off USB and never
comes back. ``booti`` is the same jump with the arm64 teardown that
makes it survivable (``dcache_disable()``, which on armv8 flushes the
caches and takes the MMU down together), which is why the debug build
selects ``LINUX_KERNEL_IMAGE_HEADER``: it makes ``u-boot.bin`` a valid
arm64 ``Image`` so ``booti`` will accept it.

The ``cp.b`` is required and not incidental. The emitted header sets
flag bit 3 ("2MB-aligned base may be anywhere"), so ``booti_setup()``
leaves the image where it finds it rather than relocating it. u-boot
is linked to run at ``CONFIG_TEXT_BASE`` (0x01000000) and nowhere else,
so the image has to already be there. The header's ``text_offset`` is
0x01000000 to match, which is what makes ``booti`` compute the same
address and skip the move.

An FDT argument is required: ``boot_prep_linux()`` panics outright with
"FDT and ATAGS support not compiled in" if it gets neither.
``${fdtcontroladdr}`` is u-boot's own control DTB and is always
present. The chainloaded u-boot ignores it (it uses its own appended
DTB) — it exists purely to satisfy the boot path.

Route 2: mask-ROM USB
~~~~~~~~~~~~~~~~~~~~~

Needs the button-1+4 chord and a reset, but does not depend on the
installed u-boot being functional at all:

.. code-block:: bash

   fip-tool ramboot out/u-boot.bin.spotify.encrypt

A RAM-loaded image is detected as ``boot_source=usb`` and auto-enters
fastboot rather than continuing to boot, so it will print its report
but will not reproduce the loop on its own. Drive the real boot path
from the host with ``fastboot oem console "ab_boot"``.

What you get
------------

**The whole boot log on the panel**, including pre-relocation output
replayed from the pre-console buffer, at ``LOGLEVEL=7``, in an 8x16
font (100x30 characters).

**A boot report**, printed before the boot router can hand off, and
re-runnable at any time with ``debugreport`` (also over
``fastboot oem console "debugreport"``)::

   ===== Car Thing debug build =====
   -- A/B state --
   slot_active=a  a_tries=1  b_tries=3
   bootcmd=ab_boot
   -- boot path --
   boot device=1  boot_source=<unset>  maskrom_failed=<unset>
   reboot reason=<none> (cold boot or already consumed)
   -- environment --
   source=uboot.env (saved)
   -- hardware --
   board rev=8  serial#=...
   -- boot artifacts --
   boot_a=part2: /extlinux/extlinux.conf=ok /extlinux/Image=ok
   boot_b=part4: /extlinux/extlinux.conf=MISSING /extlinux/Image=MISSING
   -- partitions --
   (part list mmc 0)

**A halt instead of a reset — in the transient build only.** See "Two
variants" below.

**A slow eMMC while booting, full speed for flashing.**
``CARTHING_MMC_SLOW_BOOT`` (default y) pins the bus to
``CARTHING_MMC_BOOT_MODE`` (default 1 = ``MMC_HS``, 26 MHz SDR) for the
boot path, and lifts the pin when a host session starts — fastboot, UMS,
or either bootmenu entry. Booting slow costs no measurable time;
flashing slow does, and a host session is exactly where a failure is
visible and retryable.

The mechanism is ``mmc->user_speed_mode``, which ``mmc_start_init()``
honours by masking ``host_caps`` down to the one mode. It needs
``CONFIG_MMC_SPEED_MODE_SET``, and a mode is only selectable if the DT
enables it — so the DT keeps its full DDR52/HS200 capabilities and this
*restricts* at runtime rather than removing. (An earlier version deleted
the caps from the DT, which worked but made fast unreachable.)

The pin is applied from ``board_init``, because the first eMMC access is
the env load at ``initr_env``, earlier than ``misc_init_r``. That means
probing the MMC controller by hand rather than waiting for
``initr_mmc``. It soft-fails: a probe-order surprise leaves the unit
booting fast rather than not booting, and the report prints the mode
actually negotiated so a silent fallback is visible.

Runtime control::

    emmcspeed          show the negotiated mode
    emmcspeed slow     pin to the boot mode
    emmcspeed fast     un-pin, renegotiate
    emmcspeed <n>      pin to enum bus_mode index n

This also doubles as a test for the underlying fault: **if a unit loops
on the normal build and boots on this one, the eMMC is marginal, not
the software.** Confirmed in the field on a unit that previously never
booted. Bus width stays at 8 throughout, so only one variable moves.

Scope is u-boot only. Stock BL2 has already loaded the FIP by then, and
Linux uses its own DT from the boot partition — where ``&sd_emmc_c`` is
currently identical to ours, DDR52 and HS200 included. A unit rescued
here still meets the same flash at full speed once the kernel takes
over.

Reading the result
------------------

Three shapes account for most pre-handoff loops, and the report
separates them:

``boot artifacts MISSING``
   The slot has no kernel or no extlinux.conf, usually a failed or
   interrupted OTA. ``ab_boot`` hands any slot to ``sysboot`` and
   treats the return as a failed attempt, so an empty partition looks
   exactly like a crashing kernel from outside.

``try counters that never move across boots``
   ``env_save()`` is failing — a missing or corrupt env partition, so
   the decrement never persists. The unit retries the same slot forever
   and never even reaches the failover. Check ``source=`` in the
   environment block.

``slot_active flipping a <-> b``
   Both slots are genuinely failing. This is the documented degenerate
   case of the selector: each failover refills the *other* slot's
   budget, so it oscillates rather than stopping. Deliberate — it keeps
   retrying instead of hard-bricking — but from outside it is
   indistinguishable from any other loop.

Caveats
-------

Bringing the panel up during ``console_init_r`` rather than
``misc_init_r`` is a real change to the init ordering, and it is the
part of this build least like the shipping one. It costs the ~700 ms
ST7701S init sequence before the console exists. Do not ship it; use
``spotify_carthing_defconfig`` for real images.
