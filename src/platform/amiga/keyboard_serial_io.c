/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * amiga keyboard serial interface.
 */

#include "config.h"
#include "keyboard_serial_io.h"
#include "keyboard.h"
#include "keyboard.pio.h" // generated at compile time
#include "usb_hid.h"
#include "util/output.h"
#include "util/debug_cons.h"

#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/gpio.h"
#include "class/hid/hid.h"

enum _sync_state { IDLE, SYNC };
// don't optimise variables hit by the timer isr (timer callback?)
volatile enum _sync_state sync_state = IDLE;
volatile bool clock_timer_fired = false;

// caps lock will be read by the hid loop
bool caps_lock = false;
static bool ctrl = false;
static bool lamiga = false;
static bool ramiga = false;
static bool in_reset = false;
static bool local_reset_active = false;
static bool external_reset_active = false;
static bool reset_line_was_low = false;
static bool reset_output_asserted = false;
static bool boot_reset_hold_active = false;

enum _keyboard_pin_state { LOW, HIGH };

#define AKB_EVENT_QUEUE_DEPTH 64
#define AKB_STARTUP_INIT_DELAY_MS 1000
#define AKB_STARTUP_TERM_DELAY_MS 200

typedef enum
{
    AKB_STARTUP_WAIT_INIT_POWER = 0,
    AKB_STARTUP_WAIT_TERM_POWER,
    AKB_STARTUP_READY,
} amiga_keyboard_startup_state_t;

typedef enum
{
    AKB_EVENT_SEND = 0,
    AKB_EVENT_ASSERT_RESET,
    AKB_EVENT_RELEASE_RESET,
} amiga_keyboard_event_type_t;

typedef struct
{
    amiga_keyboard_event_type_t type;
    uint8_t keycode;
    bool up;
} amiga_keyboard_event_t;

static queue_t keyboard_event_queue;
static amiga_keyboard_startup_state_t startup_state;
static absolute_time_t startup_deadline;

static inline void amiga_reset_stateful_flags(void)
{
    if (caps_lock) {
        caps_lock = false;
        usb_hid_sync_keyboard_leds();
    }
}

static void amiga_clear_queued_events(void)
{
    amiga_keyboard_event_t event;

    while (queue_try_remove(&keyboard_event_queue, &event))
        ;
}

static bool amiga_queue_event(amiga_keyboard_event_t const *event, bool critical)
{
    if (queue_try_add(&keyboard_event_queue, event))
        return true;

    if (critical) {
        amiga_clear_queued_events();

        if (queue_try_add(&keyboard_event_queue, event))
            return true;
    }

    ahprintf("[akb] queue full, dropped event type %u\n", event->type);
    return false;
}

// @todo this is copy-pasta from quad_mouse; move to util/io.c
static inline void _keyboard_gpio_set(uint gpio, enum _keyboard_pin_state state)
{
    if (state == LOW) {
        gpio_put(gpio, 0);
        gpio_set_dir(gpio, GPIO_OUT);
        return;
    }

    // assume it's high otherwise
    gpio_set_dir(gpio, GPIO_IN);
}

/*
// _.-._.-._ @todo i've not been doing sync correctly for sooooooo long; fix/remove? -._.-._.-
int64_t sync_timer_cb(alarm_id_t id, void *user_data)
{
    clock_timer_fired = true;
    _keyboard_gpio_set(KBD_AMIGA_DAT, LOW);
    sync_state = SYNC;
}
*/

uint8_t get_modifier_from_hid(hid_keyboard_modifier_bm_t modifier)
{
    const hid_to_amiga_modifier_t *mapping;

    mapping = mapHidModToAmiga;
    while (mapping->hid_modifier != 0UL) {
        if (modifier == mapping->hid_modifier) {
            return mapping->amiga_keycode;
        }
        mapping++;
    }

    return 0;
}

static void amiga_send_immediate(uint8_t keycode, bool up)
{
    uint8_t bit_position, bit_mask = 0x80, sendcode;

    // copy input code, roll left, move msb to lsb
    sendcode = keycode | (up ? 0x80 : 0x00);
    sendcode <<= 1;
    if (up || (keycode & 0x80))
        sendcode |= 1;

    for (bit_position = 0; bit_position < 8; bit_position++) {
        if (sendcode & bit_mask)
            _keyboard_gpio_set(KBD_AMIGA_DAT, LOW);
        else
            _keyboard_gpio_set(KBD_AMIGA_DAT, HIGH);

        // hold /dat for 20us before pulsing /clk, then wait 50us before next bit
        sleep_us(20);
        _keyboard_gpio_set(KBD_AMIGA_CLK, LOW);
        sleep_us(20);
        _keyboard_gpio_set(KBD_AMIGA_CLK, HIGH);
        sleep_us(50); // @todo should be 20?

        // shift the bit pattern for next iteration
        bit_mask >>= 1;
    }

    // set /dat to input for 5ms to signal end of key
    _keyboard_gpio_set(KBD_AMIGA_DAT, HIGH);
    sleep_ms(5);
}

static bool amiga_startup_service(void)
{
    if (startup_state == AKB_STARTUP_READY)
        return false;

    if (boot_reset_hold_active)
        return true;

    if (in_reset)
        return true;

    if (absolute_time_diff_us(get_absolute_time(), startup_deadline) > 0)
        return true;

    if (startup_state == AKB_STARTUP_WAIT_INIT_POWER) {
        amiga_send_immediate(AMIGA_INITPOWER, false);
        startup_deadline = make_timeout_time_ms(AKB_STARTUP_TERM_DELAY_MS);
        startup_state = AKB_STARTUP_WAIT_TERM_POWER;
        return true;
    }

    amiga_send_immediate(AMIGA_TERMPOWER, false);
    startup_state = AKB_STARTUP_READY;
    return true;
}

void amiga_init()
{
    // setup digital mode, direction and active high/low on /clk, /dat and /rst.
    gpio_init(KBD_AMIGA_DAT);
    gpio_init(KBD_AMIGA_CLK);
    gpio_init(KBD_AMIGA_RST);

    gpio_set_function(KBD_AMIGA_DAT, GPIO_FUNC_SIO);
    gpio_set_function(KBD_AMIGA_CLK, GPIO_FUNC_SIO);
    gpio_set_function(KBD_AMIGA_RST, GPIO_FUNC_SIO);
    gpio_pull_up(KBD_AMIGA_RST);

    // All pins are active low. Leave data and clock open-drain inactive.
    _keyboard_gpio_set(KBD_AMIGA_DAT, HIGH);
    _keyboard_gpio_set(KBD_AMIGA_CLK, HIGH);

#ifdef HOLD_THE_DOOR
    // Assert the dedicated reset output while TinyUSB initialises. This is
    // intentionally separate from amiga_assert_reset(), which also holds clock
    // low for big-box Amigas and performs a blocking 500 ms delay.
    _keyboard_gpio_set(KBD_AMIGA_RST, LOW);
    boot_reset_hold_active = true;
    reset_output_asserted = true;
#else
    _keyboard_gpio_set(KBD_AMIGA_RST, HIGH);
#endif

    reset_line_was_low = gpio_get(KBD_AMIGA_RST) == 0;

    queue_init(&keyboard_event_queue, sizeof(amiga_keyboard_event_t), AKB_EVENT_QUEUE_DEPTH);
    startup_state = AKB_STARTUP_WAIT_INIT_POWER;
    startup_deadline = make_timeout_time_ms(AKB_STARTUP_INIT_DELAY_MS);
}

void amiga_release_boot_reset()
{
    if (!boot_reset_hold_active)
        return;

    _keyboard_gpio_set(KBD_AMIGA_RST, HIGH);
    boot_reset_hold_active = false;
    reset_output_asserted = false;
    reset_line_was_low = false;

    // Start the keyboard power-up timing when the Amiga is allowed to boot.
    startup_deadline = make_timeout_time_ms(AKB_STARTUP_INIT_DELAY_MS);
}

bool amiga_caps_lock()
{
    return caps_lock;
}

void amiga_hid_send(uint8_t hidcode, bool up)
{
    if (mapHidToAmiga[hidcode] == AMIGA_UNKNOWN) {
        // ahprintf("[akb] cowardly refusing to send $ff to the amiga\n");
        return;
    }

    dbgcons_amiga_key(hidcode, mapHidToAmiga[hidcode], up ? "u" : "d");

    amiga_send(mapHidToAmiga[hidcode], up);
}

void amiga_hid_modifier(hid_keyboard_modifier_bm_t modifier, bool up)
{
    uint8_t amiga_code;
    amiga_code = get_modifier_from_hid(modifier);

    // @todo indicate the modifier state in dbgcons, somehow
    dbgcons_amiga_key(0, amiga_code, up ? "u" : "d");

    amiga_send(amiga_code, up);
}

void amiga_send(uint8_t keycode, bool up)
{
    amiga_keyboard_event_t event = {
        .type = AKB_EVENT_SEND,
        .keycode = keycode,
        .up = up,
    };

    // we don't care about caps lock coming up; ignore it
    if ((keycode == AMIGA_CAPSLOCK) && up)
        return;

    // check for caps lock going down and toggle; rewrite the 'up' parameter
    if (keycode == AMIGA_CAPSLOCK) {
        // amiga caps lock is odd; when it's on, it sends down code but no up, and vice versa when it comes off
        up = caps_lock;
        caps_lock = !caps_lock;

        // ahprintf("[akb] caps lock %s\n", caps_lock ? "ON" : "OFF");
    }

    if (keycode == AMIGA_CTRL)
        ctrl = !up;
    if (keycode == AMIGA_LAMIGA)
        lamiga = !up;
    if (keycode == AMIGA_RAMIGA)
        ramiga = !up;

    if ((ctrl && lamiga && ramiga) && !in_reset) {
        in_reset = true;
        local_reset_active = true;
        external_reset_active = false;
        amiga_clear_queued_events();
        amiga_reset_stateful_flags();
        event.type = AKB_EVENT_ASSERT_RESET;
        event.keycode = 0;
        event.up = false;
        amiga_queue_event(&event, true);
        return;
    }

    if (local_reset_active && !(ctrl && lamiga && ramiga)) {
        local_reset_active = false;
        in_reset = false;
        event.type = AKB_EVENT_RELEASE_RESET;
        event.keycode = 0;
        event.up = false;
        amiga_queue_event(&event, true);
        event.type = AKB_EVENT_SEND;
        event.keycode = keycode;
        event.up = up;
    }

    /*
     * Queue the transmit work so the HID callback path can return to USB
     * servicing quickly. State transitions still happen immediately above.
     */
    if (!in_reset)
        amiga_queue_event(&event, false);

    // @todo we _should_ be checking that the amiga has acked the code by watching /dat
    // for a lwo pulse. according to adcd2.1, while the computer cannot detect
    // out-of-sync, the keyboard can by looking for the pulse, sending $f9 then the
    // repeated code.
}

void amiga_assert_reset()
{
    // ahprintf("[akb] *** RESET BEING ASSERTED ***\n");
    _keyboard_gpio_set(KBD_AMIGA_RST, LOW);

    // this will hold clk low for 500ms, which is useful for a2000/3000/4000 and perhaps cdtv/cd32.
    // the pause is beneficial for ensuring the reset signal is picked up by the amiga.
    // (thanks @reinauer for submitting this in issue #31 and testing on your a3000)
    _keyboard_gpio_set(KBD_AMIGA_CLK, LOW);
    reset_output_asserted = true;
    sleep_us(500000);
}

void amiga_release_reset()
{
    // ahprintf("[akb] *** RESET BEING RELEASED ***\n");
    _keyboard_gpio_set(KBD_AMIGA_RST, HIGH);
    _keyboard_gpio_set(KBD_AMIGA_CLK, HIGH);
    reset_output_asserted = false;
}

void amiga_service()
{
    bool reset_line_low = gpio_get(KBD_AMIGA_RST) == 0;

    /*
     * On A500-style hookups the reset line can be observed from the Pico
     * side when the Amiga asserts a hard reset externally. Big-box systems
     * generally do not wire this line, so this path is best-effort only.
     */
    if (!local_reset_active && !reset_output_asserted) {
        if (reset_line_low && !reset_line_was_low) {
            external_reset_active = true;
            in_reset = true;
            amiga_clear_queued_events();
            amiga_reset_stateful_flags();
        } else if (!reset_line_low && reset_line_was_low && external_reset_active) {
            external_reset_active = false;
            in_reset = false;
        }
    }

    reset_line_was_low = reset_line_low;

    if ((sync_state == SYNC) && clock_timer_fired) {
        // @todo THIS IS WRONG
        _keyboard_gpio_set(KBD_AMIGA_RST, HIGH);
        sync_state = IDLE;
        clock_timer_fired = false;
    }

    amiga_keyboard_event_t event;

    // A reset chord can arrive over USB while keyboard power-up is pending.
    // Process its control events first, while keeping ordinary keys queued.
    if (queue_try_peek(&keyboard_event_queue, &event) && event.type != AKB_EVENT_SEND) {
        queue_try_remove(&keyboard_event_queue, &event);

        if (event.type == AKB_EVENT_ASSERT_RESET)
            amiga_assert_reset();
        else
            amiga_release_reset();

        return;
    }

    // Keep USB and the other main-loop services running during the keyboard
    // power-up delays. Input received before TERMPOWER remains queued.
    if (amiga_startup_service())
        return;

    if (!queue_try_remove(&keyboard_event_queue, &event))
        return;

    switch (event.type) {
        case AKB_EVENT_SEND:
            amiga_send_immediate(event.keycode, event.up);
            break;

        case AKB_EVENT_ASSERT_RESET:
            amiga_assert_reset();
            break;

        case AKB_EVENT_RELEASE_RESET:
            amiga_release_reset();
            break;
    }
}
