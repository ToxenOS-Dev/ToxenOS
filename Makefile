# ===== Tools =====
AS = nasm
CC = gcc
LD = ld

# ===== Flags =====
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall -Wextra -I.
LDFLAGS = -m elf_i386 -T linker.ld

# ===== Directories =====
BUILD = build
OBJ   = $(BUILD)/obj
ISO   = $(BUILD)/iso

# ===== Files =====
KERNEL = kernel.bin

OBJS = \
	$(OBJ)/multiboot.o \
	$(OBJ)/entry.o \
	$(OBJ)/vga.o

# ===== Default Target =====
all: iso

# ===== Assembly: Multiboot Header =====
$(OBJ)/multiboot.o: loader/multiboot.asm
	mkdir -p $(OBJ)
	$(AS) -f elf32 $< -o $@

# ===== C: Kernel Entry =====
$(OBJ)/entry.o: core/entry.c
	mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# ===== C: VGA Driver =====
$(OBJ)/vga.o: devices/display/vga.c
	mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# ===== Link Kernel =====
$(BUILD)/$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) $^ -o $@

# ===== Build Bootable ISO =====
iso: $(BUILD)/$(KERNEL)
	mkdir -p $(ISO)/boot/grub
	cp $(BUILD)/$(KERNEL) $(ISO)/boot/kernel.bin
	cp loader/grub/grub.cfg $(ISO)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD)/toxenos.iso $(ISO)

# ===== Run in QEMU =====
run: iso
	qemu-system-x86_64 -cdrom $(BUILD)/toxenos.iso

# ===== Clean =====
clean:
	rm -rf build
