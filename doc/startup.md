# startup sequence

init order:

1. amiga interfaces (keyboard, mouse, joystick)
2. usb host controller
3. display and debug output (optional)

usb enumeration then runs asynchronously from the main loop, alongside the amiga
services, so usb mice and gamepads are usable as soon as enumeration finishes.

## keyboard power-up

the handshake is non-blocking:

- t+1000 ms after keyboard init: send `AK_INITPOWER` (`0xfd`)
- t+1200 ms: send `AK_TERMPOWER` (`0xfe`)

keyboard events arriving during the handshake are queued and sent afterwards.
reset events are handled immediately.

## reset hold (`HOLD_THE_DOOR`)

optional build flag (`-DHOLD_THE_DOOR=ON`, default off):

- `amiga_init()` drives `KBD_AMIGA_RST` low, holding the amiga in reset
- `amiga_release_boot_reset()` releases it right after `tusb_init()` returns,
  even if init fails
- the keyboard handshake timer starts at release instead of at keyboard init

only the dedicated reset line is asserted (clock is not held low), so it has no
effect on big-box amigas, which don't wire reset through the keyboard connector.

## dependencies

- pico sdk: 2.3.1 (submodule)
- tinyusb: 0.21.0 (`dae3f9a366bfcddbf9dcf1b48d7500286a849539`)

sdk 2.3.1 pins tinyusb 0.18.0, so check out 0.21.0 manually after a recursive
submodule update:

```shell
git -C pico-sdk/lib/tinyusb fetch --tags
git -C pico-sdk/lib/tinyusb checkout 0.21.0
```

tinyusb 0.21.0 handles port debounce, reset and enumeration in its host task, so
the main loop must call `tuh_task()` frequently.
