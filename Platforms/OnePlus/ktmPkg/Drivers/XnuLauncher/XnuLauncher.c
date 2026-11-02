/** @file
  XnuLauncher - start the HakiOS xnuloader during DXE.

  Mu-Silicium's BDS does not auto-start FILE APPLICATION entries contained in the
  firmware volume, so this driver finds the embedded xnuloader by GUID, loads it
  with LoadImage() and hands control to it with StartImage().

  Every step also goes to ConOut: the phone has no serial console during bring-up,
  so the screen is the only place these diagnostics can be read.

  The loader is embedded by ktmPkg/Include/RAW.inc as
    FILE APPLICATION = 3E7A9C41-58B2-4D6F-9A03-C1D5E8F24B70 { SECTION PE32 = ... }
  so the GUID below must stay in sync with that stanza.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Protocol/FirmwareVolume2.h>

#define XNU_LOADER_GUID \
  { 0x3E7A9C41, 0x58B2, 0x4D6F, { 0x9A, 0x03, 0xC1, 0xD5, 0xE8, 0xF2, 0x4B, 0x70 } }

STATIC EFI_GUID  mXnuLoaderGuid = XNU_LOADER_GUID;

STATIC
VOID
Say (
  IN CONST CHAR16  *Text
  )
{
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
  DEBUG ((DEBUG_INFO, "%s", Text));
}

/**
  Locate the xnuloader image in any firmware volume.
**/
STATIC
EFI_STATUS
ReadXnuLoader (
  OUT VOID   **Buffer,
  OUT UINTN   *Size
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                   *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  UINT32                        AuthenticationStatus;
  EFI_FV_FILETYPE               FoundType;
  EFI_FV_FILE_ATTRIBUTES        FileAttributes;
  CHAR16                        Line[96];

  *Buffer = NULL;
  *Size   = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Say (L"XnuLauncher: no firmware volume protocol\r\n");
    return Status;
  }

  UnicodeSPrint (Line, sizeof (Line), L"XnuLauncher: %d firmware volume(s)\r\n", (UINT32)HandleCount);
  Say (Line);

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    AuthenticationStatus = 0;
    FoundType            = EFI_FV_FILETYPE_ALL;
    FileAttributes       = 0;
    Status = Fv->ReadFile (
                   Fv,
                   &mXnuLoaderGuid,
                   Buffer,
                   Size,
                   &FoundType,
                   &FileAttributes,
                   &AuthenticationStatus
                   );
    if (!EFI_ERROR (Status) && (*Buffer != NULL) && (*Size != 0)) {
      UnicodeSPrint (
        Line,
        sizeof (Line),
        L"XnuLauncher: loader in FV %d, %d bytes, type %d\r\n",
        (UINT32)Index,
        (UINT32)*Size,
        (UINT32)FoundType
        );
      Say (Line);
      gBS->FreePool (Handles);
      return EFI_SUCCESS;
    }
  }

  gBS->FreePool (Handles);
  Say (L"XnuLauncher: xnuloader NOT found in any FV\r\n");
  return EFI_NOT_FOUND;
}

/**
  Driver entry point: load and start xnuloader.
**/
EFI_STATUS
EFIAPI
XnuLauncherEntry (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID       *Image;
  UINTN       ImageSize;
  EFI_HANDLE  LoaderHandle;
  CHAR16      Line[96];

  Say (L"XnuLauncher: entry\r\n");

  //
  // Bring-up diagnostic: hold here so the message cannot be overwritten by the
  // BDS logo and a screen clear. If this line is never seen, the driver was not
  // dispatched at all. Remove the loop once dispatch is confirmed.
  //
  {
    UINTN  Waited;
    for (Waited = 0; Waited < 30; Waited++) {
      gBS->Stall (1000000);   /* 1 s per iteration, 30 s total */
      Say (L".");
    }
    Say (L"\r\nXnuLauncher: continuing\r\n");
  }

  Status = ReadXnuLoader (&Image, &ImageSize);
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;   /* no loader embedded: stay out of the boot path */
  }

  LoaderHandle = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  ImageHandle,
                  NULL,
                  Image,
                  ImageSize,
                  &LoaderHandle
                  );
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"XnuLauncher: LoadImage failed %r\r\n", Status);
    Say (Line);
    gBS->FreePool (Image);
    return Status;
  }

  Say (L"XnuLauncher: starting loader\r\n");
  gBS->FreePool (Image);

  Status = gBS->StartImage (LoaderHandle, NULL, NULL);
  UnicodeSPrint (Line, sizeof (Line), L"XnuLauncher: loader returned %r\r\n", Status);
  Say (Line);

  return EFI_SUCCESS;
}
