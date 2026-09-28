/* STAGE 4  -  Button, buzzer and LEDs
   Wiring: button GPIO4 to GND.  buzzer + to GPIO5.
           LEDs on GPIO18, 19, 21 each through a 220 ohm resistor.
   Pass: each LED lights in turn, the buzzer sounds, and a 2 second
   hold of the button prints HELD and gives two beeps. */

#define PIN_BUTTON   4
#define PIN_BUZZER   5
#define PIN_LED_PWR 18
#define PIN_LED_FIX 19
#define PIN_LED_NET 21
#define HOLD_MS   2000

uint32_t pressStart = 0;
bool wasDown = false;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_PWR, OUTPUT);
  pinMode(PIN_LED_FIX, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);

  Serial.println("STAGE 4: LED sweep, then buzzer, then hold the button.");
  int leds[] = {PIN_LED_PWR, PIN_LED_FIX, PIN_LED_NET};
  for (int r = 0; r < 2; r++)
    for (int i = 0; i < 3; i++) {
      digitalWrite(leds[i], HIGH); delay(350);
      digitalWrite(leds[i], LOW);
    }
  digitalWrite(PIN_BUZZER, HIGH); delay(300);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_PWR, HIGH);
}

void loop() {
  bool down = (digitalRead(PIN_BUTTON) == LOW);

  if (down && !wasDown) { pressStart = millis(); Serial.println("press"); }

  if (down && wasDown && millis() - pressStart >= HOLD_MS) {
    Serial.println("HELD -> this is where an alert would fire");
    for (int i = 0; i < 2; i++) {
      digitalWrite(PIN_BUZZER, HIGH); delay(120);
      digitalWrite(PIN_BUZZER, LOW);  delay(120);
    }
    while (digitalRead(PIN_BUTTON) == LOW) delay(50);
  }
  wasDown = down;
  delay(20);
}
