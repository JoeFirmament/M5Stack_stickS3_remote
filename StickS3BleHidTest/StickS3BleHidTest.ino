#include <M5Unified.h>

#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>

namespace {

constexpr char kDeviceName[] = "StickS3 Remote";
constexpr uint8_t kKeyboardReportId = 1;
constexpr uint32_t kLongPressMs = 800;

constexpr uint8_t kKeyRightArrow = 0x4F;
constexpr uint8_t kKeyLeftArrow = 0x50;
constexpr uint8_t kMediaVolumeUp = 1U << 5;
constexpr uint8_t kMediaVolumeDown = 1U << 6;

// A single input characteristic carries one 9-byte report:
//   bytes 0..7: standard keyboard report
//   byte 8:      Consumer Control bitmap
// Using one characteristic avoids a duplicate-report-characteristic issue in
// the NimBLE implementation bundled with the current M5Stack board package.
const uint8_t kHidReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard)
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
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (Reserved)
    0x29, 0x65,        //   Usage Maximum (Keyboard Application)
    0x81, 0x00,        //   Input (Data, Array, Absolute)

    0x05, 0x0C,        //   Usage Page (Consumer)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x09, 0xB5,        //   Usage (Scan Next Track)
    0x09, 0xB6,        //   Usage (Scan Previous Track)
    0x09, 0xB7,        //   Usage (Stop)
    0x09, 0xCD,        //   Usage (Play/Pause)
    0x09, 0xE2,        //   Usage (Mute)
    0x09, 0xE9,        //   Usage (Volume Increment)
    0x09, 0xEA,        //   Usage (Volume Decrement)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x07,        //   Report Count (7)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Constant padding)
    0xC0,              // End Collection
};

BLECharacteristic* keyboardInput = nullptr;

volatile bool bleConnected = false;
volatile bool screenDirty = true;
char lastAction[32] = "Waiting for pairing";

uint32_t buttonAPressedAt = 0;
uint32_t buttonBPressedAt = 0;
bool buttonALongSent = false;
bool buttonBLongSent = false;

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    bleConnected = true;
    screenDirty = true;
    Serial.println("BLE connected");
  }

  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    screenDirty = true;
    Serial.println("BLE disconnected; advertising restarted");
  }
};

void setLastAction(const char* text) {
  snprintf(lastAction, sizeof(lastAction), "%s", text);
  screenDirty = true;
  Serial.println(text);
}

void drawScreen() {
  auto& display = M5.Display;
  display.startWrite();
  display.clear(TFT_BLACK);
  display.setRotation(1);
  display.setTextDatum(top_left);

  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 7);
  display.println("BLE HID TEST");

  display.setTextSize(1);
  display.setCursor(8, 34);
  display.setTextColor(bleConnected ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  display.printf("%s: %s", kDeviceName,
                 bleConnected ? "CONNECTED" : "PAIRING");

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(8, 54);
  display.println("A short: NEXT (Right Arrow)");
  display.println("B short: PREV (Left Arrow)");
  display.println("A hold : Volume Up / Camera");
  display.println("B hold : Volume Down / Camera");

  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setCursor(8, 112);
  display.printf("Last: %s", lastAction);
  display.endWrite();
  screenDirty = false;
}

bool ensureConnected() {
  if (bleConnected) {
    return true;
  }
  setLastAction("Not connected");
  return false;
}

void sendKeyboardKey(uint8_t keyCode, const char* actionText) {
  if (!ensureConnected()) {
    return;
  }

  uint8_t report[9] = {0};
  report[2] = keyCode;
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  delay(18);

  memset(report, 0, sizeof(report));
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  setLastAction(actionText);
}

void sendConsumerKey(uint8_t mediaMask, const char* actionText) {
  if (!ensureConnected()) {
    return;
  }

  uint8_t report[9] = {0};
  report[8] = mediaMask;
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  delay(18);

  memset(report, 0, sizeof(report));
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  setLastAction(actionText);
}

void setupBleHid() {
  BLEDevice::init(kDeviceName);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  auto* hid = new BLEHIDDevice(server);
  keyboardInput = hid->inputReport(kKeyboardReportId);

  hid->manufacturer()->setValue("M5Stack");
  hid->pnp(0x02, 0x303A, 0x4002, 0x0100);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap(const_cast<uint8_t*>(kHidReportMap), sizeof(kHidReportMap));
  hid->setBatteryLevel(100);
  hid->startServices();

  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setKeySize(16);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->start();

  Serial.printf("Advertising as %s\n", kDeviceName);
}

void handleButtons() {
  const uint32_t now = millis();

  if (M5.BtnA.wasPressed()) {
    buttonAPressedAt = now;
    buttonALongSent = false;
  }
  if (M5.BtnB.wasPressed()) {
    buttonBPressedAt = now;
    buttonBLongSent = false;
  }

  if (M5.BtnA.isPressed() && !buttonALongSent &&
      now - buttonAPressedAt >= kLongPressMs) {
    buttonALongSent = true;
    sendConsumerKey(kMediaVolumeUp, "Volume Up / Camera");
  }
  if (M5.BtnB.isPressed() && !buttonBLongSent &&
      now - buttonBPressedAt >= kLongPressMs) {
    buttonBLongSent = true;
    sendConsumerKey(kMediaVolumeDown, "Volume Down / Camera");
  }

  if (M5.BtnA.wasReleased() && !buttonALongSent) {
    sendKeyboardKey(kKeyRightArrow, "Next / Right Arrow");
  }
  if (M5.BtnB.wasReleased() && !buttonBLongSent) {
    sendKeyboardKey(kKeyLeftArrow, "Prev / Left Arrow");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  auto config = M5.config();
  M5.begin(config);
  M5.Display.setBrightness(128);

  drawScreen();
  setupBleHid();
}

void loop() {
  M5.update();
  handleButtons();

  if (screenDirty) {
    drawScreen();
  }

  delay(5);
}
