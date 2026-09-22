//*******************************************************************************************
//                                   RCDTest.c
//   Project Code: UEFI
//   Module Name : RCDTest
//
//   Usage:
//     RCDTest.efi scan                                     Probe RCD addresses (0x58..0x5F)
//     RCDTest.efi read -c <ctrl> -ch <ch> -d <dimm>        Read all control words (RW00~RW5F)
//     RCDTest.efi read -c <ctrl> -ch <ch> -d <dimm> -r <reg>   Read one control word
//     RCDTest.efi write -c <ctrl> -ch <ch> -d <dimm> -r <reg> -data <8hex>
//     RCDTest.efi -h                                       Show help
//
//   References:
//     JEDEC JESD82-513 (DDR5 RCD) -- sideband control word read/write protocol sec 7.5.7/7.5.8,
//                                     control word space sec 8 (RW00~RW5F direct, RW60~RW7F paged)
//     JEDEC JESD300-5B.01 (SPD5 Hub) -- local device addressing (LID=1011 + HID) Table 5
//     SMBus Specification / EFI_SMBUS_HC_PROTOCOL (UEFI PI)
//
//   Clean-room: every symbol here comes from one of three sources -- JEDEC spec,
//   UEFI/SMBus standard, original code. No vendor NDA'd source is referenced.
//
//*******************************************************************************************/

#include "RCDTest.h"
#include <Protocol/ShellParameters.h>
#include <Protocol/SmbusHc.h>

//
// RCD Sideband Commands (JESD82-513 sec 7.5.3 / 7.5.7 / 7.5.8).
//   Bit 7 = Begin, Bit 6 = End, Bits[3:2] = Internal Command (00=Read DWord,
//   01=Write Byte, 10=Write Word, 11=Write DWord), Bits[1:0] = block/byte mode.
//   I2C mode:  Bits[1:0]=10 selects block mode (Table 49/55) -> 0xC2 / 0xCE.
//   I3C mode:  Bits[1:0] is ignored (always block); BHS MRC uses 0xC0 / 0xCC.
//   This tool drives EFI_SMBUS_HC_PROTOCOL (I2C mode), so use the I2C values.
//
#define RCD_CMD_READ_DWORD    0xC2   // 1100 0010: Read DWord (I2C block mode)
#define RCD_CMD_WRITE_DWORD   0xCE   // 1100 1110: Write DWord (I2C block mode)

//
// RCD control word read Status field (JESD82-513 Table 46)
//
#define RCD_STATUS_OK         BIT0   // Successful
#define RCD_STATUS_ABORT      BIT4   // Internal Target Abort (invalid internal address)

//
// Control word space (JESD82-513 sec 8): RW00~RW5F direct, RW60~RW7F paged via RW5F
//
#define RCD_REG_COUNT_DIRECT  96     // RW00..RW5F
#define RCD_PAGE_DIRECT       0      // page for direct registers

//
// Sideband command setup fields (JESD82-513 sec 7.5.7 / 7.5.8)
//
#define RCD_SUB_CH            0      // SubChannel number (0-based); 0 = subchannel A

//
// DDR5RCD03 Device ID (JESD82-513): DID = 0x0053
//
#define RCD_DID               0x0053

//
// RCD 7-bit address = LID(1011) + HID(slot); slot = Channel*2 + Dimm (JESD300-5B.01 Table 5).
// 8-bit form = 0xB0 + slot*2  (7-bit 0x58 << 1 = 0xB0).
//
#define RCD_SLOT_BASE_ADDR    0xB0   // 8-bit form of 7-bit 0x58 (first DIMM RCD)
#define RCD_SLOT_ADDR_STRIDE  2      // each slot advances 1 in 7-bit => 2 in 8-bit
#define RCD_SLOT_COUNT        8      // 0x58..0x5F (JESD300-5B.01 Table 5)

//
// Global protocol instance -- obtained via LocateProtocol at startup
//
EFI_SYSTEM_TABLE      *mST      = NULL;
EFI_BOOT_SERVICES     *mBS      = NULL;
EFI_SMBUS_HC_PROTOCOL *mSmbusHc = NULL;

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
  L"RCDTest.efi -- DDR5 RCD (Registering Clock Driver) control word read / write tool",
  L"",
  L"Usage:",
  L"  RCDTest.efi scan                                     Probe RCD addresses (0x58..0x5F)",
  L"  RCDTest.efi read -c <ctrl> -ch <ch> -d <dimm>        Read all control words (RW00~RW5F)",
  L"  RCDTest.efi read -c <ctrl> -ch <ch> -d <dimm> -r <reg>    Read one control word",
  L"  RCDTest.efi write -c <ctrl> -ch <ch> -d <dimm> -r <reg> -data <8hex>",
  L"                                                       Write one control word (DWord)",
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
  Params->Reg        = 0xFF;   // sentinel: 0xFF = "no -r given" (read all)

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
    } else if (StrCmp (Argv[Index], L"-r") == 0 && Index + 1 < Argc) {
      Params->Reg = (UINT8)StrHexToUint64 (Argv[++Index]);
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
  Write a block of bytes to an SMBus slave (SMBus block write).
  SlaveAddr is the 8-bit form (7-bit << 1); converted internally to 7-bit.
**/
STATIC
EFI_STATUS
SmbusWriteBlock (
  IN UINT8        SlaveAddr,
  IN UINT8        Command,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  EFI_SMBUS_DEVICE_ADDRESS  Slave;
  UINTN                     Len;

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  Slave.SmbusDeviceAddress = (SlaveAddr >> 1) & 0x7F;
  Len = Length;

  return mSmbusHc->Execute (
                    mSmbusHc,
                    Slave,
                    (UINTN)Command,
                    EfiSmbusWriteBlock,
                    FALSE,
                    &Len,
                    (VOID *)Buffer
                    );
}

/**
  Read a block of bytes from an SMBus slave (SMBus block read).
  On input, *Length is the max buffer size; on output, the actual bytes read.
**/
STATIC
EFI_STATUS
SmbusReadBlock (
  IN  UINT8   SlaveAddr,
  IN  UINT8   Command,
  OUT UINT8   *Buffer,
  IN OUT UINTN *Length
  )
{
  EFI_SMBUS_DEVICE_ADDRESS  Slave;

  if (mSmbusHc == NULL) {
    return EFI_NOT_READY;
  }

  Slave.SmbusDeviceAddress = (SlaveAddr >> 1) & 0x7F;

  return mSmbusHc->Execute (
                    mSmbusHc,
                    Slave,
                    (UINTN)Command,
                    EfiSmbusReadBlock,
                    FALSE,
                    Length,
                    Buffer
                    );
}

//===========================================================================================
//  RCD addressing + control word access (JESD82-513)
//===========================================================================================

/**
  RCD 8-bit SMBus address for a DIMM location.
  slot = Channel*2 + Dimm, addr = 0xB0 + slot*2 (7-bit 0x58 + slot).
**/
STATIC
UINT8
GetRcdAddress (
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
  return (UINT8)(RCD_SLOT_BASE_ADDR + Slot * RCD_SLOT_ADDR_STRIDE);
}

/**
  Read one RCD control word (DWord) via the sideband bus (JESD82-513 sec 7.5.7).

  1. Block write the setup: Sideband command (Read DWord) + Reserved + Dev/Ch + Page + Reg.
  2. Block read back: Status + 4 data bytes (MSB first).
**/
STATIC
EFI_STATUS
RcdReadDword (
  IN  UINT8   Addr8,
  IN  UINT8   Page,
  IN  UINT8   Reg,
  OUT UINT32  *Value
  )
{
  EFI_STATUS  Status;
  UINT8       Setup[4];
  UINT8       Data[5];
  UINTN       Length;

  //
  // 1. Write setup (Sideband command = Read DWord)
  //
  Setup[0] = 0;              // Reserved
  Setup[1] = RCD_SUB_CH;     // Device/Channel (0 = RCD's own control words)
  Setup[2] = Page;           // Page (0 = direct registers RW00~RW5F)
  Setup[3] = Reg;            // Register (RWxx)

  Length = sizeof (Setup);
  Status = SmbusWriteBlock (Addr8, RCD_CMD_READ_DWORD, Setup, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 2. Read back: Status + 4 data bytes (MSB first)
  //
  Length = sizeof (Data);
  Status = SmbusReadBlock (Addr8, RCD_CMD_READ_DWORD, Data, &Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Length < sizeof (Data)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Data[0] = Status, Data[1..4] = DWord (MSB first)
  //
  if ((Data[0] & RCD_STATUS_ABORT) != 0) {
    return EFI_DEVICE_ERROR;   // Internal Target Abort
  }

  *Value = ((UINT32)Data[1] << 24) | ((UINT32)Data[2] << 16) |
           ((UINT32)Data[3] << 8)  |  (UINT32)Data[4];
  return EFI_SUCCESS;
}

/**
  Write one RCD control word (DWord) via the sideband bus (JESD82-513 sec 7.5.8).

  Block write: Sideband command (Write DWord) + Reserved + Dev/Ch + Page + Reg + 4 data bytes.
**/
STATIC
EFI_STATUS
RcdWriteDword (
  IN UINT8   Addr8,
  IN UINT8   Page,
  IN UINT8   Reg,
  IN UINT32  Value
  )
{
  UINT8   Data[8];
  UINTN   Length;

  Data[0] = 0;                        // Reserved
  Data[1] = RCD_SUB_CH;               // Device/Channel (0 = RCD's own control words)
  Data[2] = Page;                     // Page
  Data[3] = Reg;                      // Register (RWxx)
  Data[4] = (UINT8)(Value >> 24);     // Data[31:24] (MSB first)
  Data[5] = (UINT8)(Value >> 16);     // Data[23:16]
  Data[6] = (UINT8)(Value >> 8);      // Data[15:8]
  Data[7] = (UINT8)(Value);           // Data[7:0]

  Length = sizeof (Data);
  return SmbusWriteBlock (Addr8, RCD_CMD_WRITE_DWORD, Data, Length);
}

//===========================================================================================
//  Register name lookup (JESD82-513 sec 8.6 ~ 8.8, the "Global" control words)
//===========================================================================================

typedef struct {
  UINT8         Reg;
  CONST CHAR16  *Name;
} RCD_REG_NAME;

STATIC CONST RCD_REG_NAME gRcdRegNames[] = {
  { 0x00, L"Global Features" },
  { 0x01, L"Parity/CMD Blocking/Alert" },
  { 0x02, L"Host Interface Training Mode" },
  { 0x03, L"DRAM & DB Interface Training Modes" },
  { 0x04, L"Command Space" },
  { 0x05, L"DIMM Operating Speed" },
  { 0x06, L"Fine Granularity Speed" },
  { 0x07, L"Validation Pass-Through/Lockout" },
  { 0xFF, NULL },
};

STATIC
CONST CHAR16 *
LookupRcdRegName (
  IN UINT8  Reg
  )
{
  UINTN  Index;

  for (Index = 0; gRcdRegNames[Index].Reg != 0xFF; Index++) {
    if (gRcdRegNames[Index].Reg == Reg) {
      return gRcdRegNames[Index].Name;
    }
  }
  return NULL;
}

//===========================================================================================
//  Commands: scan / read / write
//===========================================================================================

/**
  Scan all 8 RCD addresses (0x58..0x5F) and report which DIMMs have an RCD.
  Probe = read RW00 (Global Features); a successful read means the RCD is present.
**/
STATIC
VOID
RunScan (
  VOID
  )
{
  UINT8       Slot;
  UINT8       Addr8;
  UINT32      Value;
  EFI_STATUS  Status;
  UINT8       PresentCount;

  Print (L"\n===== RCD Scan (addresses 0x58..0x5F) =====\n");

  PresentCount = 0;
  for (Slot = 0; Slot < RCD_SLOT_COUNT; Slot++) {
    Addr8 = (UINT8)(RCD_SLOT_BASE_ADDR + Slot * RCD_SLOT_ADDR_STRIDE);
    Status = RcdReadDword (Addr8, RCD_PAGE_DIRECT, 0x00, &Value);
    if (!EFI_ERROR (Status)) {
      Print (L"  DIMM %d (0x%02X): RCD present (RW00=0x%08X)\n", Slot, 0x58 + Slot, Value);
      PresentCount++;
    }
  }

  Print (L"  Total: %d RCD(s) present\n", PresentCount);
  Print (L"=====================================\n");
}

/**
  Read one or all RCD control words and display them.
**/
STATIC
EFI_STATUS
RunRead (
  IN APP_PARAMS  *Params
  )
{
  UINT8           Addr8;
  UINT8           Reg;
  UINT32          Value;
  EFI_STATUS      Status;
  CONST CHAR16    *Name;

  Addr8 = GetRcdAddress (Params->Controller, Params->Channel, Params->Dimm);
  if (Addr8 == 0) {
    Print (L"[RCD] RCD address is 0 (empty slot or unconfigured)\n");
    return EFI_DEVICE_ERROR;
  }

  if (Params->Reg != 0xFF) {
    //
    // Read one control word
    //
    Status = RcdReadDword (Addr8, RCD_PAGE_DIRECT, Params->Reg, &Value);
    if (EFI_ERROR (Status)) {
      Print (L"[RCD] Read RW%02X failed: %r\n", Params->Reg, Status);
      return Status;
    }
    Name = LookupRcdRegName (Params->Reg);
    Print (L"  RW%02X = 0x%08X", Params->Reg, Value);
    if (Name != NULL) {
      Print (L"  (%s)", Name);
    }
    Print (L"\n");
  } else {
    //
    // Read all direct control words (RW00~RW5F)
    //
    Print (L"\n===== RCD Control Words (RW00~RW5F) =====\n");
    Print (L"  DIMM: Controller=%d Channel=%d Dimm=%d  (SMBus 0x%02X, 7-bit 0x%02X)\n",
           Params->Controller, Params->Channel, Params->Dimm,
           Addr8, (Addr8 >> 1) & 0x7F);

    for (Reg = 0; Reg < RCD_REG_COUNT_DIRECT; Reg++) {
      Status = RcdReadDword (Addr8, RCD_PAGE_DIRECT, Reg, &Value);
      if (EFI_ERROR (Status)) {
        Print (L"  RW%02X = <read failed %r>\n", Reg, Status);
        continue;
      }
      Name = LookupRcdRegName (Reg);
      Print (L"  RW%02X = 0x%08X", Reg, Value);
      if (Name != NULL) {
        Print (L"  (%s)", Name);
      }
      Print (L"\n");
    }
    Print (L"==========================================\n");
  }

  return EFI_SUCCESS;
}

/**
  Parse a hex string into bytes (write data).
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
  Write one RCD control word (DWord).
**/
STATIC
EFI_STATUS
RunWrite (
  IN APP_PARAMS  *Params
  )
{
  UINT8       Addr8;
  UINT8       Data[4];
  UINT32      Value;
  EFI_STATUS  Status;

  Addr8 = GetRcdAddress (Params->Controller, Params->Channel, Params->Dimm);
  if (Addr8 == 0) {
    Print (L"[RCD] RCD address is 0 (empty slot or unconfigured)\n");
    return EFI_DEVICE_ERROR;
  }

  if (Params->Reg == 0xFF) {
    Print (L"[RCD] -r <reg> is required for write\n");
    return EFI_INVALID_PARAMETER;
  }

  if (Params->Data == NULL || StrLen (Params->Data) < 8) {
    Print (L"[RCD] -data must be 8 hex chars (4 bytes)\n");
    return EFI_INVALID_PARAMETER;
  }

  if (!ParseHexBytes (Params->Data, Data, sizeof (Data))) {
    Print (L"[RCD] -data contains non-hex characters\n");
    return EFI_INVALID_PARAMETER;
  }

  Value = ((UINT32)Data[0] << 24) | ((UINT32)Data[1] << 16) |
          ((UINT32)Data[2] << 8)  |  (UINT32)Data[3];

  Print (L"\n===== RCD Control Word Write =====\n");
  Print (L"  DIMM: Controller=%d Channel=%d Dimm=%d  (SMBus 0x%02X)\n",
         Params->Controller, Params->Channel, Params->Dimm, Addr8);
  Print (L"  RW%02X = 0x%08X\n", Params->Reg, Value);

  Status = RcdWriteDword (Addr8, RCD_PAGE_DIRECT, Params->Reg, Value);
  if (EFI_ERROR (Status)) {
    Print (L"[RCD] Write RW%02X failed: %r\n", Params->Reg, Status);
  } else {
    Print (L"[RCD] Write RW%02X complete\n", Params->Reg);
  }
  Print (L"==================================\n");

  return Status;
}

//===========================================================================================
//  Entry point
//===========================================================================================

EFI_STATUS
EFIAPI
RCDTestEntry (
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
  } else if (StrCaseCmp (Params.Command, L"write") == 0 ||
             StrCaseCmp (Params.Command, L"--write") == 0) {
    Status = RunWrite (&Params);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else {
    Print (L"Unknown command: %s\n", Params.Command);
    PrintHelp ();
  }

  return EFI_SUCCESS;
}
