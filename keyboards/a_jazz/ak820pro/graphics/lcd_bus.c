// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later
//
// [UNIFIED EXPERIMENT — ak820pro-flashlcd-unified]
// LCD bus over the ChibiOS SN32 SPI driver: SPI0 (the panel) uses spiSend (FIFO-
// batched by spi_fifo_pump.diff) for all commands/pixels, and the flash->LCD DMA is
// the driver's spiSN32FlashDma* extension. SPI1 (flash reads) stays bare-metal. This
// is the experiment sibling of ak820pro-flashlcd-tiles (which does the same drawing
// fully bare-metal) -- built to measure whether one driver can replace the bare-metal
// bus without losing throughput. See docs/LCD_FLASH_LAYER.md.

#include <string.h>

#include "quantum.h"
#include "gpio.h"
#include "lcd_bus.h"

extern void display_set_paused(bool paused);   // graphics/display.c

// --- pins --------------------------------------------------------------------
#define PANEL_DC   D14
#define PANEL_CS   B8
#define PANEL_RST  A17

#define FRAME_W 128
#define FRAME_H 128
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)
#define LCD_OFF_X 1
#define LCD_OFF_Y 2

// GC9107 MADCTL. Rotation 270 = BGR(0x08) | MV(0x20) | MX(0x40) = 0x68. The dashboard
// and the animation share this orientation. (Panel came up 180 rotated with the MY
// variant (0xA8); MX is the 180-flipped sibling in the same MV=1 rotation family.)
#define MADCTL_270  0x68
#define MADCTL_ANIM MADCTL_270

// Animation slot. The header at ANIM_BASE is the stock format we reverse-engineered:
//   byte 0        = frame count
//   bytes 1..n    = per-frame duration, one byte each
//   ...           = padding to ANIM_HDR (0x00 when stock-written, 0xFF when the
//                   AJAZZ uploader wrote it into freshly erased flash)
// Frames follow at ANIM_HDR, ANIM_STRIDE each. Nothing validates this -- there is
// no magic or checksum -- so a stale slot yields garbage rather than "no animation".
#define ANIM_BASE   0x540000u
#define ANIM_HDR    0x100u
#define ANIM_STRIDE 0x8000u
// Frame-count ceiling, derived rather than guessed: the slot grows upward from
// ANIM_BASE and must not reach the asset region. hdr[0] is one byte, so 255 is
// the format's own ceiling; whichever is smaller wins.
//   (0x0CE0000 - 0x540000 - 0x100) / 0x8000 = 244
#define ANIM_ROOM   ((FLASH_ASSET_BASE - ANIM_BASE - ANIM_HDR) / ANIM_STRIDE)
#define ANIM_MAX    (ANIM_ROOM < 255u ? ANIM_ROOM : 255u)

// Playback is paced by the 10 Hz housekeeping slot -- one frame per 100 ms --
// which matches stock speed by observation. The header's per-frame duration
// bytes are deliberately IGNORED: disassembly of V1.13's player shows it never
// reads them either (only hdr[0], the count), and they are uniform in every
// stock animation we have seen (0x2D throughout Mario, 0x14 throughout the
// 125-frame one) -- exactly what an unread field looks like. What actually sets
// the stock frame rate was not identified; 100 ms is fitted to observation, not
// derived.

// ---------------------------------------------------------------------------
// Low-level bus
// ---------------------------------------------------------------------------
// [UNIFIED EXPERIMENT] SPI0 (the LCD) is driven by the ChibiOS SN32 SPI driver
// (spiSend, FIFO-batched by spi_fifo_pump.diff) instead of bare-metal pokes, and
// the flash->LCD DMA is its extension (spiSN32FlashDma*). We keep manual CS/DC as
// GPIO; every SPI0 byte goes through the driver, because leaving the driver's RX
// FIFO IRQ enabled while poking SN_SPI0->DATA directly would fire its handler
// spuriously. 8-bit, mode 0, 24 MHz -- matches the panel and the DMA extension.
static const SPIConfig spicfg = {
    .ctrl0  = SPI_DATA_LENGTH(8),
    .ctrl1  = SPI_MLSB_MSB | SPI_CPOL_LOW | SPI_CPHA_FALLING,   // mode 0, MSB first
    .clkdiv = 0,                                                // 24 MHz
};


static inline void cs(bool hi) { gpio_write_pin(PANEL_CS, hi); }
static inline void dc(bool data){ gpio_write_pin(PANEL_DC, data); }

static void tx8(uint8_t b) { spiSend(&SPID0, 1, &b); }

// RGB565 is streamed hi-byte-first to match the panel. The driver takes a byte
// buffer, but px[] is a little-endian uint16 array (lo byte first in memory), so
// we byte-swap into a scratch buffer in chunks and hand each chunk to spiSend.
// (This swap-copy is pure overhead versus the old inline tx_pipe -- it is one of
// the costs the unified experiment is meant to expose.)
static void tx_pixels(const uint16_t *px, uint32_t n) {
    static uint8_t buf[512];
    while (n) {
        uint32_t c = n < 256u ? n : 256u;
        for (uint32_t i = 0; i < c; i++) {
            buf[2*i]   = (uint8_t)(px[i] >> 8);
            buf[2*i+1] = (uint8_t)(px[i] & 0xFF);
        }
        spiSend(&SPID0, c * 2u, buf);
        px += c; n -= c;
    }
}

static void reset_panel(void) {
    gpio_set_pin_output(PANEL_RST);
    gpio_write_pin(PANEL_RST, 1); wait_ms(20);
    gpio_write_pin(PANEL_RST, 0); wait_ms(20);
    gpio_write_pin(PANEL_RST, 1); wait_ms(200);
}

// Address window + RAMWR (leaves CS asserted, DC=data). Used by the DMA blit.
static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += LCD_OFF_X; x1 += LCD_OFF_X; y0 += LCD_OFF_Y; y1 += LCD_OFF_Y;
    cs(0);
    dc(0); tx8(0x2A); dc(1); tx8(x0>>8); tx8(x0); tx8(x1>>8); tx8(x1);
    dc(0); tx8(0x2B); dc(1); tx8(y0>>8); tx8(y0); tx8(y1>>8); tx8(y1);
    dc(0); tx8(0x2C); dc(1);
}

// ---------------------------------------------------------------------------
// Bare-metal dashboard drawing (replaces Quantum Painter). RGB565 is streamed
// hi-byte-first to match the panel.
//
// Byte order differs by path, and both are correct -- verified on hardware by
// drawing the same stock asset (usb_dongle, flash 0x0D8310) each way and getting
// an identical green dongle:
//   RAM  -> CPU  (here):            uint16 colour values, emitted hi byte first.
//   flash-> DMA  (lcd_blit_flash):  bytes stored LO first; CTRL0.DL=0xF packs the
//                                   pair into a 16-bit word and shifts it out MSB
//                                   first, which swaps them back.
// So a RAM tile promoted to flash must have its bytes SWAPPED on the way in --
// it is not a straight copy. See docs/LCD_FLASH_LAYER.md (Stage D).
// ---------------------------------------------------------------------------
void lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color) {
    if (x1 < x0 || y1 < y0) return;
    lcd_window(x0, y0, x1, y1);
    uint32_t px = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    static uint8_t buf[512];
    for (uint32_t i = 0; i < 256u; i++) { buf[2*i] = color >> 8; buf[2*i+1] = color & 0xFF; }
    while (px) {
        uint32_t c = px < 256u ? px : 256u;
        spiSend(&SPID0, c * 2u, buf);
        px -= c;
    }
    cs(1);
}

// Stage C: blit a w*h RGB565 tile from RAM (firmware array) to (x,y).
// The SN32 DMA is SPI-to-SPI only -- its source is the other SPI's RX FIFO, with no
// source-address register -- so RAM-resident art cannot be DMA'd and is CPU-pushed here
// (pipelined, ~wire speed). Only flash-resident art can use lcd_blit_flash(). See
// docs/LCD_FLASH_LAYER.md.
// Clear a rect by DMA instead of pushing pixels from the CPU.
//
// The stock image keeps a 128x128 all-black frame at flash 0x000000 -- exactly
// 32768 bytes -- precisely so the panel can be cleared with zero CPU in the data
// path. A full-screen CPU fill is 32 KB through tx_pipe, ~11-13 ms of blocking;
// this is a fire-and-forget DMA.
//
// It works for ANY rect, not just full-screen: the source is uniform, so the
// usual "a sub-rect of a wide image is strided and undrawable" problem does not
// apply -- any contiguous run of w*h*2 zero bytes is the correct source.
#define FLASH_BLACK_FRAME 0x000000u

void lcd_clear_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!w || !h) return;
    if ((uint32_t)w * h * 2u > 0x8000u) return;      // larger than the black frame
    lcd_blit_flash(FLASH_BLACK_FRAME, x, y, w, h);
    for (uint32_t g = 0; g < 4000000u && lcd_blit_busy(); g++) { __asm__ volatile("nop"); }
}

void lcd_blit_ram(const uint16_t *px, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!px || !w || !h) return;
    lcd_window(x, y, x + w - 1, y + h - 1);
    tx_pixels(px, (uint32_t)w * (uint32_t)h);
    cs(1);
}

// ---------------------------------------------------------------------------
// Panel bring-up (bare-metal GC9107 init; literal opcodes, no Quantum Painter)
// ---------------------------------------------------------------------------
static void send_cmd(uint8_t c) { dc(0); tx8(c); dc(1); }
static void send_seq(const uint8_t *seq, uint32_t len) {   // cmd, delay_ms, nparams, params...
    cs(0);
    for (uint32_t i = 0; i < len;) {
        uint8_t cmd = seq[i], delay = seq[i+1], num = seq[i+2];
        send_cmd(cmd);
        for (uint8_t k = 0; k < num; k++) tx8(seq[i+3+k]);
        if (delay) wait_ms(delay);
        i += 3 + num;
    }
    cs(1);
}

void lcd_init(void) {
    gpio_set_pin_output(PANEL_CS); gpio_write_pin(PANEL_CS, 1);
    gpio_set_pin_output(PANEL_DC); gpio_write_pin(PANEL_DC, 1);
    spiStart(&SPID0, &spicfg);          // driver owns SPI0 (8-bit, mode 0, 24 MHz)
    reset_panel();
    static const uint8_t seq[] = {
        0xFE, 5, 0,                 // inter-register enable 1
        0xEF, 5, 0,                 // inter-register enable 2
        0xB6, 0, 1, 0x19,           // function ctl6: allow complement-RGB + framerate
        0xAC, 0, 1, 0xC0,           // complement RGB
        0xAB, 0, 1, 0x0E,
        0xA8, 0, 1, 0x19,           // frame rate
        0x3A, 0, 1, 0x05,           // pixel format: 16bpp RGB565
        0x21, 0, 0,                 // display inversion ON (panel powers up non-inverted; this panel/panel type needs it on)
        0x11, 120, 0,               // sleep out
        0x29, 20, 0,                // display on
        0x36, 0, 1, MADCTL_270,     // memory access ctl: rotation 270
    };
    send_seq(seq, sizeof(seq));
}

// Panel-controller power (separate from the backlight, which is display_set_power).
// sleep=true drops the GC9107 to sleep-in (display off first), cutting its own
// current; sleep=false brings it back (the 120 ms sleep-out wait the datasheet
// requires before drawing, then display on). The driver must own SPI0 -- true after
// lcd_init and between blits, which is where the sleep callers run.
void lcd_panel_sleep(bool sleep) {
    if (sleep) {
        static const uint8_t seq[] = {
            0x28, 20, 0,            // display off
            0x10,  5, 0,            // sleep in
        };
        send_seq(seq, sizeof(seq));
    } else {
        static const uint8_t seq[] = {
            0x11, 120, 0,           // sleep out (>=120 ms before any draw)
            0x29,  20, 0,           // display on
        };
        send_seq(seq, sizeof(seq));
    }
}


// ---------------------------------------------------------------------------
// Flash-resident asset index (Stage D)
//
// res/mkraw.py --flash packs every asset into one blob written at
// FLASH_ASSET_BASE: a 4K index sector, then the assets page-aligned. Entry
// offsets are stored RELATIVE to the region base so the blob can be relocated.
//
// Font atlases are packed as per-glyph CONTIGUOUS tiles, not as a wide atlas
// image: a glyph cell inside an atlas is strided (cell_w wide, img_w apart) and
// the DMA can only stream consecutive bytes, so an atlas is undrawable by it.
// Glyph n is therefore one flat blit at off + n*cell_w*cell_h*2.
// ---------------------------------------------------------------------------
#define FA_MAGIC   0x53414B41u   // "AKAS"
#define FA_MAX     32

static flash_asset_t fa_tab[FA_MAX];
static uint8_t       fa_count = 0;

bool flash_assets_init(void) {
    uint8_t hdr[8];
    fa_count = 0;
    lcd_flash_init();
    flash_read_bytes(FLASH_ASSET_BASE, hdr, sizeof hdr);
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != FA_MAGIC || hdr[4] != 1) return false;   // absent or wrong version
    uint8_t n = hdr[5] > FA_MAX ? FA_MAX : hdr[5];

    uint8_t e[16];
    for (uint8_t i = 0; i < n; i++) {
        flash_read_bytes(FLASH_ASSET_BASE + 8u + (uint32_t)i * 16u, e, sizeof e);
        flash_asset_t *a = &fa_tab[i];
        a->id     = (uint16_t)(e[0] | (e[1] << 8));
        a->off    = (uint32_t)e[2] | ((uint32_t)e[3] << 8) | ((uint32_t)e[4] << 16);
        a->fmt    = e[5];
        a->w      = (uint16_t)(e[6] | (e[7] << 8));
        a->h      = (uint16_t)(e[8] | (e[9] << 8));
        a->cell_w = e[10]; a->cell_h = e[11];
        a->first  = e[12]; a->count  = e[13];
    }
    fa_count = n;
    return true;
}

uint8_t flash_assets_count(void) { return fa_count; }

const flash_asset_t *flash_asset(uint16_t id) {
    for (uint8_t i = 0; i < fa_count; i++)
        if (fa_tab[i].id == id) return &fa_tab[i];
    return NULL;
}

// Blit a flash asset and wait for the DMA to finish. Bounded: a stuck DMA must
// not wedge the caller. A 24x24 icon is ~0.5 ms; the 128x128 splash ~13 ms.
static void blit_flash_sync(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    lcd_blit_flash(src, x, y, w, h);
    for (uint32_t g = 0; g < 4000000u && lcd_blit_busy(); g++) { __asm__ volatile("nop"); }
}

void lcd_draw_flash_image(uint16_t id, uint16_t x, uint16_t y) {
    const flash_asset_t *a = flash_asset(id);
    if (!a) return;
    blit_flash_sync(FLASH_ASSET_BASE + a->off, x, y, a->w, a->h);
}

// Draw one glyph of a flash font: tile index (c - first), each cell_w*cell_h.
void lcd_draw_flash_glyph(uint16_t font_id, char c, uint16_t x, uint16_t y) {
    const flash_asset_t *a = flash_asset(font_id);
    if (!a || a->fmt != 1) return;
    if ((uint8_t)c < a->first || (uint8_t)c >= a->first + a->count) return;
    uint32_t tile = (uint32_t)((uint8_t)c - a->first) * a->cell_w * a->cell_h * 2u;
    blit_flash_sync(FLASH_ASSET_BASE + a->off + tile, x, y, a->cell_w, a->cell_h);
}

void lcd_draw_flash_text(uint16_t font_id, uint16_t x, uint16_t y, const char *s) {
    const flash_asset_t *a = flash_asset(font_id);
    if (!a) return;
    for (; *s; s++, x += a->cell_w) lcd_draw_flash_glyph(font_id, *s, x, y);
}

uint16_t lcd_flash_text_width(uint16_t font_id, const char *s) {
    const flash_asset_t *a = flash_asset(font_id);
    return a ? (uint16_t)(strlen(s) * a->cell_w) : 0;
}

static volatile bool blit_done = true;

// DMA completion is serviced by the driver's SPI0 handler (the spiSN32FlashDma
// extension); it calls blit_done_cb below. No Vector58 here anymore.
static void blit_done_cb(void) {
    gpio_write_pin(FLASH_CS, 1);
    cs(1);
    blit_done = true;
}

// Stage C: blit a w*h RGB565 tile from flash offset `src` to the panel rect at (x,y).
// Interrupt-driven and NON-BLOCKING: arms the SPI1(flash)->SPI0(LCD) engine and returns;
// Vector58 signals completion via blit_done. Animation frames are just the full-frame case.
// NOTE: the panel's MADCTL orientation is the caller's business -- flash art authored for
// the animation orientation (MADCTL_ANIM) will not match the dashboard's (MADCTL_270).
void lcd_blit_flash(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!w || !h) return;
    // SPI1 must be up or the DMA has a dead source: it never completes, the
    // caller spins out its timeout, and SPI0 is left in DMA mode with FLASH_CS
    // asserted -- which then corrupts the next flash read. This used to be the
    // caller's job and the ordering was load-bearing but invisible (it only
    // worked because flash_assets_init() happened to run first). Cheap: a bool.
    lcd_flash_init();
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u;
    blit_done = false;
    // SPI0 (sink) into DMA config + counts; SPI1 (source) recorded for Step 2.
    // SPI0 stays 8-bit so the command phase (window) can go out first.
    spiSN32FlashDmaPrepare(&SPID0, &SPID1, bytes);
    SN_SPI1->CTRL0_b.FRESET = 0b11;                 // flash side (bare-metal, ours)
    lcd_window(x, y, x + w - 1, y + h - 1);         // via spiSend (SPI0 still 8-bit)
    gpio_write_pin(FLASH_CS, 0);
    // Prepare() disabled SPID1's NVIC vector for the DMA window, so the READ+addr
    // command goes out via the raw poll primitive (not spiSend, which needs the ISR).
    spi1_raw_byte(FLASH_CMD_READ); spi1_raw_byte((src>>16)&0xFF); spi1_raw_byte((src>>8)&0xFF); spi1_raw_byte(src&0xFF);
    SN_SPI1->IC = 0x3F;
    // Flip to 16-bit pixels and arm; blit_done_cb fires at completion.
    spiSN32FlashDmaFire(&SPID0, blit_done_cb);
}

// Animation frames are full-screen tiles.
static inline void blit_arm(uint32_t addr) { lcd_blit_flash(addr, 0, 0, FRAME_W, FRAME_H); }

// True once the in-flight DMA blit has completed (Vector58 sets it).
bool lcd_blit_busy(void) { return !blit_done; }

// The RAM/CPU text and image helpers are gone: all art is flash-resident and
// DMA-drawn now (lcd_draw_flash_*). lcd_blit_ram() stays for anything that
// still needs to push a RAM tile.

// ---------------------------------------------------------------------------
// Animation player
// ---------------------------------------------------------------------------
static bool     anim_on  = false;
static uint8_t  anim_idx = 0;
static uint8_t  anim_count = 0;              // from the header, 0 = nothing to play

// Read the slot header. Returns false if it describes nothing playable, which is
// the normal state for an empty or never-provisioned slot.
static bool anim_read_header(void) {
    uint8_t hdr;
    lcd_flash_init();
    flash_read_bytes(ANIM_BASE, &hdr, 1);
    anim_count = hdr > ANIM_MAX ? 0 : hdr;
    return anim_count != 0;
}

// True while the flash-animation player owns the bus. The bit-banged RTC I2C (SCL=A14,
// SDA=A15) shares port A with the flash SPI1 pins (SCK=A12, CS=A13); its open-drain
// pin-mode toggling glitches A12/A13 mid-DMA and corrupts the flash read. Callers must
// suspend RTC polling while this is true.
bool anim_active(void) { return anim_on; }

static void set_madctl(uint8_t v) { cs(0); dc(0); tx8(0x36); dc(1); tx8(v); cs(1); }


// One-shot self-contained flash blit: brings up SPI1, blits, waits with a bound,
// then puts SPI0 back exactly as anim_toggle's stop path does so the dashboard
// runs unaffected. The wait is bounded on purpose -- completion rides the SPI0
// IRQ, and an unbounded spin here hangs the keyboard before USB enumerates.
void lcd_blit_flash_probe(uint32_t src, uint16_t w, uint16_t h) {
    lcd_flash_init();
    lcd_blit_flash(src, 0, 0, w, h);
    for (uint32_t i = 0; i < 4000000u && !blit_done; i++) { __asm__ volatile("nop"); }
    // The DMA extension already restored SPI0 to the driver's 8-bit FIFO mode at
    // completion; nothing to tear down here.
    gpio_write_pin(FLASH_CS, 1); cs(1);
}

void anim_toggle(void) {
    lcd_flash_init();
    if (!anim_on) {
        display_set_paused(true);           // stop QP touching the bus
        set_madctl(MADCTL_ANIM);            // frames authored for this orientation
        if (!anim_read_header()) {          // empty slot: nothing to play
            set_madctl(MADCTL_270);         // undo the orientation change
            display_set_paused(false);
            return;
        }
        anim_on = true; anim_idx = 0;
        blit_arm(ANIM_BASE + ANIM_HDR);
    } else {
        anim_on = false;
        while (!blit_done) { /* let the in-flight frame finish */ }
        gpio_write_pin(FLASH_CS, 1); cs(1);
        // The DMA extension restored SPI0 to the driver's 8-bit FIFO mode at the
        // last frame's completion, so the dashboard's spiSend path is ready again.
        set_madctl(MADCTL_270);             // restore dashboard orientation
        display_set_paused(false);          // resume + full repaint
    }
}
// Called from the 10 Hz housekeeping slot, so one frame per 100 ms.
void anim_task(void) {
    if (!anim_on || !blit_done) return;     // previous frame still in flight
    anim_idx = (uint8_t)((anim_idx + 1) % anim_count);
    blit_arm(ANIM_BASE + ANIM_HDR + (uint32_t)anim_idx * ANIM_STRIDE);
}
