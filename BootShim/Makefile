ARCH              = arm64
TARGET            = aarch64-linux-gnu
CC                = clang
CFLAGS            = --target=$(TARGET)
OBJCOPY           = llvm-objcopy

# Empty when the platform does not want a payload window; the assembly guards on
# #ifdef so an undefined value simply omits the copy.
ifneq ($(PAYLOAD_BASE),)
PAYLOAD_DEFINE    = -DPAYLOAD_BASE=$(PAYLOAD_BASE)
endif

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) ${CFLAGS} -c $< -o $@ -DREQUIRES_KERNEL_HEADER=$(REQUIRES_KERNEL_HEADER) -DFD_BASE=$(FD_BASE) -DFD_SIZE=$(FD_SIZE) $(PAYLOAD_DEFINE)

BootShim.S:
