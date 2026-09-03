#include "stick_s3_ui.h"

#include <M5Unified.h>
#include <esp_log.h>

#include <cstring>

namespace {

constexpr uint32_t kHoldMs = 800;
constexpr uint32_t kHomeHoldMs = 900;
constexpr uint32_t kJoystickI2cHz = 400000;
constexpr uint8_t kJoystickAddress = 0x63;
constexpr uint8_t kJoystickOffsetRegister = 0x50;
constexpr uint8_t kJoystickButtonRegister = 0x20;
constexpr uint8_t kJoystickRgbRegister = 0x30;
constexpr int16_t kJoystickEnterThreshold = 700;
constexpr int16_t kJoystickReleaseThreshold = 350;
constexpr uint32_t kJoystickHoldMs = 850;
constexpr uint32_t kJoystickPollMs = 20;

const char *const kJoystickTag = "Joystick2";

bool joystickConnected = false;
int16_t joystickX = 0;
int16_t joystickY = 0;
bool joystickButtonPressed = false;
bool joystickButtonWasPressed = false;
bool joystickButtonHeld = false;
uint32_t joystickButtonDownMs = 0;
uint32_t joystickLastPollMs = 0;
stick_s3_event_t joystickDirection = STICK_S3_EVENT_NONE;

constexpr uint16_t kBackground = 0x0861;
constexpr uint16_t kSurface = 0x10E4;
constexpr uint16_t kSurfaceRaised = 0x1946;
constexpr uint16_t kBorder = 0x326A;
constexpr uint16_t kCyan = 0x2E9F;
constexpr uint16_t kCyanDark = 0x04B3;
constexpr uint16_t kGreen = 0x4EEB;
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

void initJoystick() {
    M5.Power.setExtOutput(true);
    M5.delay(100);

    if (!M5.Ex_I2C.begin()) {
        ESP_LOGE(kJoystickTag, "Could not start external I2C (SDA=%d SCL=%d)",
                 M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());
        return;
    }

    joystickConnected = M5.Ex_I2C.scanID(kJoystickAddress, 100000);
    if (!joystickConnected) {
        ESP_LOGW(kJoystickTag, "Not found at 0x%02X (SDA=%d SCL=%d)",
                 kJoystickAddress, M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());
        M5.Power.setExtOutput(false);
        return;
    }

    uint8_t firmware = 0;
    uint8_t bootloader = 0;
    readJoystickRegister(0xFE, &firmware, 1);
    readJoystickRegister(0xFC, &bootloader, 1);
    writeJoystickRgb(0x002000);
    ESP_LOGI(kJoystickTag,
             "Detected at 0x%02X on SDA=%d SCL=%d, firmware=%u bootloader=%u",
             kJoystickAddress, M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL(), firmware,
             bootloader);
}

stick_s3_event_t pollJoystick() {
    if (!joystickConnected) return STICK_S3_EVENT_NONE;

    const uint32_t now = lgfx::millis();
    if (now - joystickLastPollMs < kJoystickPollMs) {
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

    const int16_t absX = joystickX < 0 ? -joystickX : joystickX;
    const int16_t absY = joystickY < 0 ? -joystickY : joystickY;
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
            drawActionCard(75, "J", "VOL / TRACK", "CLICK: PLAY");
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
    M5.begin(config);

    M5.BtnA.setHoldThresh(kHoldMs);
    M5.BtnB.setHoldThresh(kHoldMs);
    M5.Display.setBrightness(144);
    M5.Display.setRotation(0);
    M5.Display.setTextDatum(top_left);
    M5.Display.clear(kBackground);
    initJoystick();
}

extern "C" stick_s3_event_t stick_s3_ui_poll(void) {
    static bool home_latched = false;
    M5.update();

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
