/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#else
#include "esp_bt_defs.h"
#if CONFIG_BT_BLE_ENABLED
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#endif
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#if CONFIG_BT_SDP_COMMON_ENABLED
#include "esp_sdp_api.h"
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */
#endif

#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "stick_s3_ui.h"

static const char *TAG = "HID_DEV_DEMO";

typedef struct
{
    TaskHandle_t task_hdl;
    esp_hidd_dev_t *hid_dev;
    uint8_t protocol_mode;
    uint8_t *buffer;
} local_param_t;

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
static local_param_t s_ble_hid_param = {0};

typedef enum {
    REMOTE_MODE_MAC = 0,
    REMOTE_MODE_CAMERA,
    REMOTE_MODE_READER,
    REMOTE_MODE_IR,
    REMOTE_MODE_COUNT,
} remote_mode_t;

typedef enum {
    READER_MAP_VOLUME = 0,
    READER_MAP_ARROWS,
    READER_MAP_PAGE_KEYS,
    READER_MAP_COUNT,
} reader_mapping_t;

static volatile bool s_ble_connected = false;
static volatile bool s_ble_ready = false;
static volatile bool s_ui_dirty = true;
static remote_mode_t s_remote_mode = REMOTE_MODE_CAMERA;
static remote_mode_t s_selected_mode = REMOTE_MODE_CAMERA;
static reader_mapping_t s_reader_mapping = READER_MAP_VOLUME;
static stick_s3_view_t s_ui_view = STICK_S3_VIEW_ACTIVE;
static char s_last_action[32] = "Waiting for host";
static TaskHandle_t s_remote_task_hdl = NULL;

const unsigned char mediaReportMap[] = {
    // Standard keyboard report for reader/page-turn mode.
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x19, 0x00,        //   Usage Minimum (Reserved)
    0x29, 0x65,        //   Usage Maximum (Keyboard Application)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0,              // End Collection

    // 16-bit Consumer Usage report. This matches Espressif's
    // TUD_HID_REPORT_DESC_CONSUMER descriptor and sends the Usage ID directly.
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x03FF)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x03FF)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x10,        //   Report Size (16)
    0x81, 0x00,        //   Input (Data,Array,Absolute)
    0xC0,              // End Collection
};
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
const unsigned char mouseReportMap[] = {
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,                    // USAGE (Mouse)
    0xa1, 0x01,                    // COLLECTION (Application)

    0x09, 0x01,                    //   USAGE (Pointer)
    0xa1, 0x00,                    //   COLLECTION (Physical)

    0x05, 0x09,                    //     USAGE_PAGE (Button)
    0x19, 0x01,                    //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,                    //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x75, 0x01,                    //     REPORT_SIZE (1)
    0x81, 0x02,                    //     INPUT (Data,Var,Abs)
    0x95, 0x01,                    //     REPORT_COUNT (1)
    0x75, 0x05,                    //     REPORT_SIZE (5)
    0x81, 0x03,                    //     INPUT (Cnst,Var,Abs)

    0x05, 0x01,                    //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,                    //     USAGE (X)
    0x09, 0x31,                    //     USAGE (Y)
    0x09, 0x38,                    //     USAGE (Wheel)
    0x15, 0x81,                    //     LOGICAL_MINIMUM (-127)
    0x25, 0x7f,                    //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,                    //     REPORT_SIZE (8)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x81, 0x06,                    //     INPUT (Data,Var,Rel)

    0xc0,                          //   END_COLLECTION
    0xc0                           // END_COLLECTION
};
// send the buttons, change in x, and change in y
void send_mouse(uint8_t buttons, char dx, char dy, char wheel)
{
    static uint8_t buffer[4] = {0};
    buffer[0] = buttons;
    buffer[1] = dx;
    buffer[2] = dy;
    buffer[3] = wheel;
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 0, buffer, 4);
}

void ble_hid_demo_task_mouse(void *pvParameters)
{
    static const char* help_string = "########################################################################\n"\
    "BT hid mouse demo usage:\n"\
    "You can input these value to simulate mouse: 'q', 'w', 'e', 'a', 's', 'd', 'h'\n"\
    "q -- click the left key\n"\
    "w -- move up\n"\
    "e -- click the right key\n"\
    "a -- move left\n"\
    "s -- move down\n"\
    "d -- move right\n"\
    "h -- show the help\n"\
    "########################################################################\n";
    printf("%s\n", help_string);
    char c;
    while (1) {
        c = fgetc(stdin);
        switch (c) {
        case 'q':
            send_mouse(1, 0, 0, 0);
            break;
        case 'w':
            send_mouse(0, 0, -10, 0);
            break;
        case 'e':
            send_mouse(2, 0, 0, 0);
            break;
        case 'a':
            send_mouse(0, -10, 0, 0);
            break;
        case 's':
            send_mouse(0, 0, 10, 0);
            break;
        case 'd':
            send_mouse(0, 10, 0, 0);
            break;
        case 'h':
            printf("%s\n", help_string);
            break;
        default:
            break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
#endif

#if CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
#define CASE(a, b, c)  \
                case a: \
				buffer[0] = b;  \
				buffer[2] = c; \
                break;\

// USB keyboard codes
#define USB_HID_MODIFIER_LEFT_CTRL      0x01
#define USB_HID_MODIFIER_LEFT_SHIFT     0x02
#define USB_HID_MODIFIER_LEFT_ALT       0x04
#define USB_HID_MODIFIER_RIGHT_CTRL     0x10
#define USB_HID_MODIFIER_RIGHT_SHIFT    0x20
#define USB_HID_MODIFIER_RIGHT_ALT      0x40

#define USB_HID_SPACE                   0x2C
#define USB_HID_DOT                     0x37
#define USB_HID_NEWLINE                 0x28
#define USB_HID_FSLASH                  0x38
#define USB_HID_BSLASH                  0x31
#define USB_HID_COMMA                   0x36
#define USB_HID_DOT                     0x37

const unsigned char keyboardReportMap[] = { //7 bytes input (modifiers, resrvd, keys*5), 1 byte output
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection

    // 65 bytes
};

static void char_to_code(uint8_t *buffer, char ch)
{
	// Check if lower or upper case
	if(ch >= 'a' && ch <= 'z')
	{
		buffer[0] = 0;
		// convert ch to HID letter, starting at a = 4
		buffer[2] = (uint8_t)(4 + (ch - 'a'));
	}
	else if(ch >= 'A' && ch <= 'Z')
	{
		// Add left shift
		buffer[0] = USB_HID_MODIFIER_LEFT_SHIFT;
		// convert ch to lower case
		ch = ch - ('A'-'a');
		// convert ch to HID letter, starting at a = 4
		buffer[2] = (uint8_t)(4 + (ch - 'a'));
	}
	else if(ch >= '0' && ch <= '9') // Check if number
	{
		buffer[0] = 0;
		// convert ch to HID number, starting at 1 = 30, 0 = 39
		if(ch == '0')
		{
			buffer[2] = 39;
		}
		else
		{
			buffer[2] = (uint8_t)(30 + (ch - '1'));
		}
	}
	else // not a letter nor a number
	{
		switch(ch)
		{
            CASE(' ', 0, USB_HID_SPACE);
			CASE('.', 0,USB_HID_DOT);
            CASE('\n', 0, USB_HID_NEWLINE);
			CASE('?', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_FSLASH);
			CASE('/', 0 ,USB_HID_FSLASH);
			CASE('\\', 0, USB_HID_BSLASH);
			CASE('|', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_BSLASH);
			CASE(',', 0, USB_HID_COMMA);
			CASE('<', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
			CASE('>', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
			CASE('@', USB_HID_MODIFIER_LEFT_SHIFT, 31);
			CASE('!', USB_HID_MODIFIER_LEFT_SHIFT, 30);
			CASE('#', USB_HID_MODIFIER_LEFT_SHIFT, 32);
			CASE('$', USB_HID_MODIFIER_LEFT_SHIFT, 33);
			CASE('%', USB_HID_MODIFIER_LEFT_SHIFT, 34);
			CASE('^', USB_HID_MODIFIER_LEFT_SHIFT,35);
			CASE('&', USB_HID_MODIFIER_LEFT_SHIFT, 36);
			CASE('*', USB_HID_MODIFIER_LEFT_SHIFT, 37);
			CASE('(', USB_HID_MODIFIER_LEFT_SHIFT, 38);
			CASE(')', USB_HID_MODIFIER_LEFT_SHIFT, 39);
			CASE('-', 0, 0x2D);
			CASE('_', USB_HID_MODIFIER_LEFT_SHIFT, 0x2D);
			CASE('=', 0, 0x2E);
			CASE('+', USB_HID_MODIFIER_LEFT_SHIFT, 39);
			CASE(8, 0, 0x2A); // backspace
			CASE('\t', 0, 0x2B);
			default:
				buffer[0] = 0;
				buffer[2] = 0;
		}
	}
}

void send_keyboard(char c)
{
    static uint8_t buffer[8] = {0};
    char_to_code(buffer, c);
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
    /* send the keyrelease event with sufficient delay */
    vTaskDelay(50 / portTICK_PERIOD_MS);
    memset(buffer, 0, sizeof(uint8_t) * 8);
    esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
}

void ble_hid_demo_task_kbd(void *pvParameters)
{
    static const char* help_string = "########################################################################\n"\
                                      "BT hid keyboard demo usage:\n"\
                                      "########################################################################\n";
                                    /* TODO : Add support for function keys and ctrl, alt, esc, etc. */
    printf("%s\n", help_string);
    char c;
    while (1) {
        c = fgetc(stdin);

        if(c != 255) {
            send_keyboard(c);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
#endif
static esp_hid_raw_report_map_t ble_report_maps[] = {
#if !CONFIG_BT_NIMBLE_ENABLED || CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1
    /* This block is compiled for bluedroid as well */
    {
        .data = mediaReportMap,
        .len = sizeof(mediaReportMap)
    }
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    {
        .data = keyboardReportMap,
        .len = sizeof(keyboardReportMap)
    },
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE && CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    {
        .data = mouseReportMap,
        .len = sizeof(mouseReportMap)
    },
#endif
};

static esp_hid_device_config_t ble_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0102,
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    .device_name        = "ESP Keyboard",
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    .device_name        = "ESP Mouse",
#else
    .device_name        = "S3V2",
#endif
    .manufacturer_name  = "M5Stack",
    .serial_number      = "STICKS3-REMOTE-02",
    .report_maps        = ble_report_maps,
    .report_maps_len    = 1
};

#define HID_CC_RPT_MUTE                 1
#define HID_CC_RPT_POWER                2
#define HID_CC_RPT_LAST                 3
#define HID_CC_RPT_ASSIGN_SEL           4
#define HID_CC_RPT_PLAY                 5
#define HID_CC_RPT_PAUSE                6
#define HID_CC_RPT_RECORD               7
#define HID_CC_RPT_FAST_FWD             8
#define HID_CC_RPT_REWIND               9
#define HID_CC_RPT_SCAN_NEXT_TRK        10
#define HID_CC_RPT_SCAN_PREV_TRK        11
#define HID_CC_RPT_STOP                 12

#define HID_CC_RPT_CHANNEL_UP           0x10
#define HID_CC_RPT_CHANNEL_DOWN         0x30
#define HID_CC_RPT_VOLUME_UP            0x40
#define HID_CC_RPT_VOLUME_DOWN          0x80

// HID Consumer Control report bitmasks
#define HID_CC_RPT_NUMERIC_BITS         0xF0
#define HID_CC_RPT_CHANNEL_BITS         0xCF
#define HID_CC_RPT_VOLUME_BITS          0x3F
#define HID_CC_RPT_BUTTON_BITS          0xF0
#define HID_CC_RPT_SELECTION_BITS       0xCF

// Macros for the HID Consumer Control 2-byte report
#define HID_CC_RPT_SET_NUMERIC(s, x)    (s)[0] &= HID_CC_RPT_NUMERIC_BITS;   (s)[0] = (x)
#define HID_CC_RPT_SET_CHANNEL(s, x)    (s)[0] &= HID_CC_RPT_CHANNEL_BITS;   (s)[0] |= ((x) & 0x03) << 4
#define HID_CC_RPT_SET_VOLUME_UP(s)     (s)[0] &= HID_CC_RPT_VOLUME_BITS;    (s)[0] |= 0x40
#define HID_CC_RPT_SET_VOLUME_DOWN(s)   (s)[0] &= HID_CC_RPT_VOLUME_BITS;    (s)[0] |= 0x80
#define HID_CC_RPT_SET_BUTTON(s, x)     (s)[1] &= HID_CC_RPT_BUTTON_BITS;    (s)[1] |= (x)
#define HID_CC_RPT_SET_SELECTION(s, x)  (s)[1] &= HID_CC_RPT_SELECTION_BITS; (s)[1] |= ((x) & 0x03) << 4

// HID Consumer Usage IDs (subset of the codes available in the USB HID Usage Tables spec)
#define HID_CONSUMER_POWER          48  // Power
#define HID_CONSUMER_RESET          49  // Reset
#define HID_CONSUMER_SLEEP          50  // Sleep

#define HID_CONSUMER_MENU           64  // Menu
#define HID_CONSUMER_SELECTION      128 // Selection
#define HID_CONSUMER_ASSIGN_SEL     129 // Assign Selection
#define HID_CONSUMER_MODE_STEP      130 // Mode Step
#define HID_CONSUMER_RECALL_LAST    131 // Recall Last
#define HID_CONSUMER_QUIT           148 // Quit
#define HID_CONSUMER_HELP           149 // Help
#define HID_CONSUMER_CHANNEL_UP     156 // Channel Increment
#define HID_CONSUMER_CHANNEL_DOWN   157 // Channel Decrement

#define HID_CONSUMER_PLAY           176 // Play
#define HID_CONSUMER_PAUSE          177 // Pause
#define HID_CONSUMER_RECORD         178 // Record
#define HID_CONSUMER_FAST_FORWARD   179 // Fast Forward
#define HID_CONSUMER_REWIND         180 // Rewind
#define HID_CONSUMER_SCAN_NEXT_TRK  181 // Scan Next Track
#define HID_CONSUMER_SCAN_PREV_TRK  182 // Scan Previous Track
#define HID_CONSUMER_STOP           183 // Stop
#define HID_CONSUMER_EJECT          184 // Eject
#define HID_CONSUMER_RANDOM_PLAY    185 // Random Play
#define HID_CONSUMER_SELECT_DISC    186 // Select Disk
#define HID_CONSUMER_ENTER_DISC     187 // Enter Disc
#define HID_CONSUMER_REPEAT         188 // Repeat
#define HID_CONSUMER_STOP_EJECT     204 // Stop/Eject
#define HID_CONSUMER_PLAY_PAUSE     205 // Play/Pause
#define HID_CONSUMER_PLAY_SKIP      206 // Play/Skip

#define HID_CONSUMER_VOLUME         224 // Volume
#define HID_CONSUMER_BALANCE        225 // Balance
#define HID_CONSUMER_MUTE           226 // Mute
#define HID_CONSUMER_BASS           227 // Bass
#define HID_CONSUMER_VOLUME_UP      233 // Volume Increment
#define HID_CONSUMER_VOLUME_DOWN    234 // Volume Decrement

#define HID_RPT_ID_CC_IN        3   // Consumer Control input report ID
#define HID_CC_IN_RPT_LEN       2   // 16-bit Consumer Usage value
static esp_err_t esp_hidd_send_consumer_value(uint16_t key_cmd, bool key_pressed)
{
    uint16_t usage = key_pressed ? key_cmd : 0;
    uint8_t buffer[HID_CC_IN_RPT_LEN] = {
        (uint8_t)(usage & 0xFF),
        (uint8_t)(usage >> 8),
    };
    return esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0,
                                  HID_RPT_ID_CC_IN, buffer, sizeof(buffer));
}

#if !CONFIG_BT_NIMBLE_ENABLED || CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1
static void set_last_action(const char *action)
{
    snprintf(s_last_action, sizeof(s_last_action), "%s", action);
    s_ui_dirty = true;
    ESP_LOGI(TAG, "%s", action);
}

static void load_remote_settings(void)
{
    nvs_handle_t handle;
    uint8_t value;
    if (nvs_open("remote_ui", NVS_READONLY, &handle) != ESP_OK) {
        s_selected_mode = s_remote_mode;
        return;
    }
    if (nvs_get_u8(handle, "profile", &value) == ESP_OK && value < REMOTE_MODE_COUNT) {
        s_remote_mode = (remote_mode_t)value;
    }
    if (nvs_get_u8(handle, "reader_map", &value) == ESP_OK && value < READER_MAP_COUNT) {
        s_reader_mapping = (reader_mapping_t)value;
    }
    nvs_close(handle);
    s_selected_mode = s_remote_mode;
}

static void save_remote_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("remote_ui", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Could not open remote settings");
        return;
    }
    nvs_set_u8(handle, "profile", (uint8_t)s_remote_mode);
    nvs_set_u8(handle, "reader_map", (uint8_t)s_reader_mapping);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool remote_is_ready(void)
{
    if (s_ble_ready && s_ble_hid_param.hid_dev) {
        return true;
    }
    set_last_action("Not connected");
    return false;
}

static void send_consumer_click(uint16_t usage, const char *action)
{
    if (!remote_is_ready()) {
        return;
    }
    esp_err_t press_result = esp_hidd_send_consumer_value(usage, true);
    vTaskDelay(pdMS_TO_TICKS(70));
    esp_err_t release_result = esp_hidd_send_consumer_value(usage, false);
    ESP_LOGI(TAG, "Consumer 0x%04X press=%s release=%s", usage,
             esp_err_to_name(press_result), esp_err_to_name(release_result));
    set_last_action(action);
}

static void send_keyboard_click(uint8_t key_code, const char *action)
{
    if (!remote_is_ready()) {
        return;
    }

    uint8_t report[8] = {0};
    report[2] = key_code;
    esp_err_t press_result = esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1,
                                                     report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(70));
    memset(report, 0, sizeof(report));
    esp_err_t release_result = esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1,
                                                       report, sizeof(report));
    ESP_LOGI(TAG, "Keyboard 0x%02X press=%s release=%s", key_code,
             esp_err_to_name(press_result), esp_err_to_name(release_result));
    set_last_action(action);
}

static void send_keyboard_chord(uint8_t modifiers, uint8_t key_code, const char *action)
{
    if (!remote_is_ready()) {
        return;
    }

    uint8_t report[8] = {0};
    report[0] = modifiers;
    report[2] = key_code;
    esp_err_t press_result = esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1,
                                                     report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(90));
    memset(report, 0, sizeof(report));
    esp_err_t release_result = esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1,
                                                       report, sizeof(report));
    ESP_LOGI(TAG, "Keyboard chord mod=0x%02X key=0x%02X press=%s release=%s",
             modifiers, key_code, esp_err_to_name(press_result),
             esp_err_to_name(release_result));
    set_last_action(action);
}

static void handle_remote_button(bool button_a)
{
    switch (s_remote_mode) {
    case REMOTE_MODE_MAC:
        send_consumer_click(button_a ? HID_CONSUMER_VOLUME_DOWN : HID_CONSUMER_VOLUME_UP,
                            button_a ? "Mac volume -" : "Mac volume +");
        break;
    case REMOTE_MODE_CAMERA:
        send_consumer_click(button_a ? HID_CONSUMER_VOLUME_UP : HID_CONSUMER_VOLUME_DOWN,
                            button_a ? "Shutter / Vol +" : "Shutter / Vol -");
        break;
    case REMOTE_MODE_READER:
        if (s_reader_mapping == READER_MAP_VOLUME) {
            send_consumer_click(button_a ? HID_CONSUMER_VOLUME_UP : HID_CONSUMER_VOLUME_DOWN,
                                button_a ? "Previous / Vol +" : "Next / Vol -");
        } else if (s_reader_mapping == READER_MAP_ARROWS) {
            // USB HID keyboard usages: Left Arrow 0x50, Right Arrow 0x4F.
            send_keyboard_click(button_a ? 0x50 : 0x4F,
                                button_a ? "Previous / Left" : "Next / Right");
        } else {
            // USB HID keyboard usages: Page Up 0x4B, Page Down 0x4E.
            send_keyboard_click(button_a ? 0x4B : 0x4E,
                                button_a ? "Previous / PgUp" : "Next / PgDn");
        }
        break;
    case REMOTE_MODE_IR:
        set_last_action("IR reserved");
        break;
    default:
        break;
    }
}

static void handle_joystick_event(stick_s3_event_t event)
{
    const bool previous = event == STICK_S3_EVENT_JOY_LEFT ||
                          event == STICK_S3_EVENT_JOY_UP;

    if (s_remote_mode == REMOTE_MODE_MAC) {
        switch (event) {
        case STICK_S3_EVENT_JOY_UP:
            send_consumer_click(HID_CONSUMER_VOLUME_UP, "Joy up / Volume +");
            break;
        case STICK_S3_EVENT_JOY_DOWN:
            send_consumer_click(HID_CONSUMER_VOLUME_DOWN, "Joy down / Volume -");
            break;
        case STICK_S3_EVENT_JOY_LEFT:
            // Standard Left/Right arrows are handled by most focused browser
            // video players as seek backward/forward.
            send_keyboard_click(0x50, "Joy left / Seek back");
            break;
        case STICK_S3_EVENT_JOY_RIGHT:
            send_keyboard_click(0x4F, "Joy right / Seek forward");
            break;
        case STICK_S3_EVENT_JOY_CLICK:
            send_consumer_click(HID_CONSUMER_PLAY_PAUSE, "Joy click / Play");
            break;
        case STICK_S3_EVENT_JOY_HOLD:
            send_keyboard_chord(0x01 | 0x08, 0x14, "Joy hold / Lock");
            break;
        default:
            break;
        }
        return;
    }

    if (s_remote_mode == REMOTE_MODE_CAMERA) {
        if (event == STICK_S3_EVENT_JOY_CLICK ||
            event == STICK_S3_EVENT_JOY_UP ||
            event == STICK_S3_EVENT_JOY_DOWN) {
            send_consumer_click(HID_CONSUMER_VOLUME_UP, "Joystick shutter");
        } else if (event == STICK_S3_EVENT_JOY_LEFT ||
                   event == STICK_S3_EVENT_JOY_RIGHT) {
            set_last_action(previous ? "Joy left" : "Joy right");
        }
        return;
    }

    if (s_remote_mode == REMOTE_MODE_READER) {
        if (event == STICK_S3_EVENT_JOY_LEFT ||
            event == STICK_S3_EVENT_JOY_RIGHT ||
            event == STICK_S3_EVENT_JOY_UP ||
            event == STICK_S3_EVENT_JOY_DOWN) {
            handle_remote_button(previous);
        } else if (event == STICK_S3_EVENT_JOY_CLICK) {
            set_last_action("Joystick click");
        }
        return;
    }

    set_last_action("IR joystick reserved");
}

static void handle_ui_event(stick_s3_event_t event)
{
    if (event == STICK_S3_EVENT_NONE) {
        return;
    }

    if (event == STICK_S3_EVENT_HOME) {
        if (s_ui_view == STICK_S3_VIEW_SELECT) {
            s_ui_view = STICK_S3_VIEW_ACTIVE;
            s_selected_mode = s_remote_mode;
        } else {
            s_selected_mode = s_remote_mode;
            s_ui_view = STICK_S3_VIEW_SELECT;
        }
        set_last_action(s_ui_view == STICK_S3_VIEW_SELECT ? "Select profile" : "Profile unchanged");
        return;
    }

    if (s_ui_view == STICK_S3_VIEW_SELECT) {
        if (event == STICK_S3_EVENT_BUTTON_A ||
            event == STICK_S3_EVENT_JOY_DOWN ||
            event == STICK_S3_EVENT_JOY_RIGHT) {
            s_selected_mode = (remote_mode_t)((s_selected_mode + 1) % REMOTE_MODE_COUNT);
            s_ui_dirty = true;
        } else if (event == STICK_S3_EVENT_JOY_UP ||
                   event == STICK_S3_EVENT_JOY_LEFT) {
            s_selected_mode = (remote_mode_t)((s_selected_mode + REMOTE_MODE_COUNT - 1) %
                                              REMOTE_MODE_COUNT);
            s_ui_dirty = true;
        } else if (event == STICK_S3_EVENT_BUTTON_B ||
                   event == STICK_S3_EVENT_JOY_CLICK) {
            s_remote_mode = s_selected_mode;
            s_ui_view = STICK_S3_VIEW_ACTIVE;
            save_remote_settings();
            set_last_action(s_remote_mode == REMOTE_MODE_IR ? "IR reserved" : "Profile selected");
        }
        return;
    }

    if (event >= STICK_S3_EVENT_JOY_UP && event <= STICK_S3_EVENT_JOY_HOLD) {
        handle_joystick_event(event);
        return;
    }

    if (event == STICK_S3_EVENT_BUTTON_A) {
        handle_remote_button(true);
    } else if (event == STICK_S3_EVENT_BUTTON_B) {
        handle_remote_button(false);
    } else if (event == STICK_S3_EVENT_HOLD_A) {
        if (s_remote_mode == REMOTE_MODE_MAC) {
            send_consumer_click(HID_CONSUMER_MUTE, "Mac mute");
        } else if (s_remote_mode == REMOTE_MODE_READER) {
            s_reader_mapping = (reader_mapping_t)((s_reader_mapping + 1) % READER_MAP_COUNT);
            save_remote_settings();
            set_last_action("Reader map changed");
        }
    } else if (event == STICK_S3_EVENT_HOLD_B && s_remote_mode == REMOTE_MODE_MAC) {
        // macOS lock screen: Control + Command + Q.
        send_keyboard_chord(0x01 | 0x08, 0x14, "Lock screen");
    }
}

static void stick_s3_remote_task(void *pvParameters)
{
    bool previous_connected = false;
    bool previous_ready = false;
    (void)pvParameters;

    while (1) {
        handle_ui_event(stick_s3_ui_poll());

        if (previous_connected != s_ble_connected || previous_ready != s_ble_ready) {
            previous_connected = s_ble_connected;
            previous_ready = s_ble_ready;
            s_ui_dirty = true;
        }
        if (s_ui_dirty) {
            const remote_mode_t display_mode = s_ui_view == STICK_S3_VIEW_SELECT
                                                   ? s_selected_mode
                                                   : s_remote_mode;
            stick_s3_ui_draw(s_ble_connected, s_ble_ready, display_mode, s_ui_view,
                             s_reader_mapping, s_last_action);
            s_ui_dirty = false;
        }
        // 50 Hz is responsive for physical controls while allowing longer idle
        // periods for dynamic frequency scaling and Bluetooth modem sleep.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

void ble_hid_task_start_up(void)
{
#if !CONFIG_BT_NIMBLE_ENABLED
    s_ble_ready = true;
    s_ui_dirty = true;
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1
    s_ble_ready = true;
    s_ui_dirty = true;

#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    /* Nimble Specific */
    xTaskCreate(ble_hid_demo_task_kbd, "ble_hid_demo_task_kbd", 3 * 1024, NULL, configMAX_PRIORITIES - 3,
                &s_ble_hid_param.task_hdl);
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    /* Nimble Specific */
    xTaskCreate(ble_hid_demo_task_mouse, "ble_hid_demo_task_mouse", 3 * 1024, NULL, configMAX_PRIORITIES - 3,
                &s_ble_hid_param.task_hdl);
#endif
}

void ble_hid_task_shut_down(void)
{
    s_ble_ready = false;
    s_ui_dirty = true;
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG = "HID_DEV_BLE";

    switch (event) {
    case ESP_HIDD_START_EVENT: {
        ESP_LOGI(TAG, "START");
        esp_hid_ble_gap_adv_start();
        break;
    }
    case ESP_HIDD_CONNECT_EVENT: {
        ESP_LOGI(TAG, "CONNECT");
        s_ble_connected = true;
        s_ble_ready = false;
        s_ui_dirty = true;
        break;
    }
    case ESP_HIDD_PROTOCOL_MODE_EVENT: {
        ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index, param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    }
    case ESP_HIDD_CONTROL_EVENT: {
        ESP_LOGI(TAG, "CONTROL[%u]: %sSUSPEND", param->control.map_index, param->control.control ? "EXIT_" : "");
        if (param->control.control)
        {
            // exit suspend
            ble_hid_task_start_up();
        } else {
            // suspend
            ble_hid_task_shut_down();
        }
    break;
    }
    case ESP_HIDD_OUTPUT_EVENT: {
        ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index, esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
        ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
        break;
    }
    case ESP_HIDD_FEATURE_EVENT: {
        ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index, esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT: {
        ESP_LOGI(TAG, "DISCONNECT: %s", esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev), param->disconnect.reason));
        s_ble_connected = false;
        ble_hid_task_shut_down();
        esp_hid_ble_gap_adv_start();
        break;
    }
    case ESP_HIDD_STOP_EVENT: {
        ESP_LOGI(TAG, "STOP");
        break;
    }
    default:
        break;
    }
    return;
}
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
static local_param_t s_bt_hid_param = {0};
const unsigned char mouseReportMap[] = {
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,                    // USAGE (Mouse)
    0xa1, 0x01,                    // COLLECTION (Application)

    0x09, 0x01,                    //   USAGE (Pointer)
    0xa1, 0x00,                    //   COLLECTION (Physical)

    0x05, 0x09,                    //     USAGE_PAGE (Button)
    0x19, 0x01,                    //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,                    //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,                    //     LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x75, 0x01,                    //     REPORT_SIZE (1)
    0x81, 0x02,                    //     INPUT (Data,Var,Abs)
    0x95, 0x01,                    //     REPORT_COUNT (1)
    0x75, 0x05,                    //     REPORT_SIZE (5)
    0x81, 0x03,                    //     INPUT (Cnst,Var,Abs)

    0x05, 0x01,                    //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,                    //     USAGE (X)
    0x09, 0x31,                    //     USAGE (Y)
    0x09, 0x38,                    //     USAGE (Wheel)
    0x15, 0x81,                    //     LOGICAL_MINIMUM (-127)
    0x25, 0x7f,                    //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,                    //     REPORT_SIZE (8)
    0x95, 0x03,                    //     REPORT_COUNT (3)
    0x81, 0x06,                    //     INPUT (Data,Var,Rel)

    0xc0,                          //   END_COLLECTION
    0xc0                           // END_COLLECTION
};

static esp_hid_raw_report_map_t bt_report_maps[] = {
    {
        .data = mouseReportMap,
        .len = sizeof(mouseReportMap)
    },
};

static esp_hid_device_config_t bt_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = "ESP BT HID1",
    .manufacturer_name  = "Espressif",
    .serial_number      = "1234567890",
    .report_maps        = bt_report_maps,
    .report_maps_len    = 1
};

// send the buttons, change in x, and change in y
void send_mouse(uint8_t buttons, char dx, char dy, char wheel)
{
    static uint8_t buffer[4] = {0};
    buffer[0] = buttons;
    buffer[1] = dx;
    buffer[2] = dy;
    buffer[3] = wheel;
    esp_hidd_dev_input_set(s_bt_hid_param.hid_dev, 0, 0, buffer, 4);
}

void bt_hid_demo_task(void *pvParameters)
{
    static const char* help_string = "########################################################################\n"\
    "BT hid mouse demo usage:\n"\
    "You can input these value to simulate mouse: 'q', 'w', 'e', 'a', 's', 'd', 'h'\n"\
    "q -- click the left key\n"\
    "w -- move up\n"\
    "e -- click the right key\n"\
    "a -- move left\n"\
    "s -- move down\n"\
    "d -- move right\n"\
    "h -- show the help\n"\
    "########################################################################\n";
    printf("%s\n", help_string);
    char c;
    while (1) {
        c = fgetc(stdin);
        switch (c) {
        case 'q':
            send_mouse(1, 0, 0, 0);
            break;
        case 'w':
            send_mouse(0, 0, -10, 0);
            break;
        case 'e':
            send_mouse(2, 0, 0, 0);
            break;
        case 'a':
            send_mouse(0, -10, 0, 0);
            break;
        case 's':
            send_mouse(0, 0, 10, 0);
            break;
        case 'd':
            send_mouse(0, 10, 0, 0);
            break;
        case 'h':
            printf("%s\n", help_string);
            break;
        default:
            break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void bt_hid_task_start_up(void)
{
    xTaskCreate(bt_hid_demo_task, "bt_hid_demo_task", 2 * 1024, NULL, configMAX_PRIORITIES - 3, &s_bt_hid_param.task_hdl);
    return;
}

void bt_hid_task_shut_down(void)
{
    if (s_bt_hid_param.task_hdl) {
        vTaskDelete(s_bt_hid_param.task_hdl);
        s_bt_hid_param.task_hdl = NULL;
    }
}

static void bt_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG = "HID_DEV_BT";

    switch (event) {
    case ESP_HIDD_START_EVENT: {
        if (param->start.status == ESP_OK) {
            ESP_LOGI(TAG, "START OK");
            ESP_LOGI(TAG, "Setting to connectable, discoverable");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            ESP_LOGE(TAG, "START failed!");
        }
        break;
    }
    case ESP_HIDD_CONNECT_EVENT: {
        if (param->connect.status == ESP_OK) {
            ESP_LOGI(TAG, "CONNECT OK");
            ESP_LOGI(TAG, "Setting to non-connectable, non-discoverable");
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            bt_hid_task_start_up();
        } else {
            ESP_LOGE(TAG, "CONNECT failed!");
        }
        break;
    }
    case ESP_HIDD_PROTOCOL_MODE_EVENT: {
        ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index, param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    }
    case ESP_HIDD_OUTPUT_EVENT: {
        ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index, esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
        ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
        break;
    }
    case ESP_HIDD_FEATURE_EVENT: {
        ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index, esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT: {
        if (param->disconnect.status == ESP_OK) {
            ESP_LOGI(TAG, "DISCONNECT OK");
            bt_hid_task_shut_down();
            ESP_LOGI(TAG, "Setting to connectable, discoverable again");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            ESP_LOGE(TAG, "DISCONNECT failed!");
        }
        break;
    }
    case ESP_HIDD_STOP_EVENT: {
        ESP_LOGI(TAG, "STOP");
        break;
    }
    default:
        break;
    }
    return;
}

#if CONFIG_BT_SDP_COMMON_ENABLED
static void esp_sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SDP_INIT_EVT: status:%d", param->init.status);
        if (param->init.status == ESP_SDP_SUCCESS) {
            esp_bluetooth_sdp_dip_record_t dip_record = {
                .hdr =
                    {
                        .type = ESP_SDP_TYPE_DIP_SERVER,
                    },
                .vendor           = bt_hid_config.vendor_id,
                .vendor_id_source = ESP_SDP_VENDOR_ID_SRC_BT,
                .product          = bt_hid_config.product_id,
                .version          = bt_hid_config.version,
                .primary_record   = true,
            };
            esp_sdp_create_record((esp_bluetooth_sdp_record_t *)&dip_record);
        }
        break;
    case ESP_SDP_DEINIT_EVT:
        ESP_LOGI(TAG, "ESP_SDP_DEINIT_EVT: status:%d", param->deinit.status);
        break;
    case ESP_SDP_SEARCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_SEARCH_COMP_EVT: status:%d", param->search.status);
        break;
    case ESP_SDP_CREATE_RECORD_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_CREATE_RECORD_COMP_EVT: status:%d, handle:0x%x", param->create_record.status,
                 param->create_record.record_handle);
        break;
    case ESP_SDP_REMOVE_RECORD_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_REMOVE_RECORD_COMP_EVT: status:%d", param->remove_record.status);
        break;
    default:
        break;
    }
}
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */

#endif

#if CONFIG_BT_NIMBLE_ENABLED
void ble_hid_device_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}
void ble_store_config_init(void);
#endif

static void configure_remote_ble_identity(void)
{
    uint8_t base_mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(base_mac));

    // Use a stable locally administered identity so iOS cannot reuse the
    // cached HID report map from the earlier test firmware.
    base_mac[0] = (uint8_t)((base_mac[0] | 0x02U) & 0xFEU);
    base_mac[4] ^= 0x6DU;
    base_mac[5] ^= 0xA7U;
    ESP_ERROR_CHECK(esp_base_mac_addr_set(base_mac));
    ESP_LOGI(TAG, "Using fresh BLE identity " MACSTR, MAC2STR(base_mac));
}

void app_main(void)
{
    esp_err_t ret;
#if HID_DEV_MODE == HIDD_IDLE_MODE
    ESP_LOGE(TAG, "Please turn on BT HID device or BLE!");
    return;
#endif
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    configure_remote_ble_identity();

    load_remote_settings();
    stick_s3_ui_init();
    stick_s3_ui_draw(false, false, s_remote_mode, s_ui_view,
                     s_reader_mapping, s_last_action);
    xTaskCreate(stick_s3_remote_task, "stick_s3_remote", 6 * 1024, NULL,
                configMAX_PRIORITIES - 4, &s_remote_task_hdl);

    ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_DEV_MODE);
    ret = esp_hid_gap_init(HID_DEV_MODE);
    ESP_ERROR_CHECK( ret );

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
#if CONFIG_EXAMPLE_HID_DEVICE_ROLE == 2
    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, ble_hid_config.device_name);
#elif CONFIG_EXAMPLE_HID_DEVICE_ROLE == 3
    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE, ble_hid_config.device_name);
#else
    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, ble_hid_config.device_name);
#endif
    ESP_ERROR_CHECK( ret );
#if CONFIG_BT_BLE_ENABLED
    if ((ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)) != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %d", ret);
        return;
    }
#endif
    ESP_LOGI(TAG, "setting ble device");
    ESP_ERROR_CHECK(
        esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble_hid_param.hid_dev));
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
    ESP_LOGI(TAG, "setting device name");
    esp_bt_gap_set_device_name(bt_hid_config.device_name);
    ESP_LOGI(TAG, "setting cod major, peripheral");
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_POINTING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "setting bt device");
    ESP_ERROR_CHECK(
        esp_hidd_dev_init(&bt_hid_config, ESP_HID_TRANSPORT_BT, bt_hidd_event_callback, &s_bt_hid_param.hid_dev));
#if CONFIG_BT_SDP_COMMON_ENABLED
    ESP_ERROR_CHECK(esp_sdp_register_callback(esp_sdp_cb));
    ESP_ERROR_CHECK(esp_sdp_init());
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */
#endif /* CONFIG_BT_HID_DEVICE_ENABLED */
#if CONFIG_BT_NIMBLE_ENABLED
    /* XXX Need to have template for store */
    ble_store_config_init();

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
	/* Starting nimble task after gatts is initialized*/
    ret = esp_nimble_enable(ble_hid_device_host_task);
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
    }
#endif
}
