/**
  XNU loader for redwood, stage 0: diagnostics only.

  Runs when BDS finds nothing to boot. Collects what the real loader will need
  (exception level, CPU feature registers, framebuffer, memory map, payload),
  writes it as text to the raw "logdump" partition so it can be read back from
  Android over adb, and cold-resets. Nothing is shown to the user on purpose:
  the phone is driven unattended.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

#include "XnuLoader.h"

#define LOG_SIZE  SIZE_256KB

STATIC CHAR8  *mLog;
STATIC UINTN   mLogLen;

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8 *Format,
  ...)
{
  VA_LIST Marker;

  if (mLog == NULL || mLogLen >= LOG_SIZE - 512) {
    return;
  }

  VA_START (Marker, Format);
  mLogLen += AsciiVSPrint (mLog + mLogLen, LOG_SIZE - mLogLen, Format, Marker);
  VA_END (Marker);
}

#define READ_SYSREG(Name) ({ UINT64 _Value; __asm__ volatile ("mrs %0, " #Name : "=r" (_Value)); _Value; })

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindPartition (
  IN CONST CHAR16 *Name)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       Count;
  CHAR16      Line[80];

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    //
    // No partition protocol at all: PartitionDxe or the storage driver behind it
    // never produced a block device, so nothing can be logged to disk.
    //
    UnicodeSPrint (Line, sizeof (Line), L"XNULOADER: no partitions (%r)\r\n", Status);
    gST->ConOut->OutputString (gST->ConOut, Line);
    return NULL;
  }

  UnicodeSPrint (Line, sizeof (Line), L"XNULOADER: %d partition handle(s)\r\n", (UINT32)Count);
  gST->ConOut->OutputString (gST->ConOut, Line);

  for (UINTN i = 0; i < Count; i++) {
    EFI_PARTITION_INFO_PROTOCOL *Info;
    EFI_BLOCK_IO_PROTOCOL       *BlockIo;

    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Print every name seen, so a mismatch is visible rather than silent.
    //
    UnicodeSPrint (Line, sizeof (Line), L"  [%d] type=%d name=%s\r\n", (UINT32)i, (UINT32)Info->Type, Info->Info.Gpt.PartitionName);
    gST->ConOut->OutputString (gST->ConOut, Line);

    if (Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status)) {
      gBS->FreePool (Handles);
      return BlockIo;
    }
  }

  gBS->FreePool (Handles);
  return NULL;
}

STATIC
VOID
LogCpu ()
{
  Log ("[cpu]\n");
  Log ("CurrentEL=%lx\n", READ_SYSREG (CurrentEL) >> 2);
  Log ("MIDR_EL1=%016lx\n", READ_SYSREG (MIDR_EL1));
  Log ("ID_AA64MMFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR0_EL1));
  Log ("ID_AA64MMFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR1_EL1));
  Log ("ID_AA64MMFR2_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR2_EL1));
  Log ("ID_AA64PFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR0_EL1));
  Log ("ID_AA64PFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR1_EL1));
  Log ("ID_AA64ISAR0_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR0_EL1));
  Log ("ID_AA64ISAR1_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR1_EL1));
  Log ("ID_AA64DFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64DFR0_EL1));
  Log ("SCTLR_EL1=%016lx\n", READ_SYSREG (SCTLR_EL1));
  Log ("TCR_EL1=%016lx\n", READ_SYSREG (TCR_EL1));
  Log ("MAIR_EL1=%016lx\n", READ_SYSREG (MAIR_EL1));
  Log ("TTBR0_EL1=%016lx\n", READ_SYSREG (TTBR0_EL1));
  Log ("VBAR_EL1=%016lx\n", READ_SYSREG (VBAR_EL1));
  Log ("CNTFRQ_EL0=%ld\n", READ_SYSREG (CNTFRQ_EL0));
  Log ("MPIDR_EL1=%016lx\n", READ_SYSREG (MPIDR_EL1));
  Log ("DAIF=%lx\n", READ_SYSREG (DAIF));
}

STATIC
VOID
LogDisplay ()
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  Log ("[display]\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Log ("gop=missing status=%r\n", Status);
    return;
  }

  Log ("fb_base=%lx fb_size=%lx\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
  Log ("width=%d height=%d stride=%d format=%d\n",
       Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
       Gop->Mode->Info->PixelsPerScanLine, Gop->Mode->Info->PixelFormat);
}

STATIC
VOID
LogMemoryMap ()
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;

  Log ("[memmap]\n");

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  MapSize += SIZE_4KB;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  for (UINTN Offset = 0; Offset < MapSize; Offset += DescSize) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);

    Log ("%016lx %016lx type=%d attr=%lx\n", Desc->PhysicalStart,
         Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages), Desc->Type, Desc->Attribute);
  }

  FreePool (Map);
}

STATIC
VOID
LogPayload ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;

  Log ("[payload]\n");
  Log ("base=%lx magic=%08x size=%x\n", (UINT64)XNU_PAYLOAD_BASE, Header->Magic, Header->Size);

}

STATIC
VOID
LogPersistence ()
{
  //
  // Does RAM written before a reset survive it? The kernel bring-up needs a
  // place for progress markers. Uses the tail of the reserved payload region.
  // (Device registers are deliberately not probed here: an MMIO read of the
  // IMEM / watchdog blocks took a synchronous exception on this platform.)
  //
  UINT32 *Ram = (UINT32 *)(UINTN)(XNU_PAYLOAD_BASE + XNU_PAYLOAD_MAX_SIZE - SIZE_4KB);

  Log ("[persistence]\n");
  Log ("ram_before=%08x %08x\n", Ram[0], Ram[1]);

  Ram[1] = (Ram[0] == XNU_MARKER_MAGIC) ? Ram[1] + 1 : 1;
  Ram[0] = XNU_MARKER_MAGIC;

  Log ("ram_after=%08x %08x\n", Ram[0], Ram[1]);
}

STATIC
VOID
FlushLog ()
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                  BlockSize;
  UINTN                  WriteSize;

  BlockIo = FindPartition (XNU_LOG_PARTITION);
  if (BlockIo == NULL) {
    //
    // No log partition under that name. Say so on the console: after BDS has
    // cleared the screen this is the only channel left, and a silent failure
    // here is indistinguishable from the loader never running.
    //
    gST->ConOut->OutputString (gST->ConOut, L"XNULOADER: log partition not found\r\n");
    return;
  }

  BlockSize = BlockIo->Media->BlockSize;
  WriteSize = ALIGN_VALUE (mLogLen + 1, BlockSize);

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, WriteSize, mLog);
  if (!EFI_ERROR (Status)) {
    BlockIo->FlushBlocks (BlockIo);
    gST->ConOut->OutputString (gST->ConOut, L"XNULOADER: log written\r\n");
  } else {
    gST->ConOut->OutputString (gST->ConOut, L"XNULOADER: log write failed\r\n");
  }
}

//
// The payload is a list of named blobs. If it carries "loader.efi", that is
// the real XNU loader: start it straight from memory, so the loader can be
// iterated on without rebuilding this firmware.
//
STATIC
VOID
StartPayloadLoader ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  if (Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_HANDLE Handle;

      if (!EFI_ERROR (gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle))) {
        gBS->StartImage (Handle, NULL, NULL);
      }

      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }
}

/**
  Diagnostics called from PlatformBootManagerAfterConsole.

  At that point the logo has been drawn, every DXE driver has been dispatched and
  the console still works, so both the screen and the block devices are usable.
  Enumerates what storage the firmware can actually see and reports whether the
  log partition was found and written - without starting anything.
**/
/**
  Draw one horizontal band. GOP only - no console protocol involved.

  After DisplayBootGraphic the console produces nothing visible, so pixel bands are
  the only reliable channel back to a human looking at the phone.
**/
STATIC
VOID
Band (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN UINTN                         Row,
  IN UINT8                         R,
  IN UINT8                         G,
  IN UINT8                         B
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Colour;

  Colour.Red      = R;
  Colour.Green    = G;
  Colour.Blue     = B;
  Colour.Reserved = 0;

  Gop->Blt (
         Gop,
         &Colour,
         EfiBltVideoFill,
         0,
         0,
         0,
         Row * 60,
         Gop->Mode->Info->HorizontalResolution,
         60,
         0
         );
}

/**
  Diagnostics called from PlatformBootManagerAfterConsole.

  At that point the logo has been drawn, every DXE driver has been dispatched and
  the console still works, so both the screen and the block devices are usable.
  Enumerates what storage the firmware can actually see and reports whether the
  log partition was found and written - without starting anything.

  Everything is reported as colour bands, one per check, top to bottom:

    red     this function ran
    green   at least one raw block device exists
    blue    at least one partition was reported
    yellow  the log partition resolved by name
    white   the probe write succeeded

  A missing band is as informative as a present one.
**/
VOID
XnuLoaderDiagnostics ()
{
  EFI_STATUS              Status;
  EFI_HANDLE             *Handles;
  UINTN                   Count;
  UINTN                   BlockIoCount;
  CHAR16                  Line[128];
  EFI_BLOCK_IO_PROTOCOL  *Target;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) ||
      (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL))
  {
    return;
  }

  // Band 0: proof of execution.
  Band (Gop, 0, 0xFF, 0x00, 0x00);

  // Band 1: raw block devices present.
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &BlockIoCount, &Handles);
  if (EFI_ERROR (Status) || (BlockIoCount == 0)) {
    return;   // no storage: stop here, bands below will be absent
  }
  gBS->FreePool (Handles);
  Band (Gop, 1, 0x00, 0xFF, 0x00);

  // Band 2: partitions enumerated.
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Count == 0)) {
    return;
  }
  gBS->FreePool (Handles);
  Band (Gop, 2, 0x00, 0x00, 0xFF);

  // Band 3: the log partition resolved by name.
  Target = FindPartition (XNU_LOG_PARTITION);
  if (Target == NULL) {
    return;
  }
  Band (Gop, 3, 0xFF, 0xFF, 0x00);

  // Band 4: the probe write succeeded.
  mLog = AllocateZeroPool (SIZE_4KB);
  if (mLog != NULL) {
    VOID  *Verify;

    Log ("== XNULOADER DIAG ==\n");
    Log ("written by the firmware into the log partition\n");

    //
    // WriteBlocks only stages into the block cache - without FlushBlocks the text
    // never reaches the medium, which is why the partition read back empty from
    // Android even though the write returned success.
    //
    Status = Target->WriteBlocks (Target, Target->Media->MediaId, 0, ALIGN_VALUE (mLogLen, Target->Media->BlockSize), mLog);
    if (!EFI_ERROR (Status)) {
      Status = Target->FlushBlocks (Target);
    }

    if (!EFI_ERROR (Status)) {
      //
      // Read it straight back. If the medium kept it, this buffer holds the text.
      //
      Verify = AllocateZeroPool (ALIGN_VALUE (mLogLen, Target->Media->BlockSize));
      if (Verify != NULL) {
        if (!EFI_ERROR (Target->ReadBlocks (Target, Target->Media->MediaId, 0, ALIGN_VALUE (mLogLen, Target->Media->BlockSize), Verify)) &&
            (CompareMem (Verify, mLog, mLogLen) == 0))
        {
          //
          // Five short bands confirm: executed, block device, partitions, log
          // partition resolved, and now the data verified on the medium.
          //
          for (UINTN b = 0; b < 5; b++) {
            Band (Gop, 4 + b, 0xFF, 0xFF, 0xFF);
          }
        }
        FreePool (Verify);
      }
    }

    FreePool (mLog);
    mLog    = NULL;
    mLogLen = 0;
  }

  // Keep the console write too: harmless if it works, ignored if it does not.
  gST->ConOut->OutputString (gST->ConOut, L"\r\nXNULOADER: diagnostics done\r\n");
  (VOID)Line;

  //
  // Band 13: an "XNUP" container is sitting at the payload base, i.e. BootShim
  // copied what was appended behind the UEFI image. This is the check that says
  // whether the handoff can ever start a loader.
  //
  {
    XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;

    if ((Header->Magic == XNU_PAYLOAD_MAGIC) && (Header->Size != 0) && (Header->Size <= XNU_PAYLOAD_MAX_SIZE)) {
      Band (Gop, 13, 0x00, 0xFF, 0xFF);   // cyan: payload present and plausible

      //
      // Walk the entries for "loader.efi" and try to start it right here, while
      // GOP and the console are both still alive. Doing this at UnableToBoot
      // gained nothing visible because the display is gone by then.
      //
      UINT8 *Cursor = (UINT8 *)(Header + 1);
      UINT8 *End    = Cursor + Header->Size;

      while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
        XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

        if (Entry->Name[0] == 0) {
          break;
        }

        if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
          EFI_HANDLE Handle = NULL;

          Band (Gop, 14, 0xFF, 0x00, 0xFF);   // magenta: the entry was found

          Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle);
          if (!EFI_ERROR (Status)) {
            Band (Gop, 15, 0x80, 0x00, 0xFF); // violet: LoadImage succeeded

            //
            // If this returns, the loader did not take over. Orange marks that.
            //
            Status = gBS->StartImage (Handle, NULL, NULL);
            Band (Gop, 16, 0xFF, 0x80, 0x00);
          }

          break;
        }

        Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
      }
    }
  }
}

VOID
XnuLoaderMain ()
{
  EFI_TIME Time;

  //
  // Announce on the console first. BDS has already cleared the screen by the
  // time this runs, so these lines stay visible - unlike anything printed during
  // DXE - and they distinguish "loader never ran" from "loader ran but could not
  // log" without needing the log partition at all.
  //
  gST->ConOut->OutputString (gST->ConOut, L"\r\nXNULOADER: entered\r\n");

  // Normally does not return
  StartPayloadLoader ();

  gST->ConOut->OutputString (gST->ConOut, L"XNULOADER: no payload loader, writing log\r\n");

  mLog = AllocateZeroPool (LOG_SIZE);
  if (mLog != NULL) {
    Log ("== XNULOADER LOG v1 ==\n");

    if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
      Log ("time=%04d-%02d-%02d %02d:%02d:%02d\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
    }

    FlushLog ();
    LogCpu ();
    FlushLog ();
    LogDisplay ();
    FlushLog ();
    LogMemoryMap ();
    FlushLog ();
    LogPayload ();
    FlushLog ();
    LogPersistence ();
    Log ("== END ==\n");
    FlushLog ();
  }

  // The phone is unattended: always come back to Android.
  gST->ConOut->OutputString (gST->ConOut, L"XNULOADER: cold reset\r\n");
  gBS->Stall (2000000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}
