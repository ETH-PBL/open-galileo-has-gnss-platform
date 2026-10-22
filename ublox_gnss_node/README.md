# ublox_gnss_node — Altium Project

Altium Designer project for the GNSS sensor node PCB (v2.0): STM32H755 MCU,
U-Blox ZED-X20P GNSS module, IMU/eCompass, Ethernet/PoE, dual USB and MikroBUS.

## Structure

- `ublox_gnss_node.PrjPcb` / `.PcbDoc` — main Altium project and board layout.
- `Schematic/` — schematic sheets (one per functional block).
- `Library/` — project-specific schematic/footprint libraries.
- `Documentation/Generate Files/` — exported fabrication outputs: Gerber, GerberX2,
  NC Drill, Pick & Place, PCB 3D Print, PDF, and BOM.

## BOM

`Documentation/Generate Files/BOM/FINAL BOM/` contains the order list, i.e. the
BOM as actually ordered, including the DigiKey/Mouser-split versions used for
ordering.
