/**
 * this file is part of amigahid-pico, (c) 2021 just nine <nine@aphlor.org>
 * please locate the full source at https://github.com/borb/amigahid-pico
 *
 * released under the terms of the Eclipse Public License 2.0 (EPL-2.0).
 * please find the complete license text at https://spdx.org/licenses/EPL-2.0
 *
 * amiga digital joystick interface. see joystick.h for the db9 pin mapping.
 */

#include "config.h"
#include "joystick.h"

#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#ifdef QM2_AMIGA_V

// active-low, open-collector style drive. asserting a signal means pulling the
// line to ground (output low); releasing means letting it float back high, which
// we do by turning the pin into a high-impedance input (the amiga side pulls it
// up). this mirrors the technique used by quad_mouse.c's _aqm_gpio_set().
static inline void _joy_gpio_set(uint gpio, bool active)
{
    if (active) {
        gpio_put(gpio, 0);
        gpio_set_dir(gpio, GPIO_OUT);
        return;
    }

    gpio_set_dir(gpio, GPIO_IN);
}

// translate a logical signal to the physical gpio carrying it. these are port 2
// (J6) - the joystick port. the mouse owns port 1 (J5, the QM1_* pins), so the two
// never contend and a keyboard, mouse and joystick can all be live at once.
static uint _joy_gpio_for(enum amiga_joystick_signal signal)
{
    switch (signal) {
        case AJOY_UP:    return QM2_AMIGA_V;
        case AJOY_DOWN:  return QM2_AMIGA_H;
        case AJOY_LEFT:  return QM2_AMIGA_VQ;
        case AJOY_RIGHT: return QM2_AMIGA_HQ;
        case AJOY_FIRE1: return QM2_AMIGA_B1;
        case AJOY_FIRE2: return QM2_AMIGA_B2;
    }

    // unreachable, but keep the compiler happy
    return QM2_AMIGA_B1;
}

void amiga_joystick_init(void)
{
    // nothing else claims the port-2 pins, so we own them outright. leave
    // everything released (high-z) so an idle joystick asserts nothing.
    const enum amiga_joystick_signal signals[] = {
        AJOY_UP, AJOY_DOWN, AJOY_LEFT, AJOY_RIGHT, AJOY_FIRE1, AJOY_FIRE2
    };

    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        uint gpio = _joy_gpio_for(signals[i]);
        gpio_init(gpio);
        gpio_set_function(gpio, GPIO_FUNC_SIO);
        _joy_gpio_set(gpio, false);
    }
}

void amiga_joystick_set(enum amiga_joystick_signal signal, bool active)
{
    _joy_gpio_set(_joy_gpio_for(signal), active);
}

void amiga_joystick_apply(const amiga_joystick_state_t *state)
{
    if (state == NULL)
        return;

    // up/down and left/right are mutually exclusive on real hardware; if a
    // decoder ever hands us both, assert both anyway and let the amiga sort it
    // out - it is no worse than a flaky stick.
    amiga_joystick_set(AJOY_UP,    state->up);
    amiga_joystick_set(AJOY_DOWN,  state->down);
    amiga_joystick_set(AJOY_LEFT,  state->left);
    amiga_joystick_set(AJOY_RIGHT, state->right);
    amiga_joystick_set(AJOY_FIRE1, state->fire1);
    amiga_joystick_set(AJOY_FIRE2, state->fire2);
}

#else // QM2_AMIGA_V

// board revision with no known port-2 pinout (see config.h). rather than drive
// port 1 and fight the mouse, do nothing at all.
void amiga_joystick_init(void) {}
void amiga_joystick_set(enum amiga_joystick_signal signal, bool active) { (void)signal; (void)active; }
void amiga_joystick_apply(const amiga_joystick_state_t *state) { (void)state; }

#endif // QM2_AMIGA_V
