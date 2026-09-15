/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * main entry point for amigahid-pico.
 */

// these reside within the tinyusb sdk and are not part of this project source
#include "bsp/board.h"
#include "tusb.h"

#include "display/disp_ssd.h"
#include "platform/amiga/keyboard_serial_io.h"
#include "platform/amiga/quad_mouse.h"
#include "platform/amiga/joystick.h"
#include "util/debug_cons.h"
#include "util/output.h"

#include "config.h"
#include "tusb_config.h"

#ifdef ENABLE_BLUETOOTH_HID
#include "bt_hid.h"
#endif

// defined within usb_hid.c
extern void hid_app_task(void);

// main entry point
int main(void)
{
    // tinyusb board init; led, uart, button, usb
    board_init();

    // Put all Amiga-facing pins in their inactive state immediately. Keyboard
    // power-up continues asynchronously from amiga_service().
    amiga_init();
    amiga_quad_mouse_init();
    amiga_joystick_init();

    // Initialise the host controller early. TinyUSB 0.21 performs attach
    // debounce, reset and enumeration asynchronously from tuh_task().
    const tusb_rhport_init_t usb_host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUH_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
    };
    bool usb_host_ready = tusb_init(BOARD_TUH_RHPORT, &usb_host_init);

    // Consume a pending attach immediately so TinyUSB can start its async
    // debounce interval while the remaining status facilities initialise.
    if (usb_host_ready)
        tuh_task();

    // Initialise status output after time-critical I/O is safe and USB host
    // enumeration has been armed.
    disp_ssd_init();
    dbgcons_init();

    if (!usb_host_ready)
        ahprintf("\n[usb] fatal: tusb_init() failed on rhport %d; no devices will enumerate\n",
            BOARD_TUH_RHPORT);
    else
        ahprintf("\n[usb] host stack up on rhport %d, waiting for devices\n", BOARD_TUH_RHPORT);

#ifdef ENABLE_BLUETOOTH_HID
    bt_hid_init();
#endif

    while (1) {
        // run host mode jobs (hotplug events, packet io callbacks)
        tuh_task();

#ifdef ENABLE_BLUETOOTH_HID
        bt_hid_task();
#endif

        // amiga keyboard service routine
        amiga_service();
    }

    return 0;
}
