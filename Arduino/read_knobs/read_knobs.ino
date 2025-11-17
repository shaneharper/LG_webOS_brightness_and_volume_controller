#include "min_max.h"

template<class T> inline Print &operator <<(Print &obj, T arg) { obj.print(arg); return obj; }  // From https://forum.arduino.cc/t/c-library-stringstream-for-serial-print/46431/3


void setup()
{
  Serial.begin(9600);
}

int pot_position_as_percent_of_maximum(uint8_t pin)
{
  return max(0, min((analogRead(pin)-12)/10, 100));
}

void loop()
{
  int brightness = pot_position_as_percent_of_maximum(A0);
  if (static int last_brightness = -1; brightness != last_brightness)
  {
    Serial << "b" << brightness << "\n";
    last_brightness = brightness;
  }

  delay(150);

  int volume = pot_position_as_percent_of_maximum(A1)/2;
  if (static int last_volume = -1; volume != last_volume)
  {
    Serial << "v" << volume << "\n";
    last_volume = volume;
  }

  delay(150);
}