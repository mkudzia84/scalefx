const uint8_t PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
const uint8_t NUM_PINS = sizeof(PINS) / sizeof(PINS[0]);

void setup() {
  for (uint8_t i = 0; i < NUM_PINS; i++) {
    pinMode(PINS[i], OUTPUT);
  }
}

void loop() {
  for (uint8_t i = 0; i < NUM_PINS; i++) {
    digitalWrite(PINS[i], HIGH);
  }
  delay(2000);
  for (uint8_t i = 0; i < NUM_PINS; i++) {
    digitalWrite(PINS[i], LOW);
  }
  delay(2000);
}
