#include <TinyGPS.h>
#define GPS_RX 17
#define GPS_TX 16

#define GPS_Serial_Baud 9600

TinyGPS gps;

void setup()
{
  Serial.begin(115200); // Beginning the serial monitor at Baudrate 115200 and make sure you select same in serial monitor
  Serial2.begin(GPS_Serial_Baud,SERIAL_8N1,GPS_RX,GPS_TX);
  Serial.println(F("TinyGPS Simple Test"));
  Serial.println(F("Aguardando dados do GPS..."));
}

void loop()
{
  bool newData = false;
  unsigned long chars;
  unsigned short sentences, failed;

  // Por um segundo, analisamos os dados do GPS e reportamos alguns valores chave
  for (unsigned long start = millis(); millis() - start < 1000;)
  {
    while (Serial2.available())
    {
      char c = Serial2.read();
      // Serial.write(c); // Descomente para ver os dados crus
      if (gps.encode(c)) // Nova sentenca valida parseada?
        newData = true;
    }
  }

  if (newData)
  {
    float flat, flon;
    unsigned long age;
    gps.f_get_position(&flat, &flon, &age);
    Serial.print(F("LAT="));
    Serial.print(flat == TinyGPS::GPS_INVALID_F_ANGLE ? 0.0 : flat, 6);
    Serial.print(F(" LON="));
    Serial.print(flon == TinyGPS::GPS_INVALID_F_ANGLE ? 0.0 : flon, 6);
    Serial.print(F(" SAT="));
    Serial.print(gps.satellites() == TinyGPS::GPS_INVALID_SATELLITES ? 0 : gps.satellites());
    Serial.print(F(" ALT="));
    Serial.print(gps.f_altitude() == TinyGPS::GPS_INVALID_F_ALTITUDE ? 0.0 : gps.f_altitude());
    Serial.print(F(" HDOP="));
    Serial.print(gps.hdop() == TinyGPS::GPS_INVALID_HDOP ? 0 : gps.hdop() / 100.0);
    Serial.print(F(" KMH="));
    Serial.print(gps.f_speed_kmph() == TinyGPS::GPS_INVALID_F_SPEED ? 0.0 : gps.f_speed_kmph());
    Serial.print(F(" CRS="));
    Serial.print(gps.f_course() == TinyGPS::GPS_INVALID_F_ANGLE ? 0.0 : gps.f_course());
    Serial.print(F(" DIR="));
    Serial.print(TinyGPS::cardinal(gps.f_course()));
    
    int year;
    byte month, day, hour, minute, second, hundredths;
    gps.crack_datetime(&year, &month, &day, &hour, &minute, &second, &hundredths, &age);
    
    // Ajuste de fuso horário forçado (-3h)
    // Nota: Isso não ajusta o dia/mês/ano em caso de virada de dia
    if (hour < 3) hour += 21;
    else hour -= 3;
    
    char buffer[32];
    sprintf(buffer, " Date=%02d/%02d/%02d Time=%02d:%02d:%02d", day, month, year, hour, minute, second);
    Serial.println(buffer);
  }
}
