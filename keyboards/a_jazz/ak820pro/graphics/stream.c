// Copyright 2026 Nikolay Aviltsev
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See stream.h.

#include "graphics/stream.h"
#include "lcd_bus.h"

extern void display_set_paused(bool paused);  // graphics/display.c

enum {
    SC_SPAN  = 0x01,  // [x(1)][y(1)][len(1)][pixels: hi,lo per px, RGB565]
    SC_START = 0x02,  // pause the dashboard, stream owns the panel
    SC_STOP  = 0x03,  // resume the dashboard (triggers a full repaint)
};

// 3-byte VIA header + x/y/len leaves ~22 payload bytes in one HID report
// (see host/doom-stream/SPEC.md) -- 11 RGB565 pixels.
#define SPAN_MAX_PX 11

void stream_hid_command(uint8_t *data, uint8_t length) {
    if (length < 3) return;

    switch (data[2]) {
        case SC_SPAN: {
            if (anim_active()) return;  // animation owns SPI0/SPI1
            if (length < 6) return;

            uint8_t x   = data[3];
            uint8_t y   = data[4];
            uint8_t len = data[5];
            if (len > SPAN_MAX_PX) len = SPAN_MAX_PX;
            if ((uint8_t)(6 + len * 2) > length) return;  // short packet

            uint16_t px[SPAN_MAX_PX];
            for (uint8_t i = 0; i < len; i++) {
                uint8_t hi = data[6 + i * 2];
                uint8_t lo = data[6 + i * 2 + 1];
                px[i]      = ((uint16_t)hi << 8) | lo;
            }
            lcd_blit_ram(px, x, y, len, 1);
            break;
        }
        case SC_START:
            display_set_paused(true);
            break;
        case SC_STOP:
            display_set_paused(false);
            break;
        default:
            break;
    }
}
