# Lithophane Lamp Firmware

Firmware for the lithophane lamp's support/base board. It runs on an **ESP32-C3**, is built with **ESP-IDF**, and does the following:

- drives the LED backlight through two **TPS61165** boost LED drivers (single-wire EasyScale dimming),
- handles three buttons: brightness down, brightness up, and power,
- stores the chosen brightness in NVS flash so it survives a power cycle,
- watches board temperature with a **TMP1075** sensor and shuts the LEDs off if the board overheats,
- configures two **STUSB4500** USB-C Power Delivery sink controllers so they ask the charger for a higher voltage,
- keeps the LEDs off until the PD controller reports *Power OK*.

## Hardware

| Function               | Part       | Interface | Pin / Address |
|------------------------|------------|-----------|---------------|
| I²C SDA                | —          | I²C0      | GPIO0         |
| I²C SCL                | —          | I²C0      | GPIO1         |
| Temperature sensor     | TMP1075    | I²C       | `0x48`        |
| USB-C PD sink (horizontal) | STUSB4500 | I²C    | `0x28`        |
| USB-C PD sink (vertical)   | STUSB4500 | I²C    | `0x29`        |
| Power OK input (active low) | —     | GPIO in   | GPIO2         |
| LED driver A           | TPS61165   | CTRL      | GPIO6         |
| LED driver B           | TPS61165   | CTRL      | GPIO7         |
| Brightness − button    | —          | GPIO in, pull-up, active low | GPIO4 |
| Brightness + button    | —          | GPIO in, pull-up, active low | GPIO5 |
| Power button           | —          | GPIO in, pull-up, active low | GPIO10 |

The I²C bus runs at 100 kHz. Pin assignments are `#define`s at the top of [main/main.c](main/main.c).

## Behavior

- **Brightness**: there are 8 levels (`MIN_BRIGHTNESS` = 1 to `MAX_BRIGHTNESS` = 8). Each button press moves one level. A new level is written to NVS (namespace `settings`, key `brightness`). On first boot, before anything is saved, the lamp starts at full brightness.
- **Power button**: turns the LEDs on and off. This setting is not saved, so the LEDs are always on after power-up.
- **Over-temperature protection**: the LEDs turn off at ≥ 50 °C and come back on at ≤ 45 °C. The gap between the two keeps the LEDs from flickering near the limit.
- **Power OK**: if the `POWER_OK` line is not asserted, the LEDs stay off.
- **Main loop**: runs every 100 ms. Temperature and Power OK are checked, and status is logged, once per second.
- **Buttons**: presses are caught by falling-edge interrupts. A button is armed again only after it has been released, so holding it down counts as one press.

### USB-C PD configuration

At boot, the firmware writes this sink profile into the NVM of both STUSB4500 chips:

| PDO | Voltage | Current |
|-----|---------|---------|
| 1   | 5 V     | (default) |
| 2   | 12 V    | 1.0 A   |
| 3   | 15 V    | 0.8 A   |

The driver reads the NVM first and writes only if the contents differ, so normal reboots do not wear out the NVM. NVM flashing is enabled by the `#if 1` block in `app_main()`. Change it to `#if 0` once the chips have been programmed.

## Project layout

```
.
├── CMakeLists.txt          # ESP-IDF project definition
├── sdkconfig               # Generated config (target: esp32c3, 4 MB flash)
├── .devcontainer/          # Docker dev container based on espressif/idf
└── main/
    ├── main.c              # Application: buttons, NVS, temperature, LED control loop
    ├── tps61165.c/.h       # TPS61165 EasyScale single-wire dimming driver
    ├── stusb4500.c/.h      # STUSB4500 runtime PDO negotiation / GPIO control
    └── stusb4500_nvm.c     # STUSB4500 NVM read/flash
```

## Building and flashing

You need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/) v5.x, which provides the `esp_driver_i2c` and `esp_driver_gpio` components. CMake 3.22 or newer is also required.

```sh
idf.py set-target esp32c3   # only needed once
idf.py build
idf.py -p <PORT> flash monitor
```

The serial console is on UART. Log lines use the tag `lithophane`.

### Dev container

[.devcontainer/](.devcontainer/) holds a VS Code dev container built on the official `espressif/idf` image, with the ESP-IDF extension preinstalled. It runs privileged so it can reach USB serial devices for flashing.

## Notes

- `sdkconfig`, `build/` and `.devcontainer/` are listed in `.gitignore`. Run `idf.py menuconfig` or `idf.py set-target esp32c3` on a fresh clone to regenerate `sdkconfig`.
- The TMP1075 and both STUSB4500s are probed at startup. A missing device logs a warning but does not stop boot.
