/* STAGE 3  -  GSM AT command console
   Wiring: GSM VCC -> boost output (NOT the ESP32).
           GSM GND -> common ground.
           GSM TX  -> GPIO26.   GSM RX -> GPIO27.
           2200uF capacitor soldered at the GSM power pins.
   Monitor at 115200. Set line ending to "Both NL & CR".
   Type AT commands and read the replies.
   Pass: AT returns OK, and an SMS actually arrives on your phone. */

HardwareSerial GSMser(1);

void setup() {
  Serial.begin(115200);
  GSMser.begin(9600, SERIAL_8N1, 26, 27);   // RX=26, TX=27
  delay(3000);
  Serial.println("STAGE 3: AT console ready.");
  Serial.println("Set line ending to Both NL & CR, then try these in order:");
  Serial.println("  AT              -> OK");
  Serial.println("  AT+CPIN?        -> +CPIN: READY");
  Serial.println("  AT+CSQ          -> first number above 10");
  Serial.println("  AT+CREG?        -> 0,1 or 0,5");
  Serial.println("  AT+CMGF=1       -> OK");
  Serial.println("  SMS +254712345678   (helper, sends a test message)");
}

void sendTestSMS(String number) {
  Serial.println("sending...");
  GSMser.println("AT+CMGF=1");  delay(500);
  GSMser.print("AT+CMGS=\""); GSMser.print(number); GSMser.println("\"");
  delay(800);
  GSMser.print("STAGE 3 test from the SOS tracker.");
  GSMser.write(26);            // Ctrl-Z terminates the message
  GSMser.println();
}

void loop() {
  while (GSMser.available()) Serial.write(GSMser.read());

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("SMS ")) sendTestSMS(line.substring(4));
    else                         GSMser.println(line);
  }
}
