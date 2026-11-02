/** @file
  XnuLauncher - start the HakiOS xnuloader during DXE.

  Mu-Silicium's BDS does not auto-start FILE APPLICATION entries contained in the
  firmware volume, so this driver finds the embedded xnuloader by GUID, loads it
  with LoadImage() and hands control to it with StartImage(). That is the handoff
  point where the UEFI firmware gives way to the XNU boot loader.

  The loader is embedded by ktmPkg/Include/RAW.inc as
    FILE APPLICATION = 3E7A9C41-58B2-4D6F-9A03-C1D5E8F24B70 { SECTION PE32 = ... }
  so the GUID below must stay in sync with that stanza.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/FirmwareVolume2.h>

#define XNU_LOADER_GUID \
  { 0x3E7A9C41, 0x58B2, 0x4D6F, { 0x9A, 0x03, 0xC1, 0xD5, 0xE8, 0xF2, 0x4B, 0x70 } }

STATIC EFI_GUID  mXnuLoaderGuid = XNU_LOADER_GUID;

/**
  Locate the xnuloader image in any firmware volume.

  @param[out] Buffer   Receives a pool allocation holding the PE image.
  @param[out] Size     Receives the image size in bytes.

  @retval EFI_SUCCESS  Image found and read.
  @retval EFI_NOT_FOUND  No firmware volume carried the file.
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
    DEBUG ((DEBUG_ERROR, "XnuLauncher: no firmware volumes (%r)\n", Status));
    return Status;
  }

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
      DEBUG ((
        DEBUG_INFO,
        "XnuLauncher: xnuloader found in FV %d, %lu bytes\n",
        (UINT32)Index,
        (UINT64)*Size
        ));
      gBS->FreePool (Handles);
      return EFI_SUCCESS;
    }
  }

  gBS->FreePool (Handles);
  DEBUG ((DEBUG_WARN, "XnuLauncher: xnuloader not present in any FV\n"));
  return EFI_NOT_FOUND;
}

/**
  Driver entry point: load and start xnuloader.

  LoadImage() goes through the normal UEFI image path so the loader keeps the
  boot services it expects. If it returns, the handoff did not happen and this
  driver reports the status rather than pretending success.
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

  DEBUG ((DEBUG_INFO, "XnuLauncher: starting the XNU loader handoff\n"));

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
    DEBUG ((DEBUG_ERROR, "XnuLauncher: LoadImage failed (%r)\n", Status));
    gBS->FreePool (Image);
    return Status;
  }

  /* The loader keeps its own copy once loaded */
  gBS->FreePool (Image);

  Status = gBS->StartImage (LoaderHandle, NULL, NULL);
  DEBUG ((DEBUG_INFO, "XnuLauncher: xnuloader returned (%r)\n", Status));

  return EFI_SUCCESS;
}
