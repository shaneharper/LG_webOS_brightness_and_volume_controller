#pragma once

class HysteresisADC
{
  const uint8_t pin;
  const uint16_t threshold;
  uint16_t filtered_value;

public:
  HysteresisADC(uint8_t pin, uint16_t threshold = 4)
    : pin(pin), threshold(threshold), filtered_value(analogRead(pin))
  {}

  uint16_t read()
  {
    if (int16_t v = analogRead(pin);
        abs(v - filtered_value) >= threshold)
    {
      filtered_value = v;
    }
    return filtered_value;
  }
};
