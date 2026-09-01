# mDjiController Linux

A native Linux userspace driver for the DJI Phantom 3 Advanced GL300C remote
controller. It reads the controller's 115200-baud USB serial protocol and
publishes the controls as a standard Linux joystick through `uinput`.

This implementation is Linux-native: it does not use vJoy, the Windows DJI
driver, or code from the original Windows application. It has been tested on
CachyOS with a GL300C controller detected as `/dev/ttyACM0`.

## Features

- Automatic detection of `ttyACM` and `ttyUSB` serial devices
- Six joystick axes and nine buttons through the kernel `uinput` interface
- Approximately 100 Hz controller polling with a 5 ms serial wait
- Safe handling of fragmented DJI DUML packets
- Optional live control-value output for testing and calibration
- CMake build and udev rules for unprivileged desktop use

## Requirements

- Linux with the standard `cdc_acm` and `uinput` kernel modules
- A C++17 compiler
- CMake 3.16 or newer
- udev on the target system

## Build and install

On CachyOS or Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
sudo udevadm control --reload-rules
sudo udevadm trigger
```

On another distribution, install a C++ compiler, CMake, and udev using its
package manager, then use the same CMake commands.

Load `uinput` if it is not already active:

```sh
sudo modprobe uinput
```

After installing the udev rule, unplug and reconnect the remote.

## Usage

Start with automatic serial-device detection:

```sh
mdji-controller
```

Specify the device explicitly:

```sh
mdji-controller --device /dev/ttyACM0
```

Print live axis and button values while testing:

```sh
mdji-controller --device /dev/ttyACM0 --verbose
```

The virtual device appears as **DJI Phantom 3 Remote Controller**. Inspect it
with `evtest`, Steam's controller settings, or the simulator of your choice.
Press `Ctrl+C` to stop the driver.

## Troubleshooting

Check whether the controller and virtual-input device are available:

```sh
lsusb
ls -l /dev/ttyACM* /dev/ttyUSB* /dev/uinput
dmesg | tail -n 50
```

If automatic detection selects the wrong serial interface, pass the correct
path with `--device`.

## Credits

This project was inspired by and builds on the protocol-discovery work of
[mishavoloshchuk/mDjiController](https://github.com/mishavoloshchuk/mDjiController),
the original Windows/vJoy implementation for DJI Phantom remote controllers.
Thanks to its author and contributors for documenting a working approach to
communicating with the controller.

The code in this repository is a new Linux implementation using the kernel's
serial and `uinput` APIs. This project is not affiliated with or endorsed by
DJI or the original project maintainers.

## License

Licensed under the [Apache License 2.0](LICENSE), matching the license used by
the original project whose protocol work is credited above.
