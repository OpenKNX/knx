#include <cstring>

#include "group_object_table_object.h"
#include "group_object.h"
#include "bits.h"
#include "data_property.h"

GroupObjectTableObject::GroupObjectTableObject(Memory& memory)
    : TableObject(memory)
{
    Property* properties[]
    {
        new DataProperty(PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_GRP_OBJ_TABLE)
    };
    TableObject::initializeProperties(sizeof(properties), properties);
}

GroupObjectTableObject::~GroupObjectTableObject()
{
    freeGroupObjects();
}

uint16_t GroupObjectTableObject::entryCount()
{
    // A restored image can be LOADED with a null table, so this is checked here rather than only on the
    // boot path.
    if (loadState() != LS_LOADED || _tableData == nullptr)
        return 0;

    return ntohs(_tableData[0]);
}

GroupObject& GroupObjectTableObject::get(uint16_t asap)
{
    return _groupObjects[asap - 1];
}

const uint8_t* GroupObjectTableObject::restore(const uint8_t* buffer)
{
    buffer = TableObject::restore(buffer);

    // Gate on the load state as AssociationTableObject::restore() does: a download interrupted after the
    // additional-load-control step would otherwise come back LS_LOADING with a half-written table.
    _tableData = (loadState() == LS_LOADED) ? (uint16_t*)data() : nullptr;
    initGroupObjects();

    return buffer;
}

void GroupObjectTableObject::groupObjects(GroupObject * objs, uint16_t size)
{
    freeGroupObjects();
    _groupObjects = objs;
    _groupObjectCount = size;
    initGroupObjects();
}

void GroupObjectTableObject::beforeStateChange(LoadState& newState)
{
    TableObject::beforeStateChange(newState);
    if (newState != LS_LOADED)
        return;

    _tableData = (uint16_t*)data();

    if (!initGroupObjects())
    {
        newState = LS_ERROR;
        TableObject::errorCode(E_SOFTWARE_FAULT);
    }
}

bool GroupObjectTableObject::initGroupObjects()
{
    if (!_tableData)
        return false;
    
    freeGroupObjects();

    uint16_t goCount = ntohs(_tableData[0]);

    // The table holds one header word plus one word per object, so a count past tableSize() cannot be
    // backed by data. Refuse the table rather than size anything from it; entryCount() then reports 0.
    if ((uint32_t)(goCount + 1) * sizeof(uint16_t) > tableSize())
    {
        println("group object table refused: header count exceeds the allocated table");
        _tableData = nullptr;
        return false;
    }

    _groupObjects = new GroupObject[goCount];
    _groupObjectCount = goCount;

    for (uint16_t asap = 1; asap <= goCount; asap++)
    {
        GroupObject& go = _groupObjects[asap - 1];
        go._asap = asap;
        go._table = this;
    
        go._dataLength = go.goSize();
        size_t sizeInMemory = go.sizeInMemory();
        go._data = new uint8_t[sizeInMemory];
        memset(go._data, 0, sizeInMemory);
        
        if (go.valueReadOnInit())
            go.requestObjectRead();
    }

    return true;
}

void GroupObjectTableObject::freeGroupObjects()
{
    if (_groupObjects)
        delete[] _groupObjects;
    
    _groupObjectCount = 0;
    _groupObjects = 0;
}
