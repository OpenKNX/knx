#pragma once
#include <stdint.h>

/**
 * Interface for classes that can save and restore data from a buffer. 
 */
class SaveRestore
{
  public:
    /**
     * This method is called when the object should save its state to the buffer.
     *  
     * @param buffer The buffer the object should save its state to.
     * 
     * @return The buffer plus the size of the object state. The next object will use this value as 
     * the start of its buffer.
     */
    virtual uint8_t* save(uint8_t* buffer)
    {
        return buffer;
    }
    
    /**
     * This method is called when the object should restore its state from the buffer.
     *  
     * @param buffer The buffer the object should restore its state from.
     * 
     * @return The buffer plus the size of the object state. The next object will use this value as 
     * the start of its buffer.
     */
    virtual const uint8_t* restore(const uint8_t* buffer)
    {
        return buffer;
    }
    
    /**
     * @brief restore() for an image of apiVersion 2, written before the NVM header carried a layout word.
     * A record whose stored layout changed since then overrides it; the default reads the current layout.
     */
    virtual const uint8_t* restoreLegacy(const uint8_t* buffer)
    {
        return restore(buffer);
    }

    /**
     * @return The maximum number of bytes the object needs to save its state. 
     */
    virtual uint16_t saveSize()
    {
        return 0;
    }

    /**
     * @brief What this record persists, for the layout word in the NVM header.
     *
     * Memory folds it in, so a firmware whose stream layout differs is refused instead of parsed
     * positionally. InterfaceObject overrides it with the identity of the properties it writes. The
     * default adds nothing beyond the length Memory already folds: a record that serialises through a
     * product-supplied callback describes its own format and stays outside the word.
     */
    virtual uint32_t layoutTag()
    {
        return saveSize();
    }
};
