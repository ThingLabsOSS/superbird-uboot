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

**A halt instead of a reset.** Both of the A/B selector's resets — the
failed ``sysboot`` and the slot-exhausted failover — stop with the log
on screen and bring fastboot up, so a host can still attach.

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
