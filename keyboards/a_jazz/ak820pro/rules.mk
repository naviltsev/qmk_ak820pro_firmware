# Dashboard backend selector. Both backends sit on the same dualspi SPI base (SPI0
# + SPI1 through the ChibiOS driver, flash->LCD DMA extension). Pick with e.g.
#   qmk compile -kb a_jazz/ak820pro -km via -e DASHBOARD_BACKEND=qp
#   - custom (default): bare-metal RGB565 tile dashboard, assets in EXTERNAL flash,
#                       SPI1 driver-managed (spiExchange). No Quantum Painter.
#   - qp:               Quantum Painter dashboard + splash from EMBEDDED qgf/qff via
#                       the stock gc9107_spi driver; SPI1 bare-metal DMA source.
# See docs/LCD_FLASH_PLAN.md.
DASHBOARD_BACKEND ?= custom

ifeq ($(strip $(DASHBOARD_BACKEND)),qp)
    OPT_DEFS += -DDASHBOARD_BACKEND_QP
    QUANTUM_PAINTER_ENABLE = yes
    QUANTUM_PAINTER_DRIVERS += gc9107_spi
    SRC += graphics/lcd_bus_qp.c
    SRC += graphics/display_qp.c
    # Embedded QP assets (dashboard icons, fonts, splash).
    SRC += graphics/res/sonixqmk.qgf.c
    SRC += graphics/res/Iosevka-Regular-30.qff.c
    SRC += graphics/res/Iosevka-Medium-20.qff.c
    SRC += graphics/res/apple_icon_24x24.qgf.c
    SRC += graphics/res/windows_icon_24x24.qgf.c
    SRC += graphics/res/cable_icon_24x24.qgf.c
    SRC += graphics/res/bluetooth_icon_24x24.qgf.c
    SRC += graphics/res/2_4_g_icon_24x24.qgf.c
else
    # (No -D needed: code only tests DASHBOARD_BACKEND_QP.)
    # The qgf/qff asset files under graphics/res/ make QMK's data-driven build
    # auto-enable Quantum Painter (info_rules.mk: QUANTUM_PAINTER_ENABLE ?= yes).
    # The custom backend does not use QP, so force it off explicitly.
    QUANTUM_PAINTER_ENABLE = no
    # Bare-metal tile dashboard: we own SPI0 for the flash->LCD DMA and decode the
    # qgf/qff asset blobs ourselves. Dashboard graphics live in EXTERNAL flash.
    SRC += graphics/lcd_bus.c
    SRC += graphics/display.c
    # Spectator pixel-stream channel (0x13, doom-stream project). Built on
    # lcd_blit_ram(), which the qp backend doesn't implement.
    SRC += graphics/stream.c
endif

# CH582F wireless module exposed through QMK's official Bluetooth driver API.
# BLUETOOTH_DRIVER = custom defines BLUETOOTH_ENABLE, CONNECTION_ENABLE and
# NO_USB_STARTUP_CHECK and compiles bluetooth.c (weak bluetooth_* defaults);
# our ch582f_ajazz.c (below in SRC) provides the strong overrides.
BLUETOOTH_ENABLE = yes
BLUETOOTH_DRIVER = custom

# WIP: external PCF8563 RTC over the ChibiOS software (bit-banged) I2C fallback LLD.
# Swaps the SN32 HW I2C driver for the SW fallback; rtc.c drives it via the I2C HAL
# API. Does NOT work on hardware yet (compiles/links fine) -- see rtc.c.
USE_HAL_I2C_FALLBACK = yes

SRC += bluetooth/ch582f_ajazz.c

# Dashboard graphics (custom backend) live in EXTERNAL FLASH, not firmware. Generate
# the blob with `python3 graphics/res/mkraw.py --flash` and upload once per keyboard:
#   ak820ctl flash write 0x0CE0000 graphics/res/flash_assets.bin
# The firmware reads the index at boot (flash_assets_init) and DMA-draws by id.

SRC += graphics/flash_io.c
SRC += graphics/nowplaying.c
SRC += rtc/rtc.c
SRC += media/media.c
VPATH += bluetooth
VPATH += graphics
VPATH += rtc
VPATH += media
