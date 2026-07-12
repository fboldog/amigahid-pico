/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * generic usb hid gamepad / joystick handling.
 *
 * tinyusb does not ship a generic gamepad parser (its host example hardcodes a
 * single dualshock 4), and real controllers vary wildly in report layout. this
 * module walks a device's hid report descriptor to discover *where* the x/y
 * axes, hat switch and buttons live, then decodes each incoming report into a
 * normalised direction + button state that the platform layer can map onto the
 * amiga joy port.
 */

#ifndef _HID_GAMEPAD_H
#define _HID_GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

// discovered position of a single field within a report (bit offset + width)
typedef struct {
    bool present;
    uint16_t offset;    // bit offset within the report payload (report id stripped)
    uint8_t size;       // width in bits
    int32_t logical_min;
    int32_t logical_max;
} gamepad_field_t;

// everything we learned about a controller's input report from its descriptor
typedef struct {
    bool valid;         // true once at least one useful field was located
    uint8_t report_id;  // report id carrying these fields (0 = device uses no ids)

    gamepad_field_t x;      // left-stick / main x axis
    gamepad_field_t y;      // left-stick / main y axis
    gamepad_field_t hat;    // d-pad / hat switch (usually 4 bits, 8 directions)

    bool has_buttons;
    uint16_t button_offset; // bit offset of the first button
    uint8_t button_count;   // number of 1-bit buttons
} gamepad_layout_t;

// normalised controller state decoded from a report
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    uint32_t buttons;   // bit N set => button N currently pressed (button 0 first)
} gamepad_state_t;

/**
 * parse a hid report descriptor, populating layout with the location of the
 * x/y axes, hat switch and buttons.
 *
 * @param layout    output; zeroed then filled in
 * @param desc      raw report descriptor bytes
 * @param desc_len  length of descriptor
 * @return true     at least one usable control (axis, hat or button) was found
 */
bool hid_gamepad_parse_descriptor(gamepad_layout_t *layout, const uint8_t *desc, uint16_t desc_len);

/**
 * decode a single incoming report into normalised state using a previously
 * parsed layout. handles report-id stripping internally.
 *
 * @param layout    layout obtained from hid_gamepad_parse_descriptor()
 * @param report    raw report as delivered by tinyusb (may include leading id)
 * @param len       length of report
 * @param out       output; fully overwritten on success
 * @return true     report matched the layout and out was populated
 */
bool hid_gamepad_decode(const gamepad_layout_t *layout, const uint8_t *report, uint16_t len, gamepad_state_t *out);

#endif // _HID_GAMEPAD_H
