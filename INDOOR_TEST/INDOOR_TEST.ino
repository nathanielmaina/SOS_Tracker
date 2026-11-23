/* INDOOR LOCATION TEST
   Sends the test commands automatically (no typing, no curly quotes).
   Same wiring as always. Serial Monitor 115200. Just wait about 2 minutes. */

HardwareSerial A9G(1);

void sendAT(const char *cmd, uint32_t waitMs) {
  Serial.print(F("\n>> ")); Serial.println(cmd);
  A9G.print(cmd); A9G.print("\r\n");
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    while (A9G.available()) {
      char c = A9G.read();
      if ((c >= 32 && c < 127) || c == '\n') Serial.write(c);   // hide radio noise
    }
  }
}

void setup() {
  Serial.begin(115200);
  A9G.begin(9600, SERIAL_8N1, 22, 23);
  delay(2000);
  Serial.println(F("\n=== INDOOR LOCATION TEST ==="));

  Serial.println(F("\n--- 1. Cell tower ---"));
  sendAT("AT+CCED=0,1", 3000);

  Serial.println(F("\n--- 2. Restart data with Google DNS ---"));
  sendAT("AT+CGACT=0,1", 5000);
  sendAT("AT+CGATT=1", 10000);
  sendAT("AT+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\"", 3000);
  sendAT("AT+CGDCONT=1,\"IP\",\"safaricom\"", 3000);
  sendAT("AT+CGACT=1,1", 15000);
  sendAT("AT+CGACT?", 3000);

  Serial.println(F("\n--- 3. DNS test (name, not number) ---"));
  sendAT("AT+HTTPGET=\"http://www.example.com\"", 20000);

  Serial.println(F("\n--- 4. Cell tower to Traccar ---"));
  sendAT("AT+HTTPGET=\"http://46.101.24.212:5055/?id=867959035242337&lat=0&lon=0&valid=false&timestamp=1793895600&cell=639,2,2271,4185\"", 20000);

  Serial.println(F("\n=== DONE. Copy everything above and send it. ==="));
}

void loop() {
  while (A9G.available()) { char c = A9G.read(); if ((c >= 32 && c < 127) || c == '\n') Serial.write(c); }
}