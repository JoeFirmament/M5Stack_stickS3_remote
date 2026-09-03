#include <M5Unified.h>

#include <BLEAdvertising.h>
#include <BLEDescriptor.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <esp_err.h>
#include <esp_mac.h>

namespace {

constexpr char kDeviceName[] = "StickS3 M5Key";
constexpr uint32_t kPairingPin = 482731;
constexpr uint8_t kReportId = 1;
constexpr uint8_t kKeyA = 0x04;
constexpr uint8_t kKeyEnter = 0x28;

const uint8_t kKeyboardReportMap[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,  //   Usage Minimum (Left Control)
    0x29, 0xE7,  //   Usage Maximum (Right GUI)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant)
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x19, 0x00,  //   Usage Minimum (Reserved)
    0x29, 0x65,  //   Usage Maximum (Keyboard Application)
    0x81, 0x00,  //   Input (Data, Array, Absolute)
    0xC0,        // End Collection
};

BLECharacteristic* keyboardInput = nullptr;
volatile bool connected = false;
volatile bool authenticated = false;
volatile bool screenDirty = true;
char lastAction[24] = "Waiting";

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    connected = true;
    authenticated = false;
    screenDirty = true;
    Serial.println("Connected; waiting for authenticated pairing");
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer*, ble_gap_conn_desc* desc) override {
    Serial.printf("Starting security on connection %u\n", desc->conn_handle);
    int rc = 0;
    const bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
    Serial.printf("Security start: %s, rc=%d\n", started ? "OK" : "FAILED", rc);
  }
#endif

  void onDisconnect(BLEServer*) override {
    connected = false;
    authenticated = false;
    screenDirty = true;
    Serial.println("Disconnected; advertising restarted");
  }
};

class SecurityCallbacks final : public BLESecurityCallbacks {
 public:
  void onPassKeyNotify(uint32_t passKey) override {
    Serial.printf("Enter PIN on iPhone: %06lu\n",
                  static_cast<unsigned long>(passKey));
    snprintf(lastAction, sizeof(lastAction), "PIN %06lu",
             static_cast<unsigned long>(passKey));
    screenDirty = true;
  }

  bool onSecurityRequest() override {
    Serial.println("Security request accepted");
    return true;
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    authenticated = desc != nullptr && desc->sec_state.encrypted &&
                    desc->sec_state.authenticated && desc->sec_state.bonded;
    if (desc == nullptr) {
      Serial.println("Authentication failed: no connection descriptor");
      snprintf(lastAction, sizeof(lastAction), "AUTH FAILED");
    } else {
      Serial.printf("AUTH RESULT enc=%u auth=%u bond=%u key=%u\n",
                    desc->sec_state.encrypted,
                    desc->sec_state.authenticated,
                    desc->sec_state.bonded,
                    desc->sec_state.key_size);
      snprintf(lastAction, sizeof(lastAction), "%s",
               authenticated ? "AUTH OK" : "AUTH FAILED");
    }
    screenDirty = true;
  }
#endif
};

class ReportCallbacks final : public BLECharacteristicCallbacks {
 public:
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc* desc,
                   uint16_t subValue) override {
    Serial.printf("Keyboard subscription: 0x%04X\n", subValue);
    Serial.printf("Security enc=%u auth=%u bond=%u key=%u\n",
                  desc->sec_state.encrypted,
                  desc->sec_state.authenticated,
                  desc->sec_state.bonded,
                  desc->sec_state.key_size);
  }

  void onStatus(BLECharacteristic*, Status status, uint32_t code) override {
    Serial.printf("Keyboard notify status: %d, code: %lu\n",
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
  display.println("iPhone Key Test");

  display.setTextSize(1);
  display.setCursor(8, 40);
  display.setTextColor(connected ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  display.printf("%s: %s", kDeviceName,
                 authenticated ? "AUTH OK" :
                 (connected ? "CONNECTED" : "PAIRING"));

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 68);
  if (authenticated) {
    display.println("A: type a");
    display.println("B: Enter");
  } else {
    display.printf("PIN: %06lu\n", static_cast<unsigned long>(kPairingPin));
    display.println("Enter on iPhone");
  }

  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 116);
  display.printf("Last: %s", lastAction);
  display.endWrite();
  screenDirty = false;
}

void sendKeyboardKey(uint8_t keyCode, const char* action) {
  if (!authenticated) {
    setLastAction("Not authenticated");
    return;
  }

  uint8_t report[8] = {0};
  report[2] = keyCode;
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  delay(80);

  memset(report, 0, sizeof(report));
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  setLastAction(action);
}

void configureTestIdentity() {
  uint8_t baseMac[6] = {0};
  if (esp_efuse_mac_get_default(baseMac) != ESP_OK) {
    Serial.println("Could not read factory MAC");
    return;
  }

  baseMac[0] = static_cast<uint8_t>((baseMac[0] | 0x02U) & 0xFEU);
  baseMac[4] ^= 0x5D;
  baseMac[5] ^= 0x82;
  Serial.printf("Key-test base MAC setup: %s\n",
                esp_err_to_name(esp_base_mac_addr_set(baseMac)));
}

void setupBle() {
  BLEDevice::init(kDeviceName);

  auto* security = new BLESecurity();
  security->setPassKey(true, kPairingPin);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK |
                                 ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK |
                                 ESP_BLE_ID_KEY_MASK);
  security->setKeySize(16);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  auto* hid = new BLEHIDDevice(server);
  // Use the official BLEHIDDevice factory.  It intentionally leaves the
  // Report characteristic readable before authentication so iOS can finish
  // HOGP enumeration, while the BLE link itself is encrypted after pairing.
  keyboardInput = hid->inputReport(kReportId);
  keyboardInput->setCallbacks(new ReportCallbacks());
  hid->manufacturer()->setValue("M5Stack");
  hid->pnp(0x02, 0x303A, 0x4004, 0x0100);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap(const_cast<uint8_t*>(kKeyboardReportMap),
                 sizeof(kKeyboardReportMap));
  hid->setBatteryLevel(100);
  hid->startServices();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->start();
  Serial.printf("Advertising as %s\n", kDeviceName);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  configureTestIdentity();

  auto config = M5.config();
  M5.begin(config);
  M5.Display.setBrightness(128);
  drawScreen();
  setupBle();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    sendKeyboardKey(kKeyA, "Typed a");
  }
  if (M5.BtnB.wasPressed()) {
    sendKeyboardKey(kKeyEnter, "Pressed Enter");
  }

  if (screenDirty) {
    drawScreen();
  }
  delay(5);
}
