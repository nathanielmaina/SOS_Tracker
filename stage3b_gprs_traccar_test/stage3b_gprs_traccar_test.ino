/* STAGE 3b  -  GPRS and Traccar upload test
   Same wiring as Stage 3 (GSM on GPIO26/27, own power supply).
   Needs: a SIM with a DATA BUNDLE, and Stage 3 passed (SMS works).
   Monitor at 115200, line ending "Both NL & CR".

   Commands:
     IMEI                 prints the module IMEI (your Traccar device identifier)
     APN safaricom        sets the APN (Airtel Kenya uses: internet)
     SERVER demo.traccar.org   sets the Traccar host
     GPRS                 opens the data connection and prints the IP address
     SEND                 uploads a fixed test point in Nairobi
     SOS                  uploads the test point with alarm=sos
     anything else        passed straight to the modem as an AT command

   Pass: SEND prints "+HTTPACTION: 0,200" and the point appears on the
   Traccar map for the device whose identifier equals the IMEI. */

HardwareSerial GSMser(1);
String apn = "safaricom";
String server = "demo.traccar.org";
String imei = "";

bool waitFor(const char *expect, uint32_t ms, String *out = nullptr) {
  uint32_t t0 = millis(); String r = "";
  while (millis() - t0 < ms) {
    while (GSMser.available()) r += (char)GSMser.read();
    if (r.indexOf(expect) >= 0) { if (out) *out = r; Serial.print(r); return true; }
    if (r.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  if (out) *out = r;
  Serial.print(r); Serial.print(F("  <-- expected ")); Serial.println(expect);
  return false;
}

bool at(String cmd, const char *expect, uint32_t ms) {
  while (GSMser.available()) GSMser.read();
  Serial.print(F("> ")); Serial.println(cmd);
  GSMser.println(cmd);
  return waitFor(expect, ms);
}

void readImei() {
  String r;
  GSMser.println("AT+GSN");
  waitFor("OK", 3000, &r);
  imei = "";
  for (char c : r) if (isDigit(c)) imei += c;
  imei = imei.substring(0, 15);
  Serial.print(F("IMEI = ")); Serial.println(imei);
}

bool gprs() {
  at("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", "OK", 3000);
  at("AT+SAPBR=3,1,\"APN\",\"" + apn + "\"", "OK", 3000);
  at("AT+SAPBR=1,1", "OK", 30000);            // ERROR here is fine if already open
  return at("AT+SAPBR=2,1", "+SAPBR: 1,1", 5000);
}

void send(bool sos) {
  if (imei.length() < 15) readImei();
  if (!gprs()) { Serial.println(F("GPRS not attached. Check data bundle, APN, AT+CSQ.")); return; }
  String url = "http://" + server + ":5055/?id=" + imei +
               "&lat=-1.286389&lon=36.817223&valid=true&batt=80";
  if (sos) url += "&alarm=sos";
  at("AT+HTTPTERM", "OK", 1000);
  at("AT+HTTPINIT", "OK", 3000);
  at("AT+HTTPPARA=\"CID\",1", "OK", 2000);
  at("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 3000);
  at("AT+HTTPACTION=0", "+HTTPACTION:", 30000);
  at("AT+HTTPTERM", "OK", 2000);
  Serial.println(F("Look for +HTTPACTION: 0,200 above. 200 means the server accepted it."));
}

void setup() {
  Serial.begin(115200);
  GSMser.begin(9600, SERIAL_8N1, 26, 27);
  delay(3000);
  Serial.println(F("STAGE 3b: GPRS + Traccar test. Commands: IMEI APN SERVER GPRS SEND SOS"));
  at("AT", "OK", 2000);
  readImei();
}

void loop() {
  while (GSMser.available()) Serial.write(GSMser.read());
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  String up = line; up.toUpperCase();
  if (up == "IMEI") readImei();
  else if (up.startsWith("APN ")) { apn = line.substring(4); Serial.println("APN = " + apn); }
  else if (up.startsWith("SERVER ")) { server = line.substring(7); Serial.println("SERVER = " + server); }
  else if (up == "GPRS") gprs();
  else if (up == "SEND") send(false);
  else if (up == "SOS")  send(true);
  else if (line.length()) GSMser.println(line);
}
