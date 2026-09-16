#pragma once

#include <stdint.h>
#include "save_restore.h"
#include "platform.h"
#include "device_object.h"
#include "table_object.h"

#define MAXSAVE 5
#define MAXTABLEOBJ 4

#ifndef KNX_FLASH_SIZE
#define KNX_FLASH_SIZE 1024
#endif

class MemoryBlock
{
  public:
    MemoryBlock(){};
    MemoryBlock(uint8_t* address, size_t size)
        : address(address), size(size) {}
    uint8_t* address = nullptr;
    size_t size = 0;
    MemoryBlock* next = nullptr;
};

enum VersionCheckResult
{
    FlashAllInvalid = 0,   //!< All flash content is not valid for this firmware; readMemory() returns early and leaves it untouched
    FlashTablesInvalid = 1,//!< All table objects are invalid for this firmware, device object and saveRestores are OK
    FlashValid = 2         //!< Flash content is valid and will be used
};

typedef VersionCheckResult (*VersionCheckCallback)(uint16_t manufacturerId, uint8_t* hardwareType, uint16_t version);

class Memory
{
    friend class TableObject;

  public:
    Memory(Platform& platform, DeviceObject& deviceObject);
    virtual ~Memory();
    void readMemory();
    void writeMemory();
    void saveMemory();
    void addSaveRestore(SaveRestore* obj);
    void addSaveRestore(TableObject* obj);
    void loop();

    uint8_t* allocMemory(size_t size);
    void freeMemory(uint8_t* ptr);
    void writeMemory(uint32_t relativeAddress, size_t size, uint8_t* data);
    void readMemory(uint32_t relativeAddress, size_t size, uint8_t* data);
    uint8_t* toAbsolute(uint32_t relativeAddress);
    uint8_t* toAbsoluteChecked(uint32_t relativeAddress, size_t size);
    uint32_t toRelative(uint8_t* absoluteAddress);
    size_t memorySize();
    void scheduleSave();  // arm the deferred metadata write

    void versionCheckCallback(VersionCheckCallback func);
    VersionCheckCallback versionCheckCallback();
    uint16_t layoutFingerprint();  // identifies the persisted stream layout of this build

  private:
    void removeFromFreeList(MemoryBlock* block);
    void addToUsedList(MemoryBlock* block);
    void removeFromUsedList(MemoryBlock* block);
    void addToFreeList(MemoryBlock* block);
    size_t alignToPageSize(size_t size);
    MemoryBlock* removeFromList(MemoryBlock* head, MemoryBlock* item);
    MemoryBlock* findBlockInList(MemoryBlock* head, uint8_t* address);
    void addNewUsedBlock(uint8_t* address, size_t size);
    static uint32_t mixRecord(uint32_t hash, uint16_t kind, SaveRestore* obj);

    void readEraseBlockToBuffer(uint32_t blockNum);
    uint8_t* eraseBlockStart(uint32_t blockNum);
    uint8_t* eraseBlockEnd(uint32_t blockNum);
    void saveBufferdEraseBlock();

    unsigned long _saveTimeout = 0; // timeout for saving memory
    VersionCheckCallback _versionCheckCallback = 0;
    Platform& _platform;
    DeviceObject& _deviceObject;
    SaveRestore* _saveRestores[MAXSAVE] = {0};
    TableObject* _tableObjects[MAXTABLEOBJ] = {0};
    uint8_t _saveCount = 0;
    uint8_t _tableObjCount = 0;
    MemoryBlock* _freeList = nullptr;
    MemoryBlock* _usedList = nullptr;
    // 4x pushWord + pushByteArray(LEN_HARDWARE_TYPE)
    uint16_t _metadataSize = 8 + LEN_HARDWARE_TYPE;
};
