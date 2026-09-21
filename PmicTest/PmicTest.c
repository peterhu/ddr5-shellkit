//*******************************************************************************************
//                                   PmicTest.c
//   Project Code: UEFI
//   Module Name : PmicTest
//
//   Usage:
//     PmicTest.efi scan                                     Probe PMIC addresses (0x48..0x4F)
//     PmicTest.efi read -c <ctrl> -ch <ch> -d <dimm>        Read PMIC vendor region (R40~R6F)
//     PmicTest.efi burn -c <ctrl> -ch <ch> -d <dimm> -b <blk> -data <32hex>
//     PmicTest.efi -h                                       Show help
//
//   References:
//     JEDEC JESD301-1A.02 (DDR5 PMIC) -- registers R37/R38/R39, vendor region R40~R6F,
//                                         password unlock, burn flow (sec 3.3.3, Table 146)
//     JEDEC JESD300-5B.01 (SPD5 Hub)  -- local device addressing (LID=1001 + HID) Table 5
//     SMBus Specification / EFI_SMBUS_HC_PROTOCOL (UEFI PI)
//
//   Clean-room: every symbol here comes from one of three sources -- JEDEC spec,
//   UEFI/SMBus standard, original code.
//
//*******************************************************************************************/

#include "PmicTest.h"
#include <Protocol/ShellParameters.h>
#include <Protocol/SmbusHc.h>

//
// PMIC registers (JESD301-1A.02)
//
#define PMIC_REG_R37             0x37   // Password Lower Byte (WO)
#define PMIC_REG_R38             0x38   // Password Upper Byte (WO)
#define PMIC_REG_R39             0x39   // Command Codes (RW)

//
// PMIC R39 Command Codes - DIMM Vendor Region (JESD301-1A.02 Table 146)
//
#define PMIC_R39_LOCK            0x00   // Lock DIMM Vendor Region
#define PMIC_R39_UNLOCK          0x40   // Unlock DIMM Vendor Region (password in R37/R38)
#define PMIC_R39_BURN_40_4F      0x81   // Burn block R40~R4F to NVM
#define PMIC_R39_BURN_50_5F      0x82   // Burn block R50~R5F to NVM
#define PMIC_R39_BURN_60_6F      0x85   // Burn block R60~R6F to NVM
#define PMIC_R39_BURN_DONE       0x5A   // Burn complete indicator (read code)

//
// PMIC default password (JESD301-1A.02 sec 3.3)
//
#define PMIC_DEFAULT_PASSWORD_LSB  0x73
#define PMIC_DEFAULT_PASSWORD_MSB  0x94

//
// PMIC DIMM Vendor Region block layout (JESD301-1A.02 sec 3.3.3)
//
#define PMIC_VENDOR_BLOCK_40      0x40   // R40~R4F: power-on sequence + voltage/threshold + mode
#define PMIC_VENDOR_BLOCK_50      0x50   // R50~R5F: current limit + power-off sequence + soft start
#define PMIC_VENDOR_BLOCK_60      0x60   // R60~R6F: reserved
#define PMIC_VENDOR_BLOCK_SIZE    16     // 16 registers per block
#define PMIC_VENDOR_TOTAL_SIZE    48     // 3 blocks x 16 bytes

//
// PMIC voltage-setting registers (JESD301-1A.02 Table 156/160/162).
// NOTE: VDD/VDDQ and VPP use different voltage bases!
//
#define PMIC_REG_R45             0x45   // SWA (VDD)  voltage setting
#define PMIC_REG_R49             0x49   // SWC (VDDQ) voltage setting
#define PMIC_REG_R4B             0x4B   // SWD (VPP)  voltage setting
#define PMIC_VOLT_BASE_VDD_MV    800    // VDD/VDDQ: 800 mV base, 5 mV/step (600 mV for low-power)
#define PMIC_VOLT_BASE_VPP_MV    1500   // VPP: 1500 mV base, 5 mV/step (2200 mV for NVDIMM)

//
// PMIC burn polling (JESD301-1A.02 sec 3.3.3: 200 ms per page)
//
#define PMIC_BURN_POLL_INTERVAL_US  (10 * 1000)   // 10 ms between R39 polls
#define PMIC_BURN_TIMEOUT_US        (500 * 1000)  // 500 ms max

//
// PMIC 7-bit address = LID(1001) + HID(slot); slot = Channel*2 + Dimm (JESD300-5B.01 Table 5).
// 8-bit form = 0x90 + slot*2  (7-bit 0x48 << 1 = 0x90; PMIC LID 1001 is one below SPD LID 1010).
//
#define PMIC_SLOT_BASE_ADDR   0x90   // 8-bit form of 7-bit 0x48 (first DIMM PMIC)
#define PMIC_SLOT_ADDR_STRIDE 2      // each slot advances 1 in 7-bit => 2 in 8-bit
#define PMIC_SLOT_COUNT       8      // 0x48..0x4F (JESD300-5B.01 Table 5)

//
// SPD5 Hub mode detection (JESD300-5B.01 sec 2.6.3 / 2.6.4, Table 117 MR18):
//   SETAASA CCC -> I3C Basic (MR18[5]=1); RSTDAA CCC -> I2C (MR18[5]=0).
// The hub sits on the same host SMBus as the PMIC, at 0xA0 + slot*2 (7-bit 0x50 + slot).
//
#define SPD5_SLOT_BASE_ADDR   0xA0   // 8-bit form of 7-bit 0x50 (SPD5 Hub)
#define SPD5_SLOT_ADDR_STRIDE 2
#define SPD5_MR18              0x12  // Mode Register 18: interface mode ([5]), read ptr ([4],[3:2])
#define SPD5_MR18_I3C_MODE     BIT5  // [5]=1 -> I3C Basic; [5]=0 -> I2C
#define SPD5_MEMREG_REG(Off)   ((UINT8)((~BIT7) & (Off)))  // clear BIT7 = MR register space

//
// Global protocol instance -- obtained via LocateProtocol at startup
//
EFI_SYSTEM_TABLE                      *mST      = NULL;
EFI_BOOT_SERVICES                     *mBS      = NULL;
EFI_SMBUS_HC_PROTOCOL                 *mSmbusHc = NULL;

//
// Case-insensitive CHAR16 string comparison (EDK2 BaseLib only provides
// AsciiStriCmp, so we inline a simple version).
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
  L"PmicTest.efi -- DDR5 PMIC read / burn tool (DIMM vendor region R40~R6F)",
  L"",
  L"Usage:",
  L"  PmicTest.efi scan                                       Probe PMIC addresses (0x48..0x4F)",
  L"  PmicTest.efi read -c <ctrl> -ch <ch> -d <dimm>          Read PMIC vendor region (R40~R6F)",
  L"  PmicTest.efi burn -c <ctrl> -ch <ch> -d <dimm>          Burn one block to PMIC NVM",
  L"                    -b <blk> -data <32 hex chars>          blk: 0=R40~R4F 1=R50~R5F 2=R60~R6F",
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
  Params->Block      = 0;

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
    } else if (StrCmp (Argv[Index], L"-c") == 0 && Index + 1 < Argc) {
      Params->Controller = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-ch") == 0 && Index + 1 < Argc) {
      Params->Channel = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-d") == 0 && Index + 1 < Argc) {
      Params->Dimm = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-b") == 0 && Index + 1 < Argc) {
      Params->Block = (UINT8)StrHexToUint64 (Argv[++Index]);
    } else if (StrCmp (Argv[Index], L"-data") == 0 && Index + 1 < Argc) {
      Params->Data = Argv[++Index];
    }
    Index++;
  }
}

//===========================================================================================
//  SMBus primitives (EFI_SMBUS_HC_PROTOCOL, UEFI PI)
//===========================================================================================

/**
  Write a byte to a PMIC register via SMBus (single byte write).
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
  // Convert packed 8-bit address (e.g. 0x90 = 0x48 << 1) to 7-bit format
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
  Read a byte from a PMIC register via SMBus (single byte read).
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

//===========================================================================================
//  PMIC addressing (JESD300-5B.01 Table 5: LID 1001 = PMIC, HID = slot)
//===========================================================================================

/**
  PMIC 8-bit SMBus address for a DIMM location.
  slot = Channel*2 + Dimm, addr = 0x90 + slot*2 (7-bit 0x48 + slot).
**/
STATIC
UINT8
GetPmicAddress (
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
  return (UINT8)(PMIC_SLOT_BASE_ADDR + Slot * PMIC_SLOT_ADDR_STRIDE);
}

/**
  Read the SPD5 Hub's interface mode (I2C vs I3C Basic) for a slot (MR18[5]).
**/
STATIC
CONST CHAR16 *
GetHubModeString (
  IN UINT8  Slot
  )
{
  EFI_STATUS  Status;
  UINT8       HubAddr8;
  UINT8       Mr18;

  HubAddr8 = (UINT8)(SPD5_SLOT_BASE_ADDR + Slot * SPD5_SLOT_ADDR_STRIDE);
  Mr18 = SmbusReadByte (HubAddr8, SPD5_MEMREG_REG (SPD5_MR18), &Status);
  if (EFI_ERROR (Status)) {
    return L"unknown";
  }
  return (Mr18 & SPD5_MR18_I3C_MODE) ? L"I3C Basic" : L"I2C";
}

/**
  Block number -> block start register + burn command (JESD301-1A.02 sec 3.3.3).
**/
STATIC
UINT8
GetBlockStart (
  IN UINT8  Block
  )
{
  switch (Block) {
    case 0:  return PMIC_VENDOR_BLOCK_40;
    case 1:  return PMIC_VENDOR_BLOCK_50;
    case 2:  return PMIC_VENDOR_BLOCK_60;
    default: return 0xFF;
  }
}

STATIC
UINT8
GetBlockBurnCmd (
  IN UINT8  Block
  )
{
  switch (Block) {
    case 0:  return PMIC_R39_BURN_40_4F;
    case 1:  return PMIC_R39_BURN_50_5F;
    case 2:  return PMIC_R39_BURN_60_6F;
    default: return 0xFF;
  }
}

//===========================================================================================
//  PMIC vendor region read / burn (JESD301-1A.02 sec 3.3)
//===========================================================================================

/**
  Unlock the PMIC DIMM Vendor Region (R40~R6F) with the default password 0x9473.
**/
STATIC
EFI_STATUS
PmicUnlock (
  IN UINT8  Addr8
  )
{
  EFI_STATUS  Status;

  Status = SmbusWriteByte (Addr8, PMIC_REG_R37, PMIC_DEFAULT_PASSWORD_LSB);
  if (EFI_ERROR (Status)) {
    Print (L"[PMIC] Write R37 (password LSB) failed: %r\n", Status);
    return Status;
  }
  Status = SmbusWriteByte (Addr8, PMIC_REG_R38, PMIC_DEFAULT_PASSWORD_MSB);
  if (EFI_ERROR (Status)) {
    Print (L"[PMIC] Write R38 (password MSB) failed: %r\n", Status);
    return Status;
  }
  Status = SmbusWriteByte (Addr8, PMIC_REG_R39, PMIC_R39_UNLOCK);
  if (EFI_ERROR (Status)) {
    Print (L"[PMIC] Unlock (R39=0x40) failed: %r\n", Status);
    return Status;
  }
  return EFI_SUCCESS;
}

/**
  Lock the PMIC DIMM Vendor Region and clear the password registers.
**/
STATIC
VOID
PmicLock (
  IN UINT8  Addr8
  )
{
  SmbusWriteByte (Addr8, PMIC_REG_R39, PMIC_R39_LOCK);
  SmbusWriteByte (Addr8, PMIC_REG_R37, 0);
  SmbusWriteByte (Addr8, PMIC_REG_R38, 0);
}

/**
  Read the PMIC DIMM Vendor Region (R40~R6F, 48 bytes).
  Requires unlock first (both read and write are blocked when locked).
**/
STATIC
EFI_STATUS
PmicVendorRead (
  IN  UINT8  Addr8,
  OUT UINT8  VendorData[PMIC_VENDOR_TOTAL_SIZE]
  )
{
  EFI_STATUS  Status;
  UINT8       RegAddr;

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  Status = PmicUnlock (Addr8);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read R40~R6F (48 bytes)
  //
  SetMem (VendorData, PMIC_VENDOR_TOTAL_SIZE, 0);
  for (RegAddr = PMIC_VENDOR_BLOCK_40; RegAddr <= 0x6F; RegAddr++) {
    VendorData[RegAddr - PMIC_VENDOR_BLOCK_40] =
      SmbusReadByte (Addr8, RegAddr, &Status);
    if (EFI_ERROR (Status)) {
      Print (L"[PMIC] Read R0x%02X failed: %r\n", RegAddr, Status);
      VendorData[RegAddr - PMIC_VENDOR_BLOCK_40] = 0xFF;
    }
  }

  PmicLock (Addr8);
  return EFI_SUCCESS;
}

/**
  Burn one DIMM Vendor Region block (16 bytes) to PMIC NVM (JESD301-1A.02 sec 3.3.3).
**/
STATIC
EFI_STATUS
PmicVendorBurn (
  IN UINT8        Addr8,
  IN UINT8        Block,
  IN CONST UINT8  Data[PMIC_VENDOR_BLOCK_SIZE]
  )
{
  EFI_STATUS  Status;
  UINT8       BlockStart;
  UINT8       BurnCmd;
  UINT8       R39;
  UINTN       Index;
  UINTN       ElapsedUs;

  BlockStart = GetBlockStart (Block);
  BurnCmd    = GetBlockBurnCmd (Block);
  if (BlockStart == 0xFF || BurnCmd == 0xFF) {
    Print (L"[PMIC] Invalid block %d (must be 0/1/2)\n", Block);
    return EFI_INVALID_PARAMETER;
  }

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  //
  // 1. Unlock
  //
  Status = PmicUnlock (Addr8);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 2. Write 16 bytes to the block registers
  //
  for (Index = 0; Index < PMIC_VENDOR_BLOCK_SIZE; Index++) {
    Status = SmbusWriteByte (Addr8, (UINT8)(BlockStart + Index), Data[Index]);
    if (EFI_ERROR (Status)) {
      Print (L"[PMIC] Write R0x%02X failed: %r\n", BlockStart + Index, Status);
      goto BurnCleanup;
    }
  }

  //
  // 3. Issue burn command (block-level)
  //
  Status = SmbusWriteByte (Addr8, PMIC_REG_R39, BurnCmd);
  if (EFI_ERROR (Status)) {
    Print (L"[PMIC] Burn command (R39=0x%02X) failed: %r\n", BurnCmd, Status);
    goto BurnCleanup;
  }

  //
  // 4. Poll R39 until it reads 0x5A (burn complete)
  //
  R39       = 0;
  ElapsedUs = 0;
  while (ElapsedUs < PMIC_BURN_TIMEOUT_US) {
    mBS->Stall (PMIC_BURN_POLL_INTERVAL_US);
    ElapsedUs += PMIC_BURN_POLL_INTERVAL_US;
    R39 = SmbusReadByte (Addr8, PMIC_REG_R39, &Status);
    if (!EFI_ERROR (Status) && R39 == PMIC_R39_BURN_DONE) {
      break;
    }
  }

  if (R39 != PMIC_R39_BURN_DONE) {
    Print (L"[PMIC] Burn block %d timed out (R39=0x%02X, Status=%r)\n", Block, R39, Status);
    Status = EFI_TIMEOUT;
  } else {
    Print (L"[PMIC] Burn block %d complete (R39=0x5A)\n", Block);
  }

BurnCleanup:
  PmicLock (Addr8);
  return Status;
}

//===========================================================================================
//  Commands: scan / read / burn
//===========================================================================================

/**
  Probe one PMIC address by reading R39 (ACK = device present).
**/
STATIC
BOOLEAN
PmicProbe (
  IN UINT8  Addr8
  )
{
  EFI_STATUS  Status;
  SmbusReadByte (Addr8, PMIC_REG_R39, &Status);
  return (!EFI_ERROR (Status));
}

/**
  Scan all 8 PMIC addresses (0x48..0x4F) and report which DIMMs have a PMIC.
**/
STATIC
VOID
RunScan (
  VOID
  )
{
  UINT8  Slot;
  UINT8  Addr8;
  UINT8  PresentCount;

  Print (L"\n===== PMIC Scan (addresses 0x48..0x4F) =====\n");

  PresentCount = 0;
  for (Slot = 0; Slot < PMIC_SLOT_COUNT; Slot++) {
    Addr8 = (UINT8)(PMIC_SLOT_BASE_ADDR + Slot * PMIC_SLOT_ADDR_STRIDE);
    if (PmicProbe (Addr8)) {
      Print (L"  DIMM %d (0x%02X): PMIC present, hub mode=%s\n",
             Slot, 0x48 + Slot, GetHubModeString (Slot));
      PresentCount++;
    }
  }

  Print (L"  Total: %d PMIC(s) present\n", PresentCount);
  Print (L"=====================================\n");
}

/**
  Decode a PMIC voltage-setting register to millivolts (JESD301 Table 156/160/162).
  [7:1] = 5 mV/step on top of a rail-specific base (800 mV for VDD/VDDQ, 1500 mV for VPP).
**/
STATIC
UINT16
DecodeVoltageMv (
  IN UINT8   Reg,
  IN UINT16  BaseMv
  )
{
  UINT8  Value = (UINT8)((Reg >> 1) & 0x7F);
  return (UINT16)(BaseMv + Value * 5);
}

/**
  Read + hex-dump the PMIC DIMM Vendor Region.
**/
STATIC
EFI_STATUS
RunRead (
  IN APP_PARAMS  *Params
  )
{
  UINT8        Addr8;
  UINT8        VendorData[PMIC_VENDOR_TOTAL_SIZE];
  EFI_STATUS   Status;
  UINTN        Index;

  Addr8 = GetPmicAddress (Params->Controller, Params->Channel, Params->Dimm);
  if (Addr8 == 0) {
    Print (L"[PMIC] PMIC address is 0 (empty slot or unconfigured)\n");
    return EFI_DEVICE_ERROR;
  }

  Status = PmicVendorRead (Addr8, VendorData);
  if (EFI_ERROR (Status)) {
    Print (L"[PMIC] Vendor region read failed: %r\n", Status);
    return Status;
  }

  Print (L"\n===== PMIC Vendor Region (R40~R6F, 48 bytes) =====\n");
  Print (L"  DIMM: Controller=%d Channel=%d Dimm=%d  (SMBus 0x%02X, 7-bit 0x%02X)\n",
         Params->Controller, Params->Channel, Params->Dimm,
         Addr8, (Addr8 >> 1) & 0x7F);
  Print (L"  Hub mode       : %s\n",
         GetHubModeString ((UINT8)(Params->Channel * 2 + Params->Dimm)));

  for (Index = 0; Index < PMIC_VENDOR_TOTAL_SIZE; Index++) {
    Print (L"%02X ", VendorData[Index]);
    if (((Index + 1) % 16) == 0) {
      Print (L"\n");
    }
  }

  Print (L"  Voltage (decoded):\n");
  Print (L"    VDD  (R45, SWA) : %d mV\n",
         DecodeVoltageMv (VendorData[PMIC_REG_R45 - PMIC_VENDOR_BLOCK_40], PMIC_VOLT_BASE_VDD_MV));
  Print (L"    VDDQ (R49, SWC) : %d mV\n",
         DecodeVoltageMv (VendorData[PMIC_REG_R49 - PMIC_VENDOR_BLOCK_40], PMIC_VOLT_BASE_VDD_MV));
  Print (L"    VPP  (R4B, SWD) : %d mV\n",
         DecodeVoltageMv (VendorData[PMIC_REG_R4B - PMIC_VENDOR_BLOCK_40], PMIC_VOLT_BASE_VPP_MV));

  Print (L"  Block 40 (R40~R4F): power-on sequence + voltage/threshold + mode\n");
  Print (L"  Block 50 (R50~R5F): current limit + power-off sequence + soft start\n");
  Print (L"  Block 60 (R60~R6F): reserved\n");
  Print (L"===================================================\n");

  return EFI_SUCCESS;
}

/**
  Parse a 32-char hex string into 16 bytes (burn data).
**/
STATIC
UINT8
HexCharToNibble (
  IN CHAR16  C
  )
{
  if (C >= L'0' && C <= L'9') {
    return (UINT8)(C - L'0');
  }
  if (C >= L'a' && C <= L'f') {
    return (UINT8)(C - L'a' + 10);
  }
  if (C >= L'A' && C <= L'F') {
    return (UINT8)(C - L'A' + 10);
  }
  return 0xFF;
}

STATIC
BOOLEAN
ParseHexBytes (
  IN  CONST CHAR16  *Hex,
  OUT UINT8         *Data,
  IN  UINTN         Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    UINT8  Hi = HexCharToNibble (Hex[Index * 2]);
    UINT8  Lo = HexCharToNibble (Hex[Index * 2 + 1]);
    if (Hi == 0xFF || Lo == 0xFF) {
      return FALSE;
    }
    Data[Index] = (UINT8)((Hi << 4) | Lo);
  }
  return TRUE;
}

/**
  Burn one block of the PMIC DIMM Vendor Region (JESD301-1A.02 sec 3.3.3).
**/
STATIC
EFI_STATUS
RunBurn (
  IN APP_PARAMS  *Params
  )
{
  UINT8        Addr8;
  UINT8        Data[PMIC_VENDOR_BLOCK_SIZE];
  EFI_STATUS   Status;

  Addr8 = GetPmicAddress (Params->Controller, Params->Channel, Params->Dimm);
  if (Addr8 == 0) {
    Print (L"[PMIC] PMIC address is 0 (empty slot or unconfigured)\n");
    return EFI_DEVICE_ERROR;
  }

  if (Params->Block > 2) {
    Print (L"[PMIC] Invalid block %d (must be 0/1/2)\n", Params->Block);
    return EFI_INVALID_PARAMETER;
  }

  if (Params->Data == NULL ||
      StrLen (Params->Data) < (PMIC_VENDOR_BLOCK_SIZE * 2)) {
    Print (L"[PMIC] -data must be 32 hex chars (16 bytes)\n");
    return EFI_INVALID_PARAMETER;
  }

  if (!ParseHexBytes (Params->Data, Data, PMIC_VENDOR_BLOCK_SIZE)) {
    Print (L"[PMIC] -data contains non-hex characters\n");
    return EFI_INVALID_PARAMETER;
  }

  Print (L"\n===== PMIC Burn =====\n");
  Print (L"  DIMM: Controller=%d Channel=%d Dimm=%d  (SMBus 0x%02X)\n",
         Params->Controller, Params->Channel, Params->Dimm, Addr8);
  Print (L"  Block %d (R%02X~R%02X) -> NVM\n",
         Params->Block,
         GetBlockStart (Params->Block),
         GetBlockStart (Params->Block) + PMIC_VENDOR_BLOCK_SIZE - 1);

  Status = PmicVendorBurn (Addr8, Params->Block, Data);
  Print (L"=====================\n");

  return Status;
}

//===========================================================================================
//  Entry point
//===========================================================================================

EFI_STATUS
EFIAPI
PmicTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_SHELL_PARAMETERS_PROTOCOL     *ShellParams;
  APP_PARAMS                        Params;
  UINTN                             Argc;
  CHAR16                            **Argv;

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
    Status = RunRead (&Params);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else if (StrCaseCmp (Params.Command, L"burn") == 0 ||
             StrCaseCmp (Params.Command, L"--burn") == 0) {
    Status = RunBurn (&Params);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else {
    Print (L"Unknown command: %s\n", Params.Command);
    PrintHelp ();
  }

  return EFI_SUCCESS;
}
