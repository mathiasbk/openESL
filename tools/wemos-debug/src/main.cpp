// Debug UART bridge: tag UART TX (ZBS243 debug pad 4) -> D5 -> USB serial.
// RX only; the tag is never driven from this board.

#include <Arduino.h>
#include <SoftwareSerial.h>

static const uint8_t TAG_RX_PIN = D5;
static const uint32_t TAG_BAUD = 9600;
static const uint32_t USB_BAUD = 115200;

// TX pin -1: receive only
static SoftwareSerial tag(TAG_RX_PIN, -1);

void setup() {
  Serial.begin(USB_BAUD);
  tag.begin(TAG_BAUD);
  delay(100);
  Serial.println();
  Serial.println("[wemos-debug] listening on D5 at 9600 baud");
}

void loop() {
  while (tag.available()) {
    Serial.write(tag.read());
  }
  yield();
}
