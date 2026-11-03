#ifndef _XNU_LOADER_H_
#define _XNU_LOADER_H_

//
// The boot shim copies everything appended after the UEFI FD to this address.
// The region is marked reserved in the device memory map.
//
// 0xF0000000..0xF3800000 is free on this phone: trust_ui_vm_region starts at
// 0xF3800000, so the window is 56 MB and must not grow past it.
//
#define XNU_PAYLOAD_BASE      0xF0000000ULL
#define XNU_PAYLOAD_MAX_SIZE  0x03800000ULL
#define XNU_PAYLOAD_MAGIC     0x50554E58 // "XNUP"

typedef struct {
  UINT32 Magic;
  UINT32 Size;
} XNU_PAYLOAD_HEADER;

// The header is followed by entries; an empty name ends the list.
typedef struct {
  CHAR8  Name[16];
  UINT32 Size;
  UINT32 Reserved;
  // UINT8 Data[Size], padded to 16 bytes
} XNU_PAYLOAD_ENTRY;

// Raw GPT partition that receives the text log (read back from Android).
#define XNU_LOG_PARTITION  L"logfs"

#define XNU_MARKER_MAGIC  0x584E5521 // "XNU!"

VOID
XnuLoaderMain ();

//
// Try to start the loader carried in the payload, without resetting on failure.
//
// Called early (PlatformBootManagerAfterConsole) while the console, GOP and the
// storage stack are all still live, so a failure leaves the normal boot flow free
// to continue instead of taking the phone down. Returns TRUE if a loader image was
// found and started; it may of course never return.
//
BOOLEAN
XnuLoaderTryStart ();

#endif /* _XNU_LOADER_H_ */
