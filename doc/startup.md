# startup sequence

amigahid-pico starts the amiga interfaces and usb host controller before it
initialises the optional display and debug output. usb device discovery then runs
asynchronously from the main loop, alongside the amiga keyboard, mouse and
joystick services.

the amiga keyboard power-up handshake keeps its existing timing without blocking
the processor. one second after the keyboard interface is initialised it sends
`AK_INITPOWER` (`0xfd`), waits another 200 ms, then sends `AK_TERMPOWER`
(`0xfe`). keyboard input received while this handshake is in progress remains in
the event queue and is sent after the handshake completes. reset events are still
handled immediately.

starting usb discovery before the keyboard delays and display setup lets attached
usb mice and gamepads become available as soon as enumeration finishes. keyboard
startup still takes about 1.2 seconds because the amiga protocol requires its
power-up sequence.

the pico sdk submodule pins sdk 2.3.1. the tested nested tinyusb checkout is
0.21.0 (`dae3f9a366bfcddbf9dcf1b48d7500286a849539`); sdk 2.3.1 itself records
tinyusb 0.18.0, so select the newer revision after a recursive submodule update.
tinyusb 0.21.0 performs port debounce, reset and device enumeration through its
asynchronous host task; the main loop must therefore call `tuh_task()` frequently.

```shell
git -C pico-sdk/lib/tinyusb fetch --tags
git -C pico-sdk/lib/tinyusb checkout 0.21.0
```
