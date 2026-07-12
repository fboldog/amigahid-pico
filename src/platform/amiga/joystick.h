/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * amiga digital joystick interface.
 *
 * the amiga has two db9 ports, each of which can carry a quadrature mouse or a
 * digital (atari style) joystick. by convention the mouse lives on port 1 and the
 * joystick on port 2, and that is what we do here: quad_mouse.c drives port 1 (the
 * QM1_AMIGA_* pins), this driver drives port 2 (QM2_AMIGA_*). they are electrically
 * independent, so a keyboard, a mouse and a joystick can all be live simultaneously.
 *
 * all lines are active-low: pulled to ground when asserted, released (high-z, pulled
 * high by the amiga) when idle.
 *
 * db9 pin -> amiga signal -> gpio (see config.h):
 *   pin 1  up     QM2_AMIGA_V
 *   pin 2  down   QM2_AMIGA_H
 *   pin 3  left   QM2_AMIGA_VQ
 *   pin 4  right  QM2_AMIGA_HQ
 *   pin 6  fire1  QM2_AMIGA_B1
 *   pin 9  fire2  QM2_AMIGA_B2
 *
 * note the direction signals reuse the mouse quadrature names (v/h/vq/hq) because
 * both devices share the same db9 pinout - they are just interpreted differently.
 */

#ifndef _PLATFORM_AMIGA_JOYSTICK_H
#define _PLATFORM_AMIGA_JOYSTICK_H

#include <stdbool.h>

// the six digital signals an amiga joystick can assert
enum amiga_joystick_signal {
    AJOY_UP,
    AJOY_DOWN,
    AJOY_LEFT,
    AJOY_RIGHT,
    AJOY_FIRE1,
    AJOY_FIRE2
};

// normalised joystick state produced by the hid decoder and consumed here
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool fire1;
    bool fire2;
} amiga_joystick_state_t;

/**
 * prepare the joy port pins. safe to call after amiga_quad_mouse_init() (which
 * already owns the same gpio); this simply ensures every signal starts released.
 */
void amiga_joystick_init(void);

/**
 * assert or release a single joystick signal.
 *
 * @param signal    which db9 line to drive
 * @param active    true = pressed (pin driven low), false = released (high-z)
 */
void amiga_joystick_set(enum amiga_joystick_signal signal, bool active);

/**
 * apply a whole joystick state at once, driving every signal to match. cheap to
 * call every report as gpio writes are idempotent.
 *
 * @param state     desired state of all six signals
 */
void amiga_joystick_apply(const amiga_joystick_state_t *state);

#endif // _PLATFORM_AMIGA_JOYSTICK_H
