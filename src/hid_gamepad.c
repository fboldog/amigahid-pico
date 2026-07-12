/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * generic usb hid gamepad / joystick handling. see hid_gamepad.h for the why.
 *
 * the descriptor parser here is deliberately small: it understands enough of
 * the hid item grammar (short items, globals/locals, push/pop, report ids) to
 * find the standard desktop x/y axes, the hat switch and the button page. it
 * does not attempt to model the whole descriptor - anything it doesn't
 * recognise it simply steps over while keeping the running bit offset correct.
 */

#include "hid_gamepad.h"

#include <string.h>

#include "class/hid/hid.h" // HID_USAGE_* / usage page constants from tinyusb

// hid short-item prefix decode
#define ITEM_SIZE(prefix)   ((prefix) & 0x03)          // 0,1,2,3 -> 0,1,2,4 bytes
#define ITEM_TYPE(prefix)   (((prefix) >> 2) & 0x03)   // 0 main, 1 global, 2 local
#define ITEM_TAG(prefix)    (((prefix) >> 4) & 0x0f)

#define ITEM_TYPE_MAIN      0
#define ITEM_TYPE_GLOBAL    1
#define ITEM_TYPE_LOCAL     2

// main item tags
#define MAIN_INPUT          0x8
#define MAIN_OUTPUT         0x9
#define MAIN_FEATURE        0xb
#define MAIN_COLLECTION     0xa
#define MAIN_END_COLLECTION 0xc

// global item tags
#define GLOBAL_USAGE_PAGE   0x0
#define GLOBAL_LOGICAL_MIN  0x1
#define GLOBAL_LOGICAL_MAX  0x2
#define GLOBAL_REPORT_SIZE  0x7
#define GLOBAL_REPORT_ID    0x8
#define GLOBAL_REPORT_COUNT 0x9
#define GLOBAL_PUSH         0xa
#define GLOBAL_POP          0xb

// local item tags
#define LOCAL_USAGE         0x0

// input main-item data bits
#define INPUT_CONSTANT      (1u << 0) // 1 = constant/padding, 0 = data
#define INPUT_VARIABLE      (1u << 1) // 1 = variable, 0 = array

#define MAX_USAGES          16 // plenty for a controller's axis/button usages
#define MAX_REPORT_IDS      256
#define GLOBAL_STACK_DEPTH  8

// mutable global-item state, snapshotted by push/pop
typedef struct {
    uint16_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
} hid_globals_t;

// read a short item's data as an unsigned little-endian value
static uint32_t item_uval(const uint8_t *data, uint8_t len)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < len; i++)
        v |= (uint32_t)data[i] << (8 * i);
    return v;
}

// read a short item's data as a signed little-endian value (sign-extended)
static int32_t item_sval(const uint8_t *data, uint8_t len)
{
    uint32_t v = item_uval(data, len);
    if (len > 0 && len < 4) {
        uint32_t sign_bit = 1u << (8 * len - 1);
        if (v & sign_bit)
            v |= ~((sign_bit << 1) - 1); // extend the sign upward
    }
    return (int32_t)v;
}

// record an axis/hat field if the usage matches and we haven't already got one
static void record_axis(gamepad_layout_t *layout, gamepad_field_t *field, const hid_globals_t *g, uint16_t offset)
{
    if (field->present)
        return;

    field->present = true;
    field->offset = offset;
    field->size = g->report_size;
    field->logical_min = g->logical_min;
    field->logical_max = g->logical_max;
    layout->valid = true;

    // lock the layout to the report id where the first field was found so we
    // never mix bit offsets from two different reports
    if (!layout->report_id)
        layout->report_id = g->report_id;
}

bool hid_gamepad_parse_descriptor(gamepad_layout_t *layout, const uint8_t *desc, uint16_t desc_len)
{
    memset(layout, 0, sizeof(*layout));

    hid_globals_t g = { 0 };
    hid_globals_t gstack[GLOBAL_STACK_DEPTH];
    uint8_t gstack_depth = 0;

    // local items (reset after every main item). buttons are located from the
    // button usage page + report count, so usage min/max carry no extra info we
    // need and are simply stepped over.
    uint16_t usages[MAX_USAGES];
    uint8_t usage_count = 0;

    // running input-report bit offset, tracked per report id (an output/feature
    // report shares the id space but has its own stream, so only input items
    // advance these counters)
    static uint16_t report_bits[MAX_REPORT_IDS];
    memset(report_bits, 0, sizeof(report_bits));

    uint16_t pos = 0;
    while (pos < desc_len) {
        uint8_t prefix = desc[pos++];

        // long items (prefix 0xfe) carry their own size; we don't use any, skip
        if (prefix == 0xfe) {
            if (pos >= desc_len)
                break;
            uint8_t long_len = desc[pos];
            pos += 2 + long_len; // size byte + tag byte + data
            continue;
        }

        uint8_t raw_size = ITEM_SIZE(prefix);
        uint8_t len = (raw_size == 3) ? 4 : raw_size;
        if (pos + len > desc_len)
            break; // truncated descriptor; give up gracefully

        const uint8_t *data = &desc[pos];
        pos += len;

        switch (ITEM_TYPE(prefix)) {
            case ITEM_TYPE_GLOBAL:
                switch (ITEM_TAG(prefix)) {
                    case GLOBAL_USAGE_PAGE:   g.usage_page = (uint16_t)item_uval(data, len); break;
                    case GLOBAL_LOGICAL_MIN:  g.logical_min = item_sval(data, len); break;
                    case GLOBAL_LOGICAL_MAX:  g.logical_max = item_sval(data, len); break;
                    case GLOBAL_REPORT_SIZE:  g.report_size = (uint8_t)item_uval(data, len); break;
                    case GLOBAL_REPORT_ID:    g.report_id = (uint8_t)item_uval(data, len); break;
                    case GLOBAL_REPORT_COUNT: g.report_count = (uint8_t)item_uval(data, len); break;
                    case GLOBAL_PUSH:
                        if (gstack_depth < GLOBAL_STACK_DEPTH)
                            gstack[gstack_depth++] = g;
                        break;
                    case GLOBAL_POP:
                        if (gstack_depth > 0)
                            g = gstack[--gstack_depth];
                        break;
                    default: break;
                }
                break;

            case ITEM_TYPE_LOCAL:
                switch (ITEM_TAG(prefix)) {
                    case LOCAL_USAGE:
                        if (usage_count < MAX_USAGES)
                            usages[usage_count++] = (uint16_t)item_uval(data, len);
                        break;
                    default: break; // usage min/max etc. carry no info we need
                }
                break;

            case ITEM_TYPE_MAIN: {
                uint8_t tag = ITEM_TAG(prefix);

                // only input main items describe controls we care about and
                // advance the input bit offset
                if (tag == MAIN_INPUT) {
                    uint32_t flags = item_uval(data, len);
                    uint16_t base = report_bits[g.report_id];
                    uint16_t total_bits = (uint16_t)g.report_size * g.report_count;

                    // constant fields are padding; variable data holds real
                    // controls. arrays (e.g. keyboard) aren't joystick input.
                    bool usable = !(flags & INPUT_CONSTANT) && (flags & INPUT_VARIABLE);

                    if (usable && g.usage_page == HID_USAGE_PAGE_DESKTOP) {
                        // walk each control in the field, matching it against
                        // the queued usages (last usage repeats if we run short)
                        for (uint8_t i = 0; i < g.report_count; i++) {
                            uint16_t usage = usage_count
                                ? usages[i < usage_count ? i : usage_count - 1]
                                : 0;
                            uint16_t off = base + (uint16_t)i * g.report_size;

                            switch (usage) {
                                case HID_USAGE_DESKTOP_X:
                                    record_axis(layout, &layout->x, &g, off);
                                    break;
                                case HID_USAGE_DESKTOP_Y:
                                    record_axis(layout, &layout->y, &g, off);
                                    break;
                                case HID_USAGE_DESKTOP_HAT_SWITCH:
                                    record_axis(layout, &layout->hat, &g, off);
                                    break;
                                default:
                                    break;
                            }
                        }
                    } else if (usable && g.usage_page == HID_USAGE_PAGE_BUTTON && g.report_size == 1) {
                        // a run of 1-bit buttons; only capture the first block
                        if (!layout->has_buttons && (!layout->report_id || layout->report_id == g.report_id)) {
                            layout->has_buttons = true;
                            layout->button_offset = base;
                            layout->button_count = g.report_count;
                            layout->valid = true;
                            if (!layout->report_id)
                                layout->report_id = g.report_id;
                        }
                    }

                    report_bits[g.report_id] = base + total_bits;
                }

                // every main item clears the local item state
                usage_count = 0;
                break;
            }

            default:
                break;
        }
    }

    return layout->valid;
}

// pull bit_count bits starting at bit_offset out of an lsb-first packed report
static uint32_t read_bits(const uint8_t *data, uint16_t data_len, uint16_t bit_offset, uint8_t bit_count)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < bit_count; i++) {
        uint16_t bit = bit_offset + i;
        uint16_t byte = bit >> 3;
        if (byte >= data_len)
            break; // ran off the end; treat missing bits as zero
        if (data[byte] & (1u << (bit & 7)))
            value |= (1u << i);
    }
    return value;
}

// read a field and, for a signed logical range, sign-extend it
static int32_t read_field(const uint8_t *data, uint16_t data_len, const gamepad_field_t *f)
{
    uint32_t raw = read_bits(data, data_len, f->offset, f->size);
    if (f->logical_min < 0 && f->size > 0 && f->size < 32) {
        uint32_t sign_bit = 1u << (f->size - 1);
        if (raw & sign_bit)
            return (int32_t)(raw | ~((sign_bit << 1) - 1));
    }
    return (int32_t)raw;
}

// map an axis value to a low/high pair (e.g. left/right or up/down) using the
// logical range midpoint and a quarter-range deadzone
static void axis_to_dirs(const uint8_t *data, uint16_t data_len, const gamepad_field_t *f, bool *low, bool *high)
{
    if (!f->present)
        return;

    int32_t value = read_field(data, data_len, f);
    int32_t center = (f->logical_min + f->logical_max) / 2;
    int32_t half = (f->logical_max - f->logical_min) / 2;
    int32_t deadzone = half / 2;
    if (deadzone < 1)
        deadzone = 1;

    if (value < center - deadzone)
        *low = true;
    else if (value > center + deadzone)
        *high = true;
}

bool hid_gamepad_decode(const gamepad_layout_t *layout, const uint8_t *report, uint16_t len, gamepad_state_t *out)
{
    if (!layout->valid || report == NULL || out == NULL)
        return false;

    // strip the report id if the device uses one, and reject reports for other ids
    if (layout->report_id) {
        if (len < 1 || report[0] != layout->report_id)
            return false;
        report++;
        len--;
    }

    memset(out, 0, sizeof(*out));

    // analog / main stick -> directions
    axis_to_dirs(report, len, &layout->x, &out->left, &out->right);
    axis_to_dirs(report, len, &layout->y, &out->up, &out->down);

    // hat switch / d-pad -> directions (or'd in with the stick)
    if (layout->hat.present) {
        int32_t hv = read_field(report, len, &layout->hat) - layout->hat.logical_min;
        switch (hv) {
            case 0: out->up = true; break;                        // N
            case 1: out->up = true; out->right = true; break;     // NE
            case 2: out->right = true; break;                     // E
            case 3: out->down = true; out->right = true; break;   // SE
            case 4: out->down = true; break;                      // S
            case 5: out->down = true; out->left = true; break;    // SW
            case 6: out->left = true; break;                      // W
            case 7: out->up = true; out->left = true; break;      // NW
            default: break;                                       // centered / null
        }
    }

    // buttons -> bitfield
    if (layout->has_buttons) {
        uint8_t count = layout->button_count > 32 ? 32 : layout->button_count;
        for (uint8_t i = 0; i < count; i++) {
            if (read_bits(report, len, layout->button_offset + i, 1))
                out->buttons |= (1u << i);
        }
    }

    return true;
}
