# BEOWULF9001

BEOWULF9001 is a two-part embedded firmware workspace for a hub-and-node system built around ESP32-class devices.

The repository is organized so the hub firmware and node firmware can live side by side while keeping each project self-contained.

### Project status

- `BEOWULF9001-Hub/` contains an **initial scaffold only**: the display/touch
  UI shell, the Menu/Devices/Settings/Mode screen flow, and a minimal
  ESP-NOW "Hello" heartbeat used to mark a node slot connected/disconnected.
  It is not a finished product and does not implement real device control,
  scanning, or settings behavior yet.
- `BEOWULF9001-Node/` does **not exist yet**. Node firmware is out of scope
  for this pass and is planned future work.
- Connected-node behavior, ESP-NOW heartbeat handling, and the
  Settings/Mode screens are scaffolded/placeholder only; they are not a
  complete mesh protocol or feature implementation.

## Repository layout

```text
BEOWULF9001/
  README.md
  BEOWULF9001-Hub/       (initial scaffold, present)
  BEOWULF9001-Node/      (planned, not yet created)
```

### Planned project roles

- `BEOWULF9001-Hub/`
  - Central hub firmware
  - UI, device orchestration, screen management, and ESP-NOW coordination
  - Maintains its own local assets and project-specific configuration files

- `BEOWULF9001-Node/`
  - Node firmware template used to build any of the six nodes
  - Node-side radio/control logic and hardware-specific routines
  - Maintains its own local assets and project-specific configuration files

## Hardware overview

### Hub

- Device: ESP32-C5-WROOM-1
- Display: CYD 2.8" (320×240, ST7789)
- Connectivity: Wi-Fi, Bluetooth 5.3 (NimBLE), ESP-NOW
- Memory: 16 MB flash, 8 MB PSRAM
- Build target: ESP-IDF v6.1-beta1

### Nodes

- Device: Seeed ESP32-C5 (with antenna)
- Connectivity: Wi-Fi, Bluetooth 5.3 (NimBLE), ESP-NOW
- Total nodes planned: 6

## Node build workflow

The node project is intended to act as a template.

A single build-time value in `main.cpp` will identify which target node is being built:

- `NODE_ID = 1`
- `NODE_ID = 2`
- `NODE_ID = 3`
- `NODE_ID = 4`
- `NODE_ID = 5`
- `NODE_ID = 6`

This makes it possible to reuse the same node project while building six distinct physical targets.

## MAC address workflow

A MAC address config file will be maintained for both projects so target addresses can be filled in as devices are provisioned.

Planned use:

- hub MAC address placeholder
- node 1 MAC address placeholder
- node 2–6 MAC address placeholders

Recommended placement:

- `BEOWULF9001-Hub/` keeps its own copy if needed for hub-side lookup
- `BEOWULF9001-Node/` keeps its own copy if needed for node-side lookup

The placeholders will be replaced manually as each device is built and flashed.

## Intended system design

The hub acts as the primary controller and user interface for the system.

The current architectural intent is:

- the hub manages screens and operator workflow
- nodes execute the requested tasks
- each project remains independent in its own folder
- target addresses and node identity are configured explicitly during build

## Initial implementation goals

The first implementation pass should focus on:

1. repository structure
2. hub project scaffold ✅ see `BEOWULF9001-Hub/`
3. node template scaffold
4. local project assets ✅ hub keeps its own assets/config, no shared folder
5. MAC address placeholder configuration ✅ `BEOWULF9001-Hub/main/peer_mac_addresses.h`
6. communication scaffolding ✅ minimal ESP-NOW Hello heartbeat in the hub
7. build configuration for ESP-IDF ✅ hub `CMakeLists.txt` files

The hub scaffold keeps the v1.0 GPIO/touch/display assumptions from
`Reference_v1.0_main` and implements the Menu/Devices/Settings/Mode screen
flow from the new hub spec. The Devices screen shows only connected nodes as
active cards; the remaining node slots (up to 6 total) are dimmed
placeholders that do not respond to touch. The node project scaffold is not
part of this pass.

## Open questions and design conflicts

The spec contains several items that should be resolved before full implementation:

### 1. Build switching method

The original workflow described renaming source files to switch builds.

**Recommendation:** avoid file renaming as a build switch mechanism; use separate project folders or explicit build targets instead.

### 2. Shared vs local assets

You requested that assets be maintained inside each project instead of a shared folder.

**Decision:** each project should keep its own local assets, even if that creates some duplication.

### 3. MAC config placement

It is still worth deciding whether the MAC placeholder file should live in both project folders or in one shared repo-level location.

**Question:** should the MAC address file be duplicated into each project, or kept once at the repository root?

### 4. Node build configuration

The node template will be driven by a single `NODE_ID` value.

**Question:** should `NODE_ID` live directly in `main.cpp`, or in a small project config header for easier editing?

### 5. Hardware definition detail

The hub hardware is fairly specific, but the node board is still described somewhat broadly.

**Question:** do you want the README to name the exact node board model or keep it generic for now?

## Next step

Once the structure is confirmed, the next step is to add the first hub scaffold and the node template scaffold with their local placeholder assets and configuration files.
