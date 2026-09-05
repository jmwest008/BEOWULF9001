#pragma once

#include <cstdint>

// Placeholder MAC address configuration for the BEOWULF9001 hub.
//
// This file is kept local to the hub project (rather than in a shared
// assets folder) so the hub and node projects can each maintain their
// own independent copy. Fill in the real per-node MAC addresses here
// as each physical node is provisioned.
//
// An all-zero address marks a node slot as "not configured" -- the hub
// will not attempt to register an ESP-NOW peer for that slot, and the
// Devices screen will always render it as a dimmed, non-interactive
// placeholder regardless of runtime connection state.

namespace hub_mesh {

// ESP-NOW/Wi-Fi channel shared by the hub and all nodes.
constexpr int ESP_NOW_CHANNEL = 1;

// The hub supports up to six logical node slots.
constexpr int MAX_NODES = 6;

constexpr uint8_t NODE_1_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t NODE_2_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t NODE_3_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t NODE_4_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t NODE_5_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t NODE_6_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr const uint8_t *const NODE_MACS[MAX_NODES] = {
    NODE_1_MAC, NODE_2_MAC, NODE_3_MAC, NODE_4_MAC, NODE_5_MAC, NODE_6_MAC,
};

} // namespace hub_mesh
