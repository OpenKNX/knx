#pragma once

#include "property.h"

class InterfaceObject;

template <class T> class CallbackProperty : public Property
{
  public:
    CallbackProperty(T* io, PropertyID id, bool writeEnable, PropertyDataType type, uint16_t maxElements,
                     uint8_t access, uint8_t (*readCallback)(T*, uint16_t, uint8_t, uint8_t*),
                     uint8_t (*writeCallback)(T*, uint16_t, uint8_t, const uint8_t*))
        : Property(id, writeEnable, type, maxElements, access),
          _interfaceObject(io), _readCallback(readCallback), _writeCallback(writeCallback)
    {}
    CallbackProperty(T* io, PropertyID id, bool writeEnable, PropertyDataType type, uint16_t maxElements,
                     uint8_t access, uint8_t (*readCallback)(T*, uint16_t, uint8_t, uint8_t*))
        : Property(id, writeEnable, type, maxElements, access), _interfaceObject(io), _readCallback(readCallback)
    {}
    
    uint8_t read(uint16_t start, uint8_t count, uint8_t* data) const override
    {
        if (count == 0 || _readCallback == nullptr || start > _maxElements || start + count > _maxElements + 1)
            return 0;

        return _readCallback(_interfaceObject, start, count, data);
    }
    uint8_t write(uint16_t start, uint8_t count, const uint8_t* data) override
    {
        // Array element 0 is the element count and is read-only (03_05_01 4.7.4 p.113; 03_03_07 3.4.4.1 p.64
        // defines index 0 for reads only). The callbacks never look at start, so a write with start 0 would
        // reach them as data.
        if (start == 0)
            return 0;

        if (count == 0 || start > _maxElements || start + count > _maxElements + 1 || _writeCallback == nullptr)
            return 0;
        return _writeCallback(_interfaceObject, start, count, data);
    }
  private:
    T* _interfaceObject = nullptr;
    uint8_t (*_readCallback)(T*, uint16_t, uint8_t, uint8_t*) = nullptr;
    uint8_t (*_writeCallback)(T*, uint16_t, uint8_t, const uint8_t*) = nullptr;
};
