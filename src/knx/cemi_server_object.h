#pragma once

#include "config.h"
#ifdef USE_CEMI_SERVER

#include "interface_object.h"

class CemiServerObject: public InterfaceObject
{
public:
  CemiServerObject();

  /** @brief Sets PID 68/69 from the device's own APDU limit; RF caps it at 15 (03_05_01 4.3.7.2.3 p.46). */
  void maxApduLength(uint16_t value);

  void setMediumTypeAsSupported(DptMedium dptMedium);
  void clearSupportedMediaTypes();
};

#endif
