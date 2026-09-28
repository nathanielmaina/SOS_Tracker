/* STAGE 2a  -  GPS raw NMEA passthrough
   Wiring: GPS VCC->5V, GPS GND->GND, GPS TX->GPIO16.
   Monitor at 115200. GO OUTSIDE with clear sky.
   Pass: lines starting $GPGGA and $GPRMC scroll past.
   If you see garbage, change 9600 to 38400 below. */

HardwareSerial GPSser(2);

void setup() {
  Serial.begin(115200);
  GPSser.begin(9600, SERIAL_8N1, 16, 17);   // RX=16, TX=17
  Serial.println("STAGE 2a: raw NMEA. Go outside.");
}

void loop() {
  while (GPSser.available()) Serial.write(GPSser.read());
}
