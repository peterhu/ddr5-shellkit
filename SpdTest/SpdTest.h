//*******************************************************************************************
//                                   SpdTest.h
//   Project Code: UEFI
//   Module Name : SpdTest
//   Description : Internal header for SpdTest UEFI Shell Application
//*******************************************************************************************/
#ifndef _SPD_TEST_APP_H_
#define _SPD_TEST_APP_H_

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
  BOOLEAN   HexDump;           // -x : also hex-dump raw SPD bytes
  CHAR16    *Command;          // "read"
  UINT8     Controller;
  UINT8     Channel;
  UINT8     Dimm;
} APP_PARAMS;

//
// Function prototypes
//
EFI_STATUS
EFIAPI
SpdTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif // _SPD_TEST_APP_H_
