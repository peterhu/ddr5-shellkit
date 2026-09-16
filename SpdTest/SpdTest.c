//*******************************************************************************************
//                                   SpdTest.c
//   Project Code: UEFI
//   Module Name : SpdTest
//
//   Usage:
//     SpdTest.efi scan                                 Scan all SPD5 Hub addresses (0x50..0x57)
//     SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm>    Read + parse + display SPD
//     SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm> -x Hex dump raw SPD bytes
//     SpdTest.efi -h                                   Show help
//
//   References:
//     JEDEC JESD400-5 (DDR5 SPD Contents)    -- field byte offsets + CRC
//     JEDEC JESD300-5 (SPD5 Hub)             -- MR11 page select, MR0 device ID
//     SMBus Specification / EFI_SMBUS_HC_PROTOCOL
//
//*******************************************************************************************/

#include "SpdTest.h"
#include <Protocol/ShellParameters.h>
#include <Protocol/SmbusHc.h>

//
// DDR5 SPD layout (JESD400-5): 8 blocks x 128 bytes = 1024 bytes
//
#define SPD5_PAGE_SIZE     128   // bytes per MR11 page
#define SPD5_PAGE_COUNT    8     // DDR5 SPD: 8 pages
#define MAX_SPD_BYTE       (SPD5_PAGE_SIZE * SPD5_PAGE_COUNT)

//
// DDR5 SPD5 Hub registers (JESD300-5)
//
#define SPD5_MR11               (0x0B)   // Mode Register 11: page/block select
#define SPD5_MR0                (0x00)   // Mode Register 0: hub device ID
#define SPD5_MR0_HUB_DEVICE_ID  (0x51)   // SPD5 Hub identifier

// Clear BIT7 (MemReg bit) to access register space
#define SPD5_MEMREG_REG(Offset)   ((UINT8) ((~BIT7) & (Offset)))
// Set BIT7 (MemReg bit) to access NVM space
#define SPD5_MEMREG_NVM(Offset)   ((UINT8) (BIT7 | (Offset)))

//
// SPD field byte offsets (JESD400-5)
//
#define SPD_BYTE_MODULE_TYPE       3     // Key Byte / Module Type
#define SPD_BYTE_DENSITY           4     // First SDRAM Density and Package
#define SPD_BYTE_TCKAVG_MIN_LSB    20    // tCKAVG min (ps), LSB
#define SPD_BYTE_TCKAVG_MIN_MSB    21    // tCKAVG min (ps), MSB
#define SPD_BYTE_TAA_LSB           30    // tAA (ps), LSB
#define SPD_BYTE_TAA_MSB           31    // tAA (ps), MSB
#define SPD_BYTE_TRCD_LSB          32    // tRCD (ps), LSB
#define SPD_BYTE_TRCD_MSB          33    // tRCD (ps), MSB
#define SPD_BYTE_TRP_LSB           34    // tRP (ps), LSB
#define SPD_BYTE_TRP_MSB           35    // tRP (ps), MSB
#define SPD_BYTE_TRAS_LSB          36    // tRAS (ps), LSB
#define SPD_BYTE_TRAS_MSB          37    // tRAS (ps), MSB
#define SPD_BYTE_MFG_ID_LSB        512   // Module Manufacturer ID, first byte
#define SPD_BYTE_MFG_ID_MSB        513   // Module Manufacturer ID, second byte
#define SPD_BYTE_PART_NUM_START    521   // Module Part Number (ASCII)
#define SPD_BYTE_PART_NUM_END      550   // Module Part Number (ASCII), inclusive
#define SPD_BYTE_DRAM_MFG_LSB      552   // DRAM Manufacturer ID, first byte
#define SPD_BYTE_DRAM_MFG_MSB      553   // DRAM Manufacturer ID, second byte

//
// Additional organization/width fields (JESD400-5) needed to compute density
//
#define SPD_BYTE_IO_WIDTH          6     // First SDRAM I/O Width (bits 7~5: x4/x8/x16/x32)
#define SPD_BYTE_MODULE_ORG        234   // Module Organization (bit 6 symmetry, bits 5~3 package ranks)
#define SPD_BYTE_BUS_WIDTH         235   // Memory Channel Bus Width (bits 6~5 channels, bits 2~0 width)

//
// CRC fields (JESD400-5)
//
#define SPD_CRC_LSB                510   // CRC for bytes 0..509 (JESD400-5B)
#define SPD_CRC_MSB                511

//
// Global protocol instance -- obtained via LocateProtocol at startup
//
EFI_SYSTEM_TABLE                      *mST      = NULL;
EFI_BOOT_SERVICES                     *mBS      = NULL;
EFI_SMBUS_HC_PROTOCOL                 *mSmbusHc = NULL;

//
// Case-insensitive CHAR16 string comparison (StriCmp in other frameworks;
// EDK2 BaseLib only provides AsciiStriCmp, so we inline a simple version).
//
STATIC
INTN
StrCaseCmp (
  IN CONST CHAR16  *Str1,
  IN CONST CHAR16  *Str2
  )
{
  while (*Str1 != 0 && *Str2 != 0) {
    CHAR16  C1 = (*Str1 >= L'a' && *Str1 <= L'z') ? (*Str1 - (L'a' - L'A')) : *Str1;
    CHAR16  C2 = (*Str2 >= L'a' && *Str2 <= L'z') ? (*Str2 - (L'a' - L'A')) : *Str2;
    if (C1 != C2) {
      return (INTN)(C1 - C2);
    }
    Str1++;
    Str2++;
  }
  return (INTN)(*Str1 - *Str2);
}

//===========================================================================================
//  Help Text
//===========================================================================================

STATIC CONST CHAR16 *gHelpText[] = {
  L"SpdTest.efi -- DDR5 SPD read / parse / verify tool",
  L"",
  L"Usage:",
  L"  SpdTest.efi scan                                     Scan all SPD5 Hub addresses (0x50..0x57)",
  L"  SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm>       Read + parse + display SPD",
  L"  SpdTest.efi read -c <ctrl> -ch <ch> -d <dimm> -x    Also hex-dump raw bytes",
  L"  -h, --help   Show this help",
  NULL
};

STATIC
VOID
PrintHelp (
  VOID
  )
{
  UINTN  Index;
  for (Index = 0; gHelpText[Index] != NULL; Index++) {
    Print (L"%s\n", gHelpText[Index]);
  }
}

STATIC
VOID
ParseCommandLine (
  IN  UINTN       Argc,
  IN  CHAR16      **Argv,
  OUT APP_PARAMS  *Params
  )
{
  UINTN  Index;

  SetMem (Params, sizeof (APP_PARAMS), 0);
  Params->Controller = 0;
  Params->Channel    = 0;
  Params->Dimm       = 0;

  if (Argc < 2) {
    Params->ShowHelp = TRUE;
    return;
  }

  // First positional argument is the command
  Params->Command = Argv[1];

  Index = 2;
  while (Index < Argc) {
    if (StrCmp (Argv[Index], L"-h") == 0 || StrCmp (Argv[Index], L"--help") == 0) {
      Params->ShowHelp = TRUE;
    } else if (StrCmp (Argv[Index], L"-x") == 0) {
      Params->HexDump = TRUE;
    } else if (StrCmp (Argv[Index], L"-c") == 0 && Index + 1 < Argc) {
      Params->Controller = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-ch") == 0 && Index + 1 < Argc) {
      Params->Channel = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-d") == 0 && Index + 1 < Argc) {
      Params->Dimm = (UINT8)StrHexToUint64 (Argv[++Index]);
    }
    Index++;
  }
}

//
// JESD300-5B.01 Table 4: SPD5 Hub 7-bit addr = 0x50 + slot, slot = Channel*2+Dimm (0..7).
// 8-bit form = 0xA0 + slot*2.
//
#define SPD5_SLOT_BASE_ADDR   0xA0    // 8-bit form of 7-bit 0x50 (first DIMM SPD)
#define SPD5_SLOT_ADDR_STRIDE 2       // each slot advances 1 in 7-bit => 2 in 8-bit
#define SPD5_SLOT_COUNT       8       // 0x50..0x57 (JESD300-5B.01 Table 4)

/**
  Standard DDR5 SPD5 Hub address for a DIMM location (JESD300-5B.01 Table 4):
  slot = Channel*2 + Dimm, addr = 0xA0 + slot*2. Controller is per-SMBus-segment
  and not part of the address here (single EFI_SMBUS_HC_PROTOCOL reaches one segment).
**/
STATIC
UINT8
GetSpdAddress (
  IN UINT8 Controller,
  IN UINT8 Channel,
  IN UINT8 Dimm
  )
{
  UINT8  Slot;

  if (Controller >= 2 || Channel >= 4 || Dimm >= 2) {
    return 0;
  }

  Slot = (UINT8)(Channel * 2 + Dimm);
  return (UINT8)(SPD5_SLOT_BASE_ADDR + Slot * SPD5_SLOT_ADDR_STRIDE);
}

/**
  Write a byte to SPD Hub register via SMBus (single byte write).
**/
STATIC
EFI_STATUS
SmbusWriteByte (
  IN UINT8  SlaveAddr,
  IN UINT8  Command,
  IN UINT8  Data
  )
{
  EFI_SMBUS_DEVICE_ADDRESS  Slave;
  UINTN                     Length;

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  //
  // Convert packed SPD address (e.g. 0xA0 = 0x50 << 1) to 7-bit format
  //
  Slave.SmbusDeviceAddress = (SlaveAddr >> 1) & 0x7F;
  Length = 1;

  return mSmbusHc->Execute (
                    mSmbusHc,
                    Slave,
                    (UINTN)Command,
                    EfiSmbusWriteByte,
                    FALSE,
                    &Length,
                    &Data
                    );
}

/**
  Read a byte from SPD Hub register via SMBus (single byte read).
**/
STATIC
UINT8
SmbusReadByte (
  IN  UINT8      SlaveAddr,
  IN  UINT8      Command,
  OUT EFI_STATUS *Status
  )
{
  EFI_SMBUS_DEVICE_ADDRESS  Slave;
  UINTN                     Length;
  UINT8                     Data;

  if (mSmbusHc == NULL) {
    *Status = EFI_NOT_READY;
    return 0;
  }

  Slave.SmbusDeviceAddress = (SlaveAddr >> 1) & 0x7F;
  Length = 1;
  Data   = 0;

  *Status = mSmbusHc->Execute (
                       mSmbusHc,
                       Slave,
                       (UINTN)Command,
                       EfiSmbusReadByte,
                       FALSE,
                       &Length,
                       &Data
                       );
  return Data;
}

/**
  Probe an SPD5 Hub (MR0 == 0x51) at an 8-bit SMBus address (JESD300-5B.01 §2.6.1).
**/
STATIC
BOOLEAN
ProbeSpd5Hub (
  IN UINT8  Addr8
  )
{
  EFI_STATUS  Status;
  UINT8       Mr0;

  SmbusWriteByte (Addr8, SPD5_MEMREG_REG (SPD5_MR11), 0);
  Mr0 = SmbusReadByte (Addr8, SPD5_MEMREG_REG (SPD5_MR0), &Status);
  if (EFI_ERROR (Status) || Mr0 != SPD5_MR0_HUB_DEVICE_ID) {
    return FALSE;
  }
  return TRUE;
}

/**
  Read SPD data from DDR5 SPD5 Hub (JESD300-5):
    1. Write MR11 to select page.
    2. Read NVM bytes from that page (BIT7 set = NVM space).
    3. Verify hub via MR0 == 0x51 before reading.
**/
STATIC
EFI_STATUS
SmbusReadSpdDdr5 (
  IN  UINT8  SpdAddr,
  OUT UINT8  *Buffer,
  IN  UINTN  BufferSize
  )
{
  EFI_STATUS  Status;
  UINT8       Page;
  UINT16      Offset;
  UINTN       Index;
  UINT8       Mr0;

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  //
  // 1. Reset to Page 0
  //
  Status = SmbusWriteByte (SpdAddr, SPD5_MEMREG_REG (SPD5_MR11), 0);
  if (EFI_ERROR (Status)) {
    Print (L"%a: Reset Page 0 failed (Status=%r)\n", __func__, Status);
    return Status;
  }

  //
  // 2. Verify SPD5 Hub (MR0 == 0x51)
  //
  Mr0 = SmbusReadByte (SpdAddr, SPD5_MEMREG_REG (SPD5_MR0), &Status);
  if (EFI_ERROR (Status) || Mr0 != SPD5_MR0_HUB_DEVICE_ID) {
    Print (L"%a: Not SPD5 Hub (MR0=0x%02X, Status=%r)\n", __func__, Mr0, Status);
    return EFI_DEVICE_ERROR;
  }

  //
  // 3. Read each page byte-by-byte
  //
  Index = 0;
  for (Page = 0; Page < SPD5_PAGE_COUNT && Index < BufferSize; Page++) {
    Status = SmbusWriteByte (SpdAddr, SPD5_MEMREG_REG (SPD5_MR11), Page);
    if (EFI_ERROR (Status)) {
      Print (L"%a: Switch to Page %d failed (Status=%r)\n", __func__, Page, Status);
      return Status;
    }

    for (Offset = 0; Offset < SPD5_PAGE_SIZE && Index < BufferSize; Offset++, Index++) {
      Buffer[Index] = SmbusReadByte (SpdAddr, SPD5_MEMREG_NVM ((UINT8)Offset), &Status);
      if (EFI_ERROR (Status)) {
        Print (L"%a: Read Page=%d Offset=%d failed (Status=%r)\n",
                __func__, Page, Offset, Status);
        Buffer[Index] = 0;
      }
    }
  }

  //
  // 4. Reset to Page 0
  //
  SmbusWriteByte (SpdAddr, SPD5_MEMREG_REG (SPD5_MR11), 0);

  return EFI_SUCCESS;
}

/**
  ReadSpd - Read SPD data from DIMM via SMBus.

  @param[in]  Controller  - Memory controller index
  @param[in]  Channel     - Channel number
  @param[in]  Dimm        - DIMM number
  @param[out] SpdData     - Buffer to receive 1024 bytes
  @param[out] DataLen     - DataLen to receive SPD length in bytes

  @retval EFI_SUCCESS           - Success
  @retval EFI_DEVICE_ERROR      - DIMM not present or SPD address is 0
  @retval EFI_INVALID_PARAMETER - Input parameter is invalid
  @retval EFI_NOT_READY         - SMBus not available
**/
EFI_STATUS
EFIAPI
ReadSpd (
  IN  UINT8  Controller,
  IN  UINT8  Channel,
  IN  UINT8  Dimm,
  OUT UINT8  *SpdData,
  OUT UINT32 *DataLen
  )
{
  EFI_STATUS  Status;
  UINT8       SpdAddr;

  if (SpdData == NULL || DataLen == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  SpdAddr = GetSpdAddress (Controller, Channel, Dimm);
  if (SpdAddr == 0) {
    Print (L"%a: SPD address is 0 (empty slot or unconfigured)\n", __func__);
    return EFI_DEVICE_ERROR;
  }

  if (mSmbusHc == NULL) {
    Print (L"%a: SMBus not available\n", __func__);
    return EFI_NOT_READY;
  }

  SetMem (SpdData, MAX_SPD_BYTE, 0);
  Status = SmbusReadSpdDdr5 (SpdAddr, SpdData, MAX_SPD_BYTE);
  if (EFI_ERROR (Status)) {
    Print (L"%a: SPD read failed (Status=%r)\n", __func__, Status);
    return Status;
  }

  *DataLen = MAX_SPD_BYTE;
  return EFI_SUCCESS;
}

//===========================================================================================
//  SPD CRC-16-CCITT (JESD400-5B: single CRC at bytes 510~511 over 0~509)
//===========================================================================================

/**
  CRC-16-CCITT (polynomial 0x1021, init 0), as specified in JESD400-5.
**/
STATIC
UINT16
Crc16 (
  IN CONST UINT8  *Buffer,
  IN UINTN        Count
  )
{
  UINT16  Crc = 0;
  UINT8   Bit;

  while (Count-- > 0) {
    Crc ^= (UINT16)(*Buffer++) << 8;
    for (Bit = 0; Bit < 8; Bit++) {
      if (Crc & 0x8000) {
        Crc = (UINT16)((Crc << 1) ^ 0x1021);
      } else {
        Crc = (UINT16)(Crc << 1);
      }
    }
  }
  return Crc;
}

/**
  Verify the SPD CRC (JESD400-5B: single CRC at 510~511 over bytes 0~509).
**/
STATIC
BOOLEAN
SpdCrcValid (
  IN CONST UINT8  *Spd
  )
{
  UINT16  Calc;
  UINT16  Stored;

  Calc   = Crc16 (Spd, 510);
  Stored = (UINT16)(Spd[SPD_CRC_LSB] | (Spd[SPD_CRC_MSB] << 8));

  return (Calc == Stored);
}

//===========================================================================================
//  SPD Parsing & Display (JESD400-5)
//===========================================================================================

// DDR5 module type decode (JESD400-5B Table 21, byte 3 bits 3:0).
//
STATIC
CONST CHAR16 *
GetModuleTypeString (
  IN UINT8  ModuleType
  )
{
  switch (ModuleType) {
    case 0x01: return L"RDIMM";
    case 0x02: return L"UDIMM";
    case 0x03: return L"SO-DIMM";
    case 0x04: return L"LRDIMM";
    case 0x05: return L"CUDIMM";      // Clocked UDIMM (CKD)
    case 0x06: return L"CSODIMM";     // Clocked SODIMM (CKD)
    case 0x07: return L"MRDIMM";      // Multiplexed Rank DIMM
    case 0x08: return L"CAMM2";       // Compression Attached Memory Module
    case 0x0A: return L"DDIMM";       // Differential DIMM
    case 0x0B: return L"Solder Down";
    default:   return L"Reserved";
  }
}

//
// Common JEDEC manufacturer IDs (module + DRAM share this table for display)
//
typedef struct {
  UINT16         Id;
  CONST CHAR16   *Name;
} MFG_ENTRY;

STATIC CONST MFG_ENTRY gMfgTable[] = {
  { 0x80CE, L"Samsung"   },
  { 0x80AD, L"SK Hynix"  },
  { 0x802C, L"Micron"    },
  { 0x803E, L"Nanya"     },
  { 0x8098, L"Kingston"  },
  { 0x8094, L"SMART"     },
  { 0,      NULL         },
};

STATIC
CONST CHAR16 *
LookupMfgName (
  IN UINT16  Id
  )
{
  UINTN  I;
  for (I = 0; gMfgTable[I].Name != NULL; I++) {
    if (gMfgTable[I].Id == Id) {
      return gMfgTable[I].Name;
    }
  }
  return NULL;
}

/**
  Extract the ASCII Module Part Number (bytes 521..550), trailing spaces trimmed.
**/
STATIC
VOID
GetPartNumber (
  IN  CONST UINT8  *Spd,
  OUT CHAR16       *PartNumber,
  IN  UINTN        PartNumberSize
  )
{
  UINTN  I;
  UINTN  Len = 0;

  for (I = SPD_BYTE_PART_NUM_START; I <= SPD_BYTE_PART_NUM_END; I++) {
    UINT8  Ch = Spd[I];
    if (Ch == 0 || Ch == 0xFF || Ch == ' ') {
      continue;   // skip padding
    }
    if (Len + 1 < PartNumberSize) {
      PartNumber[Len++] = (CHAR16)Ch;
    }
  }
  PartNumber[Len] = 0;

  if (Len == 0) {
    StrCpyS (PartNumber, PartNumberSize, L"(none)");
  }
}

//
// Density decode helpers (JESD400-5 base configuration section)
//

STATIC
UINT16
DensityPerDieGb (
  IN UINT8  Code      // Byte 4 bits 4:0
  )
{
  switch (Code) {
    case 0x01: return 4;    // 4 Gb
    case 0x02: return 8;    // 8 Gb
    case 0x03: return 12;   // 12 Gb
    case 0x04: return 16;   // 16 Gb
    case 0x05: return 24;   // 24 Gb
    case 0x06: return 32;   // 32 Gb
    case 0x07: return 48;   // 48 Gb
    case 0x08: return 64;   // 64 Gb
    default:   return 0;    // reserved / not defined
  }
}

STATIC
UINT8
DiePerPackage (
  IN UINT8  Code      // Byte 4 bits 7:5
  )
{
  switch (Code) {
    case 0: return 1;    // 1 die, monolithic SDRAM
    case 2: return 2;    // 2 die, 2H 3DS
    case 3: return 4;    // 4 die, 4H 3DS
    case 4: return 8;    // 8 die, 8H 3DS
    case 5: return 16;   // 16 die, 16H 3DS
    default: return 0;   // reserved
  }
}

STATIC
UINT16
IoWidthBits (
  IN UINT8  Code      // Byte 6 bits 7:5
  )
{
  switch (Code) {
    case 0: return 4;    // x4
    case 1: return 8;    // x8
    case 2: return 16;   // x16
    case 3: return 32;   // x32
    default: return 0;
  }
}

STATIC
UINT16
PrimaryBusWidthBits (
  IN UINT8  Code      // Byte 235 bits 2:0
  )
{
  switch (Code) {
    case 0: return 8;    // 8-bit
    case 1: return 16;   // 16-bit
    case 2: return 32;   // 32-bit
    case 3: return 64;   // 64-bit
    default: return 0;
  }
}

/**
  Parse and display the SPD contents (JESD400-5).

  Reads a 16-bit ps value from two SPD bytes and prints a timing line.
**/
STATIC
VOID
SpdParse (
  IN CONST UINT8  *Spd
  )
{
  UINT16           TCkAvgMin;
  UINT16           Taa;
  UINT16           Trcd;
  UINT16           Trp;
  UINT16           Tras;
  UINT16           MfgId;
  UINT16           DramMfgId;
  UINT32           FreqMtS;
  UINT32           Cl;
  UINT32           ClTrcd;
  UINT32           ClTrp;
  UINT32           ClTras;
  UINT8            ModuleType;
  UINT8            Symmetry;
  UINT8            DiePkg;
  UINT8            RankCount;
  UINT16           DensityDie;
  UINT16           IoWidth;
  UINT32           DensityGB;
  CONST CHAR16    *ModuleTypeStr;
  CONST CHAR16    *MfgName;
  CONST CHAR16    *DramMfgName;
  CHAR16           PartNumber[64];

  ModuleType    = Spd[SPD_BYTE_MODULE_TYPE] & 0x0F;
  ModuleTypeStr = GetModuleTypeString (ModuleType);

  TCkAvgMin = (UINT16)(Spd[SPD_BYTE_TCKAVG_MIN_LSB] | (Spd[SPD_BYTE_TCKAVG_MIN_MSB] << 8));
  Taa       = (UINT16)(Spd[SPD_BYTE_TAA_LSB]       | (Spd[SPD_BYTE_TAA_MSB]       << 8));
  Trcd      = (UINT16)(Spd[SPD_BYTE_TRCD_LSB]      | (Spd[SPD_BYTE_TRCD_MSB]      << 8));
  Trp       = (UINT16)(Spd[SPD_BYTE_TRP_LSB]       | (Spd[SPD_BYTE_TRP_MSB]       << 8));
  Tras      = (UINT16)(Spd[SPD_BYTE_TRAS_LSB]      | (Spd[SPD_BYTE_TRAS_MSB]      << 8));

  // JEP106 (JESD400-5B §512~513): first byte = continuation (high), second = code (low).
  // Read big-endian.
  MfgId     = (UINT16)((Spd[SPD_BYTE_MFG_ID_LSB]    << 8) | Spd[SPD_BYTE_MFG_ID_MSB]);
  DramMfgId = (UINT16)((Spd[SPD_BYTE_DRAM_MFG_LSB]  << 8) | Spd[SPD_BYTE_DRAM_MFG_MSB]);

  //
  // Frequency: MT/s = 2000000 / tCKAVG_min (ps)
  // CL = tAA / tCKAVG (rounded up)
  //
  FreqMtS = (TCkAvgMin != 0) ? (2000000u / TCkAvgMin) : 0;
  Cl      = (TCkAvgMin != 0) ? ((Taa + TCkAvgMin - 1) / TCkAvgMin) : 0;
  ClTrcd  = (TCkAvgMin != 0) ? ((Trcd + TCkAvgMin - 1) / TCkAvgMin) : 0;
  ClTrp   = (TCkAvgMin != 0) ? ((Trp  + TCkAvgMin - 1) / TCkAvgMin) : 0;
  ClTras  = (TCkAvgMin != 0) ? ((Tras + TCkAvgMin - 1) / TCkAvgMin) : 0;

  //
  // Density: total from GetDensityGB(); decode die/width/rank/symmetry separately
  // for the breakdown string.
  //
  DensityDie = DensityPerDieGb (Spd[SPD_BYTE_DENSITY] & 0x1F);        // Byte 4 bits 4:0
  DiePkg     = DiePerPackage ((Spd[SPD_BYTE_DENSITY] >> 5) & 0x07);   // Byte 4 bits 7:5
  IoWidth    = IoWidthBits ((Spd[SPD_BYTE_IO_WIDTH] >> 5) & 0x07);    // Byte 6 bits 7:5
  RankCount  = (UINT8)(((Spd[SPD_BYTE_MODULE_ORG] >> 3) & 0x07) + 1); // Byte 234 bits 5:3
  Symmetry   = (Spd[SPD_BYTE_MODULE_ORG] >> 6) & 0x01;                // Byte 234 bit 6

  DensityGB = GetDensityGB (Spd);

  MfgName = LookupMfgName (MfgId);
  DramMfgName = LookupMfgName (DramMfgId);
  GetPartNumber (Spd, PartNumber, sizeof (PartNumber) / sizeof (PartNumber[0]));

  //
  // Display
  //
  Print (L"\n===== SPD Summary =====\n");
  Print (L"Module Type      : %s\n", ModuleTypeStr);
  Print (L"Density          : ");
  if (DensityGB != 0) {
    Print (L"%d GB  (%d rank x %d Gb x%d", DensityGB, RankCount, DensityDie, IoWidth);
    if (DiePkg > 1) {
      Print (L", %d-die 3DS", DiePkg);
    }
    Print (L")\n");
  } else if (Symmetry != 0) {
    Print (L"(asymmetrical module -- see raw dump)\n");
  } else {
    Print (L"(unknown / reserved encoding)\n");
  }
  Print (L"Max Speed        : %d MT/s (tCKAVG = %d ps)\n", FreqMtS, TCkAvgMin);
  Print (L"Timings          : CL-%d  tRCD-%d  tRP-%d  tRAS-%d\n",
         Cl, ClTrcd, ClTrp, ClTras);
  Print (L"  (tAA=%d.%02d ns, tRCD=%d.%02d ns, tRP=%d.%02d ns, tRAS=%d.%02d ns)\n",
         Taa / 1000, (Taa % 1000) / 10,
         Trcd / 1000, (Trcd % 1000) / 10,
         Trp / 1000, (Trp % 1000) / 10,
         Tras / 1000, (Tras % 1000) / 10);
  Print (L"Module Mfg       : %s (0x%04X)\n",
         (MfgName != NULL) ? MfgName : L"Unknown", MfgId);
  Print (L"DRAM Mfg         : %s (0x%04X)\n",
         (DramMfgName != NULL) ? DramMfgName : L"Unknown", DramMfgId);
  Print (L"Part Number      : %s\n", PartNumber);
  Print (L"=======================\n");
}

/**
  Compute total DRAM capacity in GB from SPD (JESD400-5).
  Returns 0 for reserved/unknown encoding or asymmetrical modules.
**/
STATIC
UINT32
GetDensityGB (
  IN CONST UINT8  *Spd
  )
{
  UINT8   DensityCode;
  UINT8   DiePkgCode;
  UINT8   IoCode;
  UINT8   BusCode;
  UINT8   ChCode;
  UINT8   RankCode;
  UINT8   Symmetry;
  UINT16  DensityDie;
  UINT8   DiePkg;
  UINT16  IoWidth;
  UINT16  BusWidth;
  UINT8   ChCount;
  UINT8   RankCount;

  DensityCode = Spd[SPD_BYTE_DENSITY] & 0x1F;
  DiePkgCode  = (Spd[SPD_BYTE_DENSITY] >> 5) & 0x07;
  IoCode      = (Spd[SPD_BYTE_IO_WIDTH] >> 5) & 0x07;
  BusCode     = Spd[SPD_BYTE_BUS_WIDTH] & 0x07;
  ChCode      = (Spd[SPD_BYTE_BUS_WIDTH] >> 5) & 0x03;
  RankCode    = (Spd[SPD_BYTE_MODULE_ORG] >> 3) & 0x07;
  Symmetry    = (Spd[SPD_BYTE_MODULE_ORG] >> 6) & 0x01;

  DensityDie = DensityPerDieGb (DensityCode);
  DiePkg     = DiePerPackage (DiePkgCode);
  IoWidth    = IoWidthBits (IoCode);
  BusWidth   = PrimaryBusWidthBits (BusCode);
  ChCount    = (ChCode == 1) ? 2 : 1;
  RankCount  = (UINT8)(RankCode + 1);

  if (Symmetry != 0 || DensityDie == 0 || DiePkg == 0 || IoWidth == 0 || BusWidth == 0) {
    return 0;
  }
  return (UINT32)(((UINT64)ChCount * BusWidth / IoWidth * DiePkg * DensityDie * RankCount) / 8);
}

/**
  Print a one-line SPD summary for a scanned DIMM slot.
**/
STATIC
VOID
SpdScanSummary (
  IN CONST UINT8  *Spd,
  IN UINT8         Slot
  )
{
  UINT8        ModuleType;
  CONST CHAR16 *ModuleTypeStr;
  UINT32       DensityGB;
  CHAR16       PartNumber[64];

  ModuleType    = Spd[SPD_BYTE_MODULE_TYPE] & 0x0F;
  ModuleTypeStr = GetModuleTypeString (ModuleType);

  DensityGB = GetDensityGB (Spd);
  GetPartNumber (Spd, PartNumber, sizeof (PartNumber) / sizeof (PartNumber[0]));

  Print (L"  DIMM %d (0x%02X): %s, %d GB, %s\n",
         Slot, 0x50 + Slot, ModuleTypeStr, DensityGB, PartNumber);
}

/**
  Scan all 8 standard DDR5 SPD5 Hub addresses (0x50..0x57) and report each
  present DIMM (JESD300-5B.01 Table 4).
**/
STATIC
VOID
RunScan (
  VOID
  )
{
  UINT8  Slot;
  UINT8  Addr8;
  UINT8  Spd[MAX_SPD_BYTE];
  UINT8  PresentCount;

  Print (L"\n===== SPD5 Hub Scan (addresses 0x50..0x57) =====\n");

  PresentCount = 0;
  for (Slot = 0; Slot < SPD5_SLOT_COUNT; Slot++) {
    Addr8 = (UINT8)(SPD5_SLOT_BASE_ADDR + Slot * SPD5_SLOT_ADDR_STRIDE);

    if (!ProbeSpd5Hub (Addr8)) {
      continue;   // empty slot -- only report present DIMMs
    }

    SetMem (Spd, sizeof (Spd), 0);
    if (EFI_ERROR (SmbusReadSpdDdr5 (Addr8, Spd, MAX_SPD_BYTE))) {
      Print (L"  DIMM %d (0x%02X): <SPD read failed>\n", Slot, 0x50 + Slot);
      continue;
    }

    PresentCount++;
    SpdScanSummary (Spd, Slot);
  }

  Print (L"  Total: %d DIMM(s) present\n", PresentCount);
  Print (L"=====================================\n");
}

/**
  Hex dump raw SPD bytes.
**/
STATIC
VOID
SpdHexDump (
  IN CONST UINT8  *Spd
  )
{
  UINT32  Index;

  Print (L"\n===== SPD Raw Dump =====\n");
  for (Index = 0; Index < MAX_SPD_BYTE; Index++) {
    Print (L"%02X ", Spd[Index]);
    if (((Index + 1) % 16) == 0) {
      Print (L"\n");
    }
  }
  Print (L"\n========================\n");
}

EFI_STATUS
EFIAPI
SpdTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_SHELL_PARAMETERS_PROTOCOL     *ShellParams;
  APP_PARAMS                        Params;
  UINTN                             Argc;
  CHAR16                            **Argv;
  UINT8                             SpdData[MAX_SPD_BYTE];
  UINT32                            DataLen;

  mST = SystemTable;
  mBS = mST->BootServices;

  Status = mST->BootServices->OpenProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Not launched from UEFI shell -- %r\n", Status);
    return Status;
  }

  Status = mBS->LocateProtocol (&gEfiSmbusHcProtocolGuid, NULL, (VOID **)&mSmbusHc);
  if (EFI_ERROR (Status)) {
    Print (L"%a: SmbusHc not found (Status=%r)\n", __func__, Status);
    mSmbusHc = NULL;
  }

  Argc = ShellParams->Argc;
  Argv = ShellParams->Argv;

  ParseCommandLine (Argc, Argv, &Params);

  if (Params.ShowHelp || Params.Command == NULL) {
    PrintHelp ();
    return EFI_SUCCESS;
  }

  //
  // Dispatch command
  //
  if (StrCaseCmp (Params.Command, L"scan") == 0) {
    RunScan ();
  } else if (StrCaseCmp (Params.Command, L"read") == 0 ||
             StrCaseCmp (Params.Command, L"--read") == 0) {

    Status = ReadSpd (Params.Controller, Params.Channel, Params.Dimm, SpdData, &DataLen);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // CRC check
    //
    if (SpdCrcValid (SpdData)) {
      Print (L"SPD CRC          : OK\n");
    } else {
      Print (L"SPD CRC          : MISMATCH (data may be corrupted)\n");
    }

    //
    // Parse + display
    //
    SpdParse (SpdData);

    if (Params.HexDump) {
      SpdHexDump (SpdData);
    }

  } else {
    Print (L"Unknown command: %s\n", Params.Command);
    PrintHelp ();
  }

  return EFI_SUCCESS;
}
