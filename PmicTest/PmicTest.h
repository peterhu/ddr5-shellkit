//*******************************************************************************************
//                                   PmicTest.h
//   Project Code: UEFI
//   Module Name : PmicTest
//   Description : Internal header for PmicTest UEFI Shell Application
//
//   Clean-room: every symbol here comes from a JEDEC spec, the UEFI/SMBus
//   standard, or original code. No vendor NDA'd source is referenced.
//*******************************************************************************************/
#ifndef _PMIC_TEST_APP_H_
#define _PMIC_TEST_APP_H_

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
  BOOLEAN   HexDump;           // -x : also hex-dump raw bytes
  CHAR16    *Command;          // "scan" / "read" / "burn"
  UINT8     Controller;
  UINT8     Channel;
  UINT8     Dimm;
  UINT8     Block;             // burn: 0=Block40 (R40~R4F), 1=Block50, 2=Block60
  CHAR16    *Data;             // burn: 32 hex chars = 16 bytes
} APP_PARAMS;

//
// Function prototypes
//
EFI_STATUS
EFIAPI
PmicTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif // _PMIC_TEST_APP_H_
