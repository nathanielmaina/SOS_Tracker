/* =====================================================================
   IoT-Based GPS Tracking System for Monitoring of Vulnerable Individuals

   Board    : ESP32-WROOM-32 DevKit V1
   GPS      : u-blox NEO-7M  on UART2  (9600 baud)
   GSM      : SIM900A / SIM800L on UART1 (9600 baud)
   Storage  : NVS flash via Preferences  (the ESP32 equivalent of EEPROM)

   The five top-level functions map one to one onto the processes on the
   Level 1 data flow diagram:

     process1_acquireLocation()   Process 1  Acquire Location
     process2_storeLocation()     Process 2  Store Location
     process3_formatAlert()       Process 3  Format Alert
     process4_transmitAlert()     Process 4  Transmit Alert
     process5_manageContacts()    Process 5  Manage Contacts

   Libraries to install in the Arduino IDE Library Manager:
     TinyGPSPlus   by Mikal Hart
   Preferences and HardwareSerial ship with the ESP32 core.
   ===================================================================== */

#include <TinyGPSPlus.h>
#include <Preferences.h>

/* ---------------------------------------------------------------- pins */
#define PIN_GPS_RX      16      // ESP32 receives  <- GPS TX
#define PIN_GPS_TX      17      // ESP32 transmits -> GPS RX (unused by NEO-7M)
#define PIN_GSM_RX      26      // ESP32 receives  <- GSM TX
#define PIN_GSM_TX      27      // ESP32 transmits -> GSM RX
#define PIN_BUTTON       4      // SOS button, other leg to GND
#define PIN_BUZZER       5      // active buzzer, + leg here
#define PIN_LED_PWR     18      // green
#define PIN_LED_FIX     19      // blue
#define PIN_LED_NET     21      // amber

/* ------------------------------------------------------------- tunables */
#define MAX_CONTACTS         5
#define HOLD_MS           2000  // how long the button must be held
#define FIX_MIN_SATS         4  // below 4 a 3D fix is impossible
#define FIX_MAX_HDOP       5.0  // above 5 the satellite geometry is poor
#define GSM_BOOT_MS      15000  // time allowed for network registration
#define ALERT_COOLDOWN_MS 60000 // ignore repeat presses inside this window

/* ------------------------------------------------------------- globals */
HardwareSerial GPSser(2);
HardwareSerial GSMser(1);
TinyGPSPlus    gps;
Preferences    store;

struct Config {
  uint8_t contactCount = 0;
  char    contact[MAX_CONTACTS][16] = {};
  bool    voiceCall     = true;
  uint16_t pollInterval = 10;          // seconds
  char    wearerName[24] = "the wearer";
} cfg;

struct Fix {                            // data store D1, Cached Fix
  double   lat = 0, lon = 0;
  uint8_t  sats = 0;
  float    hdop = 99.9;
  uint32_t capturedAt = 0;              // millis() at capture
  bool     valid = false;
} cachedFix;

uint32_t lastPollMs   = 0;
uint32_t lastAlertMs  = 0;
uint32_t pressStartMs = 0;
bool     buttonWasDown = false;
bool     gsmRegistered = false;

/* ================================================================== */
/*  Small helpers                                                      */
/* ================================================================== */

void buzz(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(onMs);
    digitalWrite(PIN_BUZZER, LOW);  delay(offMs);
  }
}

void buzzSuccess() { buzz(2, 120, 120); }          // two short beeps
void buzzFailure() { buzz(1, 900,   0); }          // one long beep

/* Send an AT command and wait for an expected substring in the reply.
   Returns true if the expected text arrived before the timeout.        */
bool atCommand(const char *cmd, const char *expect, uint32_t timeoutMs) {
  GSMser.println(cmd);
  Serial.print(F("[AT] ")); Serial.println(cmd);

  uint32_t start = millis();
  String   reply = "";
  while (millis() - start < timeoutMs) {
    while (GSMser.available()) reply += (char)GSMser.read();
    if (reply.indexOf(expect) >= 0) {
      Serial.print(F("[OK] ")); Serial.println(reply);
      return true;
    }
    if (reply.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  Serial.print(F("[ERR] ")); Serial.println(reply);
  return false;
}

/* ================================================================== */
/*  PROCESS 1   Acquire Location                                       */
/*  In  : GPS satellite signal        Out : valid coordinates          */
/* ================================================================== */
bool process1_acquireLocation(double &lat, double &lon,
                              uint8_t &sats, float &hdop) {
  uint32_t start = millis();
  while (millis() - start < 1200) {           // drain one second of NMEA
    while (GPSser.available()) gps.encode(GPSser.read());
  }

  if (!gps.location.isValid())  return false;
  if (!gps.satellites.isValid()) return false;

  sats = gps.satellites.value();
  hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9;

  if (sats < FIX_MIN_SATS)  return false;     // validity test 1
  if (hdop > FIX_MAX_HDOP)  return false;     // validity test 2

  lat = gps.location.lat();
  lon = gps.location.lng();
  return true;
}

/* ================================================================== */
/*  PROCESS 2   Store Location                                         */
/*  In  : valid coordinates           Out : cached fix record into D1  */
/* ================================================================== */
void process2_storeLocation(double lat, double lon, uint8_t sats, float hdop) {
  cachedFix.lat        = lat;
  cachedFix.lon        = lon;
  cachedFix.sats       = sats;
  cachedFix.hdop       = hdop;
  cachedFix.capturedAt = millis();
  cachedFix.valid      = true;

  /* The fix stays in RAM only. It is overwritten on every valid reading and
     erased at power-off, so the device keeps no location history
     (Section 3.5 and Table 4.2 of the project document).                */
}

/* ================================================================== */
/*  PROCESS 3   Format Alert                                           */
/*  In  : SOS press + last known fix   Out : alert message             */
/*  One SMS holds 160 characters in the GSM-7 alphabet. Stay under it. */
/* ================================================================== */
String process3_formatAlert() {
  String msg = "EMERGENCY: ";
  msg += cfg.wearerName;
  msg += " needs help. ";

  if (!cachedFix.valid) {
    msg += "No GPS position available. Call this number.";
    return msg;
  }

  uint32_t ageSec = (millis() - cachedFix.capturedAt) / 1000UL;

  char lat[16], lon[16];
  dtostrf(cachedFix.lat, 0, 6, lat);        // 6 decimals is about 0.11 m
  dtostrf(cachedFix.lon, 0, 6, lon);

  msg += "Loc ";
  msg += lat; msg += ","; msg += lon;
  msg += " maps.google.com/?q=";
  msg += lat; msg += ","; msg += lon;
  msg += " Age ";
  msg += String(ageSec);
  msg += "s";

  if (msg.length() > 160) msg = msg.substring(0, 160);
  return msg;
}

/* ================================================================== */
/*  PROCESS 4   Transmit Alert                                         */
/*  4.1 initialise GSM   4.2 send SMS   4.3 place call   4.4 feedback  */
/* ================================================================== */

/* -- 4.1 ----------------------------------------------------------- */
bool gsm41_initialise() {
  if (!atCommand("AT", "OK", 2000))            return false;
  if (!atCommand("ATE0", "OK", 2000))          return false;   // echo off
  if (!atCommand("AT+CPIN?", "READY", 5000))   return false;   // SIM unlocked
  if (!atCommand("AT+CMGF=1", "OK", 2000))     return false;   // SMS text mode

  uint32_t start = millis();
  while (millis() - start < GSM_BOOT_MS) {     // wait for registration
    if (atCommand("AT+CREG?", "0,1", 2000)) { gsmRegistered = true; return true; }
    if (atCommand("AT+CREG?", "0,5", 2000)) { gsmRegistered = true; return true; }
    delay(1500);
  }
  gsmRegistered = false;
  return false;
}

/* -- 4.2 ----------------------------------------------------------- */
bool gsm42_sendSMS(const char *number, const String &text) {
  GSMser.print("AT+CMGS=\"");
  GSMser.print(number);
  GSMser.println("\"");
  delay(600);                                   // wait for the > prompt

  GSMser.print(text);
  GSMser.write(26);                             // Ctrl-Z ends the message
  GSMser.println();

  uint32_t start = millis();
  String   reply = "";
  while (millis() - start < 20000) {            // network can be slow
    while (GSMser.available()) reply += (char)GSMser.read();
    if (reply.indexOf("+CMGS") >= 0) return true;
    if (reply.indexOf("ERROR") >= 0) return false;
    delay(20);
  }
  return false;
}

/* -- 4.3 ----------------------------------------------------------- */
bool gsm43_placeCall(const char *number) {
  String dial = "ATD";
  dial += number;
  dial += ";";                                  // the semicolon means voice
  if (!atCommand(dial.c_str(), "OK", 8000)) return false;
  delay(20000);                                 // let it ring
  atCommand("ATH", "OK", 5000);                 // hang up
  return true;
}

/* -- 4.4 and the whole of process 4 -------------------------------- */
void process4_transmitAlert(const String &msg) {
  digitalWrite(PIN_LED_NET, LOW);

  if (!gsm41_initialise()) {                    // 4.1
    Serial.println(F("GSM not registered. Alert aborted."));
    buzzFailure();                              // 4.4 failure pattern
    return;
  }
  digitalWrite(PIN_LED_NET, HIGH);

  uint8_t sent = 0;
  for (uint8_t i = 0; i < cfg.contactCount; i++) {   // 4.2, one per contact
    bool ok = gsm42_sendSMS(cfg.contact[i], msg);
    Serial.print(F("SMS to ")); Serial.print(cfg.contact[i]);
    Serial.println(ok ? F("  SENT") : F("  FAILED"));
    if (ok) sent++;
    delay(1500);
  }

  if (cfg.voiceCall) {                               // 4.3, opt fragment
    for (uint8_t i = 0; i < cfg.contactCount; i++) {
      gsm43_placeCall(cfg.contact[i]);
      delay(1000);
    }
  }

  if (sent > 0) buzzSuccess();                       // 4.4 feedback
  else          buzzFailure();
}

/* ================================================================== */
/*  PROCESS 5   Manage Contacts                                        */
/*  A serial menu. Type SETUP in the monitor at any time to open it.   */
/* ================================================================== */
bool validNumber(const String &n) {
  if (n.length() < 10 || n.length() > 15) return false;
  uint8_t i = (n[0] == '+') ? 1 : 0;
  if (i == 0) return false;                      // require E.164 with +
  for (; i < n.length(); i++) if (!isDigit(n[i])) return false;
  return true;
}

String readLine() {
  String s = "";
  while (true) {
    while (!Serial.available()) delay(10);
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (s.length()) return s; }
    else s += c;
  }
}

void process5_manageContacts() {
  Serial.println();
  Serial.println(F("=== SOS TRACKER SETUP ==="));
  Serial.println(F("firmware v1.0   device_id 001"));

  Serial.print(F("> wearer first name: "));
  String name = readLine();
  name.toCharArray(cfg.wearerName, sizeof(cfg.wearerName));
  Serial.println(name);

  cfg.contactCount = 0;
  for (uint8_t i = 0; i < MAX_CONTACTS; i++) {
    Serial.print(F("> contact ")); Serial.print(i + 1);
    Serial.print(F(" number (blank to finish): "));
    String n = readLine();
    if (n == "." || n.length() == 0) break;

    if (!validNumber(n)) {
      Serial.println(F("  [ERR] use international format, e.g. +254712345678"));
      i--;                                       // ask for this slot again
      continue;
    }
    n.toCharArray(cfg.contact[cfg.contactCount], 16);
    Serial.print(F("  [OK] format valid  priority "));
    Serial.println(cfg.contactCount + 1);
    cfg.contactCount++;
  }

  Serial.print(F("> voice call enabled? (y/n): "));
  cfg.voiceCall = (readLine()[0] == 'y');
  Serial.println(cfg.voiceCall ? F("y") : F("n"));

  Serial.print(F("> poll interval (sec): "));
  cfg.pollInterval = readLine().toInt();
  if (cfg.pollInterval < 5) cfg.pollInterval = 10;
  Serial.println(cfg.pollInterval);

  /* write to flash, the ESP32 equivalent of writing to EEPROM */
  store.begin("sos", false);
  store.putUChar("count", cfg.contactCount);
  for (uint8_t i = 0; i < cfg.contactCount; i++) {
    char key[8]; sprintf(key, "c%u", i);
    store.putString(key, cfg.contact[i]);
  }
  store.putBool("voice", cfg.voiceCall);
  store.putUShort("poll", cfg.pollInterval);
  store.putString("name", cfg.wearerName);
  store.end();

  Serial.print(F("  [OK] ")); Serial.print(cfg.contactCount);
  Serial.println(F(" contacts written"));
  Serial.println(F("  [OK] settings written"));
  Serial.println(F("setup complete. device armed."));
  buzzSuccess();
}

void loadConfig() {
  store.begin("sos", true);
  cfg.contactCount = store.getUChar("count", 0);
  for (uint8_t i = 0; i < cfg.contactCount && i < MAX_CONTACTS; i++) {
    char key[8]; sprintf(key, "c%u", i);
    store.getString(key, cfg.contact[i], 16);
  }
  cfg.voiceCall    = store.getBool("voice", true);
  cfg.pollInterval = store.getUShort("poll", 10);
  store.getString("name", cfg.wearerName, sizeof(cfg.wearerName));
  store.end();
}

/* ================================================================== */
/*  setup and loop                                                     */
/* ================================================================== */
void setup() {
  Serial.begin(115200);
  GPSser.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  GSMser.begin(9600, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);

  pinMode(PIN_BUTTON, INPUT_PULLUP);       // pressed reads LOW
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_PWR, OUTPUT);
  pinMode(PIN_LED_FIX, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);

  digitalWrite(PIN_LED_PWR, HIGH);         // power light on for good
  delay(1500);

  Serial.println(F("\nSOS TRACKER booting"));
  loadConfig();

  if (cfg.contactCount == 0) {
    Serial.println(F("No contacts stored. Opening setup."));
    process5_manageContacts();
  } else {
    Serial.print(cfg.contactCount);
    Serial.println(F(" contacts loaded. Type SETUP to change them."));
  }

  gsm41_initialise();                      // register early so the first
  digitalWrite(PIN_LED_NET, gsmRegistered);// alert is not delayed
}

void loop() {
  /* --- background loop: acquire and cache, every pollInterval ------ */
  if (millis() - lastPollMs >= (uint32_t)cfg.pollInterval * 1000UL) {
    lastPollMs = millis();

    double lat, lon; uint8_t sats; float hdop;
    if (process1_acquireLocation(lat, lon, sats, hdop)) {
      process2_storeLocation(lat, lon, sats, hdop);
      digitalWrite(PIN_LED_FIX, HIGH);
      Serial.printf("FIX  %.6f, %.6f  sats %u  hdop %.1f\n", lat, lon, sats, hdop);
    } else {
      digitalWrite(PIN_LED_FIX, LOW);
      Serial.println(F("no valid fix this cycle"));
    }
  }

  /* --- SOS button: press and hold ---------------------------------- */
  bool down = (digitalRead(PIN_BUTTON) == LOW);

  if (down && !buttonWasDown) {
    pressStartMs = millis();               // press begins
  }
  if (down && buttonWasDown && (millis() - pressStartMs >= HOLD_MS)) {
    if (millis() - lastAlertMs > ALERT_COOLDOWN_MS) {
      lastAlertMs = millis();
      Serial.println(F("SOS HELD. Raising alert."));
      String msg = process3_formatAlert();   // process 3
      Serial.println(msg);
      process4_transmitAlert(msg);           // process 4
    }
    while (digitalRead(PIN_BUTTON) == LOW) delay(50);  // wait for release
  }
  buttonWasDown = down;

  /* --- serial command to reopen setup ------------------------------ */
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("SETUP")) process5_manageContacts();
    if (cmd.equalsIgnoreCase("TEST"))  {
      String msg = process3_formatAlert();
      Serial.println(msg);
      process4_transmitAlert(msg);
    }
  }

  delay(20);
}
