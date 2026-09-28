/* STAGE 1  -  ESP32 alone
   Wiring: USB cable only.
   Pass: blue onboard LED blinks, monitor prints on/off at 115200. */

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);              // GPIO2 = onboard blue LED
  delay(1000);
  Serial.println("STAGE 1: ESP32 alive");
}

void loop() {
  digitalWrite(2, HIGH); Serial.println("on");  delay(500);
  digitalWrite(2, LOW);  Serial.println("off"); delay(500);
}
