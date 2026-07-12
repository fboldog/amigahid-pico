/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * human interface device handling.
 *
 * if this file looks kind of sketchy, i'm just at the beginning of my
 * understanding of the tinyusb stack. apologies whilst i get to grips with
 * the terminology, or mapping it to what i understand of it.
 */

// these reside within the tinyusb sdk and are not part of this project source
#include "bsp/board.h"
#include "tusb.h"

// other includes
#include <stdint.h>

#include "input_bridge.h"
#include "platform/amiga/keyboard_serial_io.h"
#include "tusb_config.h"
#include "hid_gamepad.h"
#include "platform/amiga/keyboard_serial_io.h"  // amiga only, for now, until i get hold of an ST :D
#include "platform/amiga/keyboard.h"
#include "platform/amiga/quad_mouse.h"
#include "platform/amiga/joystick.h"
#include "util/output.h"
#include "util/debug_cons.h"
#include "usb_hid.h"

#include <string.h>

// maximum number of reports per hid device
#define MAX_REPORT 4

// controller fire mapping: which report-button bits assert each amiga fire line.
// bit N corresponds to hid report button N (see the "[joy] buttons=" serial log).
// reference pad layout: Y=bit0, B=bit1, A=bit2, X=bit3, L=bit4, R=bit5,
// menu=bit6, home=bit7. each mask may OR several buttons together.
#define JOY_FIRE1_BUTTONS (1u << 0) // Y -> fire 1 (db9 pin 6)
#define JOY_FIRE2_BUTTONS (1u << 1) // B -> fire 2 (db9 pin 9)

// textual representations of attached devices
const uint8_t hid_protocol_type[] = { AP_H_UNKNOWN, AP_H_KEYBOARD, AP_H_MOUSE };

typedef struct
{
    bool mounted;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} usb_hid_slot_t;

// per-instance controller (joystick/gamepad) state; populated at mount time
static struct _controller_info
{
    bool is_controller;
    gamepad_layout_t layout;
    gamepad_state_t last;
} controller[CFG_TUH_HID];

typedef struct
{
    uint8_t dev_addr;
    uint8_t instance;
} usb_keyboard_led_ctx_t;

static usb_hid_slot_t hid_info[CFG_TUH_HID];

static int8_t usb_hid_find_slot(uint8_t dev_addr, uint8_t instance);
static int8_t usb_hid_allocate_slot(uint8_t dev_addr, uint8_t instance);
static void usb_hid_set_keyboard_leds(void *ctx, uint8_t led_report);
static void process_report(uint8_t slot, uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);
static void handle_event_keyboard(uint8_t slot, uint8_t dev_addr, uint8_t instance,
    hid_keyboard_report_t const *report);
static void handle_event_mouse(uint8_t slot, hid_mouse_report_t const *report);
static void handle_event_gamepad(uint8_t instance, uint8_t const *report, uint16_t len);
static bool descriptor_is_controller(uint8_t instance);
static void dump_report_descriptor(uint8_t dev_addr, uint8_t instance, uint8_t const *desc, uint16_t len);

void hid_app_task(void)
{
    // null function to satisfy stack
}

void usb_hid_sync_keyboard_leds(void)
{
    uint8_t led_report = amiga_caps_lock() ? KEYBOARD_LED_CAPSLOCK : 0;

    for (uint8_t slot = 0; slot < CFG_TUH_HID; slot++) {
        if (!hid_info[slot].mounted)
            continue;

        if (tuh_hid_interface_protocol(hid_info[slot].dev_addr, hid_info[slot].instance) !=
            HID_ITF_PROTOCOL_KEYBOARD) {
            continue;
        }

        tuh_hid_set_report(hid_info[slot].dev_addr, hid_info[slot].instance, 0,
            HID_REPORT_TYPE_OUTPUT, &led_report, 1);
    }
}

static int8_t usb_hid_find_slot(uint8_t dev_addr, uint8_t instance)
{
    for (uint8_t slot = 0; slot < CFG_TUH_HID; slot++) {
        if (hid_info[slot].mounted && (hid_info[slot].dev_addr == dev_addr) && (hid_info[slot].instance == instance))
            return (int8_t)slot;
    }

    return -1;
}

static int8_t usb_hid_allocate_slot(uint8_t dev_addr, uint8_t instance)
{
    int8_t slot = usb_hid_find_slot(dev_addr, instance);

    if (slot >= 0)
        return slot;

    for (uint8_t i = 0; i < CFG_TUH_HID; i++) {
        if (!hid_info[i].mounted) {
            hid_info[i].mounted = true;
            hid_info[i].dev_addr = dev_addr;
            hid_info[i].instance = instance;
            hid_info[i].report_count = 0;
            input_bridge_reset(i);
            return (int8_t)i;
        }
    }

    return -1;
}

static void usb_hid_set_keyboard_leds(void *ctx, uint8_t led_report)
{
    usb_keyboard_led_ctx_t *led_ctx = (usb_keyboard_led_ctx_t *)ctx;

    tuh_hid_set_report(led_ctx->dev_addr, led_ctx->instance, 0, HID_REPORT_TYPE_OUTPUT, &led_report, 1);
}

// callback functions; methods below suffixed with "_cb" are called by tinyusb
// when processing certain hid events.

/**
 * HID connection callback
 *
 * @param dev_addr    Address of connected device
 * @param instance    Instance of connected device
 * @param desc_report Report descriptor
 * @param desc_len    Length of descriptor
 */
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t hid_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    int8_t slot = usb_hid_allocate_slot(dev_addr, instance);
    bool receive_ok;

    // start from a clean controller slot for this instance
    controller[instance].is_controller = false;
    memset(&controller[instance].layout, 0, sizeof(controller[instance].layout));
    memset(&controller[instance].last, 0, sizeof(controller[instance].last));

    // dump the raw report descriptor so an unrecognised controller can be
    // diagnosed from the serial console (no-op unless DEBUG_MESSAGES is set)
    dump_report_descriptor(dev_addr, instance, desc_report, desc_len);

    if (slot < 0)
        return;

    // this part doesn't entirely make sense to me; hid devices come in two modes, boot protocol and report;
    // as i understand it, boot proto is intended for simplistic software such as bios which don't want to
    // implement a full stack. so if we're not in boot proto mode, display... something?
    // this might be number of interfaces on a device (think wireless kbd+mouse receiver). maybe. speculation.
    if (hid_protocol == HID_ITF_PROTOCOL_NONE) {
        hid_info[slot].report_count = tuh_hid_parse_report_descriptor(hid_info[slot].report_info, MAX_REPORT, desc_report, desc_len);
        // ahprintf("[PLUG] %02x report(s)\n", hid_info[instance].report_count);

        // a device with no boot protocol that advertises a joystick or gamepad
        // usage is a controller; learn its report layout so we can decode events
        if (descriptor_is_controller(instance)) {
            if (hid_gamepad_parse_descriptor(&controller[instance].layout, desc_report, desc_len)) {
                controller[instance].is_controller = true;
                gamepad_layout_t const *l = &controller[instance].layout;
                ahprintf(
                    "\n[joy] controller mounted (dev %d inst %d): report id %02x, "
                    "x=%d y=%d hat=%d buttons=%d\n",
                    dev_addr, instance, l->report_id,
                    l->x.present, l->y.present, l->hat.present, l->button_count
                );
            }
        }
    }

    // announce the plug event with the correct device class
    dbgcons_plug(controller[instance].is_controller ? AP_H_CONTROLLER : hid_protocol_type[hid_protocol]);

    receive_ok = tuh_hid_receive_report(dev_addr, instance);
    dbgcons_hid_status(dev_addr, instance, hid_protocol, receive_ok, hid_info[slot].report_count, true);

    if (!receive_ok) {
        // ahprintf("[PLUG] warning! report request failed; delayed initialisation?\n");
    }
}

/**
 * Inspect the parsed report descriptor for this instance and decide whether it
 * describes a joystick or gamepad (as opposed to a keyboard/mouse in report
 * mode, which also present on the desktop usage page).
 *
 * @param instance  Instance whose hid_info was just parsed
 * @return true      Device advertises a joystick or gamepad usage
 */
static bool descriptor_is_controller(uint8_t instance)
{
    for (uint8_t i = 0; i < hid_info[instance].report_count; i++) {
        tuh_hid_report_info_t const *ri = &hid_info[instance].report_info[i];
        if (ri->usage_page == HID_USAGE_PAGE_DESKTOP
            && (ri->usage == HID_USAGE_DESKTOP_JOYSTICK || ri->usage == HID_USAGE_DESKTOP_GAMEPAD)) {
            return true;
        }
    }

    return false;
}

/**
 * Dump a raw HID report descriptor to the serial console in 16-byte rows, so an
 * unrecognised controller's descriptor can be captured and decoded by hand.
 * Compiles to nothing meaningful unless DEBUG_MESSAGES is enabled.
 *
 * @param dev_addr  Device address (for context in the log)
 * @param instance  Instance number (for context in the log)
 * @param desc      Raw report descriptor bytes (may be NULL)
 * @param len       Number of descriptor bytes
 */
static void dump_report_descriptor(uint8_t dev_addr, uint8_t instance, uint8_t const *desc, uint16_t len)
{
    ahprintf("\n[hid] report descriptor (dev %d inst %d, %d bytes):\n", dev_addr, instance, len);

    if (desc == NULL || len == 0) {
        ahprintf("  <none - descriptor larger than CFG_TUH_ENUMERATION_BUFSIZE?>\n");
        return;
    }

    // 16 bytes per line: "  0000: xx xx ... \n" (3 chars/byte + offset + nul)
    char line[8 + 16 * 3 + 1];

    for (uint16_t off = 0; off < len; off += 16) {
        int pos = sprintf(line, "  %04x:", off);
        for (uint16_t i = 0; i < 16 && (off + i) < len; i++)
            pos += sprintf(line + pos, " %02x", desc[off + i]);

        ahprintf("%s\n", line);
    }
}

/**
 * HID disconnection callback
 *
 * @param dev_addr    Address of disconnected device
 * @param instance    Instance of disconnected device
 */
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    if (controller[instance].is_controller) {
        // release every joy port line so a yanked controller leaves nothing asserted
        amiga_joystick_state_t idle = { 0 };
        amiga_joystick_apply(&idle);

        controller[instance].is_controller = false;
        dbgcons_unplug(AP_H_CONTROLLER);
        return;
    }

    uint8_t hid_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    int8_t slot = usb_hid_find_slot(dev_addr, instance);

    dbgcons_unplug(hid_protocol_type[hid_protocol]);
    dbgcons_hid_status(dev_addr, instance, hid_protocol, true, slot >= 0 ? hid_info[slot].report_count : 0, false);

    if (slot >= 0) {
        input_bridge_disconnect((uint8_t)slot);
        hid_info[slot].mounted = false;
        hid_info[slot].report_count = 0;
    }
}

/**
 * HID report event has occurred
 *
 * @param dev_addr    Address of device sending report
 * @param instance    Instance of device sending report
 * @param report      Address of report structure
 * @param len         Length of report structure
 */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    // controllers aren't boot-protocol devices, so they'd otherwise fall into
    // the generic report path below; intercept them first.
    if (controller[instance].is_controller) {
        handle_event_gamepad(instance, report, len);
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    uint8_t const hid_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    int8_t slot = usb_hid_find_slot(dev_addr, instance);

    if (slot < 0) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    switch (hid_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            handle_event_keyboard((uint8_t)slot, dev_addr, instance, (hid_keyboard_report_t const *)report);
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            handle_event_mouse((uint8_t)slot, (hid_mouse_report_t const *)report);
            break;

        default:
            // if report was not immediately identifiable as a keyboard event, read the usage page;
            // some reports have a classifier as "desktop" for media keys, power, or are just encapsulated.
            process_report((uint8_t)slot, dev_addr, instance, report, len);
            break;
    }

    // continue to request to receive report
    tuh_hid_receive_report(dev_addr, instance);
    // if (!tuh_hid_receive_report(dev_addr, instance))
        // ahprintf("[ERROR] unable to receive hid event report\n");
}

/**
 * HID Boot Protocol keeps a six-key buffer of pressed keys. Return true if keycode is "pressed".
 *
 * @param report    Address of hid_keyboard_report_t struct
 * @param keycode   Keycode to look for
 * @return true     Key is currently pressed
 * @return false    Key is not currently pressed
 */
/**
 * Process incoming event and pass off to device-centric handler.
 *
 * @param slot      Source slot for this reporting device
 * @param dev_addr  Address of reporting device
 * @param instance  Instance of reporting device
 * @param report    Address of the report data structure
 * @param len       Size of the report event
 */
static void process_report(uint8_t slot, uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    uint8_t const report_count = hid_info[slot].report_count;
    tuh_hid_report_info_t *report_info_arr = hid_info[slot].report_info;
    tuh_hid_report_info_t *report_info = NULL;

    if (report_count == 1 && report_info_arr[0].report_id == 0) {
        // single report with id of 0 is a single-shot report
        report_info = &report_info_arr[0];
    } else {
        // (possibly, but not mandatory) array of reports
        // @todo i don't understand why, if we're facing an array of reports, why we're only going to process the
        // report containing the identifier at byte 0, but maybe one day that will make sense.
        uint8_t const report_id = report[0];

        // locate the array member matching the report ID we are intending to process (then exit the loop)
        for (uint8_t i = 0; i < report_count; i++) {
            if (report_id == report_info_arr[i].report_id) {
                report_info = &report_info_arr[i];
                break;
            }
        }

        report++;
        len--;
    }

    if (!report_info) {
        // ahprintf("[ERROR] report_info pointer was not set during process_report(), value: $%x, report_count was %d\n", report_info, report_count);
        return;
    }

    // process report event based on usage_page class and usage device
    if (report_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (report_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                // keyboard event; let's hope it appears as a boot proto event or else this will break
                handle_event_keyboard(slot, dev_addr, instance, (hid_keyboard_report_t const *)report);
                break;

            case HID_USAGE_DESKTOP_MOUSE:
                // mouse event
                handle_event_mouse(slot, (hid_mouse_report_t const *)report);
                break;

            default:
                break;
        }
    }
}

/**
 * Handle the mouse event sent to us.
 *
 * @param dev_addr  Device address of report
 * @param instance  Instance number of reporting device
 * @param report    Address of hid_mouse_report_t structure of current mouse event
 */
static void handle_event_mouse(uint8_t slot, hid_mouse_report_t const *report)
{
    input_bridge_handle_mouse(slot, report);
}

/**
 * Handle a controller (joystick/gamepad) event and drive the amiga joy port.
 *
 * The report is decoded via the layout discovered at mount time into a
 * normalised up/down/left/right + button state. Button 0 maps to Fire1 (db9
 * pin 6), button 1 to Fire2 (db9 pin 9). Both the left analog stick and the
 * hat/d-pad drive the four directions.
 *
 * @param instance  Instance number of reporting device
 * @param report    Raw report as delivered by tinyusb (may include leading id)
 * @param len       Length of report
 */
static void handle_event_gamepad(uint8_t instance, uint8_t const *report, uint16_t len)
{
    gamepad_state_t state;

    if (!hid_gamepad_decode(&controller[instance].layout, report, len, &state))
        return;

    amiga_joystick_state_t joy = {
        .up    = state.up,
        .down  = state.down,
        .left  = state.left,
        .right = state.right,
        .fire1 = (state.buttons & JOY_FIRE1_BUTTONS) != 0,
        .fire2 = (state.buttons & JOY_FIRE2_BUTTONS) != 0,
    };

    amiga_joystick_apply(&joy);

    // only log when something actually changed; controllers poll continuously
    if (memcmp(&state, &controller[instance].last, sizeof(state)) != 0) {
        dbgcons_joystick(joy.up, joy.down, joy.left, joy.right, joy.fire1, joy.fire2);
        // raw button bitfield (bit N = report button N) to help identify which
        // physical buttons map to which bit for the fire mapping
        ahprintf("[joy] buttons=0x%08lx\n", (unsigned long)state.buttons);
        controller[instance].last = state;
    }
}

/**
 * Handle the keyboard event sent to us.
 *
 * @param slot      Source slot for this reporting device
 * @param dev_addr  Device address of report
 * @param instance  Instance number of reporting device
 * @param report    Address of hid_keyboard_report_t structure of current keyboard event (boot proto?)
 */
static void handle_event_keyboard(uint8_t slot, uint8_t dev_addr, uint8_t instance,
    hid_keyboard_report_t const *report)
{
    usb_keyboard_led_ctx_t led_ctx = { dev_addr, instance };
    input_bridge_keyboard_sink_t sink = { usb_hid_set_keyboard_leds, &led_ctx };

    input_bridge_handle_keyboard(slot, report, &sink);
}
