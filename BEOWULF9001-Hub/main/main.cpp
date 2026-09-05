#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "peer_mac_addresses.h"

// BEOWULF9001 hub scaffold.
//
// This is the hub-only half of the BEOWULF9001 hub-and-node system. It keeps
// the v1.0 hardware assumptions (ESP32-C5-WROOM-1, CYD 2.8" ST7789 display,
// shared SPI2 bus for LCD + resistive touch, same GPIO usage and touch
// orientation) and reuses the reference project's rendering style. The
// screen flow and node-slot model are updated for the new hub spec:
//   Menu -> Devices / Settings / Mode, with a header back button on every
//   submenu. Node alignment/messaging is intentionally minimal here so it
//   can be extended later without reworking the display/touch layer.

namespace {

// ---------------------------------------------------------------------
// Display + layout constants (mirrors Reference_v1.0_main/main/main.cpp).
// ---------------------------------------------------------------------

constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;
constexpr int LCD_TRANSFER_LINES = 20;
constexpr int BOX_MARGIN = 8;
constexpr int BOX_GAP = 8;
constexpr int BOX_BORDER_WIDTH = 3;
constexpr int HEADER_BOX_HEIGHT = 36;
constexpr int BUTTON_WIDTH = 120;
constexpr int BUTTON_HEIGHT = 34;
constexpr int RAW_TOUCH_X_MIN = 225;
constexpr int RAW_TOUCH_X_MAX = 3413;
constexpr int RAW_TOUCH_Y_MIN = 403;
constexpr int RAW_TOUCH_Y_MAX = 3334;
constexpr int TOUCH_PRESSURE_THRESHOLD = 88;
constexpr int MENU_TRANSITION_DELAY_MS = 100;
constexpr int64_t NODE_CONNECTION_TIMEOUT_US = 5'000'000; // 5 seconds without a Hello -> disconnected.

constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t panel_color(uint16_t color)
{
	return static_cast<uint16_t>((color >> 8) | (color << 8));
}
constexpr uint16_t COLOR_LIGHT_GRAY = panel_color(0xC618);
constexpr uint16_t COLOR_DIM_GRAY = panel_color(0x4208);
constexpr uint16_t COLOR_BRIGHT_ORANGE = panel_color(0xFD20);
constexpr uint16_t COLOR_DARK_ORANGE = panel_color(0xA400);

// ---------------------------------------------------------------------
// GPIO pinout -- unchanged from the v1.0 reference project.
// ---------------------------------------------------------------------

constexpr gpio_num_t LCD_SCLK = GPIO_NUM_6;
constexpr gpio_num_t LCD_MOSI = GPIO_NUM_7;
constexpr gpio_num_t LCD_CS = GPIO_NUM_23;
constexpr gpio_num_t LCD_DC = GPIO_NUM_24;
constexpr gpio_num_t LCD_BACKLIGHT = GPIO_NUM_25;
constexpr gpio_num_t TOUCH_MISO = GPIO_NUM_2;
constexpr gpio_num_t TOUCH_CS = GPIO_NUM_1;

constexpr char TAG[] = "beowulf9001_hub";

// ---------------------------------------------------------------------
// Screen state machine.
// ---------------------------------------------------------------------

enum class Screen {
	Menu,
	Devices,
	Settings,
	Mode,
};

struct Rectangle {
	int x;
	int y;
	int width;
	int height;
};

bool point_in_rectangle(int x, int y, const Rectangle &rectangle)
{
	return x >= rectangle.x && x < rectangle.x + rectangle.width && y >= rectangle.y && y < rectangle.y + rectangle.height;
}

constexpr int TOP_BOX_WIDTH = (DISPLAY_WIDTH - (2 * BOX_MARGIN) - BOX_GAP) / 2;
constexpr int CONTENT_TOP = BOX_MARGIN + HEADER_BOX_HEIGHT + BOX_GAP;
constexpr int TOP_BOX_HEIGHT = (DISPLAY_HEIGHT - CONTENT_TOP - BOX_MARGIN - BOX_GAP) / 2;
constexpr int BOTTOM_BOX_Y = CONTENT_TOP + TOP_BOX_HEIGHT + BOX_GAP;
constexpr int BOTTOM_BOX_HEIGHT = DISPLAY_HEIGHT - BOTTOM_BOX_Y - BOX_MARGIN;
constexpr Rectangle HEADER_BOX = {BOX_MARGIN, BOX_MARGIN, DISPLAY_WIDTH - (2 * BOX_MARGIN), HEADER_BOX_HEIGHT};
constexpr Rectangle BACK_BUTTON_VISUAL = {HEADER_BOX.x + HEADER_BOX.width - 31, HEADER_BOX.y + 2, 26, 22};
constexpr Rectangle BACK_BUTTON_TOUCH = {0, HEADER_BOX.y, HEADER_BOX.height, HEADER_BOX.height};

// Main menu buttons: SETTINGS (top-left), DEVICES (top-right), MODE (bottom).
constexpr Rectangle BUTTONS[] = {
	{BOX_MARGIN + (TOP_BOX_WIDTH - BUTTON_WIDTH) / 2, CONTENT_TOP + (TOP_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
	{BOX_MARGIN + TOP_BOX_WIDTH + BOX_GAP + (TOP_BOX_WIDTH - BUTTON_WIDTH) / 2, CONTENT_TOP + (TOP_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
	{(DISPLAY_WIDTH - BUTTON_WIDTH) / 2, BOTTOM_BOX_Y + (BOTTOM_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
};

int button_at(int x, int y)
{
	for (int index = 0; index < 3; ++index) {
		if (point_in_rectangle(x, y, BUTTONS[index])) {
			return index;
		}
	}
	return -1;
}

// Devices / Settings / Mode share one card grid: 2 columns x 3 rows, which
// covers all six logical node slots on the Devices screen. Settings and Mode
// only occupy the first row of that grid for now.
constexpr int GRID_COLUMNS = 2;
constexpr int GRID_ROWS = 3;
constexpr int GRID_CARD_GAP = 6;
constexpr int GRID_STATUS_HEIGHT = 22;
constexpr int GRID_CARD_WIDTH = (DISPLAY_WIDTH - (2 * BOX_MARGIN) - GRID_CARD_GAP) / GRID_COLUMNS;
constexpr int GRID_CARD_HEIGHT = (DISPLAY_HEIGHT - CONTENT_TOP - GRID_STATUS_HEIGHT - BOX_MARGIN - ((GRID_ROWS - 1) * GRID_CARD_GAP)) / GRID_ROWS;
constexpr int GRID_STATUS_Y = CONTENT_TOP + (GRID_ROWS * GRID_CARD_HEIGHT) + ((GRID_ROWS - 1) * GRID_CARD_GAP) + 4;

Rectangle grid_card_rect(int index)
{
	const int row = index / GRID_COLUMNS;
	const int column = index % GRID_COLUMNS;
	return {
		BOX_MARGIN + (column * (GRID_CARD_WIDTH + GRID_CARD_GAP)),
		CONTENT_TOP + (row * (GRID_CARD_HEIGHT + GRID_CARD_GAP)),
		GRID_CARD_WIDTH,
		GRID_CARD_HEIGHT,
	};
}

int grid_card_at(int x, int y, int card_count)
{
	for (int index = 0; index < card_count; ++index) {
		if (point_in_rectangle(x, y, grid_card_rect(index))) {
			return index;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------
// Node slot state.
//
// The hub always reserves hub_mesh::MAX_NODES (6) logical slots. Only nodes
// that are both MAC-configured (see peer_mac_addresses.h) and have recently
// said Hello over ESP-NOW are considered "connected". Disconnected slots are
// rendered as dimmed placeholders on the Devices screen and never respond to
// touch.
// ---------------------------------------------------------------------

struct NodeSlot {
	std::atomic<bool> connected{false};
	std::atomic<int64_t> last_seen_us{0};
};

NodeSlot node_slots[hub_mesh::MAX_NODES];

constexpr char NODE_LABELS[hub_mesh::MAX_NODES][8] = {
	"NODE 1", "NODE 2", "NODE 3", "NODE 4", "NODE 5", "NODE 6",
};

bool node_mac_configured(int node_index)
{
	if (node_index < 0 || node_index >= hub_mesh::MAX_NODES) {
		return false;
	}
	const uint8_t *mac = hub_mesh::NODE_MACS[node_index];
	for (int byte_index = 0; byte_index < ESP_NOW_ETH_ALEN; ++byte_index) {
		if (mac[byte_index] != 0) {
			return true;
		}
	}
	return false;
}

bool node_is_connected(int node_index)
{
	if (node_index < 0 || node_index >= hub_mesh::MAX_NODES) {
		return false;
	}
	return node_slots[node_index].connected.load(std::memory_order_relaxed);
}

int connected_node_count()
{
	int count = 0;
	for (int index = 0; index < hub_mesh::MAX_NODES; ++index) {
		if (node_is_connected(index)) {
			++count;
		}
	}
	return count;
}

// Non-blocking expiry check: called periodically from the touch task loop so
// a node that stops sending Hello messages is dropped without ever blocking
// the UI thread.
void expire_stale_nodes()
{
	const int64_t now_us = esp_timer_get_time();
	for (int index = 0; index < hub_mesh::MAX_NODES; ++index) {
		if (!node_slots[index].connected.load(std::memory_order_relaxed)) {
			continue;
		}
		const int64_t last_seen_us = node_slots[index].last_seen_us.load(std::memory_order_relaxed);
		if (now_us - last_seen_us > NODE_CONNECTION_TIMEOUT_US) {
			node_slots[index].connected.store(false, std::memory_order_relaxed);
		}
	}
}

// ---------------------------------------------------------------------
// ESP-NOW mesh message scaffold. Only a Hello heartbeat is implemented for
// now; scan/control messages will be added alongside the node project.
//
// Hello is expected to be sent periodically by each node (node -> hub only);
// the hub does not send its own Hello, it only listens and marks a node
// slot connected/disconnected based on what it receives. Sending a hub-side
// Hello/discovery message is left for future work alongside the node
// project.
// ---------------------------------------------------------------------

constexpr uint32_t MESH_MESSAGE_MAGIC = 0x42455755u;
constexpr uint8_t MESH_PROTOCOL_VERSION = 1;

enum class MeshMessageType : uint8_t {
	Hello = 1,
};

struct MeshMessage {
	uint32_t magic = MESH_MESSAGE_MAGIC;
	uint8_t version = MESH_PROTOCOL_VERSION;
	MeshMessageType type = MeshMessageType::Hello;
};
static_assert(sizeof(MeshMessage) <= ESP_NOW_MAX_DATA_LEN, "MeshMessage exceeds ESP-NOW payload size");

int mesh_node_index(const uint8_t *mac_address)
{
	for (int index = 0; index < hub_mesh::MAX_NODES; ++index) {
		if (node_mac_configured(index) && std::memcmp(mac_address, hub_mesh::NODE_MACS[index], ESP_NOW_ETH_ALEN) == 0) {
			return index;
		}
	}
	return -1;
}

void add_node_peer(int node_index)
{
	if (!node_mac_configured(node_index)) {
		return;
	}

	esp_now_peer_info_t peer = {};
	std::memcpy(peer.peer_addr, hub_mesh::NODE_MACS[node_index], ESP_NOW_ETH_ALEN);
	peer.ifidx = WIFI_IF_STA;
	peer.channel = hub_mesh::ESP_NOW_CHANNEL;
	// TODO: enable ESP-NOW encryption (peer.encrypt + esp_now_set_pmk()/lmk)
	// once a real per-deployment key is provisioned alongside the node MAC
	// addresses in peer_mac_addresses.h; unencrypted is only acceptable for
	// this placeholder-MAC scaffold.
	peer.encrypt = false;
	ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void mesh_receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_length)
{
	if (recv_info == nullptr || data == nullptr || data_length != static_cast<int>(sizeof(MeshMessage))) {
		return;
	}

	const MeshMessage *message = reinterpret_cast<const MeshMessage *>(data);
	if (message->magic != MESH_MESSAGE_MAGIC || message->version != MESH_PROTOCOL_VERSION) {
		return;
	}

	const int node_index = mesh_node_index(recv_info->src_addr);
	if (node_index < 0 || message->type != MeshMessageType::Hello) {
		return;
	}

	node_slots[node_index].last_seen_us.store(esp_timer_get_time(), std::memory_order_relaxed);
	node_slots[node_index].connected.store(true, std::memory_order_relaxed);
}

void mesh_send_callback(const esp_now_send_info_t *send_info, esp_now_send_status_t status)
{
	(void)send_info;
	(void)status;
}

// ---------------------------------------------------------------------
// Framebuffer + primitive drawing helpers.
// ---------------------------------------------------------------------

uint16_t *framebuffer = nullptr;

void fill_screen(uint16_t color)
{
	for (int pixel = 0; pixel < DISPLAY_WIDTH * DISPLAY_HEIGHT; ++pixel) {
		framebuffer[pixel] = color;
	}
}

void draw_framebuffer(esp_lcd_panel_handle_t panel_handle)
{
	for (int top = 0; top < DISPLAY_HEIGHT; top += LCD_TRANSFER_LINES) {
		const int bottom = top + LCD_TRANSFER_LINES < DISPLAY_HEIGHT ? top + LCD_TRANSFER_LINES : DISPLAY_HEIGHT;
		ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, top, DISPLAY_WIDTH, bottom, &framebuffer[top * DISPLAY_WIDTH]));
	}
}

void draw_rectangle_border(int x, int y, int width, int height, uint16_t color)
{
	for (int border = 0; border < BOX_BORDER_WIDTH; ++border) {
		const int left = x + border;
		const int right = x + width - border - 1;
		const int top = y + border;
		const int bottom = y + height - border - 1;

		for (int column = left; column <= right; ++column) {
			framebuffer[top * DISPLAY_WIDTH + column] = color;
			framebuffer[bottom * DISPLAY_WIDTH + column] = color;
		}
		for (int row = top; row <= bottom; ++row) {
			framebuffer[row * DISPLAY_WIDTH + left] = color;
			framebuffer[row * DISPLAY_WIDTH + right] = color;
		}
	}
}

void fill_rectangle(int x, int y, int width, int height, uint16_t color)
{
	for (int row = y; row < y + height; ++row) {
		for (int column = x; column < x + width; ++column) {
			framebuffer[row * DISPLAY_WIDTH + column] = color;
		}
	}
}

// Minimal 5x7 bitmap font covering the characters used by the hub UI.
const uint8_t *glyph_rows(char character)
{
	if (character >= 'a' && character <= 'z') {
		character = static_cast<char>(character - ('a' - 'A'));
	}
	static constexpr uint8_t A[] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
	static constexpr uint8_t B[] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
	static constexpr uint8_t C[] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
	static constexpr uint8_t D[] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
	static constexpr uint8_t E[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
	static constexpr uint8_t F[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
	static constexpr uint8_t G[] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
	static constexpr uint8_t I[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
	static constexpr uint8_t L[] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
	static constexpr uint8_t M[] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
	static constexpr uint8_t N[] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
	static constexpr uint8_t O[] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static constexpr uint8_t P[] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
	static constexpr uint8_t S[] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
	static constexpr uint8_t T[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
	static constexpr uint8_t U[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static constexpr uint8_t V[] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
	static constexpr uint8_t W[] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
	static constexpr uint8_t ZERO[] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static constexpr uint8_t ONE[] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
	static constexpr uint8_t TWO[] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
	static constexpr uint8_t THREE[] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
	static constexpr uint8_t FOUR[] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
	static constexpr uint8_t FIVE[] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
	static constexpr uint8_t SIX[] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
	static constexpr uint8_t SEVEN[] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
	static constexpr uint8_t EIGHT[] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
	static constexpr uint8_t NINE[] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E};

	switch (character) {
	case 'A': return A;
	case 'B': return B;
	case 'C': return C;
	case 'D': return D;
	case 'E': return E;
	case 'F': return F;
	case 'G': return G;
	case 'I': return I;
	case 'L': return L;
	case 'M': return M;
	case 'N': return N;
	case 'O': return O;
	case 'P': return P;
	case 'S': return S;
	case 'T': return T;
	case 'U': return U;
	case 'V': return V;
	case 'W': return W;
	case '0': return ZERO;
	case '1': return ONE;
	case '2': return TWO;
	case '3': return THREE;
	case '4': return FOUR;
	case '5': return FIVE;
	case '6': return SIX;
	case '7': return SEVEN;
	case '8': return EIGHT;
	case '9': return NINE;
	default: return nullptr;
	}
}

void draw_label(const Rectangle &area, const char *label, uint16_t color, int scale, int letter_spacing)
{
	constexpr int GLYPH_WIDTH = 5;
	constexpr int GLYPH_HEIGHT = 7;
	int character_count = 0;
	while (label[character_count] != '\0') {
		++character_count;
	}

	const int label_width = character_count * GLYPH_WIDTH * scale + (character_count - 1) * letter_spacing;
	const int start_x = area.x + (area.width - label_width) / 2;
	const int start_y = area.y + (area.height - GLYPH_HEIGHT * scale) / 2;

	for (int character_index = 0; character_index < character_count; ++character_index) {
		const uint8_t *rows = glyph_rows(label[character_count - 1 - character_index]);
		if (rows == nullptr) {
			continue;
		}
		const int glyph_x = start_x + character_index * (GLYPH_WIDTH * scale + letter_spacing);
		for (int row = 0; row < GLYPH_HEIGHT; ++row) {
			for (int column = 0; column < GLYPH_WIDTH; ++column) {
				if ((rows[row] & (1U << (GLYPH_WIDTH - 1 - column))) != 0) {
					fill_rectangle(glyph_x + (GLYPH_WIDTH - 1 - column) * scale, start_y + row * scale, scale, scale, color);
				}
			}
		}
	}
}

void draw_button_label(const Rectangle &area, const char *label, uint16_t color)
{
	draw_label(area, label, color, 2, 2);
}

void draw_small_label(const Rectangle &area, const char *label, uint16_t color)
{
	draw_label(area, label, color, 1, 1);
}

void draw_loading_spinner(int center_x, int center_y, int frame, uint16_t color)
{
	constexpr int RING_POINTS = 8;
	constexpr int LIT_POINTS = 6;
	static constexpr int8_t OFFSETS[RING_POINTS][2] = {
		{0, -4}, {3, -3}, {4, 0}, {3, 3}, {0, 4}, {-3, 3}, {-4, 0}, {-3, -3},
	};
	for (int point = 0; point < LIT_POINTS; ++point) {
		const int point_index = (frame + point) % RING_POINTS;
		fill_rectangle(center_x + OFFSETS[point_index][0] - 1, center_y + OFFSETS[point_index][1] - 1, 2, 2, color);
	}
}

void draw_loading_status(const Rectangle &area, int frame)
{
	constexpr int GLYPH_WIDTH = 5;
	constexpr int LETTER_SPACING = 1;
	constexpr int SPINNER_DIAMETER = 10;
	constexpr int SPINNER_GAP = 5;
	constexpr int LABEL_LENGTH = 7;
	const int label_width = LABEL_LENGTH * GLYPH_WIDTH + (LABEL_LENGTH - 1) * LETTER_SPACING;
	const int total_width = SPINNER_DIAMETER + SPINNER_GAP + label_width;
	const int start_x = area.x + (area.width - total_width) / 2;
	draw_loading_spinner(start_x + SPINNER_DIAMETER / 2, area.y + area.height / 2, frame, COLOR_BRIGHT_ORANGE);
	const Rectangle label_area = {start_x + SPINNER_DIAMETER + SPINNER_GAP, area.y, label_width, area.height};
	draw_small_label(label_area, "LOADING", COLOR_LIGHT_GRAY);
}

void draw_connection_status(const Rectangle &area)
{
	const int connected_count = connected_node_count();
	if (connected_count == 0) {
		draw_small_label(area, "NO NODES CONNECTED", COLOR_LIGHT_GRAY);
	} else if (connected_count == 1) {
		draw_small_label(area, "1 NODE CONNECTED", COLOR_LIGHT_GRAY);
	} else {
		char status_text[24];
		std::snprintf(status_text, sizeof(status_text), "%d NODES CONNECTED", connected_count);
		draw_small_label(area, status_text, COLOR_LIGHT_GRAY);
	}
}

// ---------------------------------------------------------------------
// Screen rendering.
// ---------------------------------------------------------------------

void draw_menu_frame(int active_button)
{
	fill_screen(COLOR_BLACK);
	draw_rectangle_border(HEADER_BOX.x, HEADER_BOX.y, HEADER_BOX.width, HEADER_BOX.height, COLOR_BRIGHT_ORANGE);
	draw_button_label(HEADER_BOX, "BEOWULF9001", COLOR_WHITE);
	draw_rectangle_border(BOX_MARGIN, CONTENT_TOP, TOP_BOX_WIDTH, TOP_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);
	draw_rectangle_border(BOX_MARGIN + TOP_BOX_WIDTH + BOX_GAP, CONTENT_TOP, TOP_BOX_WIDTH, TOP_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);
	draw_rectangle_border(BOX_MARGIN, BOTTOM_BOX_Y, DISPLAY_WIDTH - (2 * BOX_MARGIN), BOTTOM_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);

	for (int index = 0; index < 3; ++index) {
		const uint16_t button_color = index == active_button ? COLOR_WHITE : COLOR_LIGHT_GRAY;
		fill_rectangle(BUTTONS[index].x, BUTTONS[index].y, BUTTONS[index].width, BUTTONS[index].height, button_color);
	}
	draw_button_label(BUTTONS[0], "SETTINGS", active_button == 0 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
	draw_button_label(BUTTONS[1], "DEVICES", active_button == 1 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
	draw_button_label(BUTTONS[2], "MODE", active_button == 2 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
}

void draw_submenu_header(const char *title)
{
	fill_screen(COLOR_BLACK);
	draw_rectangle_border(HEADER_BOX.x, HEADER_BOX.y, HEADER_BOX.width, HEADER_BOX.height, COLOR_BRIGHT_ORANGE);
	draw_button_label(HEADER_BOX, title, COLOR_WHITE);
	draw_rectangle_border(BACK_BUTTON_VISUAL.x, BACK_BUTTON_VISUAL.y, BACK_BUTTON_VISUAL.width, BACK_BUTTON_VISUAL.height, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 7, BACK_BUTTON_VISUAL.y + 9, 10, 4, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 14, BACK_BUTTON_VISUAL.y + 6, 3, 10, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 17, BACK_BUTTON_VISUAL.y + 8, 3, 6, COLOR_DARK_ORANGE);
}

// Renders the Devices screen. Connected node slots are drawn as bright,
// tappable cards; disconnected slots are dimmed placeholders that never
// respond to touch (see grid_card_at() callers in the touch task).
void draw_devices_screen(int active_card = -1)
{
	draw_submenu_header("DEVICES");

	for (int index = 0; index < hub_mesh::MAX_NODES; ++index) {
		const Rectangle card = grid_card_rect(index);
		const bool connected = node_is_connected(index);

		if (!connected) {
			draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DIM_GRAY);
			continue;
		}

		if (index == active_card) {
			fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
		}
		draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
		draw_button_label(card, NODE_LABELS[index], index == active_card ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
	}

	const Rectangle status_area = {BOX_MARGIN, GRID_STATUS_Y, DISPLAY_WIDTH - (2 * BOX_MARGIN), GRID_STATUS_HEIGHT};
	draw_connection_status(status_area);
}

// Placeholder toggles; real behavior will be defined alongside node
// communication work.
bool setting_option_a_enabled = false;
bool setting_option_b_enabled = false;

void draw_settings_screen(int active_card = -1)
{
	draw_submenu_header("SETTINGS");

	const char *labels[2] = {"OPTION A", "OPTION B"};
	const bool enabled[2] = {setting_option_a_enabled, setting_option_b_enabled};
	for (int index = 0; index < 2; ++index) {
		const Rectangle card = grid_card_rect(index);
		if (index == active_card) {
			fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
		}
		if (enabled[index]) {
			fill_rectangle(card.x, card.y, card.width, card.height, COLOR_BRIGHT_ORANGE);
		}
		draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
		draw_button_label(card, labels[index], enabled[index] ? COLOR_BLACK : COLOR_WHITE);
	}
}

// Placeholder modes; real scan/control modes will be defined alongside node
// communication work.
int active_mode = -1;

void draw_mode_screen(int active_card = -1)
{
	draw_submenu_header("MODE");

	const char *labels[2] = {"MODE A", "MODE B"};
	for (int index = 0; index < 2; ++index) {
		const Rectangle card = grid_card_rect(index);
		if (index == active_card || index == active_mode) {
			fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
		}
		draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
		draw_button_label(card, labels[index], (index == active_card || index == active_mode) ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
	}
}

// ---------------------------------------------------------------------
// Touch input.
//
// LCD (esp_lcd_panel_draw_bitmap) and touch (read_touch_axis) transactions
// both run on SPI2, but all draw_framebuffer()/read_touch_axis() calls in
// this scaffold happen from touch_monitor_task alone, so accesses are
// naturally serialized -- there is no cross-task bus contention today. If a
// future change adds LCD/touch access from another task, that access must
// be synchronized (e.g. with a mutex) before running concurrently with this
// task.
// ---------------------------------------------------------------------

struct TouchMonitorContext {
	spi_device_handle_t touch_device;
	esp_lcd_panel_handle_t panel_handle;
};
TouchMonitorContext touch_context = {};

int scale_touch_coordinate(uint16_t raw_value, int raw_minimum, int raw_maximum, int display_size)
{
	if (raw_value <= raw_minimum) {
		return 0;
	}
	if (raw_value >= raw_maximum) {
		return display_size - 1;
	}
	return static_cast<int>((raw_value - raw_minimum) * (display_size - 1) / (raw_maximum - raw_minimum));
}

esp_err_t read_touch_axis(spi_device_handle_t touch_device, uint8_t command, uint16_t *value)
{
	uint8_t transmit_data[3] = {command, 0, 0};
	uint8_t receive_data[3] = {};
	spi_transaction_t transaction = {};
	transaction.length = sizeof(transmit_data) * 8;
	transaction.tx_buffer = transmit_data;
	transaction.rx_buffer = receive_data;

	esp_err_t result = spi_device_polling_transmit(touch_device, &transaction);
	if (result == ESP_OK) {
		*value = static_cast<uint16_t>(((receive_data[1] << 8) | receive_data[2]) >> 3);
	}
	return result;
}

// Per-screen touch handlers, extracted from touch_monitor_task to keep the
// polling loop itself short and to make it easy to add new screens later.

void handle_menu_touch(const TouchMonitorContext *context, int screen_x, int screen_y, Screen &current_screen)
{
	const int selected_button = button_at(screen_x, screen_y);
	if (selected_button < 0) {
		return;
	}

	draw_menu_frame(selected_button);
	draw_framebuffer(context->panel_handle);
	vTaskDelay(pdMS_TO_TICKS(MENU_TRANSITION_DELAY_MS));

	switch (selected_button) {
	case 0:
		current_screen = Screen::Settings;
		draw_settings_screen();
		break;
	case 1:
		current_screen = Screen::Devices;
		draw_devices_screen();
		break;
	case 2:
		current_screen = Screen::Mode;
		draw_mode_screen();
		break;
	default:
		break;
	}
	draw_framebuffer(context->panel_handle);
}

void handle_devices_touch(const TouchMonitorContext *context, int screen_x, int screen_y)
{
	const int selected_card = grid_card_at(screen_x, screen_y, hub_mesh::MAX_NODES);
	// Disconnected placeholders never respond to touch.
	if (selected_card < 0 || !node_is_connected(selected_card)) {
		return;
	}

	draw_devices_screen(selected_card);
	draw_framebuffer(context->panel_handle);
	vTaskDelay(pdMS_TO_TICKS(MENU_TRANSITION_DELAY_MS));
	draw_devices_screen();
	draw_framebuffer(context->panel_handle);
}

void handle_settings_touch(const TouchMonitorContext *context, int screen_x, int screen_y)
{
	const int selected_card = grid_card_at(screen_x, screen_y, 2);
	if (selected_card == 0) {
		setting_option_a_enabled = !setting_option_a_enabled;
	} else if (selected_card == 1) {
		setting_option_b_enabled = !setting_option_b_enabled;
	} else {
		return;
	}
	draw_settings_screen();
	draw_framebuffer(context->panel_handle);
}

void handle_mode_touch(const TouchMonitorContext *context, int screen_x, int screen_y)
{
	const int selected_card = grid_card_at(screen_x, screen_y, 2);
	if (selected_card < 0) {
		return;
	}
	active_mode = active_mode == selected_card ? -1 : selected_card;
	draw_mode_screen();
	draw_framebuffer(context->panel_handle);
}

void touch_monitor_task(void *parameter)
{
	const TouchMonitorContext *context = static_cast<TouchMonitorContext *>(parameter);
	Screen current_screen = Screen::Menu;
	bool touch_was_active = false;

	while (true) {
		expire_stale_nodes();

		uint16_t pressure = 0;
		if (read_touch_axis(context->touch_device, 0xB0, &pressure) != ESP_OK) {
			// Skip this cycle on a transient SPI/touch read failure rather than
			// aborting the whole hub; the next poll will retry.
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}

		const bool touch_active = pressure > TOUCH_PRESSURE_THRESHOLD;
		if (touch_active && !touch_was_active) {
			uint16_t raw_x = 0;
			uint16_t raw_y = 0;
			if (read_touch_axis(context->touch_device, 0xD0, &raw_x) != ESP_OK ||
				read_touch_axis(context->touch_device, 0x90, &raw_y) != ESP_OK) {
				touch_was_active = touch_active;
				vTaskDelay(pdMS_TO_TICKS(50));
				continue;
			}
			// Same touch orientation/mapping as the v1.0 reference project:
			// axes are swapped and mirrored to match the panel's swap_xy setup.
			const int screen_x = DISPLAY_WIDTH - 1 - scale_touch_coordinate(raw_y, RAW_TOUCH_Y_MIN, RAW_TOUCH_Y_MAX, DISPLAY_WIDTH);
			const int screen_y = DISPLAY_HEIGHT - 1 - scale_touch_coordinate(raw_x, RAW_TOUCH_X_MIN, RAW_TOUCH_X_MAX, DISPLAY_HEIGHT);

			if (current_screen == Screen::Menu) {
				handle_menu_touch(context, screen_x, screen_y, current_screen);
			} else if (point_in_rectangle(screen_x, screen_y, BACK_BUTTON_TOUCH)) {
				current_screen = Screen::Menu;
				draw_menu_frame(-1);
				draw_framebuffer(context->panel_handle);
			} else if (current_screen == Screen::Devices) {
				handle_devices_touch(context, screen_x, screen_y);
			} else if (current_screen == Screen::Settings) {
				handle_settings_touch(context, screen_x, screen_y);
			} else if (current_screen == Screen::Mode) {
				handle_mode_touch(context, screen_x, screen_y);
			}
		}
		touch_was_active = touch_active;
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

} // namespace

extern "C" void app_main(void)
{
	ESP_ERROR_CHECK(esp_psram_init());
	esp_err_t nvs_result = nvs_flash_init();
	if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		nvs_result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs_result);
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_channel(hub_mesh::ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_recv_cb(mesh_receive_callback));
	ESP_ERROR_CHECK(esp_now_register_send_cb(mesh_send_callback));
	for (int node_index = 0; node_index < hub_mesh::MAX_NODES; ++node_index) {
		add_node_peer(node_index);
	}

	// Intentionally never freed: this is a single allocation for the
	// lifetime of the firmware (app_main never returns).
	framebuffer = static_cast<uint16_t *>(heap_caps_malloc(sizeof(uint16_t) * DISPLAY_WIDTH * DISPLAY_HEIGHT, MALLOC_CAP_SPIRAM));
	if (framebuffer == nullptr) {
		ESP_LOGE(TAG, "Failed to allocate PSRAM framebuffer");
		abort();
	}

	ESP_ERROR_CHECK(gpio_set_direction(LCD_BACKLIGHT, GPIO_MODE_OUTPUT));
	ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 1));

	spi_bus_config_t bus_config = {};
	bus_config.mosi_io_num = LCD_MOSI;
	bus_config.miso_io_num = TOUCH_MISO;
	bus_config.sclk_io_num = LCD_SCLK;
	bus_config.quadwp_io_num = GPIO_NUM_NC;
	bus_config.quadhd_io_num = GPIO_NUM_NC;
	bus_config.max_transfer_sz = DISPLAY_WIDTH * LCD_TRANSFER_LINES * static_cast<int>(sizeof(uint16_t));
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

	spi_device_interface_config_t touch_config = {};
	touch_config.clock_speed_hz = 2'500'000;
	touch_config.mode = 0;
	touch_config.spics_io_num = TOUCH_CS;
	touch_config.queue_size = 1;
	spi_device_handle_t touch_device = nullptr;
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &touch_config, &touch_device));

	esp_lcd_panel_io_spi_config_t io_config = {};
	io_config.cs_gpio_num = LCD_CS;
	io_config.dc_gpio_num = LCD_DC;
	io_config.spi_mode = 0;
	io_config.pclk_hz = 20 * 1000 * 1000;
	io_config.trans_queue_depth = 10;
	io_config.lcd_cmd_bits = 8;
	io_config.lcd_param_bits = 8;
	esp_lcd_panel_io_handle_t io_handle = nullptr;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io_config, &io_handle));

	esp_lcd_panel_dev_config_t panel_config = {};
	panel_config.reset_gpio_num = GPIO_NUM_NC;
	panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
	panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
	panel_config.bits_per_pixel = 16;
	esp_lcd_panel_handle_t panel_handle = nullptr;
	ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

	draw_menu_frame(-1);
	draw_framebuffer(panel_handle);
	touch_context.touch_device = touch_device;
	touch_context.panel_handle = panel_handle;
	xTaskCreate(touch_monitor_task, "touch_monitor", 4096, &touch_context, 5, nullptr);
}
