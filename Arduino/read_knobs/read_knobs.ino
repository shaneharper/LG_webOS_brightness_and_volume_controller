#include "HysteresisADC.h"
#include "min_max.h"

template<class T> inline Print &operator <<(Print &obj, T arg) { obj.print(arg); return obj; }  // From https://forum.arduino.cc/t/c-library-stringstream-for-serial-print/46431/3


void setup()
{
  Serial.begin(9600);
}

unsigned pot_position_as_percent_of_maximum(uint16_t ADC_value)
{
  return max(0, min((int(ADC_value)-12)/10, 100));
}

void loop()
{
  static HysteresisADC brightness_ADC(A0), volume_ADC(A1);

  unsigned brightness = pot_position_as_percent_of_maximum(brightness_ADC.read());
  if (static int last_brightness = -1; brightness != last_brightness)
  {
    Serial << "b" << brightness << "\n";
    last_brightness = brightness;
  }

  delay(150);

  unsigned volume = pot_position_as_percent_of_maximum(volume_ADC.read())/2;
  if (static int last_volume = -1; volume != last_volume)
  {
    Serial << "v" << volume << "\n";
    last_volume = volume;
  }

  delay(150);
}