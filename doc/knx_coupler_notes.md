KNX Coupler
===========

Implementation Notes
--------------------
* Add support for coupler model 1.x and coupler model 2.0 (only used for TP1/RF coupler so far)
* currently implemented mask versions: 091A (IP/TP1 coupler) and 2920 (TP1/RF coupler)
* 03_03_03 2.4.2.4.1 p.13 gives every interface its own layer 3 entity. This implementation has one
  `NetworkLayerCoupler` above both, so that model is collapsed and the rules of point b and d have to be
  applied by hand where they matter.
* A locally originated broadcast is the case where it matters today: `dataBroadcastRequest()` sends on both
  interfaces and both data link layers confirm, while 2.2.3 p.9 maps one `L_Data.con` to one
  `N_Data_Broadcast.con`. Only the PRIMARY confirms upward -- it is handed the caller's own NPDU, the
  secondary a copy, and point d forbids an entity handling an NPDU from another entity to pass it to the
  transport layer. Same for the system broadcast twin.

ToDo:
-----
* class NetworkLayerCoupler: add support for all ACK modes for medium TP1, currently ALL received frames are ACK'ed (mode 2).
* handle MasterReset according to spec. for router object

Development environment
-----------------------
* see linux coupler example

