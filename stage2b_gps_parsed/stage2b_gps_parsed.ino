/* STAGE 2b  -  GPS parsed with TinyGPSPlus
   Same wiring as 2a. Install TinyGPSPlus first.
   Pass: a FIX line with real coordinates, sats >= 4, hdop <= 5.
   Record the time from power-on to first FIX. That is your TTFF
   measurement for Chapter Five. */

#include <TinyGPSPlus.h>
HardwareSerial GPSser(2);
TinyGPSPlus gps;
uint32_t lastPrint = 0;

void setup() {
  Serial.begin(115200);
  GPSser.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("STAGE 2b: waiting for fix. Clear sky needed.");
}

void loop() {
  while (GPSser.available()) gps.encode(GPSser.read());

  if (millis() - lastPrint > 2000) {
    lastPrint = millis();

    if (gps.location.isValid()) {
      Serial.printf("FIX  %.6f, %.6f   sats %lu   hdop %.1f   age %lu ms\n",
                    gps.location.lat(), gps.location.lng(),
                    gps.satellites.value(),
                    gps.hdop.isValid() ? gps.hdop.hdop() : 99.9,
                    gps.location.age());
    } else {
      Serial.printf("searching...  chars %lu   sentences %lu   checksum errors %lu   sats %lu\n",
                    gps.charsProcessed(), gps.sentencesWithFix(),
                    gps.failedChecksum(), gps.satellites.value());
    }

    if (gps.charsProcessed() < 10)
      Serial.println("  NO DATA. Check TX->GPIO16 and the common ground.");
  }
}
