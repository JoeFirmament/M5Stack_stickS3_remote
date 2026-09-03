#include <M5Unified.h>

#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <esp_err.h>
#include <esp_mac.h>

namespace {

constexpr char kDeviceName[] = "StickS3 Camera 3";
constexpr uint8_t kReportId = 1;
constexpr uint16_t kVolumeUpUsage = 0x00E9;
constexpr uint16_t kVolumeDownUsage = 0x00EA;

// Consumer Control array report used by Nordic's iOS 16-tested BLE HID
// volume-control sample. Each report contains one 16-bit Consumer Usage ID,
// little-endian: E9 00 for Volume Increment, EA 00 for Volume Decrement, and
// 00 00 for release. Newer iOS versions accept this form for camera shutter.
const uint8_t kConsumerReportMap[] = {
    0x05, 0x0C,  // Usage Page (Consumer)
    0x09, 0x01,  // Usage (Consumer Control)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x15, 0x00,  //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  // Logical Maximum (0x03FF)
    0x19, 0x00,        // Usage Minimum (0)
    0x2A, 0xFF, 0x03,  // Usage Maximum (0x03FF)
    0x75, 0x10,        // Report Size (16 bits)
    0x95, 0x01,  //   Report Count (1)
    0x81, 0x00,  //   Input (Data, Array, Absolute)
    0xC0,        // End Collection
};

BLECharacteristic* consumerInput = nullptr;
volatile bool connected = false;
volatile bool screenDirty = true;
char lastAction[24] = "Waiting";

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    connected = true;
    screenDirty = true;
    Serial.println("iPhone connected");
  }

  void onDisconnect(BLEServer*) override {
    connected = false;
    screenDirty = true;
    Serial.println("Disconnected; advertising restarted");
  }
};

class ReportCallbacks final : public BLECharacteristicCallbacks {
 public:
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc*,
                   uint16_t subValue) override {
    Serial.printf("HID report subscription: 0x%04X\n", subValue);
  }

  void onStatus(BLECharacteristic*, Status status, uint32_t code) override {
    Serial.printf("HID notify status: %d, code: %lu\n",
                  static_cast<int>(status),
                  static_cast<unsigned long>(code));
  }
};

void setLastAction(const char* action) {
  snprintf(lastAction, sizeof(lastAction), "%s", action);
  screenDirty = true;
  Serial.println(action);
}

void drawScreen() {
  auto& display = M5.Display;
  display.startWrite();
  display.clear(TFT_BLACK);
  display.setRotation(1);
  display.setTextDatum(top_left);

  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.println("iPhone Camera");

  display.setTextSize(1);
  display.setCursor(8, 40);
  display.setTextColor(connected ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  display.printf("%s: %s", kDeviceName,
                 connected ? "CONNECTED" : "PAIRING");

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 68);
  display.println("A: Shutter Vol+");
  display.println("B: Shutter Vol-");

  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 116);
  display.printf("Last: %s", lastAction);
  display.endWrite();
  screenDirty = false;
}

void sendConsumerUsage(uint16_t usage, const char* action) {
  if (!connected) {
    setLastAction("Not connected");
    return;
  }

  uint8_t report[2] = {
      static_cast<uint8_t>(usage & 0xFF),
      static_cast<uint8_t>(usage >> 8),
  };
  consumerInput->setValue(report, sizeof(report));
  consumerInput->notify();
  delay(100);

  report[0] = 0;
  report[1] = 0;
  consumerInput->setValue(report, sizeof(report));
  consumerInput->notify();
  setLastAction(action);
}

void configureCameraIdentity() {
  uint8_t baseMac[6] = {0};
  if (esp_efuse_mac_get_default(baseMac) != ESP_OK) {
    Serial.println("Could not read factory MAC; using default BLE identity");
    return;
  }

  // Create a stable, locally administered unicast address for the Camera
  // personality. It is deliberately distinct from the factory address used by
  // the keyboard/X4 personality, preventing host-side HID cache collisions.
  baseMac[0] = static_cast<uint8_t>((baseMac[0] | 0x02U) & 0xFEU);
  baseMac[4] ^= 0xCA;
  baseMac[5] ^= 0x03;

  const esp_err_t result = esp_base_mac_addr_set(baseMac);
  Serial.printf("Camera base MAC setup: %s\n", esp_err_to_name(result));
}

void setupBle() {
  BLEDevice::init(kDeviceName);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  auto* hid = new BLEHIDDevice(server);
  consumerInput = hid->inputReport(kReportId);
  consumerInput->setCallbacks(new ReportCallbacks());
  hid->manufacturer()->setValue("M5Stack");
  hid->pnp(0x02, 0x303A, 0x4003, 0x0100);
  hid->hidInfo(0x00, 0x03);
  hid->reportMap(const_cast<uint8_t*>(kConsumerReportMap),
                 sizeof(kConsumerReportMap));
  hid->setBatteryLevel(100);
  hid->startServices();

  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setKeySize(16);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(GENERIC_HID);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->start();
  Serial.printf("Advertising as %s\n", kDeviceName);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  configureCameraIdentity();

  auto config = M5.config();
  M5.begin(config);
  M5.Display.setBrightness(128);

  drawScreen();
  setupBle();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    sendConsumerUsage(kVolumeUpUsage, "Shutter Vol+");
  }
  if (M5.BtnB.wasPressed()) {
    sendConsumerUsage(kVolumeDownUsage, "Shutter Vol-");
  }

  if (screenDirty) {
    drawScreen();
  }
  delay(5);
}
