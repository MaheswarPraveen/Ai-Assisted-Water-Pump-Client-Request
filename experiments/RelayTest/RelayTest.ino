#define PIN_RELAY 26
// NPN transistor buffer now on this pin: GPIO HIGH -> transistor conducts ->
// relay IN pulled low -> active-LOW relay energizes. So HIGH = ON now.
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// Runs before setup() — shrinks the undefined boot window as much as
// software can (the pin is forced OFF as early as the app can act).
void __attribute__((constructor)) early_pin_safe() {
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);
}

void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);
  Serial.begin(115200);
}

void loop() {
  digitalWrite(PIN_RELAY, RELAY_ON);
  Serial.println("relay ON");
  delay(500);
  digitalWrite(PIN_RELAY, RELAY_OFF);
  Serial.println("relay OFF");
  delay(500);
}
