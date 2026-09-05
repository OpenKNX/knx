# BAU-per-mask architecture & the IP-Interface tunnelling flags

Audience: a future maintainer of the shared `thelsing/knx` library who is about to touch
`data_link_layer.*`, `ip_data_link_layer.*`, `knx_facade.cpp`, or `config.h`.

Goal: explain the *mask → BAU → network layer → data-link-layer(s)* model so that a change made
for one target does not silently regress another. The library is compiled once **per product** with
a single `MASK_VERSION` (+ feature flags), so every conditional here is a **compile-time** decision.

---

## Convergence of the three IP-Interface audits

Three independent audits (diff/blast-radius, per-target enumeration, router-deep-dive) of the
`ec/ip-interface-bau07B0` changes were run. **All three converged on `SAFE_NO_REGRESSION`.**
None reported a behavioural regression on any shipping non-interface target (0x07B0 plain TP,
0x27B0 RF, 0x57B0 IP, **0x091A router**, 0x2920 coupler, `USE_CEMI_SERVER`/USB without
`KNX_TUNNELING`, non-Arduino/Linux). There is therefore **no OPEN REGRESSION**.

Residual, non-blocking notes the audits raised (documented, not defects):

- **Not literal binary identity on tunnelling builds.** The three `data_link_layer` gates gain an
  `|| _forwardToTunnel` and `IpDataLinkLayer` gains one `if (!_rxRoutingIndications) break;`. These
  are provably-dead branches on non-interface targets (the flags are never mutated there), so
  *runtime behaviour* is identical, but the object code is not byte-for-byte identical.
- **+1 byte RAM per `DataLinkLayer` instance** (the `bool _forwardToTunnel` member is now
  unconditional — see "Why unconditional" below). Negligible, no functional effect.
- **SAMD/STM32 + `0x07B0 + KNX_TUNNELING` is an explicit `#error`** (`knx_facade.h:489-491` and
  `:553-555`): those platforms have no IP stack, so the combo is rejected at compile time with a clear
  message rather than silently mis-selecting a BAU. No such target exists; the combo is nonsensical
  (`Bau07B0` has no IP stack). (Previously a latent edge — now hardened.)

---

## 1. Mask → BAU → network layer → data-link-layer(s)

`MASK_VERSION` (set per product in `platformio.custom.ini`) drives three things:

1. **Which BAU class** `knx_facade.cpp` instantiates (per platform block, inside
   `ARDUINO_ARCH_*` / SAMD / STM32 guards).
2. **Which media** `config.h` enables (`USE_TP` / `USE_RF` / `USE_IP`).
3. **Whether the network layer is a *device* or a *coupler*** — determined by the BAU's base class:
   `BauSystemBDevice` holds a `NetworkLayerDevice`
   (`bau_systemB_device.h:54`), `BauSystemBCoupler` holds a `NetworkLayerCoupler`
   (`bau_systemB_coupler.h:38`).

| MASK_VERSION | Media (`config.h`) | BAU class | Network layer | Data-link layer(s) | Role |
|---|---|---|---|---|---|
| `0x07B0` | `USE_TP` | `Bau07B0` | Device | TP only (index 0) | Plain TP device |
| `0x07B0` **+ `KNX_TUNNELING`** | `USE_TP` + `USE_IP` | **`Bau07B0IP`** | Device | TP (index 0, bus) **+** IP (KNXnet/IP endpoint) | **IP Interface** (tunnelling only, *no routing*) |
| `0x27B0` | `USE_RF` | `Bau27B0` | Device | RF only | RF device |
| `0x57B0` | `USE_IP` | `Bau57B0` | Device | IP only | IP device (KNXnet/IP) |
| `0x091A` | `USE_TP` + `USE_IP` | `Bau091A` | **Coupler** | IP primary (index 0) + TP secondary (index 1) | **IP Router** (routing + tunnelling) — *shipping OAM-IP-Router* |
| `0x2920` | `USE_TP` + `USE_RF` | `Bau2920` | **Coupler** | primary + secondary | TP/RF coupler |

Facade selection lives in `src/knx_facade.h` (the `extern` decls) + `src/knx_facade.cpp` (the
definitions). The `0x07B0` branch is the only one with an inner `#ifdef KNX_TUNNELING` split — in
`knx_facade.h`: RP2040 `:504-508`, ESP8266 `:523-527`, ESP32 `:538-542` (tunnelling → `Bau07B0IP`,
else → `Bau07B0`), mirrored in `knx_facade.cpp` (RP2040 `:59`, ESP8266 `:79`, ESP32 `:95`). SAMD
(`:489-491`) and STM32 (`:553-555`) emit an explicit `#error` for the tunnelling combo (see the note
above). `config.h:50-56` mirrors this: `0x07B0` gets `USE_TP`, and **only** `0x07B0 + KNX_TUNNELING`
additionally pulls in `USE_IP`.

The guard `bau07B0.h` uses is `#if MASK_VERSION == 0x07B0 && !defined(KNX_TUNNELING)`, and
`bau07B0_ip.{h,cpp}` use `#if MASK_VERSION == 0x07B0 && defined(KNX_TUNNELING)`. They are mutually
exclusive, so a 0x07B0 build gets exactly one of the two. `knx_facade.h` includes `bau07B0_ip.h`
unconditionally, but that header's body is fully behind the tunnelling guard, so for every other
target it expands to nothing but `config.h`.

---

## 2. Router vs Interface is the advertised ROUTING service family — NOT the mask

Both `Bau091A` (0x091A) and `Bau07B0IP` (0x07B0+tunnelling) open the KNXnet/IP multicast socket and
answer `SearchRequest` discovery. The difference that makes one a **Router** and the other an
**Interface** is which KNXnet/IP **service families** it advertises in its DIB, in
`knx_ip_search_response.cpp:60-67` (and the extended variant `knx_ip_search_response_extended.cpp:75-82`):

```
Core               : always
DeviceManagement   : always
Tunnelling         : #ifdef KNX_TUNNELING
Routing            : #if MASK_VERSION == 0x091A   <-- gated to the router mask only
```

So a `0x07B0 + KNX_TUNNELING` device advertises **Core + DeviceManagement + Tunnelling** and
reports mask `0x07B0` (`bau07B0_ip.cpp:59`), but **never** advertises `Routing`. Per KNXnet/IP
(03_08_04) that is precisely the definition of a Tunnelling/DeviceManagement endpoint, i.e. an
IP Interface, not a Routing device. **Never key "is this a router?" off anything but the advertised
ROUTING service family (mask 0x091A).** Enabling `USE_IP` or binding the multicast socket does not
make a device a router.

---

## 3. The two data-link-layer flags (the whole IP-Interface mechanism)

`Bau07B0IP` reuses the shared `TpUartDataLinkLayer` (bus) and `IpDataLinkLayer` (KNXnet/IP) that the
coupler also uses. The coupler forwarding/routing logic keys off **entity index** (IP primary = 0,
TP secondary = 1). An interface is a *device* with a **single** bus link at **index 0**, so the
index-based logic does not fit it. Two opt-in flags bridge that gap. **Both default to the
coupler/legacy behaviour**, so every pre-existing target is untouched.

### 3a. `IpDataLinkLayer::enableRoutingIndications(bool)` — default `true`

- Declared `ip_data_link_layer.h:36`; member `bool _rxRoutingIndications = true;` (`:42`).
- Read in the `RoutingIndication` case (`ip_data_link_layer.cpp:114`) as `if (!_rxRoutingIndications)
  break;` (`:118`) — when disabled, inbound IP multicast group frames are dropped before entering the
  local stack.
- **Only** disabler: `bau07B0_ip.cpp:33` (`enableRoutingIndications(false)`).
- Effect: the router and the plain IP device keep it `true` → inbound routing forwarded exactly as
  before. The interface sets it `false` → **no group communication over IP**; the device's own
  group objects are reachable over **TP only**. Correct for a non-routing interface (it advertises
  no routing and emits no `RoutingIndication`).

### 3b. `DataLinkLayer::forwardToTunnel(bool)` — default `false`

- Declared `data_link_layer.h:88`; member `bool _forwardToTunnel = false;` (`:86`).
- Read at three gates in `data_link_layer.cpp`, each changed from `getEntityIndex() == 1` to
  `getEntityIndex() == 1 || _forwardToTunnel`:
  - `:184` — RX: received bus frame → tunnel (`frameReceived`, under `USE_CEMI_SERVER + KNX_TUNNELING`).
  - `:276` — `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP`: suppress a tunnel-PA-destined frame on TP.
  - `:304` — TX: own transmitted frame echoed to the tunnel (under `USE_CEMI_SERVER` **only**).
- **Only** setter: `bau07B0_ip.cpp:36` (`_tpLayer.forwardToTunnel(true)`).
- Effect: the coupler's TP secondary is at index 1, so `getEntityIndex()==1` is already true and the
  OR-term is irrelevant → unchanged. The interface's TP link is at index 0, so it opts in to make
  bus RX + own TX reach the tunnel (giving ETS a full bus view), mirroring what the coupler's
  index-1 secondary does for free.

**Why `_forwardToTunnel` is unconditional (not under `#ifdef KNX_TUNNELING`).** Gate `:304` sits
under `#ifdef USE_CEMI_SERVER` **without** an inner `KNX_TUNNELING` guard (USB/cEMI devices have a
cEMI server but no tunnelling). If the member were tunnelling-only, that USB build would fail to
compile. It is therefore declared unconditionally (comment at `data_link_layer.h:81-85`). On the USB
target it stays `false` (its only setter requires `KNX_TUNNELING`), so the gate evaluates identically
to the old `getEntityIndex()==1`.

### Invariant to preserve

> Both flags default to the legacy value (`_rxRoutingIndications=true`, `_forwardToTunnel=false`)
> and are mutated **only** in `bau07B0_ip.cpp`, which compiles solely for `0x07B0 && KNX_TUNNELING`.
> Do not add setters elsewhere, do not change the defaults, and do not remove the `|| _forwardToTunnel`
> / `!_rxRoutingIndications` terms — that is what keeps every other target byte-for-byte in behaviour.

---

## 4. Why `KNX_TUNNELING_STRICT_TOPOLOGY` / `isRoutedPA()` are wrong for an interface

`isRoutedPA(pa)` (`data_link_layer.cpp:322`) is a **coupler** notion: it masks the destination PA
against the coupler's own line/area subnet mask and returns true when the PA is on a *different*
segment — i.e. reachable via the IP primary rather than the local TP secondary:

```cpp
return (pa & own_sm) != ownpa;   // "belongs to another segment → routed over IP/primary"
```

`KNX_TUNNELING_STRICT_TOPOLOGY` (`data_link_layer.cpp:89-90`) uses `isRoutedPA()` to decide whether a
tunnel frame should be suppressed on TP, assuming a two-segment coupler topology (TP segment on one
side, everything else routed over IP). **An IP Interface has no second segment** — it has one TP link
and no routing at all — so "is this PA routed to another segment?" is meaningless: every reachable PA
is on TP. Applying the coupler's `isRoutedPA()`/STRICT_TOPOLOGY assumption to an interface would
mis-suppress or mis-forward frames.

The interface instead relies on `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` + `forwardToTunnel(true)`, which
key off tunnel-PA membership (`isTunnelingPA`, def `:317`; used e.g. at `:94`, `:278`) rather than segment topology. When
adding topology-based logic, guard it so it stays a coupler-only path; never assume a coupler subnet
model on a single-link device.

---

## 5. Checklist — if you touch `data_link_layer` / `ip_data_link_layer` / `knx_facade` / `config.h`

Re-verify each of these targets (they compile the shared code differently):

1. **0x091A IP router** (shipping OAM-IP-Router) — coupler; TP at index 1; both flags stay default.
   The three DLL gates must still collapse to `getEntityIndex()==1`; inbound `RoutingIndication`
   must still be forwarded. This is the product that must not regress.
2. **0x07B0 + KNX_TUNNELING** (new IP Interface, `Bau07B0IP`) — the *only* target that sets the flags.
3. **0x07B0 plain TP** (no tunnelling) — must still select `Bau07B0`, no IP stack.
4. **0x57B0 IP device** — `_rxRoutingIndications` default-true path must be unchanged.
5. **0x27B0 RF**, **0x2920 TP/RF coupler** — shared DLL, flags at defaults.
6. **`USE_CEMI_SERVER` + USB without `KNX_TUNNELING`** — the *only* build compiling gate `:304`
   without tunnelling; `_forwardToTunnel` must remain an unconditional member or it won't compile.
7. **Any build without `OPENKNX_HW_BUSMON`** — all HW-busmonitor code (`bau091A` bridge, `ip_tunnel_server`
   members, `tpuart_data_link_layer` `IHwBusMonitorDll` base + forwarding, `L_busmon_ind=0x2B`) must
   stay compiled out; confirm class layouts/vtables of `IpTunnelServer` and `TpUartDataLinkLayer` are
   unchanged. `OPENKNX_HW_BUSMON` is defined **only** by OAM-IP-Interface.
8. **Non-Arduino / Linux** — facade instance selection is inside `ARDUINO_ARCH_*` guards; verify the
   mask-based `config.h`/`bau07B0` guards still pick the right BAU.

Two greps that must keep returning a single hit each (the interface BAU) before you ship:

```
grep -rn "forwardToTunnel("        src/   # expect only bau07B0_ip.cpp
grep -rn "enableRoutingIndications(" src/  # expect only bau07B0_ip.cpp
```

---

*Source references are `file:line` against the working tree at the time of writing; re-check line
numbers if the files have moved.*
