/* =====================================================================
   IoT-Based GPS Tracking System for Monitoring of Vulnerable Individuals
   Firmware v4.4 (A9G)  -  ESP32 + Ai-Thinker A9G Pudding board

   What it does
     1. Reads GPS from the A9G and keeps the latest good fix in RAM.
     2. SOS button (hold 2 s): SMS to every contact, then calls them in
        order, then reports the SOS to Traccar over GPRS.
     3. Uploads the position to Traccar every UPLOAD_EVERY_S seconds.
     4. Admin manages contacts by SMS:  <PIN> ADD +2547..., <PIN> DEL +2547...,
        <PIN> LIST, <PIN> PIN 5678.  Also <PIN> WHERE replies with a maps link.
        <PIN> ADMIN +2547... hands the admin role to another number.
     5. Contacts, PIN and wearer name live in ESP32 flash (NVS).
     6. Duplicate numbers are refused everywhere and removed at start-up.

   Wiring (ESP32 right-side pins only)
     A9G TX (beside U1RX) -> D22
     D23 -> 1k -> A9G U1RX, 2k from U1RX to GND
     A9G GND -> ESP32 GND
     A9G power: its own micro USB (wall charger or power bank)
     SOS button: D4 to GND        Buzzer (+): D5
     LEDs (optional): D18 power, D19 fix, D21 network (each via 220 ohm)

   Proven settings (3-4 Nov 2026 tests)
     A9G speed 9600 baud. DNS fails on Safaricom 2G, so the server is
     addressed by IP. Traccar needs the timestamp field, or it answers 500.
     The clock comes from the GPS first (exact), the network only as backup.

   Serial Monitor (115200, "Both NL & CR") commands:
     SETUP   set wearer name, contacts, PIN
     STATUS  show fix, network, contacts
     TEST    run a full SOS alert without pressing the button
     UPLOAD  send one position to Traccar now
     LIST    show contacts
     FAKE    indoor testing only: sets a pretend fix at Strathmore
     AT...   any AT command passes straight to the A9G
   ===================================================================== */

#include <Preferences.h>

/* ----------------------------------------------------------- settings */
#define SERVER_IP       "46.101.24.212"   // demo.traccar.org (ping it if uploads stop)
#define SERVER_PORT     5055
#define APN             "safaricom"
#define UPLOAD_EVERY_S  60                // live tracking interval
#define GPS_POLL_S      15                // how often to ask the A9G for a fix
#define FIX_MAX_AGE_S   600               // older than this: marked as old in the SMS
#define CALL_RING_S     20                // ring each contact this long
#define MAX_CONTACTS    5

/* --------------------------------------------------------------- pins */
#define PIN_A9G_RX  22
#define PIN_A9G_TX  23
#define A9G_BAUD    9600
#define PIN_BUTTON   4
#define PIN_BUZZER   5
#define PIN_LED_PWR 18
#define PIN_LED_FIX 19
#define PIN_LED_NET 21
#define HOLD_MS      2000
#define COOLDOWN_MS  60000

HardwareSerial A9G(1);
Preferences    store;

/* ------------------------------------------------------------- config */
struct Config {
  char    name[24] = "the wearer";
  char    pin[8]   = "1234";
  uint8_t count    = 0;
  char    contact[MAX_CONTACTS][16] = {};
} cfg;

/* ------------------------------------------------------------ the fix */
struct Fix {
  double   lat = 0, lon = 0;
  uint32_t atMs = 0;       // millis() when captured
  bool     valid = false;
} fix;

char     imei[20]       = "000000000000000";
bool     netReady       = false;
bool     dataReady      = false;
bool     gpsTimeSet     = false;   // true once the clock comes from GPS (most accurate)
uint32_t epochAtBoot    = 0;    // network time (UTC) minus millis()/1000
uint32_t lastPollMs     = 0;
uint32_t lastUploadMs   = 0;
uint32_t lastAlertMs    = 0;
uint32_t pressStartMs   = 0;
bool     buttonWasDown  = false;

String   rxLine = "";           // A9G text being collected
String   smsFrom = "";          // set by a +CMT header
bool     smsBodyNext = false;
String   pendingSmsFrom = "", pendingSmsBody = "";   // SMS that arrived during a wait

/* ==================================================================== */
/*  helpers                                                              */
/* ==================================================================== */
void buzz(int n, int onMs, int offMs) {
  for (int i = 0; i < n; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(onMs); digitalWrite(PIN_BUZZER, LOW); delay(offMs); }
}

bool validNumber(const String &n) {
  if (n.length() < 10 || n.length() > 15 || n[0] != '+') return false;
  for (uint8_t i = 1; i < n.length(); i++) if (!isDigit(n[i])) return false;
  return true;
}

uint32_t nowEpoch() { return epochAtBoot ? epochAtBoot + millis() / 1000 : 0; }

/* Collect one byte from the A9G. Radio noise (non-printable bytes) is
   dropped. Returns true when a full line is ready in 'line'.           */
bool collect(char c, String &line) {
  if (c == '\n') { line = rxLine; line.trim(); rxLine = ""; return line.length() > 0; }
  if (c == '\r') return false;
  if (c >= 32 && c < 127 && rxLine.length() < 250) rxLine += c;
  return false;
}

/* Lines the A9G sends on its own. Returns true if the line was used.   */
bool handleUnsolicited(const String &line) {
  if (smsBodyNext) {                              // the line after +CMT is the SMS text
    smsBodyNext = false;
    pendingSmsFrom = smsFrom; pendingSmsBody = line;
    return true;
  }
  if (line.startsWith("+CMT:")) {                 // +CMT: "+2547...",,"2026/11/03,19:52:19+03"
    int a = line.indexOf('"'), b = line.indexOf('"', a + 1);
    if (a >= 0 && b > a) { smsFrom = line.substring(a + 1, b); smsBodyNext = true; }
    return true;
  }
  return line.startsWith("+CIEV") || line.startsWith("+CREG") || line.startsWith("+CTZV");
}

/* Send an AT command and wait for 'expect' (or ERROR / timeout).
   The reply text goes into *out when given.                            */
bool at(const String &cmd, const char *expect = "OK", uint32_t timeoutMs = 3000, String *out = nullptr) {
  while (A9G.available()) { String l; if (collect(A9G.read(), l)) handleUnsolicited(l); }
  if (cmd.length()) { A9G.print(cmd); A9G.print("\r\n"); }   // empty cmd: only wait
  String all = "";
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (A9G.available()) {
      String l;
      if (!collect(A9G.read(), l)) continue;
      if (l == cmd) continue;                                   // echo
      if (smsBodyNext || l.startsWith("+CMT:")) { handleUnsolicited(l); continue; }   // SMS arriving
      all += l; all += '\n';
      if (l.indexOf(expect) >= 0) { if (out) *out = all; return true; }
      if (l.indexOf("ERROR") >= 0) { if (out) *out = all; return false; }
    }
    delay(2);
  }
  if (out) *out = all;
  return false;
}

/* ==================================================================== */
/*  flash storage                                                        */
/* ==================================================================== */
/* Remove repeated numbers, keeping the first (so the admin stays first).
   Returns how many were removed.                                       */
int removeDuplicates() {
  int removed = 0;
  for (int i = 0; i < cfg.count; i++)
    for (int j = i + 1; j < cfg.count; ) {
      if (strcmp(cfg.contact[i], cfg.contact[j]) == 0) {
        for (int k = j; k + 1 < cfg.count; k++) strcpy(cfg.contact[k], cfg.contact[k + 1]);
        cfg.count--; removed++;
      } else j++;
    }
  return removed;
}

void saveConfig() {
  store.begin("sos", false);
  store.putString("name", cfg.name);
  store.putString("pin", cfg.pin);
  store.putUChar("count", cfg.count);
  for (uint8_t i = 0; i < MAX_CONTACTS; i++) {
    char k[4]; sprintf(k, "c%u", i);
    if (i < cfg.count) store.putString(k, cfg.contact[i]); else store.remove(k);
  }
  store.end();
}

void loadConfig() {
  store.begin("sos", true);
  if (store.isKey("name")) store.getString("name", cfg.name, sizeof(cfg.name));
  if (store.isKey("pin"))  store.getString("pin", cfg.pin, sizeof(cfg.pin));
  cfg.count = min((int)store.getUChar("count", 0), MAX_CONTACTS);
  for (uint8_t i = 0; i < cfg.count; i++) { char k[4]; sprintf(k, "c%u", i); store.getString(k, cfg.contact[i], 16); }
  store.end();
  int d = removeDuplicates();                      // clean up lists saved by older firmware
  if (d) { saveConfig(); Serial.printf("Removed %d duplicate contact(s).\n", d); }
}

String contactList() {
  String s = "Contacts " + String(cfg.count) + "/" + String(MAX_CONTACTS) + ":";
  for (uint8_t i = 0; i < cfg.count; i++) s += "\n" + String(i + 1) + ". " + cfg.contact[i] + (i == 0 ? " (admin)" : "");
  return s;
}

/* ==================================================================== */
/*  A9G start-up                                                         */
/* ==================================================================== */
void readNetworkTime() {
  // +CCLK: "26/11/03,20:59:12+12"  (local time, +12 quarter hours = +3 h)
  String r;
  if (!at("AT+CCLK?", "+CCLK", 3000, &r)) return;
  int q = r.indexOf('"'); if (q < 0) return;
  String t = r.substring(q + 1);
  int yy = t.substring(0, 2).toInt(), mo = t.substring(3, 5).toInt(), dd = t.substring(6, 8).toInt();
  int hh = t.substring(9, 11).toInt(), mi = t.substring(12, 14).toInt(), ss = t.substring(15, 17).toInt();
  int tz = t.substring(17, 20).toInt();                     // quarter hours, signed
  if (yy < 24) return;                                      // clock not set by the network
  int y = 2000 + yy;
  static const int cum[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  uint32_t days = (y - 1970) * 365 + (y - 1969) / 4 + cum[mo - 1] + (dd - 1) + ((mo > 2 && y % 4 == 0) ? 1 : 0);
  long local = (long)days * 86400L + hh * 3600L + mi * 60L + ss;
  long utc = local - tz * 15L * 60L;
  epochAtBoot = utc - millis() / 1000;
  Serial.printf("Network time set: %s\n", t.substring(0, 17).c_str());
}

/* Time from the GPS itself: switch on NMEA output for a moment and read
   one $GNRMC / $GPRMC sentence (hhmmss and ddmmyy, always UTC).        */
uint32_t toEpochUTC(int y, int mo, int d, int h, int mi, int s) {
  static const int cum[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  uint32_t days = (y - 1970) * 365 + (y - 1969) / 4 + cum[mo - 1] + (d - 1) + ((mo > 2 && y % 4 == 0) ? 1 : 0);
  return days * 86400UL + h * 3600UL + mi * 60UL + s;
}

bool readGpsTime() {
  A9G.print("AT+GPSRD=1\r\n");                     // NMEA every second
  uint32_t t0 = millis(); bool done = false;
  while (millis() - t0 < 4000 && !done) {
    String l;
    while (A9G.available()) if (collect(A9G.read(), l)) {
      int p = l.indexOf("RMC,");
      if (p < 0) { handleUnsolicited(l); continue; }
      // $GNRMC,hhmmss.ss,A,lat,N,lon,E,spd,crs,ddmmyy,...
      String f[10]; int n = 0, start = p + 4;
      for (int i = start; i <= (int)l.length() && n < 10; i++)
        if (i == (int)l.length() || l[i] == ',') { f[n++] = l.substring(start, i); start = i + 1; }
      if (n < 9 || f[1] != "A" || f[0].length() < 6 || f[8].length() < 6) continue;
      int hh = f[0].substring(0, 2).toInt(), mi = f[0].substring(2, 4).toInt(), ss = f[0].substring(4, 6).toInt();
      int dd = f[8].substring(0, 2).toInt(), mo = f[8].substring(2, 4).toInt(), yy = f[8].substring(4, 6).toInt();
      if (yy < 24 || mo < 1 || mo > 12) continue;
      epochAtBoot = toEpochUTC(2000 + yy, mo, dd, hh, mi, ss) - millis() / 1000;
      gpsTimeSet = true;
      Serial.printf("GPS time set: 20%02d-%02d-%02d %02d:%02d:%02d UTC\n", yy, mo, dd, hh, mi, ss);
      done = true;
    }
    delay(5);
  }
  at("AT+GPSRD=0", "OK", 2000);                     // NMEA off again
  while (A9G.available()) A9G.read();
  return done;
}

bool startData() {
  at("AT+CGDCONT=1,\"IP\",\"" APN "\"", "OK", 3000);
  at("AT+CGATT=1", "OK", 15000);
  dataReady = at("AT+CGACT=1,1", "OK", 15000);
  Serial.println(dataReady ? F("Mobile data: active") : F("Mobile data: FAILED (SMS and calls still work)"));
  return dataReady;
}

void startA9G() {
  Serial.println(F("Waiting for the A9G..."));
  for (int i = 0; i < 20 && !at("AT", "OK", 1000); i++) delay(500);
  at("ATE0");                                  // no echo: cleaner replies
  at("AT+CMGF=1");                             // SMS text mode
  at("AT+CNMI=2,2,0,0,0");                     // push new SMS straight to us
  at("AT+CMGD=1,4", "OK", 5000);               // clear the SIM inbox

  String r;
  if (at("AT+EGMR=2,7", "+EGMR", 3000, &r)) {  // IMEI
    String d = ""; for (char c : r) if (isDigit(c)) d += c;
    if (d.length() >= 15) d.substring(d.length() - 15).toCharArray(imei, sizeof(imei));
  }
  Serial.print(F("IMEI ")); Serial.println(imei);

  for (int i = 0; i < 30 && !netReady; i++) {  // wait up to 60 s for the network
    if (at("AT+CREG?", "+CREG", 2000, &r) && (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0)) netReady = true;
    else delay(2000);
  }
  digitalWrite(PIN_LED_NET, netReady);
  Serial.println(netReady ? F("Network: registered") : F("Network: NOT registered"));

  at("AT+GPS=1", "OK", 5000);                  // GPS on
  readNetworkTime();
  if (netReady) startData();
}

/* ==================================================================== */
/*  Process 1 + 2: acquire and store location                            */
/* ==================================================================== */
bool pollGPS() {
  String r;
  if (!at("AT+LOCATION=2", "OK", 4000, &r)) return false;
  int p = r.indexOf("+LOCATION:");
  String body = (p >= 0) ? r.substring(p + 10) : r;     // A9G may also print "lat,lon" alone
  body.trim();
  int comma = body.indexOf(',');
  if (comma < 0 || body.indexOf("NOT") >= 0) return false;
  double la = body.substring(0, comma).toDouble(), lo = body.substring(comma + 1).toDouble();
  if (la == 0 && lo == 0) return false;
  fix.lat = la; fix.lon = lo; fix.atMs = millis(); fix.valid = true;
  return true;
}

uint32_t fixAgeS() { return fix.valid ? (millis() - fix.atMs) / 1000 : 0; }

/* ==================================================================== */
/*  Process 3: format the alert                                          */
/* ==================================================================== */
String formatAlert() {
  String m = "SOS ALERT\n" + String(cfg.name) + " needs help.\n";
  if (fix.valid) {
    char link[80]; sprintf(link, "https://maps.google.com/?q=%.6f,%.6f", fix.lat, fix.lon);
    uint32_t age = fixAgeS();
    m += "Location (GPS";
    if (age > 60) m += ", " + String(age / 60) + " min ago";
    m += "):\n"; m += link;
    if (age > FIX_MAX_AGE_S) m += "\nPosition may be old. Call to confirm.";
  } else {
    m += "Location: no GPS fix yet. Call to confirm.";
  }
  return m;
}

/* ==================================================================== */
/*  Process 4: send SMS, place calls                                     */
/* ==================================================================== */
bool sendSMS(const char *number, const String &text) {
  A9G.print("AT+CMGS=\""); A9G.print(number); A9G.print("\"\r\n");
  uint32_t t0 = millis(); bool prompt = false;
  while (millis() - t0 < 5000 && !prompt) if (A9G.available() && A9G.read() == '>') prompt = true;
  if (!prompt) return false;
  A9G.print(text); A9G.write(26);                       // Ctrl+Z sends
  return at("", "+CMGS", 30000);                        // wait for the send report
}

/* Returns true if the contact answered. */
bool placeCall(const char *number) {
  Serial.printf("Calling %s\n", number);
  if (!at(String("ATD") + number + ";", "OK", 10000)) return false;
  bool answered = false;
  uint32_t deadline = millis() + CALL_RING_S * 1000UL;
  while ((int32_t)(deadline - millis()) > 0) {
    String l;
    while (A9G.available()) if (collect(A9G.read(), l)) {
      if (l.indexOf("NO CARRIER") >= 0 || l.indexOf("BUSY") >= 0 || l.indexOf("NO ANSWER") >= 0) {
        Serial.println(answered ? F("  call ended") : F("  no answer"));
        return answered;
      }
      handleUnsolicited(l);
    }
    delay(10);
  }
  // still connected after the ring time: the contact picked up, keep the line open up to 60 s
  if (at("AT+CLCC", "+CLCC: 1,0,0", 2000)) {             // state 0 = active call
    answered = true;
    Serial.println(F("  answered, line open 60 s"));
    deadline = millis() + 60000UL;
    while ((int32_t)(deadline - millis()) > 0) {
      String l;
      while (A9G.available()) if (collect(A9G.read(), l)) if (l.indexOf("NO CARRIER") >= 0) return true;
      delay(10);
    }
  }
  at("ATH", "OK", 3000);
  return answered;
}

/* ==================================================================== */
/*  Process 6: upload position to Traccar                                */
/* ==================================================================== */
bool upload(bool sos) {
  if (!fix.valid) { Serial.println(F("UPLOAD skipped: no fix")); return false; }
  if (!dataReady && !startData()) return false;
  if (!gpsTimeSet) readGpsTime();                   // GPS clock first: always correct
  if (!epochAtBoot) readNetworkTime();              // network clock only as a backup
  uint32_t ts = nowEpoch();
  if (ts) ts -= fixAgeS();                              // time the fix was taken
  char url[220];
  snprintf(url, sizeof(url), "http://%s:%d/?id=%s&lat=%.6f&lon=%.6f&valid=true&hdop=1.0&speed=0%s%s",
           SERVER_IP, SERVER_PORT, imei, fix.lat, fix.lon,
           ts ? ("&timestamp=" + String(ts)).c_str() : "", sos ? "&alarm=sos" : "");
  String r;
  at(String("AT+HTTPGET=\"") + url + "\"", "HTTP/1.1", 30000, &r);
  bool ok = r.indexOf(" 200") >= 0;
  if (ok) Serial.printf("UPLOAD%s 200 OK\n", sos ? " (SOS)" : "");
  else    { Serial.printf("UPLOAD%s FAILED: ", sos ? " (SOS)" : ""); r.replace("\n", " "); Serial.println(r); }
  if (!ok && r.indexOf("ERROR") >= 0) dataReady = false;   // reconnect next time
  return ok;
}

/* ==================================================================== */
/*  The full SOS sequence                                                */
/* ==================================================================== */
void raiseSOS() {
  Serial.println(F("\n=== SOS ==="));
  buzz(1, 150, 0);
  pollGPS();                                            // try for a fresh fix first
  String msg = formatAlert();
  Serial.println(msg);
  if (cfg.count == 0) { Serial.println(F("No contacts. Type SETUP.")); buzz(1, 900, 0); return; }

  int sent = 0;
  for (uint8_t i = 0; i < cfg.count; i++) {
    bool ok = sendSMS(cfg.contact[i], msg);
    Serial.printf("SMS to %s: %s\n", cfg.contact[i], ok ? "sent" : "FAILED");
    if (ok) sent++;
  }
  for (uint8_t i = 0; i < cfg.count; i++) if (placeCall(cfg.contact[i])) break;   // stop at the first answer
  upload(true);
  if (sent > 0) buzz(2, 120, 120); else buzz(1, 900, 0);
  Serial.printf("=== SOS done: %d of %d SMS sent ===\n\n", sent, cfg.count);
}

/* ==================================================================== */
/*  Process 5b: SMS commands from the admin                              */
/* ==================================================================== */
int findContact(const String &n) {
  for (uint8_t i = 0; i < cfg.count; i++) if (n == cfg.contact[i]) return i;
  return -1;
}

void smsCommand(String from, String body) {
  body.trim(); body.toUpperCase();
  Serial.printf("SMS from %s: %s\n", from.c_str(), body.c_str());
  if (cfg.count == 0 || from != cfg.contact[0]) { Serial.println(F("  ignored: not the admin")); return; }
  int sp = body.indexOf(' ');
  if (sp < 0 || body.substring(0, sp) != cfg.pin) { Serial.println(F("  ignored: wrong PIN")); return; }

  String rest = body.substring(sp + 1); rest.trim();
  sp = rest.indexOf(' ');
  String cmd = (sp < 0) ? rest : rest.substring(0, sp);
  String arg = (sp < 0) ? "" : rest.substring(sp + 1); arg.trim();
  String reply;

  if (cmd == "LIST") reply = contactList();
  else if (cmd == "WHERE") { pollGPS(); reply = formatAlert(); reply.replace("SOS ALERT\n", "Location request\n"); }
  else if (cmd == "ADD") {
    if (!validNumber(arg))               reply = "Error: use format +254712345678";
    else if (findContact(arg) >= 0)      reply = "Already saved: " + arg;
    else if (cfg.count >= MAX_CONTACTS)  reply = "Error: list full. DEL one first.";
    else {
      arg.toCharArray(cfg.contact[cfg.count++], 16); saveConfig();
      reply = "Added " + arg + ". " + String(cfg.count) + "/" + String(MAX_CONTACTS) + " used.";
      sendSMS(arg.c_str(), String("You are now an SOS contact for ") + cfg.name + ". Save this number.");
    }
  }
  else if (cmd == "DEL") {
    int i = findContact(arg);
    if (i < 0)       reply = "Not found: " + arg;
    else if (i == 0) reply = "Error: " + arg + " is the admin and cannot be removed.";
    else {
      for (uint8_t k = i; k + 1 < cfg.count; k++) strcpy(cfg.contact[k], cfg.contact[k + 1]);
      cfg.count--; saveConfig();
      reply = "Removed " + arg + ". " + String(cfg.count) + "/" + String(MAX_CONTACTS) + " used.";
    }
  }
  else if (cmd == "PIN") {
    if (arg.length() < 4 || arg.length() > 6 || arg.toInt() == 0) reply = "Error: PIN must be 4 to 6 digits.";
    else { arg.toCharArray(cfg.pin, sizeof(cfg.pin)); saveConfig(); reply = "PIN changed."; }
  }
  else if (cmd == "ADMIN") {
    int i = findContact(arg);
    if (!validNumber(arg))                        reply = "Error: use format +254712345678";
    else if (i == 0)                              reply = arg + " is already the admin.";
    else if (i < 0 && cfg.count >= MAX_CONTACTS)  reply = "Error: list full. DEL one first.";
    else {
      String oldAdmin = cfg.contact[0];
      if (i > 0) {                                  // already a contact: move it to the top
        for (int k = i; k > 0; k--) strcpy(cfg.contact[k], cfg.contact[k - 1]);
      } else {                                      // new number: insert at the top
        for (int k = cfg.count; k > 0; k--) strcpy(cfg.contact[k], cfg.contact[k - 1]);
        cfg.count++;
      }
      arg.toCharArray(cfg.contact[0], 16);
      saveConfig();
      reply = "Admin is now " + arg + ". " + oldAdmin + " stays as contact 2.";
      sendSMS(cfg.contact[0], String("You are now the admin of ") + cfg.name +
              "'s SOS device. Send <PIN> LIST to see contacts.");
      sendSMS(oldAdmin.c_str(), reply);             // tell the old admin too
      Serial.print(F("  reply: ")); Serial.println(reply);
      return;                                       // both admins already told
    }
  }
  else reply = "Commands: ADD +254..., DEL +254..., LIST, WHERE, PIN nnnn, ADMIN +254...";

  Serial.print(F("  reply: ")); Serial.println(reply);
  sendSMS(cfg.contact[0], reply);
}

/* ==================================================================== */
/*  USB setup menu                                                       */
/* ==================================================================== */
String readLineUSB() {
  String s = "";
  while (true) {
    while (!Serial.available()) delay(5);
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (s.length()) { s.trim(); return s; } }
    else s += c;
  }
}

void setupMenu() {
  Serial.println(F("\n=== SETUP ===  (type . to keep the value in brackets)"));
  Serial.printf("> wearer name [%s]: ", cfg.name);
  String v = readLineUSB(); Serial.println(v);
  if (v != ".") v.toCharArray(cfg.name, sizeof(cfg.name));

  Serial.println(F("> contacts. Contact 1 is the ADMIN. Type . when done."));
  cfg.count = 0;
  while (cfg.count < MAX_CONTACTS) {
    Serial.printf("  contact %u (+254...): ", cfg.count + 1);
    String n = readLineUSB(); Serial.println(n);
    if (n == ".") break;
    if (!validNumber(n)) { Serial.println(F("  use the format +254712345678")); continue; }
    bool dup = false;
    for (uint8_t i = 0; i < cfg.count; i++) if (n == cfg.contact[i]) dup = true;
    if (dup) { Serial.println(F("  already in the list, enter a different number")); continue; }
    n.toCharArray(cfg.contact[cfg.count++], 16);
  }
  Serial.printf("> SMS command PIN [%s]: ", cfg.pin);
  v = readLineUSB(); Serial.println(v);
  if (v != "." && v.length() >= 4 && v.length() <= 6) v.toCharArray(cfg.pin, sizeof(cfg.pin));

  saveConfig();
  Serial.println(contactList());
  Serial.println(F("Saved. Device armed.\n"));
  buzz(2, 120, 120);
}

/* ==================================================================== */
/*  setup and loop                                                       */
/* ==================================================================== */
void setup() {
  Serial.begin(115200);
  A9G.begin(A9G_BAUD, SERIAL_8N1, PIN_A9G_RX, PIN_A9G_TX);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_PWR, OUTPUT); pinMode(PIN_LED_FIX, OUTPUT); pinMode(PIN_LED_NET, OUTPUT);
  digitalWrite(PIN_LED_PWR, HIGH);

  Serial.println(F("\nSOS TRACKER v4.4 (A9G) booting"));
  loadConfig();
  delay(2000);
  startA9G();
  Serial.printf("Register in Traccar with identifier: %s\n", imei);

  if (cfg.count == 0) { Serial.println(F("No contacts stored.")); setupMenu(); }
  else Serial.println(contactList());
  Serial.println(F("Ready. Commands: SETUP STATUS TEST UPLOAD LIST FAKE, or any AT command.\n"));
  buzz(1, 80, 0);
}

void loop() {
  /* 1. text from the A9G: incoming SMS and status lines */
  while (A9G.available()) { String l; if (collect(A9G.read(), l)) handleUnsolicited(l); }
  if (pendingSmsBody.length()) {
    String f = pendingSmsFrom, b = pendingSmsBody;
    pendingSmsFrom = ""; pendingSmsBody = "";
    smsCommand(f, b);
  }

  /* 2. GPS poll */
  if (millis() - lastPollMs >= GPS_POLL_S * 1000UL) {
    lastPollMs = millis();
    if (pollGPS()) { digitalWrite(PIN_LED_FIX, HIGH); Serial.printf("FIX %.6f, %.6f\n", fix.lat, fix.lon); }
    else { digitalWrite(PIN_LED_FIX, LOW); Serial.println(fix.valid ? F("no new fix (keeping last one)") : F("waiting for GPS fix...")); }
  }

  /* 3. live tracking upload */
  if (fix.valid && millis() - lastUploadMs >= UPLOAD_EVERY_S * 1000UL) {
    lastUploadMs = millis();
    upload(false);
  }

  /* 4. SOS button: hold 2 s */
  bool down = (digitalRead(PIN_BUTTON) == LOW);
  if (down && !buttonWasDown) pressStartMs = millis();
  if (down && buttonWasDown && millis() - pressStartMs >= HOLD_MS) {
    if (millis() - lastAlertMs > COOLDOWN_MS || lastAlertMs == 0) { lastAlertMs = millis(); raiseSOS(); }
    while (digitalRead(PIN_BUTTON) == LOW) delay(20);
    down = false;
  }
  buttonWasDown = down;

  /* 5. USB commands */
  if (Serial.available()) {
    String c = Serial.readStringUntil('\n'); c.trim();
    String u = c; u.toUpperCase();
    if (u == "SETUP")       setupMenu();
    else if (u == "TEST")   raiseSOS();
    else if (u == "UPLOAD") upload(false);
    else if (u == "LIST")   Serial.println(contactList());
    else if (u == "FAKE")   { fix.lat = -1.310050; fix.lon = 36.812500; fix.atMs = millis(); fix.valid = true;
                              Serial.println(F("Pretend fix set at Strathmore (testing only).")); }
    else if (u == "STATUS") Serial.printf("IMEI %s | network %s | data %s | fix %s (%lu s old) | time %s\n",
                              imei, netReady ? "yes" : "no", dataReady ? "yes" : "no",
                              fix.valid ? "yes" : "no", (unsigned long)fixAgeS(), epochAtBoot ? (gpsTimeSet ? "from GPS" : "from network") : "not set");
    else if (u.startsWith("AT")) { String r; at(c, "OK", 5000, &r); Serial.print(r); }
  }
  delay(5);
}