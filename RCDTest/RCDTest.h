//*******************************************************************************************
//                                   RCDTest.h
//   Project Code: UEFI
//   Module Name : RCDTest
//   Description : Internal header for RCDTest UEFI Shell Application
//
//   Clean-room: every symbol here comes from a JEDEC spec (JESD82-513), the
//   UEFI/SMBus standard, or original code. No vendor NDA'd source is referenced.
//*******************************************************************************************/
#ifndef _RCD_TEST_APP_H_
#define _RCD_TEST_APP_H_

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

//
// Parsed command-line parameters
//
typedef struct {
  BOOLEAN   ShowHelp;
  CHAR16    *Command;          // "scan" / "read" / "write"
  UINT8     Controller;
  UINT8     Channel;
  UINT8     Dimm;
  UINT8     Reg;               // -r <reg>: control word register number (RWxx)
  CHAR16    *Data;             // write: 8 hex chars = 4 bytes (DWord)
} APP_PARAMS;

//
// Function prototypes
//
EFI_STATUS
EFIAPI
RCDTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif // _RCD_TEST_APP_H_
