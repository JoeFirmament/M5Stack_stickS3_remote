#include <M5Unified.h>

namespace {

uint32_t buttonACount = 0;
uint32_t buttonBCount = 0;

void drawStatus(const char* eventText, uint16_t eventColor) {
  auto& display = M5.Display;

  display.startWrite();
  display.clear(TFT_BLACK);
  display.setRotation(1);
  display.setTextDatum(top_left);

  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.println("StickS3 HW Test");

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 38);
  display.println("Standalone Arduino CLI: OK");
  display.println("USB serial: 115200");

  display.setTextSize(2);
  display.setCursor(8, 66);
  display.printf("A:%lu  B:%lu", static_cast<unsigned long>(buttonACount),
                 static_cast<unsigned long>(buttonBCount));

  display.setTextColor(eventColor, TFT_BLACK);
  display.setCursor(8, 98);
  display.println(eventText);
  display.endWrite();
}

void logEvent(const char* eventText) {
  Serial.printf("[%10lu ms] %s | A=%lu B=%lu\n", millis(), eventText,
                static_cast<unsigned long>(buttonACount),
                static_cast<unsigned long>(buttonBCount));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  auto config = M5.config();
  M5.begin(config);

  M5.Display.setBrightness(128);
  drawStatus("Press A or B", TFT_YELLOW);

  Serial.println();
  Serial.println("=== StickS3 hardware test ===");
  Serial.println("Press button A or B. The screen and serial counters should update.");
  Serial.printf("Display: %d x %d\n", M5.Display.width(), M5.Display.height());
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    ++buttonACount;
    drawStatus("Button A pressed", TFT_GREEN);
    logEvent("Button A pressed");
  }

  if (M5.BtnB.wasPressed()) {
    ++buttonBCount;
    drawStatus("Button B pressed", TFT_ORANGE);
    logEvent("Button B pressed");
  }

  delay(5);
}
