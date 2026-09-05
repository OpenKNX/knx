#pragma once

#include "config.h"

#include "table_object.h"
#include "knx_types.h"

class Memory;

enum CouplerModel
{
    Model_1x,
    Model_20
};

enum RouterObjectType
{
    Primary,
    Secondary,
    Single     // Not used, just a placeholder for better readability for coupler model 1.x
};

class RouterObject : public TableObject
{
public:
  RouterObject(Memory& memory, uint32_t staticTableAdr = 0, uint32_t staticTableSize = 0);

  void initialize1x(DptMedium mediumType, uint16_t maxApduSize);
  void initialize20(uint8_t objIndex, DptMedium mediumType, RouterObjectType rtType, uint16_t maxApduSize);
  void initialize(CouplerModel model, uint8_t objIndex, DptMedium mediumType, RouterObjectType rtType, uint16_t maxApduSize);

  bool isGroupAddressInFilterTable(uint16_t groupAddress);

  // Read-only view of the filter table for diagnostics: one bit per group address, nullptr when the
  // table is not loaded. isGroupAddressInFilterTable() answers one address and re-reads
  // PID_FILTER_TABLE_USE every time -- scanning all 65536 addresses that way is not affordable.
  // Only while the table is really loaded: _data survives LS_LOADING and LS_ERROR, and the routing
  // path itself filters everything in those states (isGroupAddressInFilterTable returns false).
  const uint8_t* filterTableData() { return loadState() == LS_LOADED ? data() : nullptr; }
  uint32_t filterTableSize() { return loadState() == LS_LOADED ? tableSize() : 0; }
  bool filterTableInUse();

  bool isRfSbcRoutingEnabled();
  bool isIpSbcRoutingEnabled();

  // masterReset is inherited from TableObject: a factory reset clears the filter table via the tested
  // unload path; RouterObject adds no code-specific behaviour of its own.

  const uint8_t* restore(const uint8_t* buffer) override;

protected:
  void beforeStateChange(LoadState& newState) override;

private:
  // Function properties
  void functionRouteTableControl(bool isCommand, uint8_t* data, uint8_t length, uint8_t* resultData, uint8_t& resultLength);
  void functionRfEnableSbc(bool isCommand, uint8_t* data, uint8_t length, uint8_t* resultData, uint8_t& resultLength);
  void functionIpEnableSbc(bool isCommand, uint8_t* data, uint8_t length, uint8_t* resultData, uint8_t& resultLength);

  void commandClearSetRoutingTable(bool bitIsSet);
  bool statusClearSetRoutingTable(bool bitIsSet);
  void commandClearSetGroupAddress(uint16_t startAddress, uint16_t endAddress, bool bitIsSet);
  bool statusClearSetGroupAddress(uint16_t startAddress, uint16_t endAddress, bool bitIsSet);

  bool _rfSbcRoutingEnabled = false;
  bool _ipSbcRoutingEnabled = false;
  CouplerModel _model = CouplerModel::Model_20;
};
