# BEOWULF9001

BEOWULF9001 is a dual-project embedded firmware workspace for a hub-and-node system built around ESP32-class devices.

The repository is organized so the hub firmware and the node firmware can live side by side while sharing common protocol definitions, UI constants, and utility code.

## Repository layout

```text
BEOWULF9001/
  README.md
  BEOWULF9001-Hub/
  BEOWULF9001-Node1/
  BEOWULF9001-Node2/
  shared/
```

### Planned folder roles

- `BEOWULF9001-Hub/`
  - Central hub firmware
  - UI, device orchestration, screen management, and ESP-NOW coordination

- `BEOWULF9001-Node1/`
  - First node firmware project
  - Node-side radio/control logic and hardware-specific routines

- `BEOWULF9001-Node2/`
  - Additional node firmware project(s)
  - Intended to be duplicated or extended as more nodes are added

- `shared/`
  - Common headers, structs, protocol constants, geometry data, colors, and reusable helpers

## Hardware overview

### Hub

- Device: ESP32-C5-WROOM-1
- Display: CYD 2.8" (320×240, ST7789)
- Connectivity: Wi-Fi, Bluetooth 5.3 (NimBLE), ESP-NOW
- Memory: 16 MB flash, 8 MB PSRAM
- Build target: ESP-IDF v6.1-beta1

### Nodes

- Device: Seeed ESP32-35 (with antenna)
- Connectivity: Wi-Fi, Bluetooth 5.3 (NimBLE), ESP-NOW

## Intended system design

The hub acts as the primary controller and user interface for the system.

The current architectural intent is:

- the hub manages screens and operator workflow
- nodes execute the requested tasks
- shared code keeps protocol formats and constants aligned
- hub-to-node communication uses a star-style control model

## Initial implementation goals

The first implementation pass should focus on:

1. repository structure
2. shared protocol definitions
3. hub UI shell
4. node firmware shell
5. communication scaffolding
6. build configuration for ESP-IDF

## Open questions and design conflicts

The spec contains several items that should be resolved before full implementation:

### 1. Repo layout vs. build layout

The repo is intended to contain multiple projects, but each folder should ideally be a normal ESP-IDF app rather than relying on file renaming to switch builds.

**Question:** Should each project folder be a standalone ESP-IDF application with its own `main/`, `CMakeLists.txt`, and `sdkconfig.defaults`?

### 2. Node naming

The spec mentions `BEOWULF9001-Node1,2 etc.` but does not define the total node count.

**Question:** How many node project folders should be created initially?

### 3. Shared code scope

The `shared/` folder can hold protocol definitions and utility code, but it is not yet clear which parts belong there.

**Question:** Should `shared/` contain only pure headers and constants, or also reusable C++ source files?

### 4. Hardware definition detail

The hub hardware is fairly specific, but the node board is still described loosely.

**Question:** What is the exact node board model so the README and build assumptions stay accurate?

### 5. Radio feature boundaries

The spec includes several radio and discovery-oriented features.

**Question:** Which features are intended for the first milestone, and which should be deferred to later phases?

### 6. File naming and project switching

The original workflow describes renaming source files to switch between hub and node builds.

**Recommendation:** Avoid file renaming as a build switch mechanism; use separate project folders or build targets instead.

## Next step

Once the repo structure is confirmed, the next step is to add the first hub project scaffold and define the shared protocol layer.
