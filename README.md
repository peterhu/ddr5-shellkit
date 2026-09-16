# ddr5-shellkit

> Read the "identity card" of your DDR5 DIMM — straight from the UEFI shell.

Every DDR5 module carries a tiny chip called the **SPD5 Hub** that stores its identity card: who made it, how fast it can run, how much memory it holds. `ddr5-shellkit` is a set of UEFI Shell tools that reads that identity card (and the module's other chips — PMIC, temperature sensor, RCD, data buffer) over SMBus, using **only public JEDEC specifications**.

> **Clean-room.** Every symbol in this repo comes from one of three sources: ① a public JEDEC spec, ② the UEFI / SMBus standard, ③ original code. No vendor NDA'd source (e.g. Intel MRC) is referenced or copied.

## Apps

| App | Device | Spec | Status |
| --- | --- | --- | --- |
| [SpdTest](SpdTest/) | SPD parser | JESD400-5B + JESD300-5B.01 | ✅ done |
| PmicMonitor | PMIC | JESD301-1 | planned |
| TempMonitor | Temperature sensor | JESD302-1 | planned |
| RcdTool | Registering clock driver | JESD82-513 | planned |
| DbTool | Data buffer | JESD82 | planned |

## SpdTest — DDR5 SPD parser

Reads the 1024-byte DDR5 SPD from the SPD5 Hub over SMBus, verifies its CRC, and prints a human-readable summary. `scan` probes all 8 standard hub addresses and lists whatever is installed; `read` decodes one DIMM in full.

### Usage

```
SpdTest.efi scan                              # probe 0x50..0x57, list present DIMMs
SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm> # read + parse + verify one DIMM
SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm> -x  # also hex-dump raw bytes
```

### Example output

Real dump from a SK Hynix `HMCG78AHBVA312N` 16 GB DDR5 **CSODIMM** (a clocked SODIMM, with an on-module CKD clock driver).

**`scan`** — who's on the bus:

```
===== SPD5 Hub Scan (addresses 0x50..0x57) =====
  DIMM 0 (0x50): CSODIMM, 16 GB, HMCG78AHBVA312N
  DIMM 2 (0x52): CSODIMM, 16 GB, HMCG78AHBVA312N
  Total: 2 DIMM(s) present
=====================================
```

**`read`** — the full identity card:

```
SPD CRC          : OK

===== SPD Summary =====
Module Type      : CSODIMM
Density          : 16 GB  (1 rank x 16 Gb x8)
Max Speed        : 6410 MT/s (tCKAVG = 312 ps)
Timings          : CL-52  tRCD-52  tRP-52  tRAS-103
  (tAA=16.00 ns, tRCD=16.00 ns, tRP=16.00 ns, tRAS=32.00 ns)
Module Mfg       : SK Hynix (0x80AD)
DRAM Mfg         : SK Hynix (0x80AD)
Part Number      : HMCG78AHBVA312N
=======================
```

**`read -x`** — plus the raw bytes (first 256 shown):

```
SPD CRC          : OK

===== SPD Summary =====
Module Type      : CSODIMM
Density          : 16 GB  (1 rank x 16 Gb x8)
Max Speed        : 6410 MT/s (tCKAVG = 312 ps)
Timings          : CL-52  tRCD-52  tRP-52  tRAS-103
  (tAA=16.00 ns, tRCD=16.00 ns, tRP=16.00 ns, tRAS=32.00 ns)
Module Mfg       : SK Hynix (0x80AD)
DRAM Mfg         : SK Hynix (0x80AD)
Part Number      : HMCG78AHBVA312N
=======================

===== SPD Raw Dump =====
30 12 12 06 04 00 20 62 00 00 00 00 B2 12 0D 00
00 00 00 00 38 01 F2 03 7A ED 07 00 00 00 80 3E
80 3E 80 3E 00 7D 80 BB 30 75 27 01 A0 00 82 00
...
80 AD 01 24 12 12 21 72 2B 48 4D 43 47 37 38 41
48 42 56 41 33 31 32 4E 20 20 20 20 20 20 20 20
...
========================
```

Spot the bytes: `12` = DDR5 SDRAM, `06` = CSODIMM, `80 AD` = SK Hynix, and the ASCII `HMCG78AHBVA312N` part number sits right after it.

## How it works

**SPD5 Hub addressing** — each DDR5 DIMM's hub listens on a fixed SMBus 7-bit address `1010 HID[2:0]` = `0x50..0x57` (JESD300-5B.01 Table 2). The 3-bit HID is set by the **HSA pin**: a resistor ladder (10.0K / 15.4K / … / 196K to GND) encodes the 8 values. Tie HSA *directly* to GND and the hub drops into *Offline Mode* (write-protect override) — which is exactly how factory programmers make SPD writable, and why it stays read-only in a running system.

**Reading** — the 1024-byte NVM is 8 pages × 128 bytes. Write **MR11** to pick a page, then read bytes with **BIT7 (0x80)** set in the command byte. BIT7=0 talks to the hub's MR register space (device ID, temperature, PMIC/RCD/TS bridge); BIT7=1 talks to the NVM.

**CRC** — JESD400-5B defines a *single* CRC-16/CCITT (poly `0x1021`, init `0`) at bytes **510~511** over bytes **0~509**. (An early 0.89 draft split it into two CRCs at 126~127 / 446~447 — the final spec replaced that.)

**Field decode**

| Field | Byte(s) | Decode |
| --- | --- | --- |
| Module type | 3 | JESD400-5B Table 21: RDIMM / UDIMM / SODIMM / LRDIMM / CUDIMM / CSODIMM / MRDIMM / CAMM2 / DDIMM / Solder-down |
| Density | 4, 6, 234, 235 | `GB = channels × (bus/io) × die_per_pkg × density_per_die / 8 × package_ranks` |
| Max speed | 20~21 | `MT/s = 2,000,000 / tCKAVG_min(ps)` |
| Timings | 30~37 | tAA/tRCD/tRP/tRAS in ps → clocks (÷tCKAVG) and ns |
| Manufacturer | 512~513, 552~553 | JEP106: first byte = continuation (high), second = code (low) → read big-endian |
| Part number | 521~550 | ASCII, space-padded |

## Build

Add `SpdTest.inf` to an EDK2 platform `.dsc` under `[Components]`, then build. `EFI_SMBUS_HC_PROTOCOL` must be present at runtime (provided by the platform's SMBus DXE driver).

```
build -p YourPlatformPkg/YourPlatform.dsc -a X64 -t VS2019 -m ShellPkg/Application/SpdTest/SpdTest.inf
```

## Spec references

- JESD400-5B — DDR5 SPD Contents (field offsets, module type Table 21, single CRC at 510~511)
- JESD300-5B.01 — SPD5 Hub (MR11 page select, MR0 device ID 0x51, HSA addressing, Table 2/4/89)
- SMBus Specification / `EFI_SMBUS_HC_PROTOCOL` (UEFI PI)

## License

MIT — see [LICENSE](LICENSE).

---

## 中文说明（简短）

`ddr5-shellkit` 是一组跑在 **UEFI Shell** 里的 DDR5 内存器件工具：直接通过 SMBus 读内存条上的 SPD5 Hub（以及后续的 PMIC/温度传感器/RCD/数据缓冲），全程只用 **JEDEC 公开规范**，不碰任何厂商 NDA 源码（如 Intel MRC）——即「合规分层」。

第一个工具 `SpdTest` 已可用：`scan` 扫描 0x50~0x57 八个标准地址列出在位 DIMM；`read` 读完整 1024 字节 SPD、校验 CRC、打印出模块类型 / 密度 / 最高频率 / 时序 / 厂商 / 料号。上文的输出样例来自一根真实的 SK Hynix 16 GB DDR5 CSODIMM。
