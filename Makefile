# =========================
# Tools
# =========================
CC = gcc
AS = nasm
LD = ld

# =========================
# Flags
# =========================
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall -Wextra -I.
LDFLAGS = -m elf_i386 -T linker.ld

# =========================
# Directories
# =========================
BUILD = build
OBJ   = $(BUILD)/obj

# =========================
# Kernel Output
# =========================
KERNEL = $(BUILD)/kernel.bin

# =========================
# Object Files
# =========================
OBJS = \
	$(OBJ)/multiboot.o \
	$(OBJ)/entry.o \
	$(OBJ)/vga.o \
	$(OBJ)/keyboard.o \
	$(OBJ)/idt.o \
	$(OBJ)/isr.o \
	$(OBJ)/shell.o \
	$(OBJ)/commands.o

# =========================
# Default Target
# =========================
all: $(KERNEL)

# =========================
# Multiboot
# =========================
$(OBJ)/multiboot.o: loader/multiboot.asm
	mkdir -p $(OBJ)
	$(AS) -f elf32 $< -o $@

# =========================
# Kernel Entry
# =========================
$(OBJ)/entry.o: core/entry.c
	mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# =========================
# VGA Driver
# =========================
$(OBJ)/vga.o: devices/display/vga.c
	$(CC) $(CFLAGS) -c $< -o $@

# =========================
# Keyboard Driver
# =========================
$(OBJ)/keyboard.o: devices/input/keyboard.c
	$(CC) $(CFLAGS) -c $< -o $@

# =========================
# IDT / Interrupts
# =========================
$(OBJ)/idt.o: cpu/interrupts/idt.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/isr.o: cpu/interrupts/isr.asm
	$(AS) -f elf32 $< -o $@

# =========================
# Shell
# =========================
$(OBJ)/shell.o: runtime/shell/shell.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/commands.o: runtime/shell/commands.c
	$(CC) $(CFLAGS) -c $< -o $@

# =========================
# Link Kernel
# =========================
$(KERNEL): $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $^

# =========================
# Run in QEMU
# =========================
run: $(KERNEL)
	qemu-system-x86_64 -kernel $(KERNEL)

# =========================
# Clean
# =========================
clean:
	rm -rf $(BUILD)
