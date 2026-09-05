#include <atomic>
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
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "../peer_mac_addresses.h"

namespace {

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
constexpr uint32_t MESH_MESSAGE_MAGIC = 0x42455755u;
constexpr uint8_t MESH_PROTOCOL_VERSION = 1;
constexpr int MESH_SCAN_RESPONSE_TIMEOUT_MS = 10'000;
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t panel_color(uint16_t color)
{
	return static_cast<uint16_t>((color >> 8) | (color << 8));
}
constexpr uint16_t COLOR_LIGHT_GRAY = panel_color(0xC618);
constexpr uint16_t COLOR_BRIGHT_ORANGE = panel_color(0xFD20);
constexpr uint16_t COLOR_DARK_ORANGE = panel_color(0xA400);

enum class MeshMessageType : uint8_t {
	Hello = 1,
	ScanRequest,
	ScanResult,
	ScanComplete,
};

enum ScanFlags : uint8_t {
	Scan2_4Ghz = 1 << 0,
	Scan5Ghz = 1 << 1,
	ScanBle = 1 << 2,
};

enum class ScanResultKind : uint8_t {
	Wifi = 1,
	Ble,
};

struct MeshMessage {
	uint32_t magic = MESH_MESSAGE_MAGIC;
	uint8_t version = MESH_PROTOCOL_VERSION;
	MeshMessageType type = MeshMessageType::Hello;
	uint16_t sequence = 0;
	uint8_t payload_length = 0;
};

struct ScanRequestPayload {
	uint16_t request_id;
	uint8_t scan_flags;
	uint8_t reserved;
	uint16_t hub_millisecond_in_second;
};

struct ScanResultPayload {
	uint16_t request_id;
	uint16_t result_index;
	ScanResultKind kind;
	int8_t rssi;
	uint8_t channel;
	uint8_t name_length;
	char name[32];
};

struct ScanCompletePayload {
	uint16_t request_id;
	uint16_t wifi_result_count;
	uint16_t ble_result_count;
	uint8_t completed_scan_flags;
	uint8_t status;
};
static_assert(sizeof(MeshMessage) <= ESP_NOW_MAX_DATA_LEN, "MeshMessage exceeds ESP-NOW payload size");
static_assert(sizeof(MeshMessage) + sizeof(ScanRequestPayload) <= ESP_NOW_MAX_DATA_LEN, "ScanRequest exceeds ESP-NOW payload size");
static_assert(sizeof(MeshMessage) + sizeof(ScanResultPayload) <= ESP_NOW_MAX_DATA_LEN, "ScanResult exceeds ESP-NOW payload size");
static_assert(sizeof(MeshMessage) + sizeof(ScanCompletePayload) <= ESP_NOW_MAX_DATA_LEN, "ScanComplete exceeds ESP-NOW payload size");

std::atomic<uint32_t> mesh_received_message_count{0};
uint16_t mesh_message_sequence = 0;
uint16_t mesh_scan_request_id = 0;

enum class Screen {
	Menu,
	Settings,
	Devices,
	Mode,
	ScanResults,
};

enum class ScanView {
	Device,
	ModeWifi,
	ModeBluetooth,
};

struct Rectangle {
	int x;
	int y;
	int width;
	int height;
};

constexpr int TOP_BOX_WIDTH = (DISPLAY_WIDTH - (2 * BOX_MARGIN) - BOX_GAP) / 2;
constexpr int CONTENT_TOP = BOX_MARGIN + HEADER_BOX_HEIGHT + BOX_GAP;
constexpr int TOP_BOX_HEIGHT = (DISPLAY_HEIGHT - CONTENT_TOP - BOX_MARGIN - BOX_GAP) / 2;
constexpr int BOTTOM_BOX_Y = CONTENT_TOP + TOP_BOX_HEIGHT + BOX_GAP;
constexpr int BOTTOM_BOX_HEIGHT = DISPLAY_HEIGHT - BOTTOM_BOX_Y - BOX_MARGIN;
constexpr Rectangle HEADER_BOX = {BOX_MARGIN, BOX_MARGIN, DISPLAY_WIDTH - (2 * BOX_MARGIN), HEADER_BOX_HEIGHT};
constexpr Rectangle BACK_BUTTON_VISUAL = {HEADER_BOX.x + HEADER_BOX.width - 31, HEADER_BOX.y + 2, 26, 22};
constexpr Rectangle BACK_BUTTON_TOUCH = {0, HEADER_BOX.y, HEADER_BOX.height, HEADER_BOX.height};
constexpr int DEVICE_GRID_COLUMNS = 2;
constexpr int DEVICE_GRID_ROWS = 4;
constexpr int DEVICE_CARD_GAP = 6;
constexpr int DEVICE_STATUS_HEIGHT = 22;
constexpr int DEVICE_CARD_WIDTH = (DISPLAY_WIDTH - (2 * BOX_MARGIN) - DEVICE_CARD_GAP) / DEVICE_GRID_COLUMNS;
constexpr int DEVICE_CARD_HEIGHT = (DISPLAY_HEIGHT - CONTENT_TOP - DEVICE_STATUS_HEIGHT - BOX_MARGIN - ((DEVICE_GRID_ROWS - 1) * DEVICE_CARD_GAP)) / DEVICE_GRID_ROWS;
constexpr int DEVICE_STATUS_Y = CONTENT_TOP + (DEVICE_GRID_ROWS * DEVICE_CARD_HEIGHT) + ((DEVICE_GRID_ROWS - 1) * DEVICE_CARD_GAP) + 4;
constexpr int SCAN_LIST_ROW_HEIGHT = 20;
constexpr int SCAN_LIST_X = BOX_MARGIN;
constexpr int SCAN_LIST_WIDTH = DISPLAY_WIDTH - (2 * BOX_MARGIN) - 14;
constexpr int SCAN_SCROLLBAR_X = BOX_MARGIN;
constexpr int SCAN_SCROLLBAR_WIDTH = 4;
constexpr int SCAN_SCROLL_TOUCH_TOP = CONTENT_TOP - 16;
constexpr int SCAN_SCROLL_TOUCH_BOTTOM = DISPLAY_HEIGHT - BOX_MARGIN - 56;
constexpr int SCAN_SCROLL_TOUCH_EDGE_MARGIN = 28;
constexpr int SCAN_SCROLL_TOUCH_COLUMN_WIDTH = 60;
constexpr int MODE_STATUS_HEIGHT = 22;
constexpr Rectangle MODE_STATUS_AREA = {SCAN_LIST_X, DISPLAY_HEIGHT - BOX_MARGIN - MODE_STATUS_HEIGHT, SCAN_LIST_WIDTH, MODE_STATUS_HEIGHT};
constexpr Rectangle BUTTONS[] = {
	{BOX_MARGIN + (TOP_BOX_WIDTH - BUTTON_WIDTH) / 2, CONTENT_TOP + (TOP_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
	{BOX_MARGIN + TOP_BOX_WIDTH + BOX_GAP + (TOP_BOX_WIDTH - BUTTON_WIDTH) / 2, CONTENT_TOP + (TOP_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
	{(DISPLAY_WIDTH - BUTTON_WIDTH) / 2, BOTTOM_BOX_Y + (BOTTOM_BOX_HEIGHT - BUTTON_HEIGHT) / 2, BUTTON_WIDTH, BUTTON_HEIGHT},
};

constexpr gpio_num_t LCD_SCLK = GPIO_NUM_6;
constexpr gpio_num_t LCD_MOSI = GPIO_NUM_7;
constexpr gpio_num_t LCD_CS = GPIO_NUM_23;
constexpr gpio_num_t LCD_DC = GPIO_NUM_24;
constexpr gpio_num_t LCD_BACKLIGHT = GPIO_NUM_25;
constexpr gpio_num_t TOUCH_MISO = GPIO_NUM_2;
constexpr gpio_num_t TOUCH_CS = GPIO_NUM_1;

constexpr char TAG[] = "nm_cyd_c5_display";
constexpr int BLE_SCAN_DURATION_SECONDS = 3;
constexpr int MODE_RESCAN_INTERVAL_MS = 30'000;
constexpr int BLE_MAX_DISCOVERED_DEVICES = 32;
constexpr int MAX_MESH_NODES = (DEVICE_GRID_COLUMNS * DEVICE_GRID_ROWS) - 1;
constexpr int MAX_MESH_RESULTS_PER_NODE = 32;
constexpr int MAX_MESH_SCAN_RESULTS = MAX_MESH_NODES * MAX_MESH_RESULTS_PER_NODE;
constexpr int MESH_SCAN_EVENT_QUEUE_LENGTH = 96;
constexpr char MESH_NODE_LABELS[MAX_MESH_NODES][10] = {
	"C5 NODE B",
	"C5 NODE C",
	"C5 NODE D",
	"C5 NODE E",
	"C5 NODE F",
	"C5 NODE G",
	"C5 NODE H",
};
const uint8_t *const MESH_NODE_MACS[MAX_MESH_NODES] = {
	mesh_reference::remote_peer_b_mac,
	mesh_reference::remote_peer_c_mac,
	mesh_reference::remote_peer_d_mac,
	mesh_reference::remote_peer_e_mac,
	mesh_reference::remote_peer_f_mac,
	nullptr,
	nullptr,
};
uint16_t *framebuffer = nullptr;
bool scan_5ghz_enabled = true;
bool bluetooth_enabled = false;
std::atomic<uint8_t> connected_mesh_node_count{0};
struct MeshScanResult {
	uint8_t node_index;
	ScanResultPayload payload;
};
struct MeshScanEvent {
	uint8_t node_index;
	MeshMessageType type;
	ScanResultPayload result;
	ScanCompletePayload complete;
};
MeshScanResult mesh_scan_results[MAX_MESH_SCAN_RESULTS] = {};
uint16_t mesh_scan_result_count = 0;
uint16_t active_mesh_scan_request_id = 0;
uint8_t pending_mesh_scan_nodes = 0;
TickType_t mesh_scan_response_deadline = 0;
QueueHandle_t mesh_scan_event_queue = nullptr;
int viewed_node_index = -1;
bool mesh_scan_in_progress = false;
int mesh_scan_spinner_frame = 0;
wifi_ap_record_t *discovered_networks = nullptr;
uint16_t discovered_network_count = 0;
uint16_t scan_scroll_offset = 0;
ScanView scan_view = ScanView::Device;
TickType_t next_mode_bluetooth_scan = 0;
TickType_t next_mode_wifi_scan = 0;
volatile bool wifi_scan_complete = false;
struct BleDevice {
	char name[33];
	uint8_t address[6];
};
BleDevice *discovered_ble_devices = nullptr;
uint16_t discovered_ble_device_count = 0;
struct TouchMonitorContext {
	spi_device_handle_t touch_device;
	esp_lcd_panel_handle_t panel_handle;
};
TouchMonitorContext touch_context = {};

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

const uint8_t *glyph_rows(char character)
{
	if (character >= 'a' && character <= 'z') {
		character = static_cast<char>(character - ('a' - 'A'));
	}
	static constexpr uint8_t A[] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
	static constexpr uint8_t B[] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
	static constexpr uint8_t TWO[] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
	static constexpr uint8_t THREE[] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
	static constexpr uint8_t FOUR[] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
	static constexpr uint8_t FIVE[] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
	static constexpr uint8_t SIX[] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
	static constexpr uint8_t SEVEN[] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
	static constexpr uint8_t EIGHT[] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
	static constexpr uint8_t D[] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
	static constexpr uint8_t E[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
	static constexpr uint8_t F[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
	static constexpr uint8_t H[] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
	static constexpr uint8_t U[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static constexpr uint8_t V[] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
	static constexpr uint8_t I[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
	static constexpr uint8_t J[] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
	static constexpr uint8_t K[] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
	static constexpr uint8_t C[] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
	static constexpr uint8_t L[] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
	static constexpr uint8_t S[] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
	static constexpr uint8_t T[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
	static constexpr uint8_t W[] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
	static constexpr uint8_t N[] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
	static constexpr uint8_t G[] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
	static constexpr uint8_t M[] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
	static constexpr uint8_t O[] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static constexpr uint8_t P[] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
	static constexpr uint8_t Q[] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
	static constexpr uint8_t R[] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
	static constexpr uint8_t ONE[] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
	static constexpr uint8_t NINE[] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E};
	static constexpr uint8_t Z[] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
	static constexpr uint8_t X[] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
	static constexpr uint8_t Y[] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
	static constexpr uint8_t HYPHEN[] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
	static constexpr uint8_t PERIOD[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};

	switch (character) {
	case 'A': return A;
	case 'B': return B;
	case '2': return TWO;
	case '3': return THREE;
	case '4': return FOUR;
	case '5': return FIVE;
	case '6': return SIX;
	case '7': return SEVEN;
	case '8': return EIGHT;
	case 'D': return D;
	case 'E': return E;
	case 'F': return F;
	case 'H': return H;
	case 'U': return U;
	case 'V': return V;
	case 'I': return I;
	case 'J': return J;
	case 'K': return K;
	case 'C': return C;
	case 'L': return L;
	case 'S': return S;
	case 'T': return T;
	case 'W': return W;
	case 'N': return N;
	case 'G': return G;
	case 'M': return M;
	case 'O': return O;
	case 'P': return P;
	case 'Q': return Q;
	case 'R': return R;
	case '0': return O;
	case '1': return ONE;
	case '9': return NINE;
	case 'Z': return Z;
	case 'X': return X;
	case 'Y': return Y;
	case '-': return HYPHEN;
	case '.': return PERIOD;
	default: return nullptr;
	}
}

const uint8_t *header_glyph_rows(char character)
{
	static constexpr uint8_t B[] = {0x06, 0x05, 0x06, 0x05, 0x06};
	static constexpr uint8_t E[] = {0x07, 0x04, 0x06, 0x04, 0x07};
	static constexpr uint8_t F[] = {0x07, 0x04, 0x06, 0x04, 0x04};
	static constexpr uint8_t L[] = {0x04, 0x04, 0x04, 0x04, 0x07};
	static constexpr uint8_t O[] = {0x02, 0x05, 0x05, 0x05, 0x02};
	static constexpr uint8_t U[] = {0x05, 0x05, 0x05, 0x05, 0x07};
	static constexpr uint8_t V[] = {0x05, 0x05, 0x05, 0x05, 0x02};
	static constexpr uint8_t W[] = {0x05, 0x05, 0x07, 0x07, 0x05};
	static constexpr uint8_t ONE[] = {0x02, 0x06, 0x02, 0x02, 0x07};
	static constexpr uint8_t NINE[] = {0x02, 0x05, 0x03, 0x01, 0x02};
	static constexpr uint8_t PERIOD[] = {0x00, 0x00, 0x00, 0x00, 0x02};

	switch (character) {
	case 'B': return B;
	case 'E': return E;
	case 'F': return F;
	case 'L': return L;
	case 'O': return O;
	case 'U': return U;
	case 'V': return V;
	case 'W': return W;
	case '0': return O;
	case '1': return ONE;
	case '9': return NINE;
	case '.': return PERIOD;
	default: return nullptr;
	}
}

void draw_button_label(const Rectangle &button, const char *label, uint16_t color)
{
	constexpr int GLYPH_WIDTH = 5;
	constexpr int GLYPH_HEIGHT = 7;
	constexpr int SCALE = 2;
	constexpr int LETTER_SPACING = 2;
	int character_count = 0;
	while (label[character_count] != '\0') {
		++character_count;
	}

	const int label_width = character_count * GLYPH_WIDTH * SCALE + (character_count - 1) * LETTER_SPACING;
	const int start_x = button.x + (button.width - label_width) / 2;
	const int start_y = button.y + (button.height - GLYPH_HEIGHT * SCALE) / 2;

	for (int character_index = 0; character_index < character_count; ++character_index) {
		const uint8_t *rows = glyph_rows(label[character_count - 1 - character_index]);
		if (rows == nullptr) {
			continue;
		}
		const int glyph_x = start_x + character_index * (GLYPH_WIDTH * SCALE + LETTER_SPACING);
		for (int row = 0; row < GLYPH_HEIGHT; ++row) {
			for (int column = 0; column < GLYPH_WIDTH; ++column) {
				if ((rows[row] & (1U << (GLYPH_WIDTH - 1 - column))) != 0) {
					fill_rectangle(glyph_x + (GLYPH_WIDTH - 1 - column) * SCALE, start_y + row * SCALE, SCALE, SCALE, color);
				}
			}
		}
	}
}

void draw_header_label(const char *label)
{
	constexpr int GLYPH_WIDTH = 3;
	constexpr int GLYPH_HEIGHT = 5;
	constexpr int SCALE = 3;
	constexpr int LETTER_SPACING = 3;
	int character_count = 0;
	while (label[character_count] != '\0') {
		++character_count;
	}

	const int label_width = character_count * GLYPH_WIDTH * SCALE + (character_count - 1) * LETTER_SPACING;
	const int start_x = HEADER_BOX.x + (HEADER_BOX.width - label_width) / 2;
	const int start_y = HEADER_BOX.y + (HEADER_BOX.height - GLYPH_HEIGHT * SCALE) / 2;

	for (int character_index = 0; character_index < character_count; ++character_index) {
		const uint8_t *rows = header_glyph_rows(label[character_count - 1 - character_index]);
		if (rows == nullptr) {
			continue;
		}
		const int glyph_x = start_x + character_index * (GLYPH_WIDTH * SCALE + LETTER_SPACING);
		for (int row = 0; row < GLYPH_HEIGHT; ++row) {
			for (int column = 0; column < GLYPH_WIDTH; ++column) {
				if ((rows[row] & (1U << (GLYPH_WIDTH - 1 - column))) != 0) {
					fill_rectangle(glyph_x + (GLYPH_WIDTH - 1 - column) * SCALE, start_y + row * SCALE, SCALE, SCALE, COLOR_DARK_ORANGE);
				}
			}
		}
	}
}

void draw_small_label(const Rectangle &area, const char *label, uint16_t color)
{
	constexpr int GLYPH_WIDTH = 5;
	constexpr int GLYPH_HEIGHT = 7;
	constexpr int SCALE = 1;
	constexpr int LETTER_SPACING = 1;
	int character_count = 0;
	while (label[character_count] != '\0') {
		++character_count;
	}

	const int label_width = character_count * GLYPH_WIDTH * SCALE + (character_count - 1) * LETTER_SPACING;
	const int start_x = area.x + (area.width - label_width) / 2;
	const int start_y = area.y + (area.height - GLYPH_HEIGHT * SCALE) / 2;

	for (int character_index = 0; character_index < character_count; ++character_index) {
		const uint8_t *rows = glyph_rows(label[character_count - 1 - character_index]);
		if (rows == nullptr) {
			continue;
		}
		const int glyph_x = start_x + character_index * (GLYPH_WIDTH * SCALE + LETTER_SPACING);
		for (int row = 0; row < GLYPH_HEIGHT; ++row) {
			for (int column = 0; column < GLYPH_WIDTH; ++column) {
				if ((rows[row] & (1U << (GLYPH_WIDTH - 1 - column))) != 0) {
					fill_rectangle(glyph_x + (GLYPH_WIDTH - 1 - column) * SCALE, start_y + row * SCALE, SCALE, SCALE, color);
				}
			}
		}
	}
}

void draw_small_label_left(const Rectangle &area, const char *label, uint16_t color)
{
	constexpr int GLYPH_WIDTH = 5;
	constexpr int GLYPH_HEIGHT = 7;
	constexpr int LETTER_SPACING = 1;
	const int start_x = area.x + area.width - 3;
	const int start_y = area.y + (area.height - GLYPH_HEIGHT) / 2;

	for (int character_index = 0; label[character_index] != '\0'; ++character_index) {
		const uint8_t *rows = glyph_rows(label[character_index]);
		if (rows == nullptr) {
			continue;
		}
		const int glyph_x = start_x - character_index * (GLYPH_WIDTH + LETTER_SPACING) - GLYPH_WIDTH;
		for (int row = 0; row < GLYPH_HEIGHT; ++row) {
			for (int column = 0; column < GLYPH_WIDTH; ++column) {
				if ((rows[row] & (1U << (GLYPH_WIDTH - 1 - column))) != 0) {
					fill_rectangle(glyph_x + (GLYPH_WIDTH - 1 - column), start_y + row, 1, 1, color);
				}
			}
		}
	}
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
	if (mesh_scan_in_progress) {
		draw_loading_status(area, mesh_scan_spinner_frame);
		return;
	}
	const uint8_t connected_node_count = connected_mesh_node_count.load(std::memory_order_relaxed);
	if (connected_node_count == 0) {
		draw_small_label(area, "NO OTHER DEVICES CONNECTED", COLOR_LIGHT_GRAY);
	} else if (connected_node_count == 1) {
		draw_small_label(area, "1 DEVICE CONNECTED", COLOR_LIGHT_GRAY);
	} else {
		draw_small_label(area, "DEVICES CONNECTED", COLOR_LIGHT_GRAY);
	}
}

void draw_menu_frame(int active_button)
{
	const int displayed_active_button = active_button == 0 ? 1 : active_button == 1 ? 0 : active_button;

	fill_screen(COLOR_BLACK);
	draw_rectangle_border(HEADER_BOX.x, HEADER_BOX.y, HEADER_BOX.width, HEADER_BOX.height, COLOR_BRIGHT_ORANGE);
	draw_header_label("BEOWULF9001 V1.0");
	draw_rectangle_border(BOX_MARGIN, CONTENT_TOP, TOP_BOX_WIDTH, TOP_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);
	draw_rectangle_border(BOX_MARGIN + TOP_BOX_WIDTH + BOX_GAP, CONTENT_TOP, TOP_BOX_WIDTH, TOP_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);
	draw_rectangle_border(BOX_MARGIN, BOTTOM_BOX_Y, DISPLAY_WIDTH - (2 * BOX_MARGIN), BOTTOM_BOX_HEIGHT, COLOR_BRIGHT_ORANGE);

	for (int index = 0; index < 3; ++index) {
		const uint16_t button_color = index == displayed_active_button ? COLOR_WHITE : COLOR_LIGHT_GRAY;
		fill_rectangle(BUTTONS[index].x, BUTTONS[index].y, BUTTONS[index].width, BUTTONS[index].height, button_color);
	}
	draw_button_label(BUTTONS[1], "DEVICES", displayed_active_button == 1 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
	draw_button_label(BUTTONS[0], "SETTINGS", displayed_active_button == 0 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
	draw_button_label(BUTTONS[2], "MODE", displayed_active_button == 2 ? COLOR_BRIGHT_ORANGE : COLOR_BLACK);
}

void draw_submenu_screen(const char *title)
{
	fill_screen(COLOR_BLACK);
	draw_rectangle_border(HEADER_BOX.x, HEADER_BOX.y, HEADER_BOX.width, HEADER_BOX.height, COLOR_BRIGHT_ORANGE);
	draw_button_label(HEADER_BOX, title, COLOR_WHITE);
	draw_rectangle_border(BACK_BUTTON_VISUAL.x, BACK_BUTTON_VISUAL.y, BACK_BUTTON_VISUAL.width, BACK_BUTTON_VISUAL.height, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 7, BACK_BUTTON_VISUAL.y + 9, 10, 4, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 14, BACK_BUTTON_VISUAL.y + 6, 3, 10, COLOR_DARK_ORANGE);
	fill_rectangle(BACK_BUTTON_VISUAL.x + 17, BACK_BUTTON_VISUAL.y + 8, 3, 6, COLOR_DARK_ORANGE);
}

void draw_devices_screen(int active_card = -1)
{
	draw_submenu_screen("DEVICES");
	const uint8_t connected_node_count = connected_mesh_node_count.load(std::memory_order_relaxed);

	for (int row = 0; row < DEVICE_GRID_ROWS; ++row) {
		for (int column = 0; column < DEVICE_GRID_COLUMNS; ++column) {
			const int card_index = row * DEVICE_GRID_COLUMNS + column;
			const Rectangle card = {
				BOX_MARGIN + ((DEVICE_GRID_COLUMNS - 1 - column) * (DEVICE_CARD_WIDTH + DEVICE_CARD_GAP)),
				CONTENT_TOP + (row * (DEVICE_CARD_HEIGHT + DEVICE_CARD_GAP)),
				DEVICE_CARD_WIDTH,
				DEVICE_CARD_HEIGHT,
			};
			if (card_index == active_card) {
				fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
			}
			draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
			if (card_index == 0) {
				draw_button_label(card, "SELF-C5", card_index == active_card ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
			} else if (card_index <= connected_node_count) {
				draw_button_label(card, MESH_NODE_LABELS[card_index - 1], card_index == active_card ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
			}
		}
	}

	const Rectangle status_area = {BOX_MARGIN, DEVICE_STATUS_Y, DISPLAY_WIDTH - (2 * BOX_MARGIN), DEVICE_STATUS_HEIGHT};
	draw_connection_status(status_area);
}

void draw_settings_screen(int active_card = -1)
{
	draw_submenu_screen("SETTINGS");

	for (int row = 0; row < DEVICE_GRID_ROWS; ++row) {
		for (int column = 0; column < DEVICE_GRID_COLUMNS; ++column) {
			const int card_index = row * DEVICE_GRID_COLUMNS + column;
			const bool setting_enabled = card_index == 0 ? scan_5ghz_enabled : card_index == 1 ? bluetooth_enabled : false;
			const Rectangle card = {
				BOX_MARGIN + ((DEVICE_GRID_COLUMNS - 1 - column) * (DEVICE_CARD_WIDTH + DEVICE_CARD_GAP)),
				CONTENT_TOP + (row * (DEVICE_CARD_HEIGHT + DEVICE_CARD_GAP)),
				DEVICE_CARD_WIDTH,
				DEVICE_CARD_HEIGHT,
			};
			if (card_index == active_card) {
				fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
			}
			if (setting_enabled) {
				fill_rectangle(card.x, card.y, card.width, card.height, COLOR_BRIGHT_ORANGE);
			}
			draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
			if (card_index == 0) {
				draw_button_label(card, "5GHZ", setting_enabled ? COLOR_BLACK : COLOR_WHITE);
			} else if (card_index == 1) {
				draw_button_label(card, "BLUETOOTH", setting_enabled ? COLOR_BLACK : COLOR_WHITE);
			}
		}
	}
}

void draw_mode_screen(int active_card = -1)
{
	draw_submenu_screen("MODE");

	for (int row = 0; row < DEVICE_GRID_ROWS; ++row) {
		for (int column = 0; column < DEVICE_GRID_COLUMNS; ++column) {
			const int card_index = row * DEVICE_GRID_COLUMNS + column;
			const Rectangle card = {
				BOX_MARGIN + ((DEVICE_GRID_COLUMNS - 1 - column) * (DEVICE_CARD_WIDTH + DEVICE_CARD_GAP)),
				CONTENT_TOP + (row * (DEVICE_CARD_HEIGHT + DEVICE_CARD_GAP)),
				DEVICE_CARD_WIDTH,
				DEVICE_CARD_HEIGHT,
			};
			if (card_index == active_card) {
				fill_rectangle(card.x, card.y, card.width, card.height, COLOR_WHITE);
			}
			draw_rectangle_border(card.x, card.y, card.width, card.height, COLOR_DARK_ORANGE);
			if (card_index == 0) {
				draw_button_label(card, "WI-FI", card_index == active_card ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
			} else if (card_index == 1) {
				draw_button_label(card, "BLUETOOTH", card_index == active_card ? COLOR_BRIGHT_ORANGE : COLOR_WHITE);
			}
		}
	}
}

bool mesh_result_duplicates_local(const MeshScanResult &result)
{
	if (result.payload.kind == ScanResultKind::Wifi) {
		for (uint16_t network_index = 0; network_index < discovered_network_count; ++network_index) {
			if (result.payload.channel == discovered_networks[network_index].primary &&
				std::strcmp(result.payload.name, reinterpret_cast<const char *>(discovered_networks[network_index].ssid)) == 0) {
				return true;
			}
		}
	} else {
		for (uint16_t device_index = 0; device_index < discovered_ble_device_count; ++device_index) {
			if (std::strcmp(result.payload.name, discovered_ble_devices[device_index].name) == 0) {
				return true;
			}
		}
	}
	return false;
}

bool mesh_result_duplicates_earlier_remote(uint16_t result_index)
{
	const MeshScanResult &result = mesh_scan_results[result_index];
	for (uint16_t earlier_index = 0; earlier_index < result_index; ++earlier_index) {
		const MeshScanResult &earlier_result = mesh_scan_results[earlier_index];
		if (result.payload.kind == earlier_result.payload.kind &&
			std::strcmp(result.payload.name, earlier_result.payload.name) == 0 &&
			(result.payload.kind == ScanResultKind::Ble || result.payload.channel == earlier_result.payload.channel)) {
			return true;
		}
	}
	return false;
}

int unique_mesh_result_count(ScanResultKind kind)
{
	int result_count = 0;
	for (uint16_t result_index = 0; result_index < mesh_scan_result_count; ++result_index) {
		if (mesh_scan_results[result_index].payload.kind == kind && !mesh_result_duplicates_local(mesh_scan_results[result_index]) &&
			!mesh_result_duplicates_earlier_remote(result_index)) {
			++result_count;
		}
	}
	return result_count;
}

const MeshScanResult *unique_mesh_result_at(ScanResultKind kind, int requested_index)
{
	for (uint16_t result_index = 0; result_index < mesh_scan_result_count; ++result_index) {
		const MeshScanResult &result = mesh_scan_results[result_index];
		if (result.payload.kind == kind && !mesh_result_duplicates_local(result) && !mesh_result_duplicates_earlier_remote(result_index) &&
			requested_index-- == 0) {
			return &result;
		}
	}
	return nullptr;
}

int mesh_node_result_count(int node_index, ScanResultKind kind)
{
	int result_count = 0;
	for (uint16_t result_index = 0; result_index < mesh_scan_result_count; ++result_index) {
		if (mesh_scan_results[result_index].node_index == node_index && mesh_scan_results[result_index].payload.kind == kind) {
			++result_count;
		}
	}
	return result_count;
}

const MeshScanResult *mesh_node_result_at(int node_index, ScanResultKind kind, int requested_index)
{
	for (uint16_t result_index = 0; result_index < mesh_scan_result_count; ++result_index) {
		const MeshScanResult &result = mesh_scan_results[result_index];
		if (result.node_index == node_index && result.payload.kind == kind && requested_index-- == 0) {
			return &result;
		}
	}
	return nullptr;
}

int scan_content_reserved_height()
{
	return (scan_view == ScanView::ModeWifi || scan_view == ScanView::ModeBluetooth) ? MODE_STATUS_HEIGHT + 4 : 0;
}

int scan_list_max_rows()
{
	return (DISPLAY_HEIGHT - CONTENT_TOP - BOX_MARGIN - scan_content_reserved_height()) / SCAN_LIST_ROW_HEIGHT;
}

int scan_result_row_count()
{
	if (scan_view == ScanView::ModeWifi) {
		return discovered_network_count + unique_mesh_result_count(ScanResultKind::Wifi);
	}
	if (scan_view == ScanView::ModeBluetooth) {
		return discovered_ble_device_count + unique_mesh_result_count(ScanResultKind::Ble);
	}
	const int wifi_count = viewed_node_index < 0 ? discovered_network_count : mesh_node_result_count(viewed_node_index, ScanResultKind::Wifi);
	const int ble_count = viewed_node_index < 0 ? discovered_ble_device_count : mesh_node_result_count(viewed_node_index, ScanResultKind::Ble);
	return 1 + wifi_count + (bluetooth_enabled ? 1 + ble_count : 0);
}

void draw_scan_results_screen()
{
	static char node_title_buffer[24];
	const char *title;
	if (scan_view == ScanView::ModeWifi) {
		title = "WI-FI SCAN";
	} else if (scan_view == ScanView::ModeBluetooth) {
		title = "BLUETOOTH SCAN";
	} else if (viewed_node_index >= 0) {
		int position = 0;
		const char *label = MESH_NODE_LABELS[viewed_node_index];
		while (label[position] != '\0' && position < static_cast<int>(sizeof(node_title_buffer)) - 6) {
			node_title_buffer[position] = label[position];
			++position;
		}
		const char *suffix = " SCAN";
		for (int suffix_index = 0; suffix[suffix_index] != '\0'; ++suffix_index) {
			node_title_buffer[position++] = suffix[suffix_index];
		}
		node_title_buffer[position] = '\0';
		title = node_title_buffer;
	} else {
		title = "SELF-C5 SCAN";
	}
	draw_submenu_screen(title);
	const int result_row_count = scan_result_row_count();
	const int max_rows = scan_list_max_rows();
	const int displayed_row_count = result_row_count - scan_scroll_offset < max_rows ? result_row_count - scan_scroll_offset : max_rows;
	const int wifi_count = viewed_node_index < 0 ? discovered_network_count : mesh_node_result_count(viewed_node_index, ScanResultKind::Wifi);
	for (int row_index = 0; row_index < displayed_row_count; ++row_index) {
		const int result_index = scan_scroll_offset + row_index;
		const Rectangle row = {SCAN_LIST_X, CONTENT_TOP + (row_index * SCAN_LIST_ROW_HEIGHT), SCAN_LIST_WIDTH, SCAN_LIST_ROW_HEIGHT - 2};
		if (scan_view == ScanView::Device && result_index == 0) {
			draw_small_label_left(row, "WI-FI", COLOR_BRIGHT_ORANGE);
		} else if (scan_view == ScanView::Device && result_index <= wifi_count) {
			const char *name = "";
			if (viewed_node_index < 0) {
				name = reinterpret_cast<const char *>(discovered_networks[result_index - 1].ssid);
			} else {
				const MeshScanResult *result = mesh_node_result_at(viewed_node_index, ScanResultKind::Wifi, result_index - 1);
				if (result != nullptr) {
					name = result->payload.name;
				}
			}
			draw_small_label_left(row, name, COLOR_WHITE);
		} else if (scan_view == ScanView::Device && result_index == wifi_count + 1) {
			draw_small_label_left(row, "BLUETOOTH", COLOR_BRIGHT_ORANGE);
		} else if (scan_view == ScanView::Device) {
			const int ble_index = result_index - wifi_count - 2;
			const char *name = "";
			if (viewed_node_index < 0) {
				name = discovered_ble_devices[ble_index].name;
			} else {
				const MeshScanResult *result = mesh_node_result_at(viewed_node_index, ScanResultKind::Ble, ble_index);
				if (result != nullptr) {
					name = result->payload.name;
				}
			}
			draw_small_label_left(row, name, COLOR_WHITE);
		} else {
			const ScanResultKind kind = scan_view == ScanView::ModeWifi ? ScanResultKind::Wifi : ScanResultKind::Ble;
			const int local_result_count = scan_view == ScanView::ModeWifi ? discovered_network_count : discovered_ble_device_count;
			if (result_index < local_result_count) {
				const char *name = scan_view == ScanView::ModeWifi ? reinterpret_cast<const char *>(discovered_networks[result_index].ssid) : discovered_ble_devices[result_index].name;
				draw_small_label_left(row, name, COLOR_WHITE);
			} else {
				const MeshScanResult *result = unique_mesh_result_at(kind, result_index - local_result_count);
				if (result != nullptr) {
					draw_small_label_left(row, result->payload.name, COLOR_WHITE);
				}
			}
		}
	}

	if (result_row_count > max_rows) {
		const int track_height = DISPLAY_HEIGHT - CONTENT_TOP - BOX_MARGIN - scan_content_reserved_height();
		const int thumb_height = track_height * max_rows / result_row_count;
		const int maximum_scroll_offset = result_row_count - max_rows;
		const int thumb_y = CONTENT_TOP + (track_height - thumb_height) * scan_scroll_offset / maximum_scroll_offset;
		fill_rectangle(SCAN_SCROLLBAR_X, CONTENT_TOP, SCAN_SCROLLBAR_WIDTH, track_height, COLOR_DARK_ORANGE);
		fill_rectangle(SCAN_SCROLLBAR_X, thumb_y, SCAN_SCROLLBAR_WIDTH, thumb_height, COLOR_WHITE);
	}

	if (scan_view == ScanView::ModeWifi || scan_view == ScanView::ModeBluetooth) {
		draw_connection_status(MODE_STATUS_AREA);
	}
}

void draw_bluetooth_scan_progress(bool text_is_orange)
{
	draw_scan_results_screen();
	draw_button_label(HEADER_BOX, "BLUETOOTH SCAN", text_is_orange ? COLOR_DARK_ORANGE : COLOR_WHITE);
}

void draw_wifi_scan_progress(bool text_is_orange)
{
	draw_scan_results_screen();
	draw_button_label(HEADER_BOX, "WI-FI SCAN", text_is_orange ? COLOR_DARK_ORANGE : COLOR_WHITE);
}

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

bool point_in_rectangle(int x, int y, const Rectangle &rectangle)
{
	return x >= rectangle.x && x < rectangle.x + rectangle.width && y >= rectangle.y && y < rectangle.y + rectangle.height;
}

int button_at(int x, int y)
{
	for (int index = 0; index < 3; ++index) {
		const Rectangle &button = BUTTONS[index];
		if (point_in_rectangle(x, y, button)) {
			return index;
		}
	}
	return -1;
}

int grid_card_at(int x, int y)
{
	for (int row = 0; row < DEVICE_GRID_ROWS; ++row) {
		for (int column = 0; column < DEVICE_GRID_COLUMNS; ++column) {
			const Rectangle card = {
				BOX_MARGIN + (column * (DEVICE_CARD_WIDTH + DEVICE_CARD_GAP)),
				CONTENT_TOP + (row * (DEVICE_CARD_HEIGHT + DEVICE_CARD_GAP)),
				DEVICE_CARD_WIDTH,
				DEVICE_CARD_HEIGHT,
			};
			if (point_in_rectangle(x, y, card)) {
				return row * DEVICE_GRID_COLUMNS + column;
			}
		}
	}
	return -1;
}

void wifi_scan_done_handler(void *argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	wifi_scan_complete = true;
}

int mesh_node_index(const uint8_t *mac_address)
{
	for (int index = 0; index < MAX_MESH_NODES; ++index) {
		if (MESH_NODE_MACS[index] != nullptr && std::memcmp(mac_address, MESH_NODE_MACS[index], ESP_NOW_ETH_ALEN) == 0) {
			return index;
		}
	}
	return -1;
}

bool mesh_node_is_configured(int node_index)
{
	if (node_index < 0 || node_index >= MAX_MESH_NODES || MESH_NODE_MACS[node_index] == nullptr) {
		return false;
	}

	for (int byte_index = 0; byte_index < ESP_NOW_ETH_ALEN; ++byte_index) {
		if (MESH_NODE_MACS[node_index][byte_index] != 0) {
			return true;
		}
	}
	return false;
}

void add_mesh_node_peer(int node_index)
{
	if (!mesh_node_is_configured(node_index)) {
		return;
	}

	esp_now_peer_info_t peer = {};
	std::memcpy(peer.peer_addr, MESH_NODE_MACS[node_index], ESP_NOW_ETH_ALEN);
	peer.ifidx = WIFI_IF_STA;
	peer.channel = mesh_reference::esp_now_channel;
	peer.encrypt = false;
	ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

esp_err_t send_mesh_message(int node_index, MeshMessageType type, const void *payload, uint8_t payload_length)
{
	if (!mesh_node_is_configured(node_index) || payload_length > ESP_NOW_MAX_DATA_LEN - sizeof(MeshMessage)) {
		return ESP_ERR_INVALID_ARG;
	}

	uint8_t packet[ESP_NOW_MAX_DATA_LEN] = {};
	MeshMessage message = {};
	message.type = type;
	message.sequence = mesh_message_sequence++;
	message.payload_length = payload_length;
	std::memcpy(packet, &message, sizeof(message));
	if (payload_length > 0) {
		std::memcpy(packet + sizeof(message), payload, payload_length);
	}
	return esp_now_send(MESH_NODE_MACS[node_index], packet, sizeof(message) + payload_length);
}

void begin_mesh_scan_request(uint8_t scan_flags, uint8_t node_mask)
{
	ScanRequestPayload request = {};
	request.request_id = mesh_scan_request_id++;
	request.scan_flags = scan_flags;
	request.hub_millisecond_in_second = static_cast<uint16_t>((esp_timer_get_time() / 1000) % 1000);
	active_mesh_scan_request_id = request.request_id;
	mesh_scan_result_count = 0;
	pending_mesh_scan_nodes = 0;

	const uint8_t connected_node_count = connected_mesh_node_count.load(std::memory_order_relaxed);
	for (int node_index = 0; node_index < connected_node_count; ++node_index) {
		if ((node_mask & static_cast<uint8_t>(1U << node_index)) == 0) {
			continue;
		}
		if (send_mesh_message(node_index, MeshMessageType::ScanRequest, &request, sizeof(request)) == ESP_OK) {
			pending_mesh_scan_nodes |= static_cast<uint8_t>(1U << node_index);
		}
	}
	mesh_scan_response_deadline = pending_mesh_scan_nodes == 0 ? 0 : xTaskGetTickCount() + pdMS_TO_TICKS(MESH_SCAN_RESPONSE_TIMEOUT_MS);
}

void request_mesh_scan(uint8_t scan_flags)
{
	begin_mesh_scan_request(scan_flags, 0xFF);
}

void request_mesh_scan_single(int node_index, uint8_t scan_flags)
{
	begin_mesh_scan_request(scan_flags, static_cast<uint8_t>(1U << node_index));
}

void drain_mesh_scan_events()
{
	MeshScanEvent event = {};
	while (xQueueReceive(mesh_scan_event_queue, &event, 0) == pdPASS) {
		if (event.type == MeshMessageType::ScanResult && event.result.request_id == active_mesh_scan_request_id &&
			mesh_scan_result_count < MAX_MESH_SCAN_RESULTS) {
			MeshScanResult &stored_result = mesh_scan_results[mesh_scan_result_count++];
			stored_result.node_index = event.node_index;
			stored_result.payload = event.result;
			stored_result.payload.name[stored_result.payload.name_length] = '\0';
		} else if (event.type == MeshMessageType::ScanComplete && event.complete.request_id == active_mesh_scan_request_id) {
			pending_mesh_scan_nodes &= static_cast<uint8_t>(~(1U << event.node_index));
		}
	}
}

bool mesh_scan_pending_expired()
{
	return pending_mesh_scan_nodes != 0 && static_cast<int32_t>(xTaskGetTickCount() - mesh_scan_response_deadline) >= 0;
}

void wait_for_pending_mesh_scan(esp_lcd_panel_handle_t panel_handle, void (*draw_progress)(bool))
{
	int frame = 0;
	while (pending_mesh_scan_nodes != 0 && !mesh_scan_pending_expired()) {
		drain_mesh_scan_events();
		if (pending_mesh_scan_nodes == 0) {
			break;
		}
		mesh_scan_spinner_frame = frame;
		draw_progress((frame / 3) % 2 == 0);
		draw_framebuffer(panel_handle);
		vTaskDelay(pdMS_TO_TICKS(100));
		++frame;
	}
	drain_mesh_scan_events();
	if (mesh_scan_pending_expired()) {
		pending_mesh_scan_nodes = 0;
	}
}

uint8_t wifi_scan_flags()
{
	return static_cast<uint8_t>(Scan2_4Ghz | (scan_5ghz_enabled ? Scan5Ghz : 0));
}

void mesh_receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_length)
{
	if (recv_info == nullptr || data == nullptr || data_length < static_cast<int>(sizeof(MeshMessage))) {
		return;
	}

	const MeshMessage *message = reinterpret_cast<const MeshMessage *>(data);
	if (message->magic != MESH_MESSAGE_MAGIC || message->version != MESH_PROTOCOL_VERSION ||
		data_length != static_cast<int>(sizeof(MeshMessage) + message->payload_length)) {
		return;
	}

	mesh_received_message_count.fetch_add(1, std::memory_order_relaxed);
	const int node_index = mesh_node_index(recv_info->src_addr);
	if (node_index < 0) {
		return;
	}
	if (message->type == MeshMessageType::Hello && message->payload_length == 0) {
		const uint8_t node_count = static_cast<uint8_t>(node_index + 1);
		uint8_t previous_count = connected_mesh_node_count.load(std::memory_order_relaxed);
		while (previous_count < node_count &&
			!connected_mesh_node_count.compare_exchange_weak(previous_count, node_count, std::memory_order_relaxed)) {
		}
		return;
	}

	if (mesh_scan_event_queue == nullptr) {
		return;
	}

	MeshScanEvent event = {};
	event.node_index = static_cast<uint8_t>(node_index);
	event.type = message->type;
	if (message->type == MeshMessageType::ScanResult && message->payload_length == sizeof(ScanResultPayload)) {
		std::memcpy(&event.result, data + sizeof(MeshMessage), sizeof(event.result));
		if (event.result.name_length >= sizeof(event.result.name) ||
			(event.result.kind != ScanResultKind::Wifi && event.result.kind != ScanResultKind::Ble)) {
			return;
		}
	} else if (message->type == MeshMessageType::ScanComplete && message->payload_length == sizeof(ScanCompletePayload)) {
		std::memcpy(&event.complete, data + sizeof(MeshMessage), sizeof(event.complete));
	} else {
		return;
	}
	xQueueSend(mesh_scan_event_queue, &event, 0);
}

void mesh_send_callback(const esp_now_send_info_t *send_info, esp_now_send_status_t status)
{
	(void)send_info;
	(void)status;
}

void scan_local_networks(esp_lcd_panel_handle_t panel_handle = nullptr, bool animate_progress = false)
{
	wifi_scan_config_t scan_config = {};
	scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
	scan_config.scan_time.passive = 120;
	if (!scan_5ghz_enabled) {
		scan_config.channel_bitmap.ghz_5_channels = 1;
	}
	wifi_scan_complete = false;
	ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, !animate_progress));
	if (animate_progress) {
		while (!wifi_scan_complete) {
			draw_wifi_scan_progress((xTaskGetTickCount() / pdMS_TO_TICKS(300)) % 2 == 0);
			draw_framebuffer(panel_handle);
			vTaskDelay(pdMS_TO_TICKS(100));
		}
	}

	uint16_t access_point_count = 0;
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&access_point_count));
	wifi_ap_record_t *new_networks = nullptr;
	if (access_point_count > 0) {
		new_networks = static_cast<wifi_ap_record_t *>(heap_caps_malloc(sizeof(wifi_ap_record_t) * access_point_count, MALLOC_CAP_SPIRAM));
		if (new_networks == nullptr) {
			ESP_ERROR_CHECK(esp_wifi_clear_ap_list());
			ESP_LOGE(TAG, "Failed to store %u discovered Wi-Fi networks", access_point_count);
			return;
		}
		uint16_t stored_network_count = access_point_count;
		ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&stored_network_count, new_networks));
		access_point_count = 0;
		for (uint16_t index = 0; index < stored_network_count; ++index) {
			bool duplicate = false;
			for (uint16_t existing_index = 0; existing_index < access_point_count; ++existing_index) {
				bool same_address = true;
				for (int address_byte = 0; address_byte < 6; ++address_byte) {
					if (new_networks[index].bssid[address_byte] != new_networks[existing_index].bssid[address_byte]) {
						same_address = false;
						break;
					}
				}
				if (same_address) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				new_networks[access_point_count++] = new_networks[index];
			}
		}
	}

	heap_caps_free(discovered_networks);
	discovered_networks = new_networks;
	discovered_network_count = access_point_count;
	scan_scroll_offset = 0;
}

int ble_gap_callback(struct ble_gap_event *event, void *parameter)
{
	if (event->type != BLE_GAP_EVENT_DISC || discovered_ble_devices == nullptr || discovered_ble_device_count >= BLE_MAX_DISCOVERED_DEVICES) {
		return 0;
	}

	ble_hs_adv_fields fields = {};
	if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0 || fields.name == nullptr || fields.name_len == 0) {
		return 0;
	}

	for (uint16_t device_index = 0; device_index < discovered_ble_device_count; ++device_index) {
		bool same_address = true;
		for (int address_byte = 0; address_byte < 6; ++address_byte) {
			if (discovered_ble_devices[device_index].address[address_byte] != event->disc.addr.val[address_byte]) {
				same_address = false;
				break;
			}
		}
		if (same_address) {
			return 0;
		}
	}

	BleDevice &device = discovered_ble_devices[discovered_ble_device_count++];
	const uint8_t copied_length = fields.name_len < sizeof(device.name) - 1 ? fields.name_len : sizeof(device.name) - 1;
	for (uint8_t index = 0; index < copied_length; ++index) {
		device.name[index] = static_cast<char>(fields.name[index]);
	}
	device.name[copied_length] = '\0';
	for (int address_byte = 0; address_byte < 6; ++address_byte) {
		device.address[address_byte] = event->disc.addr.val[address_byte];
	}
	return 0;
}

void nimble_host_task(void *parameter)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

void nimble_on_sync()
{
}

void scan_ble_devices(esp_lcd_panel_handle_t panel_handle = nullptr, bool animate_progress = false)
{
	if (discovered_ble_devices == nullptr) {
		discovered_ble_devices = static_cast<BleDevice *>(heap_caps_malloc(sizeof(BleDevice) * BLE_MAX_DISCOVERED_DEVICES, MALLOC_CAP_SPIRAM));
		if (discovered_ble_devices == nullptr) {
			ESP_LOGE(TAG, "Failed to allocate BLE scan list");
			return;
		}
	}

	discovered_ble_device_count = 0;
	scan_scroll_offset = 0;
	uint8_t own_address_type = 0;
	ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &own_address_type));
	ble_gap_disc_params scan_parameters = {};
	scan_parameters.filter_duplicates = 1;
	scan_parameters.passive = 1;
	ESP_ERROR_CHECK(ble_gap_disc(own_address_type, BLE_SCAN_DURATION_SECONDS * 1000, &scan_parameters, ble_gap_callback, nullptr));
	if (animate_progress) {
		for (int frame = 0; frame < BLE_SCAN_DURATION_SECONDS * 10; ++frame) {
			draw_bluetooth_scan_progress((frame / 3) % 2 == 0);
			draw_framebuffer(panel_handle);
			vTaskDelay(pdMS_TO_TICKS(100));
		}
	} else {
		vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_DURATION_SECONDS * 1000));
	}
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

void touch_monitor_task(void *parameter)
{
	const TouchMonitorContext *context = static_cast<TouchMonitorContext *>(parameter);
	Screen current_screen = Screen::Menu;
	bool touch_was_active = false;

	while (true) {
		if (current_screen == Screen::ScanResults && scan_view == ScanView::ModeWifi && static_cast<int32_t>(xTaskGetTickCount() - next_mode_wifi_scan) >= 0) {
			mesh_scan_in_progress = true;
			request_mesh_scan(wifi_scan_flags());
			scan_local_networks(context->panel_handle, true);
			wait_for_pending_mesh_scan(context->panel_handle, draw_wifi_scan_progress);
			mesh_scan_in_progress = false;
			draw_scan_results_screen();
			draw_framebuffer(context->panel_handle);
			next_mode_wifi_scan = xTaskGetTickCount() + pdMS_TO_TICKS(MODE_RESCAN_INTERVAL_MS);
		}

		if (current_screen == Screen::ScanResults && scan_view == ScanView::ModeBluetooth && static_cast<int32_t>(xTaskGetTickCount() - next_mode_bluetooth_scan) >= 0) {
			mesh_scan_in_progress = true;
			request_mesh_scan(ScanBle);
			scan_ble_devices(context->panel_handle, true);
			wait_for_pending_mesh_scan(context->panel_handle, draw_bluetooth_scan_progress);
			mesh_scan_in_progress = false;
			draw_scan_results_screen();
			draw_framebuffer(context->panel_handle);
			next_mode_bluetooth_scan = xTaskGetTickCount() + pdMS_TO_TICKS(MODE_RESCAN_INTERVAL_MS);
		}

		uint16_t pressure = 0;
		ESP_ERROR_CHECK(read_touch_axis(context->touch_device, 0xB0, &pressure));

		const bool touch_active = pressure > TOUCH_PRESSURE_THRESHOLD;
		if (touch_active && (!touch_was_active || current_screen == Screen::ScanResults)) {
			uint16_t raw_x = 0;
			uint16_t raw_y = 0;
			ESP_ERROR_CHECK(read_touch_axis(context->touch_device, 0xD0, &raw_x));
			ESP_ERROR_CHECK(read_touch_axis(context->touch_device, 0x90, &raw_y));
			const int screen_x = DISPLAY_WIDTH - 1 - scale_touch_coordinate(raw_y, RAW_TOUCH_Y_MIN, RAW_TOUCH_Y_MAX, DISPLAY_WIDTH);
			const int screen_y = DISPLAY_HEIGHT - 1 - scale_touch_coordinate(raw_x, RAW_TOUCH_X_MIN, RAW_TOUCH_X_MAX, DISPLAY_HEIGHT);

			if (current_screen == Screen::Menu) {
				const int selected_button = button_at(screen_x, screen_y);
				if (selected_button >= 0) {
					draw_menu_frame(selected_button);
					draw_framebuffer(context->panel_handle);
					vTaskDelay(pdMS_TO_TICKS(MENU_TRANSITION_DELAY_MS));

					switch (selected_button) {
					case 0:
						current_screen = Screen::Devices;
						draw_devices_screen();
						break;
					case 1:
						current_screen = Screen::Settings;
						draw_settings_screen();
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
			} else {
				if (point_in_rectangle(screen_x, screen_y, BACK_BUTTON_TOUCH)) {
					if (current_screen == Screen::ScanResults) {
						if (scan_view == ScanView::Device) {
							current_screen = Screen::Devices;
							draw_devices_screen();
						} else {
							current_screen = Screen::Mode;
							draw_mode_screen();
						}
					} else {
						current_screen = Screen::Menu;
						draw_menu_frame(-1);
					}
					draw_framebuffer(context->panel_handle);
				} else if (current_screen == Screen::ScanResults && screen_x >= DISPLAY_WIDTH - SCAN_SCROLL_TOUCH_COLUMN_WIDTH && screen_y >= SCAN_SCROLL_TOUCH_TOP) {
					const int result_row_count = scan_result_row_count();
					if (result_row_count > scan_list_max_rows()) {
						const int maximum_scroll_offset = result_row_count - scan_list_max_rows();
						int scroll_touch_position = screen_y - SCAN_SCROLL_TOUCH_TOP;
						const int scroll_touch_height = SCAN_SCROLL_TOUCH_BOTTOM - SCAN_SCROLL_TOUCH_TOP;
						const int inner_touch_height = scroll_touch_height - (2 * SCAN_SCROLL_TOUCH_EDGE_MARGIN);
						if (scroll_touch_position <= SCAN_SCROLL_TOUCH_EDGE_MARGIN) {
							scroll_touch_position = 0;
						} else if (scroll_touch_position >= scroll_touch_height - SCAN_SCROLL_TOUCH_EDGE_MARGIN) {
							scroll_touch_position = scroll_touch_height;
						} else {
							scroll_touch_position = (scroll_touch_position - SCAN_SCROLL_TOUCH_EDGE_MARGIN) * scroll_touch_height / inner_touch_height;
						}
						scan_scroll_offset = maximum_scroll_offset * scroll_touch_position / scroll_touch_height;
						draw_scan_results_screen();
						draw_framebuffer(context->panel_handle);
					}
				} else if (current_screen == Screen::Devices || current_screen == Screen::Settings || current_screen == Screen::Mode) {
					const int selected_card = grid_card_at(screen_x, screen_y);
					if (selected_card >= 0) {
						if (current_screen == Screen::Devices) {
							draw_devices_screen(selected_card);
						} else if (current_screen == Screen::Settings) {
							draw_settings_screen(selected_card);
						} else {
							draw_mode_screen(selected_card);
						}
						draw_framebuffer(context->panel_handle);
						vTaskDelay(pdMS_TO_TICKS(MENU_TRANSITION_DELAY_MS));
						if (current_screen == Screen::Devices && selected_card == 0) {
							viewed_node_index = -1;
							scan_local_networks();
							if (bluetooth_enabled) {
								scan_ble_devices();
							}
							scan_view = ScanView::Device;
							current_screen = Screen::ScanResults;
							draw_scan_results_screen();
						} else if (current_screen == Screen::Devices && selected_card >= 1 && selected_card <= connected_mesh_node_count.load(std::memory_order_relaxed)) {
							const int node_index = selected_card - 1;
							const uint8_t node_scan_flags = static_cast<uint8_t>(wifi_scan_flags() | (bluetooth_enabled ? ScanBle : 0));
							mesh_scan_in_progress = true;
							request_mesh_scan_single(node_index, node_scan_flags);
							int frame = 0;
							while (pending_mesh_scan_nodes != 0 && !mesh_scan_pending_expired()) {
								drain_mesh_scan_events();
								if (pending_mesh_scan_nodes == 0) {
									break;
								}
								mesh_scan_spinner_frame = frame;
								draw_devices_screen(selected_card);
								draw_framebuffer(context->panel_handle);
								vTaskDelay(pdMS_TO_TICKS(120));
								++frame;
							}
							drain_mesh_scan_events();
							if (mesh_scan_pending_expired()) {
								pending_mesh_scan_nodes = 0;
							}
							mesh_scan_in_progress = false;
							viewed_node_index = node_index;
							scan_view = ScanView::Device;
							current_screen = Screen::ScanResults;
							scan_scroll_offset = 0;
							draw_scan_results_screen();
						} else if (current_screen == Screen::Settings && selected_card == 0) {
							scan_5ghz_enabled = !scan_5ghz_enabled;
						} else if (current_screen == Screen::Settings && selected_card == 1) {
							bluetooth_enabled = !bluetooth_enabled;
						} else if (current_screen == Screen::Mode && selected_card == 0) {
							scan_view = ScanView::ModeWifi;
							current_screen = Screen::ScanResults;
							mesh_scan_in_progress = true;
							request_mesh_scan(wifi_scan_flags());
							scan_local_networks(context->panel_handle, true);
							wait_for_pending_mesh_scan(context->panel_handle, draw_wifi_scan_progress);
							mesh_scan_in_progress = false;
							next_mode_wifi_scan = xTaskGetTickCount() + pdMS_TO_TICKS(MODE_RESCAN_INTERVAL_MS);
							draw_scan_results_screen();
						} else if (current_screen == Screen::Mode && selected_card == 1) {
							scan_view = ScanView::ModeBluetooth;
							current_screen = Screen::ScanResults;
							mesh_scan_in_progress = true;
							request_mesh_scan(ScanBle);
							scan_ble_devices(context->panel_handle, true);
							wait_for_pending_mesh_scan(context->panel_handle, draw_bluetooth_scan_progress);
							mesh_scan_in_progress = false;
							next_mode_bluetooth_scan = xTaskGetTickCount() + pdMS_TO_TICKS(MODE_RESCAN_INTERVAL_MS);
							draw_scan_results_screen();
						}
						if (current_screen == Screen::Devices) {
							draw_devices_screen();
						} else if (current_screen == Screen::Settings) {
							draw_settings_screen();
						} else if (current_screen == Screen::Mode) {
							draw_mode_screen();
						}
						draw_framebuffer(context->panel_handle);
					}
				}
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
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_channel(mesh_reference::esp_now_channel, WIFI_SECOND_CHAN_NONE));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_scan_done_handler, nullptr));
	ESP_ERROR_CHECK(esp_now_init());
	mesh_scan_event_queue = xQueueCreate(MESH_SCAN_EVENT_QUEUE_LENGTH, sizeof(MeshScanEvent));
	if (mesh_scan_event_queue == nullptr) {
		ESP_LOGE(TAG, "Failed to create mesh scan event queue");
		abort();
	}
	ESP_ERROR_CHECK(esp_now_register_recv_cb(mesh_receive_callback));
	ESP_ERROR_CHECK(esp_now_register_send_cb(mesh_send_callback));
	for (int node_index = 0; node_index < MAX_MESH_NODES; ++node_index) {
		add_mesh_node_peer(node_index);
	}
	ESP_ERROR_CHECK(nimble_port_init());
	ble_hs_cfg.sync_cb = nimble_on_sync;
	nimble_port_freertos_init(nimble_host_task);

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
	xTaskCreate(touch_monitor_task, "touch_monitor", 2048, &touch_context, 5, nullptr);
}
