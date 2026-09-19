// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ak820pro.h"

#include "gpio.h"
#include "connection.h"

#include "graphics/display.h"
#include "graphics/lcd_bus.h"
#include "bluetooth/ch582f_ajazz.h"
#include "rtc/rtc.h"
#include "media/media.h"
#include "graphics/stream.h"
#include "raw_hid.h"
#include "rgb_matrix.h"
#include "usb_main.h"     // USB_DRIVER (USBD1), USB_SUSPENDED

// Current wireless mode, derived from the tri-state slider. The Fn BT controls
// are only meaningful in the matching mode (e.g. Fn+Q selects a BT slot only
// while the slider is in the BT position), so they are gated on this.
enum wireless_mode {
    WL_MODE_USB = 0,
    WL_MODE_BT,
    WL_MODE_24G
};
static uint8_t wireless_mode = WL_MODE_USB;

// Persisted keyboard config (EEPROM kb datablock; 4 bytes reserved, room to grow).
typedef struct __attribute__((packed)) {
    uint8_t bt_profile;   // last BT slot selected (CH582_PROFILE_BT_1..3)
    uint8_t _pad[3];
} kb_config_t;
static kb_config_t kb_config;

// Last BT slot the user selected. Entering BT mode used to hardcode slot 1, so
// leaving BT for USB/2.4G and coming back silently dropped you onto slot 1
// whatever you had been connected to. Persisted to EEPROM so it survives mode
// switches and power cycles.
static ch582_profile_t last_bt_profile = CH582_PROFILE_BT_1;

// Write the remembered slot back only when it actually changed -- wear-leveling
// on internal flash is cheap but not free, and slot changes are user-initiated.
static void save_bt_profile(ch582_profile_t p) {
    if (p == last_bt_profile) return;
    last_bt_profile     = p;
    kb_config.bt_profile = (uint8_t)p;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

// Fn+P long-press tracking: pairing only starts after a sustained hold, and
// only while in a wireless mode.
#define BT_PAIR_HOLD_MS 1000
static uint16_t bt_pair_timer = 0;
static bool     bt_pair_armed = false;

void early_hardware_init_post(void) {
    // Configure SPI0 pins for the LCD panel. SEL0 is left UNMUXED: our bare-metal bus
    // (graphics/lcd_bus.c) drives CS (B8) as a plain GPIO and must hold it low across
    // a whole DMA frame, so B8 must be free of the SPI SEL function.
    SN_PFPA->SPI_b.MISO0 = 0b11;
    SN_PFPA->SPI_b.MOSI0 = 0b11;
    SN_PFPA->SPI_b.SCK0  = 0b11;

    // Configure UART2 pins for the CH582F wireless module
    SN_PFPA->UART_b.UTXD2 = 0b11;
    SN_PFPA->UART_b.URXD2 = 0b11;
}

 void keyboard_post_init_kb(void) {
    // Windows Lock and Charging LEDs: outputs, off initially. update_leds() then
    // tracks their real state, writing only on a change.
    gpio_set_pin_output(LED_WINLOCK_PIN);
    gpio_write_pin(LED_WINLOCK_PIN, false);
    gpio_set_pin_output(LED_CHARGING_PIN);
    gpio_write_pin(LED_CHARGING_PIN, false);

    // Set up GPIO pins for the charging status inputs
    gpio_set_pin_input_high(CHARGE_CHRG_PIN);   // input with pull-up
    gpio_set_pin_input_high(CHARGE_STDBY_PIN);  // input with pull-up

    // Restore persisted config. A fresh/invalid EEPROM zero-fills the block, so a
    // 0 (or out-of-range) bt_profile falls back to slot 1.
    eeconfig_read_kb_datablock(&kb_config, 0, sizeof(kb_config));
    if (kb_config.bt_profile >= CH582_PROFILE_BT_1 && kb_config.bt_profile <= CH582_PROFILE_BT_3)
        last_bt_profile = (ch582_profile_t)kb_config.bt_profile;

    // dip_switch_init() runs BEFORE this hook, so its boot-time BT selection used
    // the default slot (EEPROM had not been read yet). Now that we have the saved
    // slot, re-select it if we booted with the slider already on Bluetooth.
    if (wireless_mode == WL_MODE_BT)
        ch582_set_profile(last_bt_profile);

    // Bring up the clock: I2C, the SN32 1 Hz counter, and a first PCF8563 seed.
    rtc_init();

    // Initialize the display subsystem (painter, fonts, images, etc.) and draw the splash screen.
    display_init_kb();

    // Chain the user hook: overriding keyboard_post_init_kb() replaces QMK's
    // default, which is what normally calls keyboard_post_init_user().
    keyboard_post_init_user();
 }

 bool dip_switch_update_kb(uint8_t index, bool active) {
    // Let the keymap handle layer logic (Mac/Win, no_gui) first.
    if(!dip_switch_update_user(index, active)) {
        return false;
    }

    if (index == 0) {  // Mac/Windows switch -- icon only (layer set by keymap)
        if (active) display_draw_windows_logo();
        else        display_draw_mac_logo();
    } else if (index == 1 || index == 2) {
        // The mode slider is a tri-state encoded by two dip pins: index 1 = BT,
        // index 2 = 2.4G, both inactive = USB. We must look at BOTH pins together
        // -- handling them independently lets the inactive sibling's "else" branch
        // call ch582_cancel_connect() and clobber connect_requested even while the
        // other mode is active (e.g. at boot in BT position), silently disabling
        // wireless key forwarding. Recompute the mode from the latched pin states.
        static bool bt_on  = false;
        static bool g24_on = false;
        if (index == 1) bt_on = active;
        if (index == 2) g24_on = active;

        // The CH582F handles both BT and 2.4G over the same UART, and QMK's
        // CONNECTION_HOST_2P4GHZ is not wired, so BOTH wireless positions map to
        // CONNECTION_HOST_BLUETOOTH (QMK then routes key reports to bt_driver ->
        // our bluetooth_send_keyboard). Our own A6 profile-select tells the
        // module which radio to use.
        if (bt_on) {
            wireless_mode = WL_MODE_BT;
            ch582_set_profile(last_bt_profile);  // resume the slot last selected
            connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
            display_draw_bluetooth_logo();
        } else if (g24_on) {
            wireless_mode = WL_MODE_24G;
            ch582_set_profile(CH582_PROFILE_PEER_24G);
            connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
            display_draw_2_4_g_logo();
        } else {
            // USB mode (both inactive): stop retrying, but keep the module alive
            // (it must stay powered to keep reporting battery level). Route key
            // reports back to USB.
            wireless_mode = WL_MODE_USB;
            ch582_cancel_connect();
            connection_set_host_noeeprom(CONNECTION_HOST_USB);
            display_draw_usb_logo();
        }
    }
    return true;
 }

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    // Let the keymap handle/override keycodes first (e.g. Mac media keys).
    if (!process_record_user(keycode, record)) {
        return false;
    }

    switch (keycode) {
        case SCR_TOG:
            if (record->event.pressed) display_toggle_power();
            return false;
        case ANIM_TOG:
            if (record->event.pressed) anim_toggle();
            return false;
        case SCR_MEDIA:
            if (record->event.pressed) display_toggle_media();
            return false;
#ifdef RGB_MATRIX_ENABLE
        // VIA-assignable RGB-matrix controls (see ak820pro.h). One step per press.
        case RGBM_TOG:  if (record->event.pressed) rgb_matrix_toggle();         return false;
        case RGBM_MOD:  if (record->event.pressed) rgb_matrix_step();           return false;
        case RGBM_RMOD: if (record->event.pressed) rgb_matrix_step_reverse();   return false;
        case RGBM_HUI:  if (record->event.pressed) rgb_matrix_increase_hue();   return false;
        case RGBM_HUD:  if (record->event.pressed) rgb_matrix_decrease_hue();   return false;
        case RGBM_SAI:  if (record->event.pressed) rgb_matrix_increase_sat();   return false;
        case RGBM_SAD:  if (record->event.pressed) rgb_matrix_decrease_sat();   return false;
        case RGBM_VAI:  if (record->event.pressed) rgb_matrix_increase_val();   return false;
        case RGBM_VAD:  if (record->event.pressed) rgb_matrix_decrease_val();   return false;
        case RGBM_SPI:  if (record->event.pressed) rgb_matrix_increase_speed(); return false;
        case RGBM_SPD:  if (record->event.pressed) rgb_matrix_decrease_speed(); return false;
#endif
        /* BT slot keys use the @isuua/edthu devctrl model: TAP = select the slot
         * (A6 <slot>, reconnect the existing bond); HOLD = select + pair (adds
         * A6 0x51 on the held slot). Select happens on press for instant feedback;
         * the pair is added if the key is still held at BT_PAIR_HOLD_MS. This
         * replaces needing a separate pair key, though Fn+P still works. */
        case BT1:  // Fn+Q -> tap: select BT1 | hold: pair BT1
        case BT2:  // Fn+W -> tap: select BT2 | hold: pair BT2
        case BT3:  // Fn+E -> tap: select BT3 | hold: pair BT3
            if (record->event.pressed) {
                if (wireless_mode == WL_MODE_BT) {
                    ch582_profile_t slot = keycode == BT1 ? CH582_PROFILE_BT_1
                                         : keycode == BT2 ? CH582_PROFILE_BT_2
                                                          : CH582_PROFILE_BT_3;
                    save_bt_profile(slot);
                    ch582_set_profile(last_bt_profile);          // tap action: select
                    connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
                    display_draw_bluetooth_logo();
                    bt_pair_timer = timer_read();                // arm hold-to-pair
                    bt_pair_armed = true;
                }
            } else if (bt_pair_armed) {
                bt_pair_armed = false;
                if (timer_elapsed(bt_pair_timer) >= BT_PAIR_HOLD_MS)
                    ch582_enter_pairing();                        // hold action: pair active slot
            }
            return false;
        case BT24G:  // Fn+R -> select 2.4G (2.4G mode only)
            if (record->event.pressed && wireless_mode == WL_MODE_24G) {
                ch582_set_profile(CH582_PROFILE_PEER_24G);
                connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
                display_draw_2_4_g_logo();
            }
            return false;
        case BT_PAIR:  // Fn+P (long press) -> pair, BT/2.4G modes only
            if (record->event.pressed) {
                // Arm a long-press only in a wireless mode; ignore in USB.
                bt_pair_armed = (wireless_mode != WL_MODE_USB);
                bt_pair_timer = timer_read();
            } else if (bt_pair_armed) {
                bt_pair_armed = false;
                if (timer_elapsed(bt_pair_timer) >= BT_PAIR_HOLD_MS) {
                    ch582_enter_pairing();
                }
            }
            return false;
        default:
            return true;
    }
}

// Apply a 7-byte time payload to the RTC:
//   [0]=year-2000 [1]=month [2]=day [3]=weekday [4]=hour [5]=min [6]=sec
// Sets both the PCF8563 (persist) and the live SN32 clock; the display picks it up
// within a second via rtc_get_time(). Returns the PCF (persistence) write status.
static bool rtc_apply_bytes(const uint8_t *p) {
    rtc_time_t t = {
        .year    = (uint16_t)(2000 + p[0]),
        .month   = p[1],
        .day     = p[2],
        .weekday = p[3],
        .hours   = p[4],
        .minutes = p[5],
        .seconds = p[6],
    };
    return rtc_set_time(&t);
}

// Clock-set command framing, identical for VIA and non-VIA builds so the host
// set-clock utility speaks ONE protocol. It's VIA's custom-value layout:
//   [SET_VALUE, RTC_CHANNEL, RTC_SET_TIME, year-2000, month, day, weekday,
//    hour, min, sec]
// The reply echoes the packet: data[0] stays SET_VALUE when handled, or becomes
// UNHANDLED (0xFF) when rejected. SET_VALUE/UNHANDLED mirror VIA's
// id_custom_set_value / id_unhandled so the same bytes work against either build.
enum {
    RTC_SET_VALUE = 0x07, // == VIA id_custom_set_value
    RTC_UNHANDLED = 0xFF, // == VIA id_unhandled
    RTC_CHANNEL   = 0x10,
    RTC_SET_TIME  = 0x01,
};

// ---------------------------------------------------------------------------
// Flash provisioning channel (Stage D). Same VIA custom-value framing as the
// clock above, on its own channel, so one host tool speaks one protocol:
//   [SET_VALUE, FLASH_CHANNEL, cmd, payload...]
// Replies are written back into the same buffer (VIA echoes it); data[3] is a
// status byte, with any returned data from data[4].
//
// Nothing here ever blocks on the chip. A page program is ~1-3 ms and a sector
// erase 50-300 ms; waiting for either inside the HID callback would stall the
// matrix scan (measured: a blocking erase costs ~6% of one scan window). So a
// command that needs an idle chip returns FS_BUSY and the host re-sends.
enum {
    FLASH_CHANNEL   = 0x11,
    // commands
    FC_INFO         = 0x01,  // -> jedec[3], asset_base[3]
    FC_ERASE        = 0x02,  // addr[3]            (4K sector)
    FC_WRITE_BEGIN  = 0x03,  // addr[3]
    FC_WRITE_DATA   = 0x04,  // len[1], bytes...
    FC_WRITE_END    = 0x05,  // flush a partial page
    FC_CRC32        = 0x06,  // addr[3], len[3]  -> crc[4]
    FC_STATUS       = 0x07,  // -> busy[1]
    FC_UNLOCK       = 0x08,  // on[1]  (animation slots)
    FC_CRC_NEXT     = 0x09,  // continue a running CRC -> crc[4] when done
    // status codes returned in data[3]
    FS_OK           = 0x00,
    FS_BUSY         = 0x01,  // chip busy -- resend this packet
    FS_REFUSED      = 0x02,  // write floor / locked / animation owns the bus
    FS_BADARG       = 0x03,
    FS_MORE         = 0x04,  // CRC still running -- send FC_CRC_NEXT
};

// CRC is computed in slices. Reading a whole range inside one HID callback
// blocks the matrix scan for the entire read -- a 184 KB verify measured a drop
// from ~1396 Hz to ~300 Hz. Everything else in this channel is non-blocking, so
// the CRC must be too: each call folds at most CRC_SLICE bytes (~0.3 ms of SPI)
// and returns FS_MORE until the range is consumed.
#define CRC_SLICE 1024u
static uint32_t crc_addr, crc_left, crc_acc;

// Streaming write state. Bytes accumulate here until a 256-byte page boundary,
// because the chip WRAPS rather than continuing when a program crosses one.
static uint32_t fw_addr  = 0;      // flash address of pg[0]
static uint16_t fw_fill  = 0;      // bytes buffered
static uint8_t  fw_pg[256];
static bool     fw_open  = false;

static uint8_t flash_flush_page(void) {
    if (!fw_fill) return FS_OK;
    if (flash_busy()) return FS_BUSY;
    if (!flash_page_program(fw_addr, fw_pg, fw_fill)) return FS_REFUSED;
    fw_addr += fw_fill;
    fw_fill  = 0;
    return FS_OK;
}

static void flash_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd = data[2];
    uint8_t *p   = &data[3];
    uint32_t a   = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];

    switch (cmd) {
        case FC_INFO: {
            uint32_t id = flash_jedec_id();
            data[3] = FS_OK;
            data[4] = (uint8_t)(id >> 16); data[5] = (uint8_t)(id >> 8); data[6] = (uint8_t)id;
            data[7] = (uint8_t)(FLASH_ASSET_BASE >> 16);
            data[8] = (uint8_t)(FLASH_ASSET_BASE >> 8);
            data[9] = (uint8_t)(FLASH_ASSET_BASE);
            return;
        }
        case FC_STATUS:
            data[3] = FS_OK;
            data[4] = flash_busy() ? 1 : 0;
            return;

        case FC_UNLOCK:
            flash_set_unlocked(p[0] != 0);
            data[3] = FS_OK;
            return;

        case FC_ERASE:
            if (flash_busy())               { data[3] = FS_BUSY;    return; }
            data[3] = flash_erase_sector(a) ? FS_OK : FS_REFUSED;
            return;

        case FC_WRITE_BEGIN:
            // A page-aligned start keeps every later flush inside one page.
            if (a & 0xFFu)                  { data[3] = FS_BADARG;  return; }
            if (!flash_writable(a, 1))      { data[3] = FS_REFUSED; return; }
            fw_addr = a; fw_fill = 0; fw_open = true;
            data[3] = FS_OK;
            return;

        case FC_WRITE_DATA: {
            if (!fw_open)                   { data[3] = FS_BADARG;  return; }
            uint8_t n = p[0];
            if (n == 0 || n > length - 4)   { data[3] = FS_BADARG;  return; }
            // Buffer, flushing whenever a full page is ready. On FS_BUSY nothing
            // is consumed, so the host simply re-sends the identical packet.
            for (uint8_t i = 0; i < n; i++) {
                fw_pg[fw_fill++] = p[1 + i];
                if (fw_fill == sizeof fw_pg) {
                    uint8_t st = flash_flush_page();
                    if (st != FS_OK) { fw_fill -= (uint16_t)(i + 1); data[3] = st; return; }
                }
            }
            data[3] = FS_OK;
            return;
        }

        case FC_WRITE_END:
            data[3] = flash_flush_page();
            if (data[3] == FS_OK) fw_open = false;
            return;

        case FC_CRC32: {
            uint32_t len = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
            if (!len)                       { data[3] = FS_BADARG;  return; }
            crc_addr = a; crc_left = len; crc_acc = 0xFFFFFFFFu;
        }
        /* fall through: fold the first slice immediately */
        case FC_CRC_NEXT: {
            if (!crc_left)                  { data[3] = FS_BADARG;  return; }
            uint32_t n = crc_left < CRC_SLICE ? crc_left : CRC_SLICE;
            crc_acc   = flash_crc32_acc(crc_acc, crc_addr, n);
            crc_addr += n;
            crc_left -= n;
            if (crc_left) { data[3] = FS_MORE; return; }
            uint32_t c = ~crc_acc;
            data[3] = FS_OK;
            data[4] = (uint8_t)(c >> 24); data[5] = (uint8_t)(c >> 16);
            data[6] = (uint8_t)(c >> 8);  data[7] = (uint8_t)c;
            return;
        }
        default:
            data[0] = RTC_UNHANDLED;
            return;
    }
}

static inline bool is_flash_cmd(const uint8_t *data, uint8_t length) {
    return length >= 4 && data[0] == RTC_SET_VALUE && data[1] == FLASH_CHANNEL;
}

static inline bool rtc_is_set_time_cmd(const uint8_t *data, uint8_t length) {
    return length >= 10 && data[0] == RTC_SET_VALUE &&
           data[1] == RTC_CHANNEL && data[2] == RTC_SET_TIME;
}

#if defined(VIA_ENABLE)

// VIA owns raw_hid_receive() and dispatches custom-value commands here. VIA echoes
// the buffer back itself -- do NOT call raw_hid_send().
void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    if (rtc_is_set_time_cmd(data, length)) {
        rtc_apply_bytes(&data[3]); // leave data[0] = SET_VALUE -> "handled"
        return;
    }
    if (is_flash_cmd(data, length)) {
        flash_command(data, length);
        return;
    }
    if (length >= 2 && data[0] == RTC_SET_VALUE && data[1] == MEDIA_CHANNEL) {
        media_hid_command(data, length);   // now-playing state (channel 0x12)
        return;
    }
    if (length >= 2 && data[0] == RTC_SET_VALUE && data[1] == STREAM_CHANNEL) {
        stream_hid_command(data, length);  // spectator pixel stream (channel 0x13)
        return;
    }
    data[0] = RTC_UNHANDLED;
}

#else // no VIA: handle the same packet directly and echo it back like VIA would.

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (rtc_is_set_time_cmd(data, length)) {
        rtc_apply_bytes(&data[3]);
    } else if (is_flash_cmd(data, length)) {
        flash_command(data, length);
    } else if (length >= 2 && data[0] == RTC_SET_VALUE && data[1] == MEDIA_CHANNEL) {
        media_hid_command(data, length);   // now-playing state (channel 0x12)
    } else if (length >= 2 && data[0] == RTC_SET_VALUE && data[1] == STREAM_CHANNEL) {
        stream_hid_command(data, length);  // spectator pixel stream (channel 0x13)
    } else {
        data[0] = RTC_UNHANDLED;
    }
    raw_hid_send(data, length);
}

#endif


static void update_leds(void) {
    // Charging LED: on only while actively charging -- CHRG low (active) AND
    // STDBY high (not "done").
    static bool last_chrg = false;
    bool is_charging = !gpio_read_pin(CHARGE_CHRG_PIN) && gpio_read_pin(CHARGE_STDBY_PIN);
    if (is_charging != last_chrg) {
        gpio_write_pin(LED_CHARGING_PIN, is_charging);
        last_chrg = is_charging;
    }

    // Windows Lock LED: mirrors the GUI-lock flag.
    static bool last_winlock = false;
    bool winlock = keymap_config.no_gui;
    if (winlock != last_winlock) {
        gpio_write_pin(LED_WINLOCK_PIN, winlock);
        last_winlock = winlock;
    }
}

__attribute__((weak)) void display_housekeeping_task(void) {}

static void kb_sleep_task(void);   // idle-sleep (B-lite) + USB-suspend (A); below

void housekeeping_task_kb(void) {

    // Throttle the housekeeping to 10 Hz
    static uint32_t last_t = 0;
    if (timer_elapsed32(last_t) >= 100) {
        last_t = timer_read32();

        kb_sleep_task();                  // idle-sleep (B-lite) + USB-suspend (A)
        update_leds();
#ifdef AK_DEBUG_SUSPEND
        // Diagnostic (uncommitted): expose the two decisive signals on the
        // indicator LEDs so we can see, while the host sleeps, whether the bus
        // actually suspends and whether the wired-host gate passes.
        //   WinLock LED  (C15) = USB_DRIVER.state == USB_SUSPENDED
        //   Charging LED (B18) = USB_DRIVER.state == USB_ACTIVE
        // Normal wired use: Charging on, WinLock off. On host sleep, watch which
        // (if either) changes -- tells us the real state the link drops to.
        gpio_write_pin(LED_WINLOCK_PIN,  USB_DRIVER.state == USB_SUSPENDED);
        gpio_write_pin(LED_CHARGING_PIN, USB_DRIVER.state == USB_ACTIVE);
#endif
        if (!anim_active()) rtc_task();   // RTC I2C (port A) glitches the flash SPI1 pins (A12/A13) mid-DMA
        anim_task();                      // one animation frame per 100 ms
        display_housekeeping_task();
    }

    // Chain the user hook
    housekeeping_task_user();
}

// --- Display/RGB sleep (B-lite idle timer + A USB-suspend) -------------------
// Blank the LCD (panel sleep-in + backlight off) and the RGB matrix on two
// triggers, whichever comes first, and restore on wake:
//
//   (A) a genuine USB bus suspend, gated on the wired host. We CANNOT use QMK's
//       suspend_power_down_kb hooks -- the tmk suspend loop that calls them is
//       compiled out by NO_USB_STARTUP_CHECK (set by BLUETOOTH_ENABLE), so we
//       poll USB_DRIVER.state (kept current by usb_event_queue_task() off the
//       SN32 BUS_SUSPEND/WAKEUP IRQs) ourselves. NOTE: many hosts (observed:
//       macOS, AC and battery) keep the bus USB_ACTIVE through sleep and never
//       fire this -- hence (B).
//
//   (B) inactivity: DISPLAY_SLEEP_TIMEOUT_MS with no key/encoder input, using
//       QMK's own last_input_activity_elapsed() (covers keys and the encoder).
//       The MCU keeps scanning, so the waking key still types; the display just
//       relights one housekeeping tick later.
static void kb_sleep_task(void) {
    static bool asleep = false;

    bool host_suspended = (connection_get_host() == CONNECTION_HOST_USB) &&
                          (USB_DRIVER.state == USB_SUSPENDED);
    bool idle = (DISPLAY_SLEEP_TIMEOUT_MS > 0) &&
                (last_input_activity_elapsed() >= (uint32_t)DISPLAY_SLEEP_TIMEOUT_MS);
    bool want_sleep = host_suspended || idle;

    if (want_sleep && !asleep) {
        asleep = true;
        rgb_matrix_set_suspend_state(true);   // render effect 0 (all off)
        display_enter_sleep();                // panel sleep-in + backlight off
    } else if (!want_sleep && asleep) {
        asleep = false;
        rgb_matrix_set_suspend_state(false);
        display_exit_sleep();                 // sleep-out + backlight on + repaint
    }
}
