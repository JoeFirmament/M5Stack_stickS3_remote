#include "stick_s3_ui.h"

#include <M5Unified.h>
#include <esp_log.h>

#include <cstring>

namespace {

constexpr uint32_t kHoldMs = 800;
constexpr uint32_t kHomeHoldMs = 900;
constexpr uint8_t kDisplayBrightness = 144;
constexpr uint32_t kDisplaySleepMs = 20000;
constexpr uint32_t kJoystickPowerSaveMs = 300000;
constexpr uint32_t kPowerLogMs = 60000;
constexpr uint32_t kJoystickI2cHz = 400000;
constexpr uint8_t kJoystickAddress = 0x63;
constexpr uint8_t kJoystickOffsetRegister = 0x50;
constexpr uint8_t kJoystickButtonRegister = 0x20;
constexpr uint8_t kJoystickRgbRegister = 0x30;
constexpr int16_t kJoystickEnterThreshold = 700;
constexpr int16_t kJoystickReleaseThreshold = 350;
constexpr uint32_t kJoystickHoldMs = 850;
constexpr uint32_t kJoystickActivePollMs = 20;
constexpr uint32_t kJoystickIdlePollMs = 100;
constexpr uint32_t kJoystickActiveWindowMs = 2000;
constexpr uint32_t kMotionPollMs = 100;
constexpr uint32_t kMotionArmDelayMs = 600;
constexpr float kMotionThresholdG = 0.32f;
constexpr uint8_t kMotionSamplesRequired = 2;

const char *const kJoystickTag = "Joystick2";
const char *const kPowerTag = "Power";
const char *const kMotionTag = "Motion";

bool joystickConnected = false;
bool joystickDetected = false;
bool joystickPowered = false;
int16_t joystickX = 0;
int16_t joystickY = 0;
bool joystickButtonPressed = false;
bool joystickButtonWasPressed = false;
bool joystickButtonHeld = false;
uint32_t joystickButtonDownMs = 0;
uint32_t joystickLastPollMs = 0;
uint32_t joystickLastActiveMs = 0;
stick_s3_event_t joystickDirection = STICK_S3_EVENT_NONE;
bool displayAwake = true;
uint32_t lastUserActivityMs = 0;
uint32_t lastPowerLogMs = 0;
bool motionSensorReady = false;
bool motionBaselineValid = false;
float motionBaselineX = 0.0f;
float motionBaselineY = 0.0f;
float motionBaselineZ = 0.0f;
uint32_t motionArmedAtMs = 0;
uint32_t motionLastPollMs = 0;
uint8_t motionHitCount = 0;

constexpr uint16_t kBackground = 0x0861;
constexpr uint16_t kSurface = 0x10E4;
constexpr uint16_t kSurfaceRaised = 0x1946;
constexpr uint16_t kBorder = 0x326A;
constexpr uint16_t kCyan = 0x2E9F;
constexpr uint16_t kCyanDark = 0x04B3;
constexpr uint16_t kGreen = 0x4EEB;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kAmber = 0xFD68;
constexpr uint16_t kText = 0xE73C;
constexpr uint16_t kMuted = 0x8CD3;

const char *modeName(int mode) {
    static const char *const names[] = {"MAC", "CAMERA", "READER", "IR REMOTE"};
    return (mode >= 0 && mode < 4) ? names[mode] : "UNKNOWN";
}

const char *modeSubtitle(int mode) {
    static const char *const subtitles[] = {
        "VOLUME / LOCK",
        "IPHONE SHUTTER",
        "PAGE TURNER",
        "LEARN + SEND / SOON",
    };
    return (mode >= 0 && mode < 4) ? subtitles[mode] : "";
}

const char *readerMapName(int mapping) {
    static const char *const names[] = {"VOLUME", "ARROWS", "PAGE KEYS"};
    return (mapping >= 0 && mapping < 3) ? names[mapping] : "UNKNOWN";
}

bool readJoystickRegister(uint8_t reg, uint8_t *data, size_t length) {
    return M5.Ex_I2C.readRegister(kJoystickAddress, reg, data, length,
                                  kJoystickI2cHz);
}

bool writeJoystickRgb(uint32_t color) {
    uint8_t data[4] = {
        static_cast<uint8_t>(color),
        static_cast<uint8_t>(color >> 8),
        static_cast<uint8_t>(color >> 16),
        static_cast<uint8_t>(color >> 24),
    };
    return M5.Ex_I2C.writeRegister(kJoystickAddress, kJoystickRgbRegister,
                                   data, sizeof(data), kJoystickI2cHz);
}

bool detectJoystickWithRetries() {
    constexpr int kAttempts = 5;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        if (M5.Ex_I2C.scanID(kJoystickAddress, 100000)) {
            return true;
        }
        ESP_LOGW(kJoystickTag, "Probe %d/%d failed; waiting for startup",
                 attempt, kAttempts);
        M5.delay(100);
    }
    return false;
}

void resetJoystickState() {
    joystickX = 0;
    joystickY = 0;
    joystickButtonPressed = false;
    joystickButtonWasPressed = false;
    joystickButtonHeld = false;
    joystickButtonDownMs = 0;
    joystickDirection = STICK_S3_EVENT_NONE;
}

void noteUserActivity();

bool configureMotionSensor() {
    if (!M5.Imu.isEnabled() || M5.Imu.getType() != m5::imu_t::imu_bmi270) {
        ESP_LOGW(kMotionTag, "BMI270 not available; A/B wake remains active");
        return false;
    }

    auto *imu = M5.Imu.getImuInstancePtr(0);
    if (imu == nullptr) {
        ESP_LOGW(kMotionTag, "BMI270 instance unavailable");
        return false;
    }

    // BMI270 low-power accelerometer: 25 Hz, low-power filter, gyro/temp/AUX
    // disabled. The ESP32 remains awake at its DFS minimum so BLE stays paired;
    // we only read the sensor at 10 Hz while the display is asleep.
    bool ok = imu->writeRegister8(0x7C, 0x00);  // Disable advanced power save.
    M5.delay(1);
    ok = imu->writeRegister8(0x40, 0x26) && ok;  // ACC_CONF: 25 Hz LP.
    ok = imu->writeRegister8(0x7D, 0x04) && ok;  // ACC on; gyro/temp/AUX off.
    M5.delay(3);
    ok = imu->writeRegister8(0x7C, 0x01) && ok;  // Advanced power save on.
    M5.delay(1);

    ESP_LOGI(kMotionTag,
             "BMI270 low-power setup=%s addr=0x%02X acc_conf=0x%02X pwr_ctrl=0x%02X pwr_conf=0x%02X",
             ok ? "ok" : "failed", imu->getAddress(), imu->readRegister8(0x40),
             imu->readRegister8(0x7D), imu->readRegister8(0x7C));
    return ok;
}

void armMotionWake(uint32_t now) {
    motionBaselineValid = false;
    motionHitCount = 0;
    motionArmedAtMs = now;
    motionLastPollMs = 0;
}

bool pollMotionWake(uint32_t now) {
    if (!motionSensorReady || displayAwake ||
        now - motionLastPollMs < kMotionPollMs) {
        return false;
    }
    motionLastPollMs = now;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!M5.Imu.getAccel(&x, &y, &z)) {
        return false;
    }

    if (!motionBaselineValid) {
        motionBaselineX = x;
        motionBaselineY = y;
        motionBaselineZ = z;
        motionBaselineValid = true;
        return false;
    }
    if (now - motionArmedAtMs < kMotionArmDelayMs) {
        return false;
    }

    const float dx = x - motionBaselineX;
    const float dy = y - motionBaselineY;
    const float dz = z - motionBaselineZ;
    const float deltaSquared = dx * dx + dy * dy + dz * dz;
    const float thresholdSquared = kMotionThresholdG * kMotionThresholdG;
    if (deltaSquared >= thresholdSquared) {
        ++motionHitCount;
        if (motionHitCount >= kMotionSamplesRequired) {
            ESP_LOGI(kMotionTag, "Pickup detected delta2=%.3f; waking controls",
                     static_cast<double>(deltaSquared));
            motionBaselineValid = false;
            motionHitCount = 0;
            noteUserActivity();
            return true;
        }
    } else {
        motionHitCount = 0;
        // A slow baseline follows temperature drift and tiny table vibration,
        // but remains far slower than a normal pickup gesture.
        constexpr float kBaselineFollow = 1.0f / 32.0f;
        motionBaselineX += (x - motionBaselineX) * kBaselineFollow;
        motionBaselineY += (y - motionBaselineY) * kBaselineFollow;
        motionBaselineZ += (z - motionBaselineZ) * kBaselineFollow;
    }
    return false;
}

bool powerOnJoystick() {
    if (!joystickDetected || joystickPowered) return joystickConnected;

    M5.Power.setExtOutput(true);
    joystickPowered = true;
    M5.delay(100);
    ESP_LOGI(kJoystickTag, "5V output=%s", M5.Power.getExtOutput() ? "on" : "off");
    joystickConnected = detectJoystickWithRetries();
    if (!joystickConnected) {
        ESP_LOGW(kJoystickTag, "Did not respond after power restore");
        M5.Power.setExtOutput(false);
        joystickPowered = false;
        return false;
    }
    writeJoystickRgb(0x000000);
    joystickLastPollMs = 0;
    joystickLastActiveMs = lgfx::millis();
    ESP_LOGI(kJoystickTag, "Power restored");
    return true;
}

void noteUserActivity() {
    lastUserActivityMs = lgfx::millis();
    // When both loads are asleep, restore the Grove rail first and wait for
    // the Joystick2 MCU to settle before switching the LCD/backlight on. This
    // avoids stacking both inrush currents and tripping the brownout detector
    // on a partly discharged battery.
    powerOnJoystick();
    if (!displayAwake) {
        M5.Display.wakeup();
        displayAwake = true;
        motionBaselineValid = false;
        motionHitCount = 0;
        ESP_LOGI(kPowerTag, "Display awake");
    }
}

void initJoystick() {
    M5.Power.setExtOutput(true);
    joystickPowered = true;
    M5.delay(100);

    if (!M5.Ex_I2C.begin()) {
        ESP_LOGE(kJoystickTag, "Could not start external I2C (SDA=%d SCL=%d)",
                 M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());
        M5.Power.setExtOutput(false);
        joystickPowered = false;
        return;
    }

    ESP_LOGI(kJoystickTag, "5V output=%s", M5.Power.getExtOutput() ? "on" : "off");
    joystickConnected = detectJoystickWithRetries();
    joystickDetected = joystickConnected;
    if (!joystickConnected) {
        ESP_LOGW(kJoystickTag, "Not found at 0x%02X (SDA=%d SCL=%d)",
                 kJoystickAddress, M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());
        M5.Power.setExtOutput(false);
        joystickPowered = false;
        return;
    }

    uint8_t firmware = 0;
    uint8_t bootloader = 0;
    readJoystickRegister(0xFE, &firmware, 1);
    readJoystickRegister(0xFC, &bootloader, 1);
    // The status LED is decorative; keep it off to avoid a permanent load.
    writeJoystickRgb(0x000000);
    ESP_LOGI(kJoystickTag,
             "Detected at 0x%02X on SDA=%d SCL=%d, firmware=%u bootloader=%u",
             kJoystickAddress, M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL(), firmware,
             bootloader);
}

stick_s3_event_t pollJoystick() {
    if (!joystickConnected) return STICK_S3_EVENT_NONE;

    const uint32_t now = lgfx::millis();
    const bool active = joystickButtonPressed ||
                        joystickDirection != STICK_S3_EVENT_NONE ||
                        now - joystickLastActiveMs < kJoystickActiveWindowMs;
    const uint32_t poll_interval = active ? kJoystickActivePollMs
                                          : kJoystickIdlePollMs;
    if (now - joystickLastPollMs < poll_interval) {
        return STICK_S3_EVENT_NONE;
    }
    joystickLastPollMs = now;

    uint8_t axes[4] = {0};
    uint8_t button = 1;
    if (!readJoystickRegister(kJoystickOffsetRegister, axes, sizeof(axes)) ||
        !readJoystickRegister(kJoystickButtonRegister, &button, 1)) {
        ESP_LOGW(kJoystickTag, "Read failed");
        return STICK_S3_EVENT_NONE;
    }

    joystickX = static_cast<int16_t>(static_cast<uint16_t>(axes[0]) |
                                     (static_cast<uint16_t>(axes[1]) << 8));
    joystickY = static_cast<int16_t>(static_cast<uint16_t>(axes[2]) |
                                     (static_cast<uint16_t>(axes[3]) << 8));
    joystickButtonPressed = button == 0;

    const int16_t absX = joystickX < 0 ? -joystickX : joystickX;
    const int16_t absY = joystickY < 0 ? -joystickY : joystickY;
    if (joystickButtonPressed || absX >= kJoystickReleaseThreshold ||
        absY >= kJoystickReleaseThreshold) {
        joystickLastActiveMs = now;
        noteUserActivity();
    }

    if (joystickButtonPressed && !joystickButtonWasPressed) {
        joystickButtonDownMs = now;
        joystickButtonHeld = false;
        ESP_LOGI(kJoystickTag, "button down x=%d y=%d", joystickX, joystickY);
    }
    if (joystickButtonPressed && !joystickButtonHeld &&
        now - joystickButtonDownMs >= kJoystickHoldMs) {
        joystickButtonHeld = true;
        joystickButtonWasPressed = true;
        ESP_LOGI(kJoystickTag, "button hold x=%d y=%d", joystickX, joystickY);
        return STICK_S3_EVENT_JOY_HOLD;
    }
    if (!joystickButtonPressed && joystickButtonWasPressed) {
        joystickButtonWasPressed = false;
        if (!joystickButtonHeld) {
            ESP_LOGI(kJoystickTag, "button click x=%d y=%d", joystickX, joystickY);
            return STICK_S3_EVENT_JOY_CLICK;
        }
    }
    joystickButtonWasPressed = joystickButtonPressed;

    if (joystickDirection != STICK_S3_EVENT_NONE) {
        if (absX <= kJoystickReleaseThreshold &&
            absY <= kJoystickReleaseThreshold) {
            joystickDirection = STICK_S3_EVENT_NONE;
        }
        return STICK_S3_EVENT_NONE;
    }

    if (absX < kJoystickEnterThreshold && absY < kJoystickEnterThreshold) {
        return STICK_S3_EVENT_NONE;
    }
    if (absX >= absY) {
        joystickDirection = joystickX > 0 ? STICK_S3_EVENT_JOY_RIGHT
                                         : STICK_S3_EVENT_JOY_LEFT;
    } else {
        joystickDirection = joystickY > 0 ? STICK_S3_EVENT_JOY_UP
                                         : STICK_S3_EVENT_JOY_DOWN;
    }
    ESP_LOGI(kJoystickTag, "direction=%d x=%d y=%d", joystickDirection,
             joystickX, joystickY);
    return joystickDirection;
}

void useSmallFont() {
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
}

void useTitleFont() {
    M5.Display.setFont(&fonts::FreeSansBold9pt7b);
    M5.Display.setTextSize(1);
}

void drawStatus(bool connected, bool ready) {
    auto &display = M5.Display;
    useSmallFont();

    const uint16_t status_color = ready ? kGreen : (connected ? kAmber : kMuted);
    display.fillCircle(9, 9, 4, status_color);
    display.setTextColor(status_color, kBackground);
    display.setCursor(18, 5);
    display.print(ready ? "CONNECTED" : (connected ? "PAIRING" : "ADVERTISING"));

    // Compact Joystick2 status badge between the BLE label and battery.
    // Filled red means available; an outlined muted badge means absent/off.
    if (joystickConnected) {
        display.fillCircle(90, 9, 6, kRed);
        display.setTextColor(TFT_WHITE, kRed);
    } else {
        display.drawCircle(90, 9, 6, kBorder);
        display.setTextColor(kMuted, kBackground);
    }
    display.setTextDatum(middle_center);
    display.drawString("J", 90, 9);
    display.setTextDatum(top_left);

    const int battery = M5.Power.getBatteryLevel();
    display.drawRoundRect(98, 3, 31, 13, 6, kBorder);
    display.setTextColor(kMuted, kBackground);
    display.setCursor(102, 5);
    if (battery >= 0) {
        display.printf("%d%%", battery);
    } else {
        display.print("--%");
    }
}

void drawLaptopIcon(int x, int y, uint16_t color) {
    auto &display = M5.Display;
    display.drawRoundRect(x, y, 28, 18, 3, color);
    display.fillRect(x - 3, y + 20, 34, 3, color);
    display.fillRect(x + 9, y + 18, 10, 3, color);
}

void drawCameraIcon(int x, int y, uint16_t color) {
    auto &display = M5.Display;
    display.drawRoundRect(x, y + 5, 31, 21, 4, color);
    display.fillRect(x + 6, y + 2, 11, 5, color);
    display.drawCircle(x + 16, y + 15, 7, color);
    display.fillCircle(x + 16, y + 15, 3, color);
}

void drawBookIcon(int x, int y, uint16_t color) {
    auto &display = M5.Display;
    display.drawRoundRect(x, y, 15, 27, 3, color);
    display.drawRoundRect(x + 16, y, 15, 27, 3, color);
    display.drawFastVLine(x + 15, y + 3, 22, color);
}

void drawIrIcon(int x, int y, uint16_t color) {
    auto &display = M5.Display;
    display.fillCircle(x + 4, y + 14, 4, color);
    display.drawArc(x + 5, y + 14, 12, 10, 300, 60, color);
    display.drawArc(x + 5, y + 14, 20, 18, 300, 60, color);
}

void drawModeIcon(int mode, int x, int y, uint16_t color) {
    switch (mode) {
    case 0: drawLaptopIcon(x, y + 3, color); break;
    case 1: drawCameraIcon(x, y, color); break;
    case 2: drawBookIcon(x, y, color); break;
    case 3: drawIrIcon(x, y, color); break;
    default: break;
    }
}

void drawFooter(const char *left, const char *right) {
    auto &display = M5.Display;
    display.drawFastHLine(7, 214, 121, kBorder);
    useSmallFont();
    display.setTextColor(kMuted, kBackground);
    display.setCursor(8, 224);
    display.print(left);
    const int width = display.textWidth(right);
    display.setCursor(127 - width, 224);
    display.print(right);
}

void drawFittedLine(const char *text, int x, int y, int max_width,
                    uint16_t foreground, uint16_t background) {
    auto &display = M5.Display;
    char fitted[40] = {0};
    snprintf(fitted, sizeof(fitted), "%s", text ? text : "");

    if (display.textWidth(fitted) > max_width) {
        const int dots_width = display.textWidth("..");
        size_t length = std::strlen(fitted);
        while (length > 0 && display.textWidth(fitted) > max_width - dots_width) {
            fitted[--length] = '\0';
        }
        if (length + 2 < sizeof(fitted)) {
            fitted[length++] = '.';
            fitted[length++] = '.';
            fitted[length] = '\0';
        }
    }

    // The clip rectangle is a final guard: text can never paint over the
    // rounded edge even if a future label is wider than expected.
    display.setClipRect(x, y, max_width, 10);
    display.setTextColor(foreground, background);
    display.setCursor(x, y);
    display.print(fitted);
    display.clearClipRect();
}

void drawSelect(int mode) {
    auto &display = M5.Display;
    useTitleFont();
    display.setTextColor(kText, kBackground);
    display.setTextDatum(top_center);
    display.drawString("CHOOSE", 67, 27);
    display.setTextDatum(top_left);

    display.fillRoundRect(8, 55, 119, 146, 12, kSurface);
    display.drawRoundRect(8, 55, 119, 146, 12, kCyanDark);
    display.fillRoundRect(39, 68, 57, 57, 12, kSurfaceRaised);
    drawModeIcon(mode, 52, 82, mode == 3 ? kAmber : kCyan);

    useTitleFont();
    display.setTextColor(mode == 3 ? kAmber : kCyan, kSurface);
    display.setTextDatum(top_center);
    display.drawString(modeName(mode), 67, 135);
    useSmallFont();
    display.setTextColor(kMuted, kSurface);
    display.drawString(modeSubtitle(mode), 67, 163);

    for (int i = 0; i < 4; ++i) {
        display.fillCircle(49 + i * 12, 187, i == mode ? 3 : 2,
                           i == mode ? kCyan : kBorder);
    }
    display.setTextDatum(top_left);
    drawFooter("A NEXT", "B SELECT");
}

void drawActionCard(int y, const char *key, const char *action, const char *detail) {
    auto &display = M5.Display;
    display.fillRoundRect(7, y, 121, 45, 10, kSurface);
    display.drawRoundRect(7, y, 121, 45, 10, kBorder);
    display.fillCircle(27, y + 22, 12, kCyanDark);
    if (std::strlen(key) > 1) {
        useSmallFont();
    } else {
        useTitleFont();
    }
    display.setTextColor(TFT_WHITE, kCyanDark);
    display.setTextDatum(middle_center);
    display.drawString(key, 27, y + 22);
    display.setTextDatum(top_left);

    useSmallFont();
    constexpr int kTextX = 44;
    constexpr int kTextWidth = 79;
    drawFittedLine(action, kTextX, y + 10, kTextWidth, kText, kSurface);
    drawFittedLine(detail, kTextX, y + 27, kTextWidth, kMuted, kSurface);
}

void drawActive(int mode, int reader_mapping, const char *last_action) {
    auto &display = M5.Display;
    useTitleFont();
    display.setTextColor(mode == 3 ? kAmber : kCyan, kBackground);
    display.setTextDatum(top_center);
    display.drawString(modeName(mode), 67, 27);
    display.setTextDatum(top_left);

    useSmallFont();
    display.setTextColor(kMuted, kBackground);
    const char *badge = mode == 2 ? readerMapName(reader_mapping) : last_action;
    if (badge && badge[0]) {
        char clipped[20] = {0};
        snprintf(clipped, sizeof(clipped), "%.19s", badge);
        display.setTextDatum(top_center);
        display.drawString(clipped, 67, 54);
        display.setTextDatum(top_left);
    }

    switch (mode) {
    case 0:
        if (joystickConnected) {
            drawActionCard(75, "J", "VOL / SEEK", "CLICK: PLAY");
            drawActionCard(128, "A/B", "VOLUME - / +", "HOLD B: LOCK");
        } else {
            drawActionCard(75, "A", "VOLUME DOWN", "HOLD: MUTE");
            drawActionCard(128, "B", "VOLUME UP", "HOLD: LOCK");
        }
        break;
    case 1:
        if (joystickConnected) {
            drawActionCard(75, "J", "SHUTTER", "CLICK / UP-DN");
            drawActionCard(128, "A/B", "SHUTTER", "VOLUME + / -");
        } else {
            drawActionCard(75, "A", "SHUTTER", "VOLUME +");
            drawActionCard(128, "B", "SHUTTER", "VOLUME -");
        }
        break;
    case 2:
        if (joystickConnected) {
            drawActionCard(75, "J", "TURN PAGE", "LEFT / RIGHT");
            drawActionCard(128, "A/B", "PREV / NEXT", readerMapName(reader_mapping));
        } else {
            drawActionCard(75, "A", "PREVIOUS PAGE", "HOLD: MAP");
            drawActionCard(128, "B", "NEXT PAGE", readerMapName(reader_mapping));
        }
        break;
    case 3:
        display.fillRoundRect(7, 75, 121, 98, 10, kSurface);
        display.drawRoundRect(7, 75, 121, 98, 10, kBorder);
        drawIrIcon(51, 89, kAmber);
        useTitleFont();
        display.setTextColor(kText, kSurface);
        display.setTextDatum(top_center);
        display.drawString("RESERVED", 67, 128);
        useSmallFont();
        display.setTextColor(kMuted, kSurface);
        display.drawString("IR LEARNING", 67, 157);
        display.setTextDatum(top_left);
        break;
    default:
        break;
    }
    drawFooter("HOLD A+B", "PROFILES");
}

}  // namespace

extern "C" void stick_s3_ui_init(void) {
    auto config = M5.config();
    // Keep 5V on during board/display initialization so the Joystick2 MCU has
    // enough time to boot before its first I2C probe. It is switched off below
    // when no module is found, and after the normal idle timeout.
    config.output_power = true;
    config.internal_imu = true;
    config.internal_rtc = false;
    config.internal_mic = false;
    config.internal_spk = false;
    M5.begin(config);

    M5.BtnA.setHoldThresh(kHoldMs);
    M5.BtnB.setHoldThresh(kHoldMs);
    M5.Display.setBrightness(kDisplayBrightness);
    M5.Display.setRotation(0);
    M5.Display.setTextDatum(top_left);
    M5.Display.clear(kBackground);
    lastUserActivityMs = lgfx::millis();
    lastPowerLogMs = lastUserActivityMs;
    motionSensorReady = configureMotionSensor();
    initJoystick();
}

extern "C" stick_s3_event_t stick_s3_ui_poll(void) {
    static bool home_latched = false;
    M5.update();

    const uint32_t now = lgfx::millis();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
        noteUserActivity();
    }

    if (displayAwake && now - lastUserActivityMs >= kDisplaySleepMs) {
        M5.Display.sleep();
        displayAwake = false;
        armMotionWake(now);
        ESP_LOGI(kPowerTag, "Display asleep after %u ms", kDisplaySleepMs);
    }

    if (joystickPowered && joystickDetected &&
        now - lastUserActivityMs >= kJoystickPowerSaveMs) {
        writeJoystickRgb(0x000000);
        M5.Power.setExtOutput(false);
        joystickPowered = false;
        joystickConnected = false;
        resetJoystickState();
        ESP_LOGI(kPowerTag,
                 "Joystick power off after %u ms; move device or press A/B to restore",
                 kJoystickPowerSaveMs);
    }

    if (pollMotionWake(now)) {
        return STICK_S3_EVENT_WAKE;
    }

    if (now - lastPowerLogMs >= kPowerLogMs) {
        lastPowerLogMs = now;
        ESP_LOGI(kPowerTag,
                 "battery=%d%% voltage=%dmV display=%s joystick_power=%s",
                 M5.Power.getBatteryLevel(), M5.Power.getBatteryVoltage(),
                 displayAwake ? "on" : "off",
                 joystickPowered ? "on" : "off");
    }

    const bool both_pressed = M5.BtnA.isPressed() && M5.BtnB.isPressed();
    if (both_pressed && M5.BtnA.pressedFor(kHomeHoldMs) &&
        M5.BtnB.pressedFor(kHomeHoldMs) && !home_latched) {
        home_latched = true;
        return STICK_S3_EVENT_HOME;
    }
    if (home_latched) {
        if (M5.BtnA.isReleased() && M5.BtnB.isReleased()) {
            home_latched = false;
        }
        return STICK_S3_EVENT_NONE;
    }
    if (both_pressed) {
        return STICK_S3_EVENT_NONE;
    }

    if (M5.BtnA.wasHold()) return STICK_S3_EVENT_HOLD_A;
    if (M5.BtnB.wasHold()) return STICK_S3_EVENT_HOLD_B;
    if (M5.BtnA.wasClicked()) return STICK_S3_EVENT_BUTTON_A;
    if (M5.BtnB.wasClicked()) return STICK_S3_EVENT_BUTTON_B;
    return pollJoystick();
}

extern "C" bool stick_s3_joystick_connected(void) {
    return joystickConnected;
}

extern "C" void stick_s3_ui_draw(bool connected, bool ready, int mode, int view,
                                  int reader_mapping, const char *last_action) {
    auto &display = M5.Display;
    display.startWrite();
    display.fillScreen(kBackground);
    display.setTextDatum(top_left);
    drawStatus(connected, ready);

    if (view == STICK_S3_VIEW_SELECT) {
        drawSelect(mode);
    } else {
        drawActive(mode, reader_mapping, last_action);
    }
    display.endWrite();
}
