// Copyright 2026 Nikolay Aviltsev
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Spectator pixel-stream channel (0x13): a host pushes small RGB565 spans
// straight to the panel over raw HID, driven by a Doom source port on the
// host side. Fully ephemeral -- unlike media/flash, no state is kept here,
// each span is blitted the instant it arrives. See
// host/doom-stream/SPEC.md in naviltsev/qmk_keyboard for the full design.
//
// custom-backend only: built on lcd_blit_ram(), which the qp backend does
// not implement.
#pragma once

#include <stdint.h>

#define STREAM_CHANNEL 0x13

// Handle one 0x13 frame: [SET_VALUE, STREAM_CHANNEL, subcmd, payload...].
// Call only for frames already matched to STREAM_CHANNEL (see raw_hid_receive).
void stream_hid_command(uint8_t *data, uint8_t length);
