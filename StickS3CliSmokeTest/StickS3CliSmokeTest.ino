// Minimal compile-only smoke test for the standalone Arduino CLI.
// This does not depend on Arduino IDE or third-party libraries.

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("StickS3 CLI smoke test started");
}

void loop() {
  Serial.println("alive");
  delay(1000);
}
