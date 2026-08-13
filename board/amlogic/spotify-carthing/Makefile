# SPDX-License-Identifier: GPL-2.0+

obj-y	:= spotify-carthing.o cmd_wheel.o cmd_setbright.o \
	   charger.o cmd_chargerinfo.o cmd_tsensor.o boardrev.o cmd_hwinfo.o \
	   mfi.o cmd_mfi.o cert_parse.o qrcodegen.o hwinfo_qr.o panel_probe.o

# On-panel menu: shared UI primitives, the menu itself, and one file per
# sub-screen (each owns its own input loop).
obj-y	+= menu_ui.o cmd_bootmenu.o cmd_fastboot_screen.o \
	   menu_settings.o menu_charger.o menu_hwinfo.o
