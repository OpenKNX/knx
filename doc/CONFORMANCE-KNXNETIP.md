# KNXnet/IP Konformitäts-Prüfung — knx-Stack (Interface-Build, Mask 0x07B0)

Prüfung dieses `knx`-Stacks gegen den **KNX Standard v3.0.0** (03_08_01 Overview, 03_08_02 Core,
03_08_03 Management, 03_08_04 Tunnelling, 03_06_03 cEMI/EMI). Quellen **nur als Klausel + Seite** (die
PDFs sind © KNX Association, liegen lokal unter `doc/knx-standard/`, gitignored, werden nicht
weitergegeben). Code-Verweise sind `src/knx/…`.

**Geprüftes Produkt:** KNXnet/IP-**Interface** = `Bau07B0IP`, Mask **0x07B0** + `KNX_TUNNELING`, **kein**
ROUTING. Verifiziert gegen `ec/v1dev-ec`. Wo sich der **Router** (0x091A) durch Build-Flags
unterscheidet, ist das vermerkt (siehe „Produkt-Unterschiede").

**Legende:** ✅ **KONFORM** · 🔧 **BEHOBEN/IMPLEMENTIERT** · ⚙️ **AKTIV (build-flag-gated)** · ⏳ **OFFEN**.

---

## ✅ Konform — kein Handlungsbedarf
- **Kein ROUTING auf 0x07B0.** Beworben nur {Core 02, DeviceMgmt 03, Tunnelling 04}; ROUTING 05 nur unter
  Mask 0x091A (`knx_ip_search_response.cpp:60-67`, erweitert `knx_ip_search_response_extended.cpp:75-82`).
  Kernsatz „Interface, kein Router" — und macht den HW-Busmonitor spec-konform. *Core §7.5.4.3 S.26;
  Tunnelling §2.2.4 S.8.*
- **Busmonitor exklusiv** (nur unter `OPENKNX_HW_BUSMON`): eine Busmon-Verbindung; ein zweiter Busmon/Tunnel
  wird abgelehnt (`E_NO_MORE_CONNECTIONS`), Device-Mgmt bleibt erlaubt (`ip_tunnel_server.cpp` `HandleBusMonitorConnect()`,
  :717-724`). *Tunnelling §2.2.4 S.8.*
- **120-s-Server-Timeout** — Inline-Literal `120000` an zwei Stellen (`ip_tunnel_server.cpp` `loop()`, zwei Stellen;
  grep-fragil, ggf. in eine Konstante ziehen). Korrekter Server-Wert, keine Abweichung von den 60 s/10 s des
  **Clients**. *Core §5.4 S.14.*
- **`E_NO_MORE_UNIQUE_CONNECTIONS` (0x25)** statt 0x24, wenn ein Slot frei ist, aber die zuweisbare Tunnel-IA
  nicht eindeutig (`ip_tunnel_server.cpp` `HandleConnectRequest()`). *Tunnelling §2.2.2 S.6-7.*
- **Channel-ID eindeutig über ALLE Slots** (Daten- + Device-Mgmt- + Busmon-Kanal) (`:1002-1009`). *Core §5.3.3 S.13.*
- **Unbekannte Channel-ID → still verwerfen**, kein fehlgeformtes Paket an 0.0.0.0:0 (`:1259-1268, :1198-1206`).
  *Core §5.5 S.14.*
- **120-s-Timer auch durch gültige Tunnel-Daten nachgetriggert** (nach dem Sequenz-Gate; Dup/Out-of-order
  triggern bewusst nicht) (`:1307, :1240, :1111`). *Core §5.4 S.14.*
- **Busmon-Status-Byte Sequenz `& 0x07`** (Lost-Bit via `status & 0xF8` erhalten) — im Busmon-Bau
  `ip_tunnel_server.cpp` `busMonitorFrame()` (nicht in `cemi_frame.cpp`). *cEMI/EMI §3.3.3.2 S.19-20.*
- **Mindestlängen-Prüfung** (Datagramm-Ebene + je Service) verhindert uint16-Underflow / OOB
  (`ip_data_link_layer.cpp:86-103`, Service-Guards `ip_tunnel_server.cpp` `dataIndicationToTunnel()`). *Tunnelling §5.4.6 S.31.*
- **L_Data-Richtungsfilter:** eigene-IA-Frames werden immer von TP gefiltert + zum Tunnel bestätigt
  (`data_link_layer.cpp:85-97`); die routed-PA/tunnel-PA-Filter liegen bewusst hinter
  `KNX_TUNNELING_STRICT_TOPOLOGY` / `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` (nicht unbedingt aktiv).
- Header/Version, alle Service-Type-IDs, HPAI/CRI/CRD/Connection-Header-Layouts, Device-Info- +
  Supported-Service-Families-DIB, empfangsseitiges Sequence-Handling, cEMI-Server-Object samt
  Pflicht-Properties (`cemi_server_object.cpp:13-30`), L2-ACK je **verbundener** Zusatz-IA
  (`bau07B0_ip.cpp:190-204`; Gruppen-ACK-on-behalf abgelehnt), Speicherung/Permanenz/Eindeutigkeit der
  Zusatz-IAs (`ip_parameter_object.cpp:38`). *Core §7; Tunnelling §2.2; cEMI §4.*

## 🔧 Behoben / implementiert (gegenüber dem Erst-Audit)
- **`totalLength()` gegen Datagramm-Länge geprüft** — deklarierte Länge (Okt. 4-5) ≠ `len` → verworfen
  (`ip_data_link_layer.cpp:93-103`). *(war offen)*
- **DESCRIPTION_RESPONSE-Service-Versionen** über die `KNX_SERVICE_FAMILY_*`-Makros statt hart 1
  (`knx_ip_description_response.cpp:62-68`) — konsistent mit SEARCH. *(war offen)*
- **P2P-`L_Data.ind` ohne passende Tunnel-IA** landet nicht mehr auf einem Device-Mgmt-Kanal: nur der Tunnel
  mit `IA == Ziel` wird gewählt, sonst Drop (`ip_tunnel_server.cpp` `dataRequestToChannelId()`). *(war offen)*
- **cEMI-Truncation-Guard / Slot-Reaper (scannt alle Slots) / Dangling-Buffer jetzt unbedingt** — das frühere
  Flag `KNX_FIXES_EC` ist **entfernt** (0 Treffer); Reaper `ip_tunnel_server.cpp` `loop()` (120-s-Timeout),
  Guard `cemi_frame.cpp:399-410`. *(war „fragil hinter Flag")*
- **`M_PropWrite`-Fehlerantwort** — `BauSystemB::property()` (`bau_systemB.cpp:938-942`); `Void_DP` bei
  fehlender, `Read_Only` bei schreibgeschützter Property (`cemi_server.cpp:444-450`). `PID_COMM_MODE` ist
  `writeEnable=false` → M_PropWrite darauf gibt `Read_Only` (`cemi_server_object.cpp:15`).
  *cEMI §4.1.7.3.7.3 S.109.*
- Ferner behoben (Details oben unter ✅): 0x25, Channel-ID-Eindeutigkeit über alle Slots, unbekannte
  Channel-ID still ignorieren, 120-s-Timer durch Tunnel-Daten, Busmon-Sequenz-Maske `& 0x07`,
  Mindestlängen-Prüfung.

## ⚙️ Aktive/optionale Features (build-flag-gated)
- **#1 TUNNELLING_ACK-Zuverlässigkeit** — **AKTIV** hinter `KNX_TUNNEL_RESEND` (in **beiden** Produkten
  gesetzt). **Nicht** mehr Drop-on-busy: eine **Tiefe-3-FIFO je Tunnel** (`knx_ip_tunnel_connection.h:36-39`),
  ein Paket on-the-wire (`pumpTunnel` `:443-458`), Retry aus `loop()` (Daten 1 s ×1, Config 10 s ×3,
  `:190-210`), der ACK-Case gibt den Slot frei (`:475-499`). Bei FIFO-voll: **nur** Best-Effort-Gruppenframes
  verworfen (Verbindung bleibt), ein CO/Mgmt-Overflow → Disconnect (`:401-409`). Damit ist der ursprüngliche
  „eigene, schnell aufeinanderfolgende CO-Antworten verworfen"-Bug behoben. *Tunnelling §2.6.1 S.9.* *(Der
  Audit-Stand „deaktiviert" ist überholt.)*
- **cEMI-Transport-Layer** — **AKTIV im Interface** hinter `KNX_CEMI_TRANSPORT_LAYER`: `T_Data_Individual_req
  0x4A` / `T_Data_Connected_req 0x41` werden lokal bedient (`cemi_server.cpp:191-204, :512-543`;
  `transport_layer.cpp:433-467`), die Antwort geht als `T_Data_*_ind` auf derselben Verbindung raus, nichts
  erreicht den Bus. **Auch im Router gesetzt** (`OAM-IP-Router/platformio.custom.ini:12`, mit Verweis auf
  fn 60202) — die frühere Notiz „Router: Flag nicht gesetzt" ist überholt. *Management §2.6.1.2 S.18; AN118.*
- **Device-Management bei offener Transportverbindung** — abgewiesen wird **nur ein FREMDER** Transport-Partner,
  Vorgabe `KNX_CEMI_TRANSPORT_STRICT` (in `config.h`, **default AUS**) stellt die unbedingte Abweisung her.
  08_TSSH 8.3.2 S.158 (fn 60202) treibt den Prüffall mit einem Partner, den der Prüfling **hereinbekommt**
  (`L_Data.ind 1.1.255 -> 15.15.255 Connect` als ROUTING_INDICATION); die Verbindung gehört also **nicht**
  dem Client, der danach Device Management öffnet. §2.6.1.2 S.18 verlangt gar keine Abweisung, sondern
  beschreibt nur das Umschalten. „Eigen" heißt: die Partneradresse ist die IA eines Tunnels **derselben IP**
  (`ip_tunnel_server.cpp`, DEVICE_MGMT-Zweig). Ohne diese Unterscheidung sperrte sich ETS selbst aus — es
  liest die GA-Tabellen über ein `T_Connect` durch seinen Tunnel und will danach Device Management, bis der
  6-s-Verbindungs-Timeout (03_03_04 §5.1) die vom Client nie getrennte Verbindung freigibt. **HW-verifiziert
  am Router 11.11.0.126:** eigener Tunnel → `E_NO_ERROR`, `1.1.255` über Routing → `E_NO_MORE_CONNECTIONS`.
  Abgelehnte Versuche melden eigenen Grund `END_REJ_TRANSPORT` (vorher als „kein freier Tunnel" sichtbar,
  bei 16 freien Tunneln).
- **#3 M_PropWrite** — aktiv (kein Flag), siehe 🔧.
- **DISCONNECT_RESPONSE (0x20A)** abgefangen (kein „Unhandled"-Log) (`ip_tunnel_server.cpp` `HandleIpFrame()`, `case DisconnectResponse`);
  **Busmon-Kick** schließt laufende Tunnel bei Busmon-Start (`:1367, :1332-1350`, gated `OPENKNX_HW_BUSMON`).
- **#2 Zusatz-IA-Defense** — **COMPILED-OUT** (`KNX_TUNNEL_IA_DEFENCE` in keinem Produkt gesetzt). Der
  aktuelle, geschützte Code ist **nur ein ACK-Bit** (kein fabriziertes T_Disconnect mehr → kein
  TP-Protocol-Error; `bau07B0_ip.cpp:193-199`). Reaktivierung bei Bedarf mit HW-Test. *Tunnelling §2.2.2 S.7.*

## 🆕 Router-spezifische Konformitäts-Arbeit (Mask 0x091A, im selben Stack)
- **Non-Router PID 66 liest 0** — `PID_ROUTING_MULTICAST_ADDRESS` ist im `#else`-Zweig eine read-0-
  CallbackProperty (`ip_parameter_object.cpp:122-137`); der Router behält die echte DataProperty.
- **Config-Tunnel-Resend 10 s / Original+3** via `IsConfig`-Gates (`ip_tunnel_server.cpp` `loop()`, `IsConfig`-Zweige).
- **KNXnet/IP-Telegrammzähler PID 72-75** (QUEUE_OVERFLOW / MSG_TRANSMIT to IP/KNX) als read-only
  CallbackProperties unter `#ifdef KNX_IS_ROUTER` (`ip_parameter_object.cpp:139-169`).

## 🆕 Run State Machine (08_TSSI) — neu implementiert

`PID_RUN_STATE_CONTROL` (PID 6) am Applikationsprogramm-Objekt (Objekttyp 3; Objektindex 4 bei den
Geraete-Masken, 2 beim Koppler 091A - der Index ist geraetespezifisch, ueber PID 1 aufloesen). Vorher existierte nur
der Bezeichner in `property.h`; kein Objekt legte die Eigenschaft an, damit war **08_TSSI gar nicht
prüfbar** — und TSS I ist für ein Gerät mit nicht zertifiziertem Stack pflichtig (*08_01 Kl. 3.3 Fig. 5
S. 10*).

**Lesen** (*03_05_01 Tabelle 95, S. 299*): `3` Terminated nach einem Stop-Ereignis, sonst `0` Halted wenn
das Applikationsobjekt nicht geladen ist und `1` Running. Terminated wird **zuerst** geprüft und gilt auch
im entladenen Zustand — *Tabelle 97, S. 301* führt Stop auch aus Halted nach Terminated, und *08_TSSI
Kl. 2.2.4 S. 7* entlädt die Anwendung und verlangt danach auf Stop die Antwort `03`. Die Zwischenzustände
Ready, Starting und Shutting down erscheinen nie; *08_TSSI Kl. 2.4.1 S. 17* lässt das ausdrücklich zu,
wenn die Anwendung sofort startet.

**Schreiben** (*Tabelle 96, S. 299*): `0` NOP, `1` Restart, `2` Stop. Andere Werte werden ignoriert.
Der Zustand liegt im RAM, wie *Kl. 4.24.2.2 S. 298* verlangt — ein Reset stellt Running wieder her. Ein
Unload-Ereignis löscht ihn ebenfalls, auch wenn das Objekt bereits entladen war (*Tabelle 97, S. 301*).

**Wirkung von Stop, Gerät:** die **Gruppenkommunikation** ruht. `sendNextGroupTelegram()` sendet nicht
mehr, die Group-Value-Indications — Read, Write und Response — werden verworfen, die Anwendung wird also
weder gerufen noch gehört.

**Wirkung von Stop, Koppler:** die **Kopplung** ruht, also jedes Telegramm, das von einer Schnittstelle auf
die andere getragen würde. Ein Koppler hat keine Gruppenobjekte; die Kopplung *ist* sein ausführbarer
Teil. Nicht angehalten werden die lokale Zustellung, die eigenen Telegramme des Kopplers und der
**Tunnel-Pfad**: Tunnelling ist ein KNXnet/IP-Dienst (*03_08_04*), nicht der ausführbare Teil, den
*Kl. 4.24.1 S. 298* meint — und ohne ihn verlöre ein angehaltenes Gerät den ETS-Zugang zur Unterlinie,
der eine Diagnose-Abschaltung erst brauchbar macht. Ein angehaltener Koppler ist also **nicht** still auf
der Unterlinie. Was die Norm dazu sagt: nichts — *03_03_03 Kl. 2.4.2.4.1 S. 12* lässt den Fall offen
(„The use case where the Coupler has itself an Application Program is not considered"). Die Auslegung ist
eine Produktentscheidung, getroffen damit der gemeldete Zustand dem entspricht, was wirklich stillsteht.

Layer 2 quittiert weiterhin, was ein angehaltener Koppler nicht mehr trägt. *03_03_03 Kl. 2.4.2.4.5.1
S. 15* definiert das als IGNORE_ACKED und verlangt, dass die Layer-2-Entscheidung „independent of the
Routing conclusion" ist — es verhält sich also wie ein gewöhnlicher Filtertabellen-Verwurf.

In beiden Fällen bleiben Properties, Speicher, Restart, Programmiermodus und Adressdienste erreichbar —
das ist der Zweck, den die Norm nennt (*S. 299*: „for diagnostic purposes"). Nicht zu verwechseln mit
`BauSystemB::enabled(false)`, das den Data Link Layer abschaltet und das Gerät **komplett** vom Bus nimmt.

**So spricht man sie an** (Management-Client, verbindungsorientiert auf das Gerät):

| | Objektindex | PID | Count | Start | Daten |
|---|---|---|---|---|---|
| `A_PropertyValue_Read` | 4 | 6 | 1 | 1 | — |
| `A_PropertyValue_Write` | 4 | 6 | 1 | 1 | 1 Oktett: 0/1/2 |

Geprüft von `D-26.1` … `D-26.3` der Gerätesuite. **Kein Werkzeug der eigenen Kette kann das heute
bedienen**: `ftc prop` adressiert lokale Objekte der Schnittstelle über IOT+Instanz, nicht den
Objektindex eines entfernten Geräts. Wer das Feature betrieblich nutzen will, braucht einen Client, der
`A_PropertyValue_Write` auf das Applikationsprogramm-Objekt / PID 6 schickt — ETS oder ein eigenes Werkzeug.

---

## ⏳ Noch offen (Backlog: Zert / v2 / Kosmetik)
- **`E_VERSION_NOT_SUPPORTED`** wird bei nicht unterstützter KNXnet/IP-Version nicht gesendet (stilles
  Verwerfen `ip_data_link_layer.cpp:89-91`). Kein Einzeiler — eine korrekte Antwort braucht Service-Typ +
  Endpunkt; geringer Nutzen, breit toleriert. *Core §6.2 S.15.*
- **Klassische SEARCH_RESPONSE ohne Zero-HPAI-Rückweg-Fallback** (`ip_data_link_layer.cpp:140`; CONNECT/
  State/Disconnect haben ihn). Geringe Praxisrelevanz (SEARCH-HPAI ist normalerweise gesetzt). *Core §4.2 S.10.*
- **Tunnelling v2 unvollständig:** das Info-DIB wird beworben und die Slot-Flags werden jetzt live aus der
  Tunnel-Nutzung berechnet (`knx_ip_search_response_extended.cpp:140-197`), **aber** `TUNNELLING_FEATURE_*`
  = 0, der Extended-CRI-Requested-IA-Pfad ist `TODO EC` (`ip_tunnel_server.cpp` `HandleConnectRequest()`, Marker `TODO EC`), die Codes
  `E_NO_TUNNELLING_ADDRESS / E_CONNECTION_IN_USE / E_AUTHORISATION_ERROR` sind definiert aber ungenutzt, und
  der Platzhalter `apduLength=254 //FIXME` steht noch (`…extended.cpp:146`). Vor Zert klären. *Tunnelling §3, §5.4.3.*
- **`PID_MAX_INTERFACE_APDU_LENGTH` (68)** nicht instanziiert (nur Enum `property.h:165`). Bedarf gegen
  03_08_02 prüfen.
- **`PID_COMM_MODE`** wirkungslos (kein Moduswechsel), harmlos bei 00h (Data Link Layer)
  (`cemi_server_object.cpp:15`). *cEMI §4.2.2.4.2 S.115-116.*
- **CemiFrame-`(data,length)`-Ctor** leitet den NPDU-Offset aus `data[1]` (AddIL) ab, auch bei M_*-Frames
  ohne AddIL — latent (`cemi_frame.cpp:77-84`).

## Produkt-Unterschiede Interface (0x07B0) vs Router (0x091A)
| Build-Flag | Interface | Router |
|---|:---:|:---:|
| `KNX_TUNNEL_RESEND` (Tunnel-ACK-FIFO) | ✔ | ✔ |
| `KNX_CEMI_TRANSPORT_LAYER` (0x4A/0x41 lokal) | ✔ | ✘ |
| `OPENKNX_HW_BUSMON` (HW-Busmon + Busmon-Kick) | ✔ | ✘ |
| `KNX_TUNNEL_IA_DEFENCE` (Zusatz-IA-ACK) | ✘ | ✘ |
| `KNX_IS_ROUTER` (PID 72-75, PID 66 real, Routing) | ✘ | ✔ |

## Hinweise zur Zertifizierung
- Für den vollständigen cEMI-/Profil-Abschluss noch **03_06_03** (komplette cEMI-Message-Code-Matrix) und
  **Vol 6 Profiles** (Pflicht-Interface-Objekte) heranziehen — nicht im lokalen Auszug enthalten.
- `E_NO_MORE_CONNECTIONS` für den Busmon-aktiv-Reject ist **vertretbar, nicht vorgeschrieben** (Core
  §5.4.3.3.3 S.30 überlässt den Code der Implementierung) — vor dem Zert-Lab begründen. Die v2-Alternative
  wären die `Usable`-Bits im Tunnelling-Info-DIB.

---
*Stand: verifiziert gegen `ec/v1dev-ec` (drei unabhängige Code-Traces über Core / Tunnelling / Device-Mgmt+
cEMI). Zeilennummern gegen einen bewegten Baum — bei Verschiebung neu prüfen. Änderungen an `src/knx/*` sind
Shared-Lib-Änderungen (Symlink) und betreffen auch OAM-IP-Router.*
