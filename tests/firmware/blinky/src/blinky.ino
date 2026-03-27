/**
 * Blinky PWM test — GPIO0
 * No libraries, no protocol, just raw PWM fade in/out.
 */

const uint8_t PIN = 0;

void setup() {
    pinMode(PIN, OUTPUT);
    analogWriteFreq(1000);
    analogWriteRange(255);
}

void loop() {
    // Fade up
    for (int i = 0; i <= 255; i += 5) {
        analogWrite(PIN, i);
        delay(10);
    }
    // Fade down
    for (int i = 255; i >= 0; i -= 5) {
        analogWrite(PIN, i);
        delay(10);
    }
}
