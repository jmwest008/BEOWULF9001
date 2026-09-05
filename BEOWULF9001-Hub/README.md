# BEOWULF9001-Hub

Initial hub-only scaffold for the BEOWULF9001 hub-and-node system.

This project is self-contained: it keeps its own local assets and
configuration (no shared folder with the node project) and can be built on
its own with ESP-IDF.

## Scope of this scaffold

- Hub UI and screen state machine only (Menu, Devices, Settings, Mode).
- Same v1.0 hardware assumptions: ESP32-C5-WROOM-1, CYD 2.8" ST7789 display,
  shared SPI2 bus for LCD + touch, identical GPIO usage and touch
  orientation/calibration as `Reference_v1.0_main`.
- Up to six logical node slots. The Devices screen only shows connected
  nodes as active, tappable cards; disconnected slots are rendered as dimmed
  placeholders and never respond to touch.
- A minimal ESP-NOW "Hello" heartbeat is wired up so a node slot becomes
  "connected" when a Hello is received from its configured MAC address, and
  automatically expires (non-blocking) if no Hello arrives for 5 seconds.
  Scan/control messaging will be added alongside the node project.

## Hardware overview

| Aspect | Details |
|:--|:--|
| **Device** | ESP32-C5-WROOM-1 |
| **Display** | CYD 2.8" (320×240 px, ST7789) |
| **Connectivity** | Wi-Fi, ESP-NOW |
| **Build Target** | ESP-IDF v6.1-beta1 |

### GPIO Pinout

Unchanged from `Reference_v1.0_main`. LCD and touch share a single SPI2 bus.

| Pin | GPIO | Bus | Direction | Purpose |
|:--|:-:|:-:|:-:|:--|
| **LCD_SCLK** | GPIO6 | SPI2 | Output | Shared clock line (LCD + touch) |
| **LCD_MOSI** | GPIO7 | SPI2 | Output | Shared data-out line (LCD + touch) |
| **TOUCH_MISO** | GPIO2 | SPI2 | Input | Shared data-in line (LCD + touch) |
| **LCD_CS** | GPIO23 | SPI2 | Output | LCD chip select |
| **LCD_DC** | GPIO24 | SPI2 | Output | LCD data/command select |
| **LCD_BACKLIGHT** | GPIO25 | GPIO | Output | Backlight enable (driven high at boot) |
| **TOUCH_CS** | GPIO1 | SPI2 | Output | Touch controller chip select |

## Screen flow

| Screen | Reachable from | Notes |
|:--|:--|:--|
| **Menu** | (start) | SETTINGS, DEVICES, MODE buttons |
| **Devices** | Menu | Up to 6 node cards; only connected nodes are visible/tappable |
| **Settings** | Menu | Placeholder toggles, to be defined with node communication work |
| **Mode** | Menu | Placeholder mode selection, to be defined with node communication work |

Every submenu screen (Devices/Settings/Mode) shows a header back button that
returns to the Menu.

## MAC address placeholder

`main/peer_mac_addresses.h` holds a local, hub-only copy of the six node MAC
address placeholders (all-zero until a node is provisioned) plus the shared
ESP-NOW channel. The node project maintains its own separate copy.

## Building

```bash
# Set up ESP-IDF environment
cd BEOWULF9001-Hub
idf.py set-target esp32c5

# Build firmware
idf.py build

# Flash to device (adjust port as needed)
idf.py -p /dev/ttyUSB0 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor
```

## Future work

- Populate `peer_mac_addresses.h` with real node MAC addresses as nodes are
  provisioned.
- Extend the ESP-NOW message set beyond the Hello heartbeat (scan requests,
  scan results, node control).
- Flesh out the Settings and Mode screens once node behavior is defined.
- Align the node project (`BEOWULF9001-Node/`) with this hub's message
  framing and MAC configuration convention.
