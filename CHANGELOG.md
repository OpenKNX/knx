# Changelog


## unreleased

The largest batch so far: memory safety and conformance across the application, transport, network and
data link layers, the property and load-state machinery, the KNXnet/IP tunnel server, and the datapoint
type conversions. Several entries name a value an ETS or a tunnel client actually observes rather than a
parse bound, because a well-formed but wrong answer is the class that survives a memory-safety review.
Built on OAM-IP-Interface and OAM-IP-Router (RP2040 + ESP32); the TP-only paths additionally run under a
host harness with AddressSanitizer, 77 cases, red-before/green-after per fix.

### Datapoint types
* Fix: the encode path has a real buffer-size guard. `ENSURE_PAYLOAD` expanded to nothing, so a group object written with a DPT larger than its configured data length overran the output buffer
* Fix: DPT 27.001 decodes unsigned. It is B32 with 16 output-state and 16 validity bits (03_07_02 3.26.1 p.62), and decoding it signed made every status with bit 31 set unwritable by the unsigned encoder
* Fix: DPT 5.003 covers 0..360; the `uint8_t` cast wrapped every value above 255. DPT 8.010 is capped at 327.66 and rounded, so a reading never goes out as the reserved 7FFFh, and 7FFFh is taken as invalid data for `DPT_Percent_V16` on decode (03_07_02 3.9.1 p.37 footnote b) instead of returning 327.67
* Fix: DPT 7.002-7.007 and 8.002-8.007 encode as the plain U16/V16 durations they are (03_07_02 3.8.2, 3.9.2); the `tm`/`mktime` round trip shifted them by the device timezone
* Fix: the DPT 9 minimum is -671088.64 (03_07_02 p.39); the former bound was the maximum of 9.029/9.030 and refused legal values below it. The float16 encoder no longer rescales magnitude 2048, which is legal for a negative mantissa (3.10 p.39) and sent the spec minimum as -10.24
* Fix: DPT 14 is bounded by the IEEE 754 single-precision range (03_07_02 3.15 p.44) and NaN is rejected; the former bound was the field ranges multiplied together
* Fix: the date and date-time encoders reject a C-convention `struct tm` and bound month and day -- this stack carries `tm_year` absolute and `tm_mon` 1..12. All four octets of the serial number are written, and the mR/mG/mB/mW validity mask of DPT 251.600 (03_07_02 6.18 p.194) is set, which had left every channel declared invalid
* Fix: a DateTime whose own flags invalidate it is rejected. 03_07_02 3.20 p.51 gives NT the meaning "hour, minutes and seconds not valid" and it was never read; the combined value carries date and time alike, so NY, ND and NT all have to invalidate it. p.51 also requires minutes and seconds to be zero at hour 24 and such a message to be ignored, on the decode and on the encode side
* Fix: an out-of-range time is rejected instead of folded into the field. 03_07_02 3.11 p.41 gives DPT 10.001 an hour of 0 to 23; the mask turned 99 into 3. The weekday shares octet 0 with the hour and was dropped on every round trip, because only the hour was written; out of range it becomes 0, "no day", rather than a rejection, since the day is optional in this DPT
* Fix: a DateTime is encoded with NWD set, because the value carries no working-day information; leaving it clear made WD read as 0, which means "bank day" rather than "unknown"
* Fix: the `Dpt` members are initialised, which `GroupObject` holds as a member and never assigns

### Group objects
* Fix: a sub-byte object is sized from its value field. The guard tested the size code, and `asapValueSize` returns 0 for every value field below one octet, so each 2..7 bit object (DPT 2.x, 3.x) got a zero-byte allocation while `goSize()` reported 1
* Fix: the confirmation reaches the object it belongs to. One confirm slot lost the older object's confirmation as soon as two group objects sent in the same period -- the send pump emits one telegram per loop pass and never waits, so the second overwrote the first and the first object stayed in `Transmitting` for good. A ring of outstanding local writes is matched by GROUP ADDRESS, not by arrival order, because four priority buckets mean a later high-priority telegram can leave before an earlier low-priority one. The oldest entry is dropped when the ring is full, which costs a status and never confirms the wrong object

### Application layer
* Fix: every APDU length is guarded before it is used as an index. The broadcast entry point had no check at all while every neighbouring one has them; the octet count is sender-controlled and cEMI `valid()` enforces only consistency, never a minimum, so a one-octet broadcast reached the handlers and read up to eight octets past the frame -- on TP an out-of-bounds read of a heap allocation, on the IP paths stale bytes of the previous datagram. `individualConfirm()` is guarded the same way, where a tunnel client picks the APDU content
* Fix: a count that does not match the payload writes nothing. 03_03_07 p.113 ignores the indication when the number differs from the octets received in either direction; the old test caught only a count that was too large, so a count below the payload wrote a short value and reported success. The count is passed on as 0, which answers `nr_of_elem` 0 with no data in verify mode (08_03_07 2.7.7 p.34). The same applies to the router memory and filter table services (03_03_07 p.141)
* Fix: the count of the router-memory and filter-table services is sent as a full octet, 1 to 254 (03_03_07 3.6.5 p.139, 3.6.2 p.132)
* Fix: a manufacturer info read is answered with its own response type
* Fix: two response frames are sized to what they write
* Fix: the print loop of an APDU is bounded against a 255-octet length field. The counter was `uint8_t` while the bound `length() + 1` promotes to `int`, so a length octet of 255 wrapped it back to 0 and the loop never ended -- reachable from the bus through the unhandled-APCI arm of `individualIndication()`
* Change: the connected-vs-connectionless decision lives in `individualSend()`. Six send paths carried a byte-identical copy, so a change to that decision could reach only five of them

### Properties and interface objects
* Fix: the write-enable flag is honoured on the bus and tunnel paths. It was applied when persisting and on the cEMI path but not in `propertyValueWriteIndication`/`propertyValueExtWriteIndication`, so a read-only property was writable from the bus and from a tunnel
* Fix: element 0 is the element count and is read-only (03_05_01 4.7.4 p.113; 03_03_07 3.4.4.1 p.64 defines index 0 for reads). The callbacks never inspect `start`, so a 7-octet `A_PropertyValue_Write` set the device address to 0.0.0 and executed the load event carried in the payload
* Fix: a refused write is answered with `nr_of_elem` zero and no data, and directly rather than through the read path -- that path answers index 0 with the element count, so a write refused at index 0 came back looking like a successful read (03_03_07 3.4.4.2 p.66). The extended write reports the count the property itself accepted, since `writeProperty()` returns it as an in/out parameter
* Fix: the index 0 write payload is sized by the element count, not by the element size. `DataProperty::write()` reads two octets there, while the guard asked for count times `ElementSize()`, so a one-octet property passed with a single octet and the write read one octet past the payload -- on TP the frame buffer ends right behind the declared payload
* Fix: a property read that does not fit the PDU is answered with zero elements instead of a truncated positive response (03_03_07 3.4.4.1 p.63), and the element count is clamped so `elementSize * count` fits the octet buffer
* Fix: `propertyData()` is type-checked. It cast every `Property` to `DataProperty` and dereferenced the result unconditionally, so a PID served by a callback or function property returned whatever sat at that offset, and an absent PID was dereferenced as nullptr before any caller could check. The MAC filter of an extended SEARCH_REQUEST compared six octets of code that way and now reads the value through the property
* Fix: a function property cannot report a result longer than the response frame carries. `resultLength` is in/out -- the caller grants what fits, the callee reports what it wrote -- and nothing checked the value on the way back. The result buffer is zeroed, so a callee reporting more octets than it wrote cannot leak stack content
* Fix: a one-octet PDT takes a one-octet default. `Property::write(uint16_t)` requires `ElementSize() == 2`, so the initial value of `PID_MEDIUM_STATUS`, `PID_COUPLER_SERVICES_CONTROL`, `PID_FILTER_TABLE_USE` and the cEMI server counterpart was dropped
* Fix: the device object initialises the bytes it reads back. `Property::read(uint8_t&)` returns 0 and leaves the argument untouched when the property holds no elements, and the stack value was then written back into the property and persisted. The order number is padded to the 10 octets `PDT_GENERIC_10` copies, which otherwise carried whatever followed the string literal in `.rodata`
* Fix: `PDT_BITSET8` stores only the bit the programming mode uses -- it stored all eight while `progMode()` tests the byte for exactly 1, so a write of 03h read back as "not in programming mode" while the device reported 03h
* Fix: a zero-length automatic array is no longer declared when a read returns nothing

### Load state, tables and non-volatile memory
* Fix: the load state is persisted, a restored state is clamped, and the error code is cleared when the object leaves Error. The metadata save was armed by `freeMemory()` alone, which a static table never reaches, so unloading a coupler and power-cycling it brought the old table back as loaded; `LS_UNLOADING` and `LS_LOADCOMPLETING` fall into the default arm of `loadEvent()`, so restoring one froze the object and every later load event including Unload was ignored (03_05_01 Table 94 p.296)
* Fix: the error code is cleared at the state change rather than on the `LE_UNLOAD` path alone, so a master reset that unloads through `resetTable()` clears it too (03_05_01 4.2.28 p.40)
* Fix: `PID_TABLE_REFERENCE` reports zero when there is no allocation (03_05_01 4.2.7 p.31); with a null pointer it computed 0 minus the flash start, which ETS would use as the base for its relative memory writes
* Fix: the address, association and group-object entry counts are bounded against the allocated table size. The count is the table's own header word and was returned unbounded on the path of every received group telegram; a forged header -- reachable unauthenticated from TP or a tunnel through an ETS-style load sequence with a manipulated table reference -- read up to 128 kB past the address table and about 256 kB past the association table. The group-object table reached `new GroupObject[65535]`, about 1.3 MB on a 264 kB RP2040, and then wrote through the failed allocation
* Fix: `LE_LOAD_COMPLETED` reads back what `allocTableStatic()` returned and goes to Error with `E_GOT_MEM_ALLOC_ZERO` instead of reporting Loaded. A static table's build constants were taken on trust: below 8704 bytes of NVM every 091A device booted into a block that does not exist
* Fix: every non-volatile access is bounded, wrap-safe. `alignToPageSize()` went into a `uint16_t` while `flashSize` minus the block is a `size_t`, so an oversized stream wrapped to about 4 GiB and the free list handed out addresses outside the NVM; the overlap is carved off the front of a free block instead of underflowing its size the same way. The null guard runs before the dereference in `removeFromList()`, the absorbed node is taken before advancing in `addToFreeList()`, and the save timer is armed on an allocation as well as on a free
* Fix: the buffered erase block is read in octets, not in pages
* Fix: the ESP32 EEPROM buffer is validated. A pointer from an earlier `begin()` may describe a smaller buffer than this caller needs, and the begin result was discarded -- both ended past the allocation. A size of 64 KiB or more is refused, where `EEPROM.length()` reports in 16 bits and every call would reallocate and dangle the pointers handed out before it
* Fix: a management read writes back a pending sector first. On a flash-backed platform a read goes through the flash mapping while a write waits in the driver's sector buffer, so a read right after a write returned the old content; an unchanged sector is not rewritten, and an Eeprom-backed platform needs nothing
* Fix: verify mode answers with the data the write accepted. A read-back answers from the flash mapping while the write still sits in the sector buffer -- a device that had just been written answered FFh for that block. Out-of-range writes, which `writeMemory()` drops silently, are answered with no data
* Fix: an extended memory write is bounded before it runs and answers `AddressVoid` with zero elements when the range does not fit; it reported success either way and echoed a pointer computed without any check
* Fix: an extended read whose response would not fit is refused with F4h. The data sits at APDU offset 5, so `5 + number` has to fit `MAX_APDU_OCTET_COUNT`; the builder clamps for memory safety, which answered a short read as Success
* Feature: the persisted layout is fingerprinted in the NVM header. The records are read back positionally with no per-record length or identity, so a firmware whose stream layout differs parsed the previous one field by field, including the device object's own address and the tables' relative data pointers. Neither existing check covers this -- the api version describes the header format and the product callback compares the ETS application, which does not change when a build flag resizes an array or registers another save-restore object. `DeviceObject::apiVersion` rises to 3, so the first boot after the update reports the api change; one consequence is that every record shifts by two octets and ETS reprograms the address and the application once per device

### Restart and master reset
* Fix: an unsupported erase code is answered without rebooting. 03_05_02 Table 4 p.83 says the server shall neither execute a Basic Restart nor any Master Reset in that case, and every path fell through to the platform restart, so `A_Restart` with erase code FFh rebooted the device while answering Unsupported Erase Code. The restart response is addressed to the requester instead of `_connectedTsap`, an `int32_t` that is -1 with no connection open and truncates to 0xFFFF -- a connectionless master reset was answered to 15.15.255 and, with a stranger's connection open, into that connection
* Fix: the response leaves before the reset. Resetting inside `restartRequestIndication()` dropped the response still queued for the medium; it now runs from `nextRestartState()` 1000 ms later, so the response goes out with the address it was requested on, and a basic restart arriving inside that window carries out the pending master reset first
* Fix: the announced process time covers the real restart. 03_05_02 3.7.3 p.89 makes it the time-out after which the client counts contact as failed; a TP build announced 3 s, an example value inherited from upstream and never derived from a measurement. Measured after a master reset: TP 3.5 s, IP 8.0 to 9.8 s, so it becomes 10 s and 20 s under `USE_IP`. Hardware-verified on two TP devices, `A_Restart_Response` carrying 000Ah with both back after 3.5 s
* Fix: the coupler BAU runs the restart state machine. The device BAU calls `nextRestartState()` in its loop and the coupler one never did, so a master reset on a coupler answered and then stayed pending, and an outgoing `A_Restart_Request` stalled past the Connected state
* Fix: a channel number the device cannot serve is refused with 03h (03_05_02 p.84). No `masterReset()` implementation in this stack reads the channel, so such a request was answered 00h and then reset everything
* Fix: the 07B0, 27B0 and 57B0 device BAUs reset their tables. All three skipped their own base and called `BauSystemB::doMasterReset()`, which covers two objects, so the address table, the association table, the group object table and the security object stayed loaded through a factory reset

### Transport layer and cEMI server
* Fix: the order of deferred connected requests is kept. 03_03_04 A11 p.20 stores the event back and forbids reordering `T_Data_Connected.req`; there is room for exactly one, and overwriting it dropped the older request without ever delivering it. The newer one is dropped and counted instead
* Fix: an `L_Data` from the management channel is validated. A `DEVICE_CONFIGURATION_REQUEST` carrying `L_data_req` went out unvalidated while the identical frame over a tunnel was checked; the check is scoped to `L_data_req`, because `valid()` tests the L_Data control fields and would otherwise reject every legitimate `M_Prop` and `M_Reset` frame
* Fix: the RF telegram length subtracts the cEMI additional information, as `_ctrl1` already does -- an RF frame carrying any additional info reported more octets than the telegram has
* Feature: the transport layer records whether the open connection was opened by a remote peer or by this device, and reports the peer's individual address through `CemiServer::transportPeer()`. `transportLayerBusy()` answers from the remote flag alone; counting a connection this device originated made every table read refuse ETS its management connection until that connection closed or the device restarted
* Feature: the function property services are served on local device management. `M_FuncPropCommand.req` and `M_FuncPropStateRead.req` were dropped with a log line, so a KNXnet/IP client could not reach a single function property -- they were reachable over the bus only. Local device management names object type and instance, not the object index the bus services use (03_06_03 4.1.7.4); the `.con` repeats the five request octets and appends the return code and the data the function produced, and both services confirm with code FAh (4.1.7.4.4 p.111)

### Network layer and coupler
* Fix: both operands of the routed-address test are masked. The right operand was unmasked, so a device address with a non-zero device octet reported every target as routed
* Fix: the routing count is read where the frame is built instead of being latched in the `NetworkLayer` constructor, which runs at BAU construction -- long before `readMemory()` restores `PID_ROUTING_COUNT` and before ETS can write it. Writing the routing count therefore never took effect, and a power cycle did not help because the latch runs before the restore on every boot
* Fix: a broadcast is excluded from the multicast repeat clause. It arrives as `GroupAddress` with destination 0, so the effective permission was `GROUP_REPEAT` and `BROADCAST_REPEAT` together and unchecking the group repetition silently disabled broadcast repetition -- 03_05_01 4.5.5 p.93 assigns broadcast communication to `BROADCAST_REPEAT` and 4.5.6 p.94 assigns multicast to `GROUP_REPEAT`, separate parameters and separate check boxes in the ETS product
* Fix: the hop-count else hangs off the inner if and the RF override is tracked. A secondary to primary frame was forwarded with its hop count untouched whenever the override did not apply, so a hop-count-0 telegram never expired
* Fix: a frame this device originates is given an interface. `routeDataIndividual()` dropped it once the address had a device part, so the coupler acknowledged on layer 2 and then answered nothing. The mask is derived from the address as `DataLinkLayer::isRoutedPA()` does, so the two address-derived topology rules cannot contradict each other; the address is read once, because this runs in the TP receive context while the main loop may write it, and a non-coupler address gets its own enumerator instead of leaving the member uninitialised
* Fix: the filter table is judged against what the allocation holds. It is null until ETS loads it and the commands walked it through a pointer nobody checked; the group address span is 8192 octets and the lookup indexed up to octet 8191 whatever the table really holds. A 091A build allocates all 8192, a coupler whose ETS loads a shorter table does not, and only the octets of the requested range have to exist. 03_05_01 p.95 wants FFh when the clearing or setting did not happen
* Change: `IGNORE_TOTALLY` is renamed `IGNORE_ACKED` -- the frame is not routed but the data link layer still acknowledges it (03_03_03 2.4.2.4.5.1 p.15, 03_02_02 2.4.2 p.38 case 1.2)
* Feature: IP system broadcast. 03_02_06 4.1.3 puts it on the system setup group, which need not be the routing group, and gives it its own service type; a frame marked `SysBroadcast` leaves as `ROUTING_SYSTEM_BROADCAST` instead of as a routing indication, is received on a socket of its own so that "arrived on the system setup group" is structural rather than a comparison, and is checked against the four cEMI conditions before it is accepted. The router object holds the mode as a deadline that expires 20 s after it was enabled (03_05_01 4.5.16.1.1) and the coupler asks it each pass, so the socket exists only while the mode is on. Towards TP1 nothing changes
* Fix: the IP system broadcast mode is read from the ServiceInfo octet. 03_05_01 4.5.16.1.2/4.5.16.1.3 put the reserved octet at `data[0]`, the ServiceID at `data[1]` and the ServiceInfo at `data[2]`; the mode was taken from `data[0]`, so the reserved octet decided it. The write response repeats the ServiceID without reporting the mode, only the read response appends it
* Fix: a locally originated broadcast is confirmed once. 03_03_03 2.2.3 p.9 maps one `L_Data.con` to one `N_Data_Broadcast.con`, while `dataBroadcastRequest()` sends on both interfaces and both data link layers confirm, so the transport layer saw two. 2.4.2.4.1 d p.13 settles which one counts: an entity handling an NPDU provided by another layer 3 entity does not pass it upward, and the secondary is the one sent the routed copy while the primary gets the caller's own NPDU. The same split applies to the system broadcast twin

### Data link layer
* Fix: a frame the transmitter discarded is confirmed. The dropped-frame callback was never registered, so a frame thrown away on a chip reset, a watchdog or monitor entry vanished and an ETS download or a tunnel client waited out its own timeout. The duplicate filter moves below the confirmation branch, so it no longer swallows the `L_Data.con` of a retried transmission, and duplicate filtering applies on every mask -- 03_02_02 2.4.2 p.39 requires it of every device, and the 091A exemption put each repetition onto IP a second time
* Fix: a request refused before it reached the medium is confirmed negatively, and only what really went out is forwarded to the tunnel
* Fix: the forward tick and the overflow arm of the frame count ring were swapped, so every forward tick took the millis-overflow arm and zeroed all ten buckets every 100 ms. The ring was one bucket deep and the 50 datagram per second limit of 03_02_06 2.1 never engaged -- the device throttled at roughly ten times that rate
* Fix: the cEMI frame offsets are derived from the message code. The additional-info length is read only for `L_Data` and `L_Busmon` frames; in an `M_Prop` frame octet 1 is the sender-chosen high byte of the interface object type, and deriving the NPDU, TPDU, APDU and ctrl1 offsets from it displaced every pointer by up to 255 octets past the buffer -- with the cEMI server building its `.con` over small stack arrays copied from the request
* Change: `dataRequestToTunnel`, `dataConfirmationToTunnel`, `dataIndicationToTunnel` and `isTunnelAddress` are removed. Declaration and definition both sat under `#ifdef KNX_TUNNELING_`, a trailing-underscore name that matches nothing, so neither ever compiled and nothing called or overrode them; enabling them would add four vtable slots to every `DataLinkLayer` for default bodies that only print. The same-named methods on `CemiServer` are live and unrelated

### KNXnet/IP discovery
* Fix: only the service families actually served are advertised. The family count and the DIB content are derived from one predicate each so the declared length and the written families cannot drift; Device Management and Tunnelling are advertised only with `KNX_TUNNELING`, whose absence had ETS discover a service it could never connect to, and Routing is keyed on one predicate in the DESCRIPTION_RESPONSE -- the length keyed it on `MASK_VERSION` while the content keyed it on `KNX_IS_ROUTER`
* Fix: a short SEARCH_REQUEST is bounded before its HPAI is read (03_08_02 7.6.1). A 6-octet request passes the header and declared-length checks above, and `hpai()` was then a view over uninitialised stack, so the SEARCH_RESPONSE went to whatever address and port that memory held
* Change: a DIB length computed in four mask branches of the CONNECTIONSTATE_RESPONSE and read by none of them is dropped

### Tunnelling
* Fix: the configured tunnelling addresses are defended. 03_08_04 2.2.2 p.7 asks the server to acknowledge a frame addressed to an individual address ETS configured as a tunnelling address even when no connection holds it; the gap let ETS hand one to a new device, because its address-in-use check got no acknowledge and read the address as free (03_05_02 2.22.3 p.51). Only the acknowledge bit is set and nothing is transmitted -- an earlier attempt fabricated a `T_Disconnect` here and the unacknowledged transmission became a protocol error on the transceiver. The answer comes from a cached 16-entry copy refreshed once a second, so the acknowledge path never walks the property store, and zero elements are skipped because the property grows with zero fill when anything writes a high element index (03_05_01 4.15.12.2 p.211)
* Fix: a reserved tunnel slot whose address is zero is refused instead of handing the hole out as 0.0.0, and a master reset resets the element count instead of writing max zero elements, which had grown the count to its maximum and made a factory-reset interface answer every connect with `E_NO_MORE_UNIQUE_CONNECTIONS`
* Fix: a reply goes to the client's own endpoint. Route-back is applied on the connection-state and disconnect replies (03_08_02 8.6.2.2) -- a zeroed control HPAI means answer where this came from, and without it the reply went to 0.0.0.0:0. The CONNECT_RESPONSE is addressed to the client's control endpoint (7.8.2 p.38); it was sent to the data endpoint's address with the control endpoint's port, which belongs to neither. A half-zero control HPAI is accepted as a route-back request, which 08_TSSH 5.4.5 p.99 and 5.4.6 p.101 require, and a CONNECT_REQUEST whose host protocol code is not `IPV4_UDP` is discarded (8.6.2.1 p.49)
* Fix: a request is acted on only when it comes from the endpoint that owns the channel. The channel id alone identified the session, so any host on the LAN could sweep its 255 values and end a running download in about two seconds. Only the IP is compared, since a client may use different source ports for its control and data endpoints
* Fix: the acknowledge is checked the same way. Three sibling handlers already resolved the endpoint with `fromTunnelPeer()` and the acknowledge did not, so any host could clear another client's in-flight frame by guessing channel id and sequence number
* Fix: an acknowledge whose status signals an error counts exactly like a missing one (03_08_04 2.6.1 p.9). The frame stays armed and the resend timer decides when to repeat and when to give up, and the error status is counted per connection instead of looking like silence
* Fix: channel 0 is refused when a frame is routed to a channel. A free slot carries `ChannelId` 0, so a frame addressed to channel 0 matched the first unused slot and was handed to a closed connection
* Fix: the service is picked from the connection, not from the message code. 03_08_03 2.3.2 p.7 gives device management `DEVICE_CONFIGURATION_REQUEST` and 03_08_04 2.2.1 p.6 gives tunnelling `TUNNELLING_REQUEST`; a client may send `M_Prop` inside a `TUNNELLING_REQUEST`, and the answer went back as a configuration request, which the client does not acknowledge on a tunnelling connection -- so the resend timer tore the connection down
* Fix: a connection whose protocol version changes mid-stream is shut down (03_08_02 6.2 p.15). Only a datagram naming a channel counts, and only from the endpoint that owns it; without that check eight forged octets from any host would end a running session. No reply is sent, because 08_TSSH 3.2.2 p.16 requires a frame with an unknown version to be ignored
* Fix: a structurally invalid tunnel connect is discarded rather than answered. The frame is too short to hold the CRI, and `E_CONNECTION_TYPE` would claim the requested type is unsupported -- `TUNNEL_CONNECTION` is supported. The rejection is still recorded, so it stays visible in the connection history
* Fix: device management is refused only when a foreign peer holds the transport layer, and reported as its own reason; counting the client's own connection locked ETS out, and the generic no-free-tunnel message named the wrong cause on a device with sixteen free tunnels
* Feature: a queued group telegram is sacrificed instead of the connection. When the send FIFO is full and a connection-oriented frame arrives, the oldest queued group telegram is dropped rather than disconnecting -- the queue is usually full of unrelated bus traffic while the frame that no longer fits carries a running download. The armed head is skipped, being already on the wire with a stamped sequence number that must be repeated verbatim (03_08_04 2.6.1), the survivors shift towards the head so the FIFO order is kept, and a disconnect follows only when nothing is evictable
* Change: the repeat-or-disconnect step is extracted out of `loop()`; same behaviour, one repeat on a data connection and three on a configuration one

### Busmonitor
* Feature: the monitor is served from an array of connections sized by `KNX_BUSMON_CONNECTIONS`, default 1 as 03_08_04 2.2.4 p.8 asks for one per subnetwork. The data tunnels are closed only when the first monitor arrives and chip monitor mode is left only when the last one is gone; the cEMI payload is built once and only the connection header varies per client, a monitor frame is sent fire and forget because a failed send under bus load is exactly what a busmonitor must not report as captured, and a late joiner does not disturb the sequence numbering of a running client. `KNX_BUSMON_REFUSE_WHEN_TUNNELS_OPEN` answers `E_NO_MORE_CONNECTIONS` instead of evicting open tunnels
* Fix: a collision is reported as what the bus carried. Both acknowledge flags set means two devices answered at once and a logical 0 dominates, so the line carried `0x0C & 0xC0` = 0x00 while the monitor was told BUSY -- an invented verdict, on the one path whose purpose is to reproduce the bus
* Fix: the Lost bit is raised after a capture the server had to drop. 03_06_03 EMI 3.3.3.2 defines it as how the next indication reports a gap before it; an oversized LPDU was counted but the following indication still looked like an unbroken capture. A datagram that could not go out on IP does not set it -- 4.1.5.8.1 p.97 scopes the bit to what the data link layer lost

### Security
* Fix: the group object index is checked before its security flags are read. `getGroupObjectSecurity()` reads `PID_GO_SECURITY_FLAGS` into a buffer sized by the element size, one octet, and `DataProperty::read()` answers index 0 with the element count, which `pushWord()` writes as two octets -- one past that buffer on the stack. It gets there from an association table entry naming ASAP 0, which `nextAsap()` returns unvalidated; the bounds check existed but sat below the lookup
* Fix: a received `A_GroupValue_Response` is checked against the group object security. It updates the object exactly as a write does and is the one shape of this service that carries a value into the device without being a write, but only the read and the write path checked it
* Fix: the security interface object writes the element count that index 0 answers with, which it reported without filling the buffer

### Platform and build
* Feature: `setupMultiCastSecondary`, `closeMultiCastSecondary` and `readBytesMultiCastSecondary` add a second multicast socket for the system setup group, with a base default that reports the group as not joined so a platform without them stays off rather than dropping frames silently. Sending needs no membership, so the existing unicast API carries the destination. Implemented for RP2040, ESP8266 and ESP32
* Fix: `Platform::readNonVolatileMemory` handles Callback memory, which fell through to the EEPROM buffer -- a nullptr on RP2040 without EEPROM emulation -- and the RP2040 default constructor sets the memory type, so a product that never registers flash callbacks does not run in EEPROM mode
* Fix: the tunnelling identities and the downloaded IP configuration are cleared on a factory reset, and the properties that have a declared default go back to it rather than to zero: `PID_TTL` to 16 and the routing multicast address to 224.0.23.12, the latter under `KNX_IS_ROUTER` where the writable property is declared
* Change: `KNX_TUNNELING_DEVMGMT`, `KNX_TUNNEL_RESEND`, `KNX_TUNNEL_RESEND_DEPTH`, `KNX_TUNNEL_RESEND_BUF`, `KNX_BUSMON_CONNECTIONS` and `KNX_CEMI_TRANSPORT_STRICT` are documented and defaulted in `config.h`. `KNX_TUNNEL_RESEND` is on by default, because 03_08_04 2.6.1 p.9 makes the repetition of an unconfirmed `TUNNELLING_REQUEST` mandatory and a tunnelling server is a sending device the moment it forwards bus traffic; `KNX_NO_TUNNEL_RESEND` opts out and is then knowingly non-conformant. The three that size member arrays must come from `config.h` with no local fallback, since a diverging second definition would change the object size between translation units
* Change: the flag combinations that do not build are refused at compile time -- `KNX_TUNNELING` without an IP-capable mask or below 1, `OPENKNX_HW_BUSMON` or `USE_CEMI_SERVER` without it, `KNX_SERVICE_FAMILY_CORE` of 2 or more, and `KNX_TUNNELING_STRICT_TOPOLOGY` on a non-router, where it withholds every tunnelled unicast from TP. Each surfaced as a pile of unrelated errors in files that have nothing to do with the cause, or did not surface at all
* Change: the tunnelling identity log is gated behind `KNX_LOG_TUNNELING`; it ran in every `KNX_TUNNELING` build, on a path ETS walks on every download

### Build and pipeline
* Fix: the example projects no longer name TPUart themselves. Each listed `https://github.com/OpenKNX/tpuart.git` with no ref, which resolves to the default branch and overrode the pin in `library.json`; that branch predates the `Frame` API the stack has used since `eeae877`, so `esp32dev_tp` and `rp2040` failed on `isAckOnly`, `isErrored`, `isTruncated` and `isRaw` while the IP-only environment passed. The pin decides now
* Feature: `examples/knx-demo-knxnetip` builds the two flag combinations the KNXnet/IP products select their BAU from -- `0x07B0 + KNX_TUNNELING` for the interface and `0x091A + KNX_TUNNELING` for the router -- on ESP32, RP2040 and RP2350. Neither was compiled anywhere, although both pull the tunnel server, the IP data link layer and `tpuart_data_link_layer.cpp`, which is where a drift against the pinned TPUart surfaces. Its sketch carries no application, so a failure can only mean the stack does not compile
* The library version is `2.5.0-beta.1`


Memory-safety and conformance work on the management path, plus the removal of the download counter.
Every bound below was re-derived from the response builder and the frame buffer, not from the handler
that reads the data. Built on OAM-IP-Interface (RP2040, RP2350, ESP32), OAM-IP-Router (RP2040, ESP32)
and OAM-RaumController (57B0).

### Reachable over the bus
* Fix: a property that exists but is not `PDT_FUNCTION` is answered without return code and without data (03_03_07 3.4.7.3). `functionPropertyStateIndication` started with `handled = true`, so such a request was answered with `resultLength` still at its 255 initialiser: `CemiFrame(3 + 255)` truncated through the `uint8_t` parameter to `apduLength` 2 while `memcpy` wrote 255 octets from `buffer+13`, four octets past the 264-octet buffer and into the frame's own data pointer, which `sendTelegram` writes through on its next statement. Reachable with `A_FunctionPropertyState_Read` on object 0 property 1, over TP and through a tunnel, without programming mode and without authentication
* Fix: `functionPropertyExtStateIndication` starts `resultLength` at 1, because its error paths write only `resultData[0]` and 03_03_07 3.4.8.3 says such a response carries no data field
* Fix: the response builders are bound to the frame buffer -- `propertyValueRead` 249, `propertyValueExtRead` 245, `functionPropertyStateResponse` 251, its extended twin 248, and `systemNetworkParameterReadResponse` 250, which had no bound at all: a 250-octet `test_info` in programming mode wrapped `frame(260)` to `frame(4)` and overran by six octets
* Fix: `CemiFrame(apduLength)` takes `uint16_t`; a request above 255 wrapped to a short frame that the caller then filled to its own length. An oversized request now leaves `octetCount` at 0 and `sendTelegram` drops it before touching any field
* Fix: busmonitor carriers stay out of the cEMI path on every build -- a standalone acknowledge, a raw poll frame and a truncated frame are not full LPDUs, and the rejection sat inside the busmonitor build gate, so a build without it converted them and read past the buffer

### Group objects
* Fix: the association-table lookup terminates. The binary search used a closed interval, so `high = i - 1` wrapped to `0xFFFF` whenever the searched ASAP was below the table's first entry and the loop never ended -- a watchdog boot loop, not repairable over the bus, triggered by an ordinary project with group object 1 unassigned and group object 2 assigned
* Fix: `entryCount()` no longer believes an erased segment, which reported `0xFFFF` entries and read up to 262 KiB past the table
* Fix: the unsorted path no longer re-arms the binary search it just rejected, which made a group object silently never send
* Fix: an unassigned ASAP is dropped instead of sent -- `groupValueSend` cast `translateAsap`'s `-1` to `0xFFFF`, which the address table maps to 0, so a group value write went out as a broadcast for every group object ETS left without a group address
* Fix: the confirm is attributed to the right object: the ASAP slot is claimed once the telegram is going out, and cleared when nothing goes out

### Datapoint types
* Fix: negative values encode on the signed types. `signed8/16/32ToPayload` were fed `(uint64_t)value`, and for a `DoubleType` that is a cast of a negative double to an unsigned integer, which is undefined and saturates to zero on ARM -- a negative float on a DPT 6, 8 or 13 group object went on the bus as `0x00`. RP2040 and RP2350 were affected, the ESP32 arrived at the right value by accident

### Device management
* Fix: the ETS writability probe is answered. ETS probes a property with a 7-byte `M_PropWrite` request carrying no data before it writes; the branch handling `PID_DEVICE_ADDR` and `PID_SUBNET_ADDR` required a data octet, so the probe fell through to the generic branch, where both properties are declared write-protected and the answer was `Read_Only`. ETS reported the individual-address download as a memory write failure
* Change: `PID_DOWNLOAD_COUNTER` is removed. No KNX tool reads it, and on every product but the IP interface it reported 0 after each restart because the value lived in RAM and an ETS download ends in a restart -- a counter that decreases is worse than none, and 03_05_01 5.3.2.2 defines the fallback to a full download when the property is unavailable. The implementation also incremented a `uint16_t` without a ceiling, which 03_05_01 4.2.30.2 forbids. 03_05_01 4.2.30.1 makes the property conditional, so a device without a download counter is conformant. Removing it changes the persisted stream by zero octets
* Fix: `Frame::cemiData()` can fail its allocation, and both callers check the result

### KNXnet/IP endpoint
* Fix: the multicast endpoint is rebuilt when the network comes back. It was joined once at startup and never again, so after a link change a device kept its address, its web server and its open tunnels but stopped answering SEARCH_REQUEST and stayed undiscoverable until a reboot -- 03_08_02 Core 4.2 requires a server to support discovery for its whole operational life. `IpDataLinkLayer::networkChanged(afterOutage)` joins when there is no endpoint, leaves and re-joins after an outage, and keeps a fresh one otherwise; `BauSystemB` carries the virtual so a network module can report the event without knowing the mask
* Fix (RP2040): `closeMultiCast()` leaves the IGMP group explicitly. `WiFiUDP::stop()` does not, and `igmp_joingroup_netif()` sends a Membership Report only for a group in NON_MEMBER state -- so a re-join was invisible on the wire, a switch that had pruned the port never learned the device was back, and `group->use` grew without a matching leave on a `u8_t` counter
* Fix: `enabled()` reflects whether the endpoint exists instead of only the intent, so a device whose join failed no longer reports its KNX-IP state as healthy
* Change: the multicast group is read once per device lifetime, matching 03_08_03 2.5.17 (a runtime write to PID 66 becomes active on reset); a value outside 224.0.0.0/4 or inside the link-local block falls back to 224.0.23.12 with a log line
* Change: `Platform::setupMultiCast()` returns whether the group was joined; the base returns true, because a platform without multicast has nothing that can fail

### Tunnelling
* Fix: a connect that is turned away is recorded. Three of the four reject paths wrote a history entry; the fourth -- no slot free, or a reserved slot busy and configured to decline -- sent the error response and left no trace. `detail` now carries the code the client received, 0x24 for no free connection and 0x25 for no unique individual address. Repeated identical refusals still fold into one entry
* Feature: `TunnelEvent` reports `slot` and `resSlot`, and the connection stores the slot the reservation table held for its client at connect time. Recomputing it afterwards cannot work: the reservation is matched against the control HPAI while `IpAddress` is the data HPAI, and the two need not carry the same address. Device-management and busmonitor connections report 0xFF, they sit outside the reservable pool
* Feature: `reservedTunnelsCtrl()` and `reservedTunnelsIp()` read back the ETS reservation table for diagnostics, with the same length check the connect path uses. The returned memory belongs to the property and is freed on the next ETS write, so both are for the KNX loop only


## ec/v2.5.0-beta.1 - 2026-08-29

Conformance and hardening release on top of `ec/v2.4.0-beta.1` (`cec1b35` .. `eef0ade`). Most entries
come from tracing the KNXnet/IP and device-management paths against the specification with a test
client, so nearly every fix names a value or timing an ETS or a tunnel client actually observes.
Field-tested on OAM-IP-Interface and OAM-IP-Router (RP2040 + ESP32).

### Breaking
* Breaking: `OPENKNX_FTC` is now `OPENKNX_FTC_CLIENT`. A product that sets the old name loses the client role silently, so rename it in the ini
* Change: the FunctionProperty confirm cases are handled unconditionally, no longer only under the FTC flag

### Discovery and description (a non-router must not look like a router)
* Fix: ROUTING is advertised only on a router — `DESCRIPTION_RESPONSE`, `SEARCH_RESPONSE` and `SEARCH_RESPONSE_EXTENDED` announced the routing service family and the routing multicast on an interface as well, which is what makes an interface fail the HW-busmonitor rules of 03_08_04 §2.2.4
* Fix: a non-routing device reads routing multicast as `0` and still joins the discovery multicast, so it stays findable without claiming routing
* Fix: the extended search no longer advertises the TP1 medium as permanently unavailable
* Fix: `SEARCH_REQUEST`/`SEARCH_REQUEST_EXTENDED` carrying a TCP HPAI is discarded instead of answered over UDP
* Fix: the extended-search SRP parser is bound-checked and the `requestedDIB` read guard corrected from `>` to `>=`
* Fix: the IP Current Config DIB (`0x04`) is part of `DESCRIPTION_RESPONSE`, with corrected info1/info2 offsets
* Fix: the KNX-Addresses DIB writes the individual address as 2 octets
* Fix: `PID_CURRENT_IP_ASSIGNMENT_METHOD` and `PID_IP_CAPABILITIES` accept 1 element
* Fix: the `setTunnelingInfo` address buffer is hoisted out of the `else` block, where it went out of scope before use

### Tunnelling
* Fix: the interface no longer L2-acknowledges foreign group telegrams on behalf of a tunnel client -- `isSentToTunnel()` reports true for EVERY group address while any tunnel is open, so `Bau07B0IP::isAckRequired` acknowledged group telegrams the device is not addressed by, and the TP1 repetition a receiver that missed the frame depends on never happened (the scene telegrams that prompted this were later traced to a REG2 powered from the KNX supply, not to the acknowledge; the acknowledge is wrong on its own terms)
* Fix: the interface acknowledges group telegrams only for broadcast and its own address table; acknowledging on behalf is coupler behaviour and stays in `Bau091A`
* Fix: `L_Data.con` carries the real TP result and is emitted once per request — a client could see a positive confirmation for a frame the bus never took
* Fix: a self-addressed tunnel probe (`src == dest`) gets its `L_Data.con`
* Fix: a truncated TUNNEL `CONNECT_REQUEST` is rejected with `E_CONNECTION_TYPE` instead of being parsed
* Fix: requests on an unopened channel are rejected, and a connection error response echoes the requested channel id
* Fix: `CONNECTIONSTATE_RESPONSE`, `DISCONNECT_RESPONSE` and `CONNECT_RESPONSE` rejects go back to the stored or resolved control endpoint, not to wherever the last datagram came from
* Fix: NAT route-back for a `0.0.0.0` data HPAI, per field rather than for the whole endpoint
* Fix: the config channel resends at 10 s / 3 attempts, data stays at 1 s / 1 attempt (03_08_04 H-4.2.11); both used the data timing before
* Fix: the connectionstate heartbeat reports `E_KNX_CONNECTION` while the bus is down instead of claiming a healthy link
* Fix: the session is recorded before a reserved-slot takeover resets it, so the history no longer loses the entry
* Fix: the KNXnet/IP header total length is validated before anything is read from it
* Change: drop the dead `IpTunnelServer::dataRequestToTunnel` (no caller; requests run through `dataRequestToChannelId`) and note the missing Extended-CRI (tunnelling v2 requested-IA) path

### cEMI and device management
* Feature: local Transport Layer over cEMI — `T_Data_Individual` and `T_Data_Connected` message codes `0x4A`/`0x41` are served instead of dropped (AN118, 03_08_04 H-4.3.5/H-4.3.7)
* Feature: `PID_DOWNLOAD_COUNTER` (30) on the Device Object — read-only change token, +1 per download session (armed by a read, spent by the first table load), kept in RAM so the NVM layout and apiVersion are unchanged; a product may persist it in one of its OpenKNX modules
* Fix: `M_PropRead` skips the client-address patch on a `start_index == 0` count read, which returned a patched value where the count belongs
* Fix: `M_PropWrite` guards its address patch against a request with no data
* Fix: `M_PropRead`/`M_PropWrite` frames that are truncated are dropped instead of parsed
* Fix: `M_PropWrite` reports the real failure code
* Fix: `A_PropertyExtDescription_Read` parses the 8-octet APDU correctly, and the response returns the resolved PID and index
* Fix: property writes are bounded against the received payload length; property value reads count elements in the high nibble (`|=` instead of `&=`)
* Fix: the FunctionProperty handlers null-check the property and drop a zero-length PDU
* Fix: the `A_Restart` master-reset erase codes now actually erase instead of answering `0x00` and doing nothing — ResetLinks/ResetAP/ResetParam/ResetIA and both factory resets clear the matching tables, individual address and IP configuration via the tested unload path (03_05_02 3.7.1.2)

### Management APDU bounds
* Fix: `A_MemoryExtended_Read` clamps its response length to the CemiFrame buffer — the previous version could write past it
* Fix: management memory reads are bounded against the NVM size
* Fix: short APDUs are rejected before any length-derived read in the management handlers, including `A_Restart` MasterReset
* Fix: a zero-octet group APDU is dropped before the length underflow
* Fix: `LE_ADDITIONAL_LOAD_CONTROLS` reads are bounded by the payload length, and a short write is dropped before the out-of-bounds read
* Fix: the full variable-length `test_info` is forwarded and the system-broadcast reads are guarded
* Fix: the group-object ASAP is bounded before `GroupObjectTableObject::get()`, and `AssociationTableObject::entryCount()` is guarded until the table is loaded
* Fix: `telegramLengthtTP` subtracts the cEMI additional-info length

### Transport layer
* Fix: an undefined transport control PDU is rejected instead of acted on
* Fix: in the `CONNECTING` state an E20 event closes and then sends A5, per 03_03_04 style 3

### Busmonitor
* Fix: the status octet carries bit-error, truncated and lost, and an rx-frame-buffer overflow counts into the lost-frame status
* Fix: the busmon channel id stays unique against the data channels and the `L_Busmon.ind` sequence is reset on connect
* Fix: refused tunnel connects are recorded
* Fix: busmon-only carrier handling is gated under `OPENKNX_HW_BUSMON`

### Coupler
* Fix: hop count 7 is decremented on closed-media routing (post-AN189)
* Fix: `functionRouteTableControl` is guarded against a short PDU
* Change: the stray system-broadcast `println` is compile-guarded with `KNX_LOG_COUPLER`
* Feature: KNXnet/IP telegram counters (`PID_QUEUE_OVERFLOW_TO_IP`/`_TO_KNX` 72/73, `PID_MSG_TRANSMIT_TO_IP`/`_TO_KNX` 74/75, 03_08_03 2.5.23-2.5.26) on a routing device — saturating, counted on the send paths (every emitted datagram incl. ACKs), read-only and only under `KNX_IS_ROUTER`, so the interface advertises none

### Datapoint types
* Fix: DPT 6 decodes as signed (V8)
* Fix: DPT 225/239 scaling decode rounds the same way the encoder does, so encode-decode round-trips
* Fix: the DPT Locale decode no longer returns a dangling pointer and NUL-terminates
* Fix: DPT 231 (Locale) has a `dataLength()` entry
* Fix: the value accessors read the matching `KNXValue` union member

### Diagnostics and portability
* Feature: `OPENKNX_CON_DIAG` counts confirmation generation on the tunnel path (indications, acks, header drops); off by default
* Feature: `KNX_LOG_TUNNELING` names the failing check on an invalid frame and prints property errors in readable form
* Feature: device type, role and capabilities are decoded once per `MASK_VERSION` instead of at each call site
* Fix: `0x07B0` plus `KNX_TUNNELING` is compile-guarded on SAMD and STM32, which have no IP platform
* Fix: the 711/713 unhandled-APDU confirm log is suppressed on FTC console-only builds
* Doc: the unimplemented requester primitives and the KNX-Secure confirm are annotated in the source
* Change: TPUart dependency pinned to `ec/1.2.0-beta.1`

### KNXnet/IP telegram counters (03_08_03 2.5.23-2.5.26)
* Feature: the four counters exist for the first time — `PID_QUEUE_OVERFLOW_TO_IP/KNX` (72/73) and `PID_MSG_TRANSMIT_TO_IP/KNX` (74/75) were enum values no `.cpp` ever used, so a routing device could not answer them at all
* Feature: `->IP` counts every KNXnet/IP datagram the stack emits — tunnelling, core, device management, ACKs — as 2.5.25 requires, not just routed group telegrams; every `IpTunnelServer` send goes through one helper, so no path escapes the count
* Feature: `->KNX` counts frames the TP link accepted; a rejected transmit queue counts as a loss towards KNX, the IP send limit as a loss towards IP
* Feature: `routedToIp`/`routedToKnx` and `filteredToIp`/`filteredToKnx` record the coupler's routing decision per direction — not in the spec, for the console and the display
* Change: the four spec counters saturate instead of wrapping, as 2.5.23 demands
* Change: the properties are gated on `KNX_IS_ROUTER`, so the parameter object of a tunnelling-only device stays unchanged
* Fix: the saturating increment no longer uses `v++` on a `volatile`, which C++20 deprecates
* Fix: the counter header is included next to the other includes instead of inside `#ifdef ARDUINO_ARCH_RP2040` -- an ESP32 target without `KNX_TUNNELING`, so a plain TP device, lost the declaration while the members stayed and did not compile

## ec/v2.4.0-beta.1 - 2026-08-09

A large feature/robustness release on top of the 2.3.1 base, serving both coupler/router (`0x091A`) and
the new IP-interface (`0x07B0`) products: a KNX IP Interface, HW busmonitor, tunnelling reliability +
conformance, an FTC client and broad memory-safety fixes. All additions follow the KNX specification
(03_08 KNXnet/IP Core/Tunnelling/Management, 03_06 cEMI, 03_06_03 busmonitor).
**BETA:** field-tested on the OAM-IP-Interface and OAM-IP-Router (RP2040 + ESP32).
The additions are grouped below; the upstream v1dev entries follow.

### KNX IP Interface (new)
- `Bau07B0IP`: a KNXnet/IP tunnelling interface on a TP1 line (mask `0x07B0` + `KNX_TUNNELING`) -- a BAU that combines the 07B0 device layer (property/memory/group objects, so ETS can download and use KOs) with an IP data-link layer, cEMI server and tunnel server. Non-routing (never advertises ROUTING), so the HW busmonitor stays spec-conform.
- Tunnel hooks for single-interface KNXnet/IP devices (TP at entity index 0): bus-RX->tunnel, own-TX->tunnel and the `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` gate work without a coupler topology; the interface can answer a tunnel client addressed to itself (self-programming).
- `ftcPacingRate` hook for delivery-rate send pacing.

### HW busmonitor (`OPENKNX_HW_BUSMON`)
- Real hardware busmonitor over an ETS Busmonitor tunnel (NCN passive mode); raw LPDUs wrapped as cEMI `L_Busmon.ind`, runs in parallel with the link layer, no TX/ACK while monitoring.
- `bcu mon` local start/stop toggle with local/ETS dual-owner coordination; a local monitor closes data/config tunnels (exclusive, like ETS); ETS busmon takes over active data tunnels; console echo for `bcu mon`.
- L2-ACK + extended timestamp transferred (03_06_03 `L_Busmon.ind` conformance); honest status octet (lost + frame-error bits); standalone L2 acknowledges and FCS-errored frames passed through raw; `busMonitorFrame` bounded vs `MAX_LPDU`.

### Tunnelling (reliability / conformance)
- Per-tunnel FIFO for reliable server->client requests (never drops CO bursts); session history + read-only introspection; connect/disconnect history grown 10 -> 32.
- KNXnet/IP conformance hardening; dedup device-config requests + reject sub-spec datagrams; L_Data routing guarded on ChannelId (dead config-channel fallback dropped); first free unique IA assigned from the pool (not `addresses[slot]`); frame size computed only when the resend queue is enabled.
- Opt-in additional-IA defence via L2-ACK (`KNX_TUNNEL_IA_DEFENCE`).

### File-Transfer client (`OPENKNX_FTC_CLIENT`, formerly `OPENKNX_FTC` — the old name is gone, rename in your `ini` and `main.cpp`)
- Connectionless File-Transfer client role in the stack; device diagnosis (property-write, memory-read) + connection-oriented scan (reaches old BCU1/BCU2 masks); client-side `A_ADC_Read` (remote bus-voltage); responder source-PA passed to the FunctionPropertyState callback.

### IP / DESCRIPTION_RESPONSE
- IP Current Config DIB (0x04) added to `DESCRIPTION_RESPONSE` (current IP/subnet/gateway/assignment-method without a device-mgmt connection); IP-config DIB info1/info2 offsets corrected for the 0x04 layout; `PID_CURRENT_IP_ASSIGNMENT_METHOD` / `PID_IP_CAPABILITIES` allow 1 element.

### Robustness / memory-safety
- `CemiFrame::valid()` OOB guard; truncated `M_PropRead`/`M_PropWrite` dropped before dereference; inbound routing cEMI validated before forwarding to TP; negative `L_Data.con` on a send-limit drop; all expired tunnel slots reaped (not just the first) + dangling `addresses` pointer fixed; LC-config property pointer guarded (not the always-non-zero default); TpUart `sendFrame` guarded against malloc failure; queue-full log rate-limited (no watchdog-starving print flood).
- Property reads, memory writes and DPT encodes bounded against OOB; FunctionProperty(Ext)/Memory response indications guarded against short-frame over-read; full 254-octet APDU allowed (NPDU length `uint16` + FTC send guard 251); cEMI frame buffer sized for a full 255-byte APDU; real `M_PropWrite` failure code reported; `PropertyValue` read counts elements in the high nibble.

### Transport / performance
- Originator `T_Connect` sets `_connectedTsap` to the peer PA (not 0); IP->TP frames serialized on the stack (removes a per-frame malloc/free on the hot path); dead cEMI copy on the IP tunnel fan-out dropped; `CemiFrame` taken by const reference in the IP encoders; malloc'd cEMI RX buffer `free()`d instead of `delete`d.

### Docs
- README: index, architecture overview + build-flags table.

### Upstream base (v1dev)

- Pin TPUart to ec/v1.2.0-beta.1
- Fix: memory leak of `TPUart::Frame` on discarded TP frames. The frame was not deleted when the transmit queue was full or when the BCU was not connected / in busmonitor mode
- Fix: memory leak in the cEMI server on a negative `M_PropRead` response. The buffer allocated by `propertyValueRead()` was only freed on the positive path, so reading an unknown PID (e.g. during an ETS property scan) leaked it
- Fix: `uniqueSerialNumber()` returned no unique id on RP2350. Use `pico_get_unique_board_id()`, this  is compatible to previous implementation:
  - For RP2040 it will return the same serial as `flash_get_unique_id()` in old implementation
  - For RP2350 it uses an OTP-based unique chip ID
- deactivate by default not KNX-Standard compatible 'Tunnel-Optimization" which reduces traffic on TP (can be used with compiler option KNX_TUNNELING_NO_TUNNEL_PA_ON_TP and KNX_TUNNELING_STRICT_TOPOLOGY)
- refactor KNX IP Tunneling
- the Device Management Connection is now handled independently of Tunnel Connections and do not consume a Tunnel PA anymore
  new compiler option KNX_TUNNELING_DEVMGMT (default = 1) to set the number of available Device Management Connections
- new Compiler Option KNX_ROUTING_BC_DC: Unicast packets from the device itself is sent to both interfaces (IP and TP in case of 0x091A)
- change default PID_MAX_APDU_LENGTH_ROUTER from 220 to 254
- fix broken ConfigReq Responses
- fix programming application when FlashTablesInvalid for 0x091A
- add GroupObject::valueCompareTime() to send a GroupObject only when value changed or after some time without sending

## v2.3.1 - 2026-03-04

- Hotfix: DPT16 was not correctly handled for uninitialized KOs

## v2.3.0 - 2026-01-28
- Fix Define for 'DPT_FlowRate_m3/h'
- Enhance multicast initialization logging
- Allow write to hidden KO for DPT of size > 1 Byte
- Add function paramString to access string parameters
- Update TPUart dependency to version 1.0.4


## v2.2.2 - 2025-10-21
- Fix: DPT subgroup 0 handling

## v2.2.1 - 2025-08-22
- Fix: Distinguish between tunnel and TP PAs when reading PID_ADDITIONAL_INDIVIDUAL_ADDRESSES. This resulted in a failed PA assignment for 0x091A devices with KNX_TUNNELING
- Fix: set repeat correctly in DataLinkLayer when sending to other mediums as TP (https://github.com/OpenKNX/knx/issues/40)
- Fix: `dataConReceived` is not suppressed anymore. This prevented sending device reset telegrams.
- Fix: Update TPUart lib to 1.0.2
- Fix: Add individual address handling in TpUartDataLinkLayer
- Fix: [Unload application was not permanent](https://github.com/thelsing/knx/issues/144)
- Extend documentation for KO-state

## V2.2.0 - 2025-07-04
- Fix [#30](https://github.com/OpenKNX/knx/pull/30): Unexpected behaviour of `GroupObject` on failed conversion to DPT
  - `GroupObject::value[No]SendCompare(..)` resulted in value 0 (and returned change based on this value)
  - `GroupObject::valueNoSend(..)` updated state from unitialized to OK, without updating the value
  - `GroupObject::value(..)` wrote to GA without setting the KO value
- Extension [#30](https://github.com/OpenKNX/knx/pull/30): Return successful conversion to DPT on values update operations in `GroupObject` (changed result-type of some methods from `void` to `bool`) 
- only set pinMode of Prog button pin if valid (PROG_BUTTON_PIN >= 0)
- Strings are now \0 terminated in group objects (#25)
- change defines in the rp2040 plattform for LAN / WLAN usage to KNX_IP_LAN or KNX_IP_WIFI, remove KNX_IP_GENERIC
- better Routing and Tunneling support
- add DPT 27.001
- increase device object api version to 2 (invalidation of knx flash data stored by older versions)
- add #pragma once to Arduino plattform to allow derived plattforms
- change esp32 plattform to use KNX_NETIF
- remove examples to deprecated plattforms, update remaining examples
- use tpuart library (https://github.com/OpenKNX/tpuart)

## V2.1.2 - 2024-12-09
- adds unicast auto ack

## V2.1.1 - 2024-09-16
- fix minor bug in TP-Uart Driver (RX queue out of boundary)

## V2.1.0 - 2024-07-03
- complete rework of the TPUart DataLinkLayer with support interrupt-based handling and optimized queue handling
- added DMA support for RP2040 platform
- fix some issues with continous integration causing github actions to fail
- added rp2040 plattform to knx-demo example
- added bool GroupObject::valueCompare method for only sending the value when it has changed 

## V2.0.0 - 2024-02-13
- first OpenKNX version
