/* =====================================================================
   IoT-Based GPS Tracking System for Monitoring of Vulnerable Individuals
   Firmware v2.0  -  SMS alerts + live tracking to a Traccar server

   Board    : ESP32-WROOM-32 DevKit V1
   GPS      : u-blox NEO-7M      on UART2 (9600 baud)
   GSM      : SIM900A V4.0        on UART1 (9600 baud)  SMS, voice, GPRS
   Server   : Traccar, OsmAnd protocol, HTTP port 5055
   Storage  : contacts + settings in NVS flash (Preferences)
              latest fix in RAM only (no location history on the device)

   Functions map to the Level 1 data flow diagram (Figure 4.3):
     process1_acquireLocation()   Process 1  Acquire Location
     process2_storeLocation()     Process 2  Store Location
     process3_formatAlert()       Process 3  Format Alert
     process4_transmitAlert()     Process 4  Transmit Alert (4.1 - 4.4)
     process5_manageContacts()    Process 5  Manage Contacts
     process6_uploadPosition()    Process 6  Upload Position
   Process 7 (Record and Display Position) runs on the Traccar server.

   DESIGN RULE: the SMS alert never waits for GPRS or the server.
   SMS and calls go first; the SOS report to Traccar goes last.

   Library: TinyGPSPlus by Mikal Hart (Library Manager)
   ===================================================================== */

#include <TinyGPSPlus.h>
#include <Preferences.h>

/* ---------------------------------------------------------------- pins */
#define PIN_GPS_RX      16
#define PIN_GPS_TX      17
#define PIN_GSM_RX      26
#define PIN_GSM_TX      27
#define PIN_BUTTON       4
#define PIN_BUZZER       5
#define PIN_LED_PWR     18
#define PIN_LED_FIX     19
#define PIN_LED_NET     21
#define PIN_BATT        35      // 100k/100k divider from TP4056 OUT+

/* ------------------------------------------------------------- tunables */
#define MAX_CONTACTS          5
#define HOLD_MS            2000
#define FIX_MIN_SATS          4
#define FIX_MAX_HDOP        5.0
#define GSM_BOOT_MS       20000
#define ALERT_COOLDOWN_MS 60000
#define HTTP_TIMEOUT_MS   30000
#define TRACCAR_PORT       5055

/* ------------------------------------------------------------- globals */
HardwareSerial GPSser(2);
HardwareSerial GSMser(1);
TinyGPSPlus    gps;
Preferences    store;

struct Config {
  uint8_t  contactCount = 0;
  char     contact[MAX_CONTACTS][16] = {};
  bool     voiceCall      = true;
  uint16_t pollInterval   = 10;               // seconds between GPS reads
  uint16_t uploadInterval = 60;               // seconds between reports
  char     wearerName[24] = "the wearer";
  char     apn[32]        = "safaricom";      // Airtel Kenya: "internet"
  char     server[64]     = "demo.traccar.org";
} cfg;

struct Fix {
  double   lat = 0, lon = 0;
  uint8_t  sats = 0;
  float    hdop = 99.9, speedKn = 0;
  uint32_t capturedAt = 0;                    // millis() at capture
  uint32_t epoch = 0;                         // UTC seconds from GPS
  bool     valid = false;
} cachedFix;

char     imei[20]      = "000000000000000";
uint32_t lastPollMs    = 0;
uint32_t lastUploadMs  = 0;
uint32_t lastAlertMs   = 0;
uint32_t pressStartMs  = 0;
bool     buttonWasDown = false;
bool     gsmRegistered = false;

/* ================================================================== */
/*  helpers                                                            */
/* ================================================================== */
void buzz(int n, int on, int off) {
  for (int i = 0; i < n; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(on);
                                digitalWrite(PIN_BUZZER, LOW);  delay(off); }
}
void buzzSuccess() { buzz(2, 120, 120); }
void buzzFailure() { buzz(1, 900, 0); }

void feedGPS() { while (GPSser.available()) gps.encode(GPSser.read()); }

/* wait for a substring in the GSM output, keep feeding the GPS parser */
bool waitFor(const char *expect, uint32_t timeoutMs, String *out = nullptr) {
  uint32_t t0 = millis(); String r = "";
  while (millis() - t0 < timeoutMs) {
    while (GSMser.available()) r += (char)GSMser.read();
    feedGPS();
    if (r.indexOf(expect) >= 0) { if (out) *out = r; return true; }
    if (r.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  if (out) *out = r;
  Serial.print(F("[GSM] no '")); Serial.print(expect); Serial.print(F("' in: ")); Serial.println(r);
  return false;
}

bool at(const char *cmd, const char *expect, uint32_t timeoutMs) {
  while (GSMser.available()) GSMser.read();              // clear old output
  GSMser.println(cmd);
  Serial.print(F("[AT] ")); Serial.println(cmd);
  return waitFor(expect, timeoutMs);
}

/* days from 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
uint32_t toEpoch(int y, int m, int d, int hh, int mm, int ss) {
  y -= m <= 2;
  int era = y / 400; unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

/* battery percent from the divider on GPIO35 (linear 3.3 V .. 4.2 V) */
int batteryPercent() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BATT);
  float v = (mv / 8.0f) * 2.0f / 1000.0f;                // undo the 1:2 divider
  int pct = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);
  return constrain(pct, 0, 100);
}

/* ================================================================== */
/*  PROCESS 1  Acquire Location                                        */
/* ================================================================== */
bool process1_acquireLocation() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1200) feedGPS();                // one second of NMEA
  if (!gps.location.isValid() || !gps.satellites.isValid()) return false;
  if (gps.satellites.value() < FIX_MIN_SATS) return false;
  float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9;
  if (hdop > FIX_MAX_HDOP) return false;
  return true;
}

/* ================================================================== */
/*  PROCESS 2  Store Location   (RAM only)                             */
/* ================================================================== */
void process2_storeLocation() {
  cachedFix.lat        = gps.location.lat();
  cachedFix.lon        = gps.location.lng();
  cachedFix.sats       = gps.satellites.value();
  cachedFix.hdop       = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9;
  cachedFix.speedKn    = gps.speed.isValid() ? gps.speed.knots() : 0;
  cachedFix.capturedAt = millis();
  if (gps.date.isValid() && gps.time.isValid())
    cachedFix.epoch = toEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                              gps.time.hour(), gps.time.minute(), gps.time.second());
  cachedFix.valid = true;
}

/* ================================================================== */
/*  PROCESS 3  Format Alert   (160 characters, GSM-7)                  */
/* ================================================================== */
String process3_formatAlert() {
  String msg = "EMERGENCY: "; msg += cfg.wearerName; msg += " needs help. ";
  if (!cachedFix.valid) { msg += "No GPS position available. Call this number."; return msg; }
  char la[16], lo[16];
  dtostrf(cachedFix.lat, 0, 6, la); dtostrf(cachedFix.lon, 0, 6, lo);
  uint32_t age = (millis() - cachedFix.capturedAt) / 1000UL;
  msg += "Loc "; msg += la; msg += ","; msg += lo;
  msg += " maps.google.com/?q="; msg += la; msg += ","; msg += lo;
  msg += " Age "; msg += String(age); msg += "s";
  return msg.length() > 160 ? msg.substring(0, 160) : msg;
}

/* ================================================================== */
/*  PROCESS 4  Transmit Alert                                          */
/* ================================================================== */
bool gsm41_initialise() {
  if (!at("AT", "OK", 2000))          return false;
  at("ATE0", "OK", 2000);
  if (!at("AT+CPIN?", "READY", 5000)) return false;
  if (!at("AT+CMGF=1", "OK", 2000))   return false;
  uint32_t t0 = millis();
  while (millis() - t0 < GSM_BOOT_MS) {
    GSMser.println("AT+CREG?");
    String r;
    if (waitFor("+CREG:", 2000, &r) && (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0)) {
      gsmRegistered = true; digitalWrite(PIN_LED_NET, HIGH); return true;
    }
    delay(1500);
  }
  gsmRegistered = false; digitalWrite(PIN_LED_NET, LOW);
  return false;
}

bool gsm42_sendSMS(const char *number, const String &text) {
  while (GSMser.available()) GSMser.read();
  GSMser.print("AT+CMGS=\""); GSMser.print(number); GSMser.println("\"");
  if (!waitFor(">", 5000)) return false;
  GSMser.print(text); GSMser.write(26);                  // Ctrl-Z sends
  return waitFor("+CMGS", 30000);
}

bool gsm43_placeCall(const char *number) {
  String d = "ATD"; d += number; d += ";";
  if (!at(d.c_str(), "OK", 8000)) return false;
  uint32_t t0 = millis(); while (millis() - t0 < 20000) { feedGPS(); delay(10); }
  at("ATH", "OK", 5000);
  return true;
}

bool process6_uploadPosition(bool sosAlarm);            // defined below

void process4_transmitAlert(const String &msg) {
  if (!gsm41_initialise()) {                             // 4.1
    Serial.println(F("GSM not registered. Alert aborted."));
    buzzFailure(); return;
  }
  uint8_t sent = 0;
  for (uint8_t i = 0; i < cfg.contactCount; i++) {       // 4.2
    bool ok = gsm42_sendSMS(cfg.contact[i], msg);
    Serial.printf("DELIVERY sms  %s  %s\n", cfg.contact[i], ok ? "SENT" : "FAILED");
    if (ok) sent++;
    delay(1500);
  }
  if (cfg.voiceCall)                                     // 4.3
    for (uint8_t i = 0; i < cfg.contactCount; i++) {
      bool ok = gsm43_placeCall(cfg.contact[i]);
      Serial.printf("DELIVERY call %s  %s\n", cfg.contact[i], ok ? "SENT" : "FAILED");
    }
  if (sent > 0) buzzSuccess(); else buzzFailure();       // 4.4

  /* only after every SMS and call: raise the alarm on the server */
  bool up = process6_uploadPosition(true);
  Serial.println(up ? F("SOS alarm reported to Traccar") : F("SOS alarm upload failed (SMS already sent)"));
}

/* ================================================================== */
/*  PROCESS 6  Upload Position   (OsmAnd protocol over GPRS HTTP)      */
/* ================================================================== */
bool gprsAttach() {
  GSMser.println("AT+SAPBR=2,1");                        // bearer already open?
  String r;
  if (waitFor("+SAPBR:", 3000, &r) && r.indexOf("+SAPBR: 1,1") >= 0) return true;
  at("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", "OK", 3000);
  String apn = String("AT+SAPBR=3,1,\"APN\",\"") + cfg.apn + "\"";
  at(apn.c_str(), "OK", 3000);
  if (!at("AT+SAPBR=1,1", "OK", 30000)) return false;    // open bearer
  GSMser.println("AT+SAPBR=2,1");
  return waitFor("+SAPBR: 1,1", 5000);
}

bool process6_uploadPosition(bool sosAlarm) {
  if (!gsmRegistered && !gsm41_initialise()) return false;
  if (!gprsAttach()) { Serial.println(F("GPRS attach failed")); return false; }

  char la[16], lo[16];
  dtostrf(cachedFix.lat, 0, 6, la); dtostrf(cachedFix.lon, 0, 6, lo);
  String url = "http://"; url += cfg.server; url += ":"; url += TRACCAR_PORT;
  url += "/?id="; url += imei;
  if (cachedFix.valid) {
    url += "&lat="; url += la; url += "&lon="; url += lo;
    if (cachedFix.epoch) { url += "&timestamp="; url += cachedFix.epoch; }
    url += "&hdop=";  url += String(cachedFix.hdop, 1);
    url += "&speed="; url += String(cachedFix.speedKn, 1);
    url += "&valid=true";
  } else {
    url += "&valid=false";
  }
  url += "&batt="; url += batteryPercent();
  if (sosAlarm) url += "&alarm=sos";

  at("AT+HTTPTERM", "OK", 1000);                         // clear any stale session
  if (!at("AT+HTTPINIT", "OK", 3000)) return false;
  at("AT+HTTPPARA=\"CID\",1", "OK", 2000);
  String para = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
  if (!at(para.c_str(), "OK", 3000)) { at("AT+HTTPTERM", "OK", 2000); return false; }

  GSMser.println("AT+HTTPACTION=0");                     // 0 = GET
  String r;
  bool ok = waitFor("+HTTPACTION:", HTTP_TIMEOUT_MS, &r) && r.indexOf(",200,") >= 0;
  at("AT+HTTPTERM", "OK", 2000);
  Serial.print(F("UPLOAD ")); Serial.print(ok ? F("200 OK  ") : F("FAILED  ")); Serial.println(url);
  return ok;
}

/* ================================================================== */
/*  PROCESS 5  Manage Contacts and settings (serial menu)              */
/* ================================================================== */
bool validNumber(const String &n) {
  if (n.length() < 10 || n.length() > 16 || n[0] != '+') return false;
  for (uint8_t i = 1; i < n.length(); i++) if (!isDigit(n[i])) return false;
  return true;
}

String readLine() {
  String s = "";
  while (true) {
    while (!Serial.available()) { feedGPS(); delay(5); }
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (s.length()) return s; }
    else s += c;
  }
}

String ask(const __FlashStringHelper *prompt, const char *current) {
  Serial.print(prompt); Serial.print(F(" [")); Serial.print(current); Serial.print(F("]: "));
  String v = readLine(); v.trim();
  Serial.println(v);
  return (v == ".") ? String(current) : v;               // "." keeps current value
}

void saveConfig() {
  store.begin("sos", false);
  store.putUChar("count", cfg.contactCount);
  for (uint8_t i = 0; i < cfg.contactCount; i++) { char k[6]; sprintf(k, "c%u", i); store.putString(k, cfg.contact[i]); }
  store.putBool("voice", cfg.voiceCall);
  store.putUShort("poll", cfg.pollInterval);
  store.putUShort("upl", cfg.uploadInterval);
  store.putString("name", cfg.wearerName);
  store.putString("apn", cfg.apn);
  store.putString("srv", cfg.server);
  store.end();
}

void loadConfig() {
  store.begin("sos", true);
  cfg.contactCount = store.getUChar("count", 0);
  for (uint8_t i = 0; i < cfg.contactCount && i < MAX_CONTACTS; i++) { char k[6]; sprintf(k, "c%u", i); store.getString(k, cfg.contact[i], 16); }
  cfg.voiceCall      = store.getBool("voice", true);
  cfg.pollInterval   = store.getUShort("poll", 10);
  cfg.uploadInterval = store.getUShort("upl", 60);
  store.getString("name", cfg.wearerName, sizeof(cfg.wearerName));
  if (store.isKey("apn")) store.getString("apn", cfg.apn, sizeof(cfg.apn));
  if (store.isKey("srv")) store.getString("srv", cfg.server, sizeof(cfg.server));
  store.end();
}

void process5_manageContacts() {
  Serial.println(F("\n=== SOS TRACKER SETUP ===   (type . to keep the value in brackets)"));
  Serial.print(F("firmware v2.0   IMEI ")); Serial.println(imei);

  String v = ask(F("> wearer first name"), cfg.wearerName);
  v.toCharArray(cfg.wearerName, sizeof(cfg.wearerName));

  cfg.contactCount = 0;
  for (uint8_t i = 0; i < MAX_CONTACTS; i++) {
    Serial.printf("> contact %u number, +254... (blank line or . to finish): ", i + 1);
    String n = readLine(); n.trim(); Serial.println(n);
    if (n == "." || n.length() == 0) break;
    if (!validNumber(n)) { Serial.println(F("  [ERR] use international format, e.g. +254712345678")); i--; continue; }
    n.toCharArray(cfg.contact[cfg.contactCount], 16);
    Serial.printf("  [OK] format valid  priority %u\n", cfg.contactCount + 1);
    cfg.contactCount++;
  }

  v = ask(F("> voice call enabled? (y/n)"), cfg.voiceCall ? "y" : "n");
  cfg.voiceCall = (v[0] == 'y' || v[0] == 'Y');
  char buf[8];
  sprintf(buf, "%u", cfg.pollInterval);   v = ask(F("> GPS poll interval (sec)"), buf);
  cfg.pollInterval = max(5, (int)v.toInt());
  sprintf(buf, "%u", cfg.uploadInterval); v = ask(F("> upload interval (sec)"), buf);
  cfg.uploadInterval = max(15, (int)v.toInt());
  v = ask(F("> mobile data APN"), cfg.apn);                 v.toCharArray(cfg.apn, sizeof(cfg.apn));
  v = ask(F("> Traccar server address"), cfg.server);       v.toCharArray(cfg.server, sizeof(cfg.server));

  saveConfig();
  Serial.printf("  [OK] %u contacts written\n  [OK] settings written\n", cfg.contactCount);
  Serial.print(F("  Register this device in Traccar with identifier: ")); Serial.println(imei);
  Serial.println(F("setup complete. device armed."));
  buzzSuccess();
}

/* ================================================================== */
/*  setup and loop                                                     */
/* ================================================================== */
void readIMEI() {
  while (GSMser.available()) GSMser.read();
  GSMser.println("AT+GSN");
  String r;
  if (waitFor("OK", 3000, &r)) {
    String digits = "";
    for (char c : r) if (isDigit(c)) digits += c;
    if (digits.length() >= 15) digits.substring(0, 15).toCharArray(imei, sizeof(imei));
  }
}

void setup() {
  Serial.begin(115200);
  GPSser.setRxBufferSize(1024);                          // survive long GSM waits
  GPSser.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  GSMser.begin(9600, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);
  analogSetPinAttenuation(PIN_BATT, ADC_11db);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_PWR, OUTPUT); pinMode(PIN_LED_FIX, OUTPUT); pinMode(PIN_LED_NET, OUTPUT);
  digitalWrite(PIN_LED_PWR, HIGH);

  Serial.println(F("\nSOS TRACKER v2 booting"));
  delay(3000);                                           // let the modem start
  loadConfig();
  gsm41_initialise();
  readIMEI();
  Serial.print(F("IMEI ")); Serial.println(imei);

  if (cfg.contactCount == 0) { Serial.println(F("No contacts stored. Opening setup.")); process5_manageContacts(); }
  else Serial.println(F("Contacts loaded. Commands: SETUP  TEST  UPLOAD  STATUS"));
}

void loop() {
  feedGPS();

  /* background: acquire and cache */
  if (millis() - lastPollMs >= (uint32_t)cfg.pollInterval * 1000UL) {
    lastPollMs = millis();
    if (process1_acquireLocation()) {
      process2_storeLocation();
      digitalWrite(PIN_LED_FIX, HIGH);
      Serial.printf("FIX  %.6f, %.6f  sats %u  hdop %.1f\n", cachedFix.lat, cachedFix.lon, cachedFix.sats, cachedFix.hdop);
    } else {
      digitalWrite(PIN_LED_FIX, LOW);
      Serial.println(F("no valid fix this cycle"));
    }
  }

  /* background: live position upload */
  if (cachedFix.valid && millis() - lastUploadMs >= (uint32_t)cfg.uploadInterval * 1000UL) {
    lastUploadMs = millis();
    process6_uploadPosition(false);
  }

  /* SOS button: press and hold */
  bool down = (digitalRead(PIN_BUTTON) == LOW);
  if (down && !buttonWasDown) pressStartMs = millis();
  if (down && buttonWasDown && millis() - pressStartMs >= HOLD_MS) {
    if (millis() - lastAlertMs > ALERT_COOLDOWN_MS) {
      lastAlertMs = millis();
      Serial.println(F("SOS HELD. Raising alert."));
      String msg = process3_formatAlert();
      Serial.println(msg);
      process4_transmitAlert(msg);
    }
    while (digitalRead(PIN_BUTTON) == LOW) delay(50);
  }
  buttonWasDown = down;

  /* serial commands */
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();
    if (cmd == "SETUP")  process5_manageContacts();
    if (cmd == "TEST")   { String m = process3_formatAlert(); Serial.println(m); process4_transmitAlert(m); }
    if (cmd == "UPLOAD") process6_uploadPosition(false);
    if (cmd == "STATUS") Serial.printf("IMEI %s  fix %s  battery %d%%  server %s  apn %s\n",
                                       imei, cachedFix.valid ? "yes" : "no", batteryPercent(), cfg.server, cfg.apn);
  }
  delay(10);
}
