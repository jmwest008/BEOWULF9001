[README.md](https://github.com/user-attachments/files/31622773/README.md)
# BEOWULF9001 V1.0

A central interface for an ESP32-C5 mesh network with integrated Wi-Fi and Bluetooth scanning, built on a CYD 2.8" touchscreen display.

## Quick Start

| Aspect | Details |
|:--|:--|
| **Device** | ESP32-C5-WROOM-1 |
| **Display** | Seeed CYD 2.8" (320×240 px, ST7789) |
| **Connectivity** | Wi-Fi (802.11a/b/g/n), Bluetooth 5.3 (NimBLE), ESP-NOW |
| **Memory** | 16 MB flash, 8 MB PSRAM |
| **Build Target** | ESP-IDF v6.1-beta1 |
| **GPIO Pinout** | See [GPIO Pinout](#gpio-pinout) table below |

## Key Features

| Feature | Description |
|:--|:--|
| **SELF-C5 Card** | One-tap scan of all local Wi-Fi networks + Bluetooth advertisers (if enabled) in a unified scrollable list |
| **Devices Menu** | View network discovery with Settings-aware filtering (Bluetooth toggled on/off) |
| **Settings Menu** | Quick toggles for 5 GHz Wi-Fi scanning and Bluetooth inclusion |
| **Mode → Wi-Fi** | Independent high-frequency Wi-Fi scans with 30-sec auto-rescan and animated header pulse |
| **Mode → Bluetooth** | Independent Bluetooth advertiser scans, always running regardless of Settings toggle |
| **ESP-NOW Transport** | Versioned, magic-validated message framework ready for future peer discovery |
| **Responsive Touch** | Comfortable scrollbar reach; no finger stretch required at bottom of list |

## GPIO Pinout

All pins are defined as `constexpr gpio_num_t` in [main.cpp](main/main.cpp). LCD and touch share a single SPI2 bus.

| Pin | GPIO | Bus | Direction | Purpose |
|:--|:-:|:-:|:-:|:--|
| **LCD_SCLK** | GPIO6 | SPI2 | Output | Shared clock line (LCD + touch) |
| **LCD_MOSI** | GPIO7 | SPI2 | Output | Shared data-out line (LCD + touch) |
| **TOUCH_MISO** | GPIO2 | SPI2 | Input | Shared data-in line (LCD + touch) |
| **LCD_CS** | GPIO23 | SPI2 | Output | LCD chip select |
| **LCD_DC** | GPIO24 | SPI2 | Output | LCD data/command select |
| **LCD_BACKLIGHT** | GPIO25 | GPIO | Output | Backlight enable (driven high at boot) |
| **TOUCH_CS** | GPIO1 | SPI2 | Output | Touch controller chip select |
| **LCD_RESET** | Not connected | — | — | `reset_gpio_num = GPIO_NUM_NC`, tied to chip reset instead |
| **SPI_QUADWP** | Not connected | SPI2 | — | `quadwp_io_num = GPIO_NUM_NC`, unused in single-bit SPI mode |
| **SPI_QUADHD** | Not connected | SPI2 | — | `quadhd_io_num = GPIO_NUM_NC`, unused in single-bit SPI mode |

## Building

### Prerequisites

- ESP-IDF v6.1-beta1 or later
- `riscv32-esp-elf-gcc` (included with Espressif tools)
- Windows PowerShell or Unix shell with `idf.py`

### Build Steps

```bash
# Set up ESP-IDF environment
cd /path/to/ESP32-CYD/2.8_C5-WROOM-1
idf.py set-target esp32c5

# Build firmware
idf.py build

# Flash to device (adjust COM port as needed)
idf.py -p COM3 flash

# Monitor serial output
idf.py -p COM3 monitor
```

## Usage Guide

### Main Menu

| Menu Item | Action | Result |
|:--|:--|:--|
| **DEVICES** | Tap card | Scan networks + Bluetooth (if enabled in Settings) |
| **SETTINGS** | Adjust toggles | Control 5 GHz inclusion and Bluetooth filtering |
| **MODE** | Select Mode | Enter high-frequency scan mode with 30-sec auto-rescan |

### Devices Screen

1. Tap the **SELF-C5** card to initiate a combined scan
2. **Wi-Fi section** appears first (always scanned)
3. **Bluetooth section** appears second (only if Settings → Bluetooth is orange/enabled)
4. Scroll through results with smooth scrollbar navigation
5. Tap **BACK** to return to Devices menu

**Result Format**:
```
WI-FI
  ├─ Network A (signal, channel)
  └─ Network B (signal, channel)
BLUETOOTH
  ├─ Device Name A
  └─ Device Name B
```

### Mode Screen

| Button | Behavior | Auto-Rescan | Rescan Animation |
|:--|:--|:-:|:--|
| **WI-FI** | Tap to enter Wi-Fi scan mode | Every 30 seconds | Header pulses white ↔ dark orange |
| **BLUETOOTH** | Tap to enter BT scan mode | Every 30 seconds | Header pulses white ↔ dark orange |

- Results update automatically while you view them (no manual refresh needed)
- Tap **BACK** to exit scan mode and stop auto-rescan timers
- Wi-Fi rescans run independently of Bluetooth rescans

### Settings Screen

| Setting | Color When Enabled | Effect |
|:--|:-:|:--|
| **5GHZ** | Orange | Wi-Fi scan includes 5 GHz channels (a/n bands) |
| **5GHZ** | White | Wi-Fi scan uses only 2.4 GHz channels (b/g/n bands) |
| **BLUETOOTH** | Orange | Devices screen includes Bluetooth advertiser results |
| **BLUETOOTH** | White | Devices screen shows Wi-Fi results only |

⚠️ **Note**: Mode → Bluetooth scans always run regardless of this Settings toggle.

## Architecture

### State Machine

| State | Available Transitions | Purpose |
|:--|:--|:--|
| **Menu** | → Devices, Settings, Mode | Main navigation hub |
| **Devices** | → ScanResults (on SELF-C5 tap), Settings | View/tap network card |
| **Settings** | → Menu | Adjust Wi-Fi + BT filters |
| **Mode** | → ScanResults (Wi-Fi or BT selected) | Enter high-frequency scan |
| **ScanResults** | → Mode or Devices (BACK) | Display scrollable list, 30-sec rescans |

### Scan Strategies

| Scan Type | Trigger | Duration | Deduplication | Frequency |
|:--|:--|:-:|:-:|:-:|
| **Wi-Fi (Devices)** | SELF-C5 tap | ~120 ms/channel | By BSSID | On-demand |
| **Bluetooth (Devices)** | SELF-C5 tap | 3 seconds | By BLE address | On-demand (if enabled) |
| **Wi-Fi (Mode)** | Mode → Wi-Fi tap | ~120 ms/channel | By BSSID | Every 30 sec (auto) |
| **Bluetooth (Mode)** | Mode → Bluetooth tap | 3 seconds | By BLE address | Every 30 sec (auto) |

### Memory Layout

| Region | Size | Purpose |
|:--|:-:|:--|
| **Framebuffer (PSRAM)** | 307 KB | Display buffer (320×240×2 bytes) |
| **Wi-Fi Network List (PSRAM)** | Variable | Deduplicated APs (max 32 entries) |
| **Bluetooth Device List (PSRAM)** | Variable | Deduplicated advertisers (max 32 entries) |
| **BLE Host (Internal RAM)** | ~64 KB | NimBLE stack + event buffers |
| **FreeRTOS Heap (Internal RAM)** | Remaining | Task stacks + dynamic allocation |

### ESP-NOW Transport (Foundation)

| Component | Value | Purpose |
|:--|:--|:--|
| **Magic Number** | `0x42455755` | Validate packet authenticity |
| **Protocol Version** | `1` | Ensure compatibility across peers |
| **Message Types** | Hello, ScanRequest, ScanResult | Future: peer discovery and data exchange |
| **Max Payload** | ~245 bytes (ESP_NOW_MAX_DATA_LEN - header) | Limited by ESP-NOW frame size |
| **Callbacks** | recv_callback, send_callback | Transport handlers (registered, not yet used) |

### Display Rendering Pipeline

| Step | Target | Buffer Size | Timing |
|:-:|:--|:--|:--|
| 1. Render frame | Internal draw_framebuffer() | 20-line strips (~13 KB) | Full screen redraw per menu change |
| 2. SPI transfer | LCD via SPI2 | 20 lines at 40 MHz | Avoid DMA bounce-buffer exhaustion |
| 3. Display update | ST7789 | Partial regions | Smooth visual feedback |

## Configuration Reference

| Setting | Default | Range/Options | Effect |
|:--|:-:|:-:|:--|
| **LCD_TRANSFER_LINES** | 20 | 1–40 | Lines per SPI batch; higher = faster but more DMA memory |
| **SCAN_LIST_ROW_HEIGHT** | 20 px | 16–32 px | Pixel height per list entry |
| **SCAN_LIST_MAX_ROWS** | 9 | Calculated | Visible rows on screen (240 − 56 − 8) / 20 |
| **Mode Rescan Interval** | 30 sec | 10–60 sec | Time between auto-rescans in Mode screens |
| **Animation Pulse Period** | 300 ms | 100–500 ms | Header text color toggle speed during scan |
| **Max Wi-Fi APs** | 32 | 8–64 | PSRAM limit per scan result buffer |
| **Max Bluetooth Devices** | 32 | 8–64 | PSRAM limit per scan result buffer |
| **BLE Scan Duration** | 3 sec | 1–10 sec | How long to listen for advertisements |

## Troubleshooting

| Issue | Likely Cause | Solution |
|:--|:--|:--|
| Display won't initialize | SPI pins not connected | Verify GPIO6, 7, 23, 24, 25 against breadboard |
| Touchscreen not responding | CST816S interrupt not wired or wrong CS pin | Check touch CS=GPIO1, also verify SPI MISO=GPIO2 |
| Wi-Fi scan hangs | NimBLE consuming too much RAM | Reduce BLE buffer size in sdkconfig (CONFIG_BT_NIMBLE_MAX_CCONNECTIONS) |
| Bluetooth no results | Scan doesn't reach settings-enabled threshold | Ensure at least one named advertiser is transmitting nearby |
| Scrollbar at bottom unreachable | Finger can't quite press bottom edge | SCAN_SCROLL_TOUCH_BOTTOM already adjusted upward; reduce further if needed |
| Build fails: volatile error | Modern C++ deprecated volatile for atomics | Ensure `#include <atomic>` is present and `std::atomic<uint32_t>` is used |
| ESP-NOW messages not received | Peers not registered or wrong channel | Verify esp_now_add_peer() called with correct MAC; both devices on same Wi-Fi channel |

## Future Work

- **ESP-NOW Peer Discovery**: Broadcast Hello frames to nearby C5 nodes
- **Remote Scanning**: Request Wi-Fi/Bluetooth scans from peer devices and merge results
- **Mesh Routing**: Forward packets between nodes; maintain peer tables
- **Device Management**: Store paired/discovered nodes with connection metadata
- **Over-the-Air Updates**: Distribute firmware and configuration via mesh

## License

Proprietary. See LICENSE file for details.

## Support

For issues or feature requests, contact the development team.
