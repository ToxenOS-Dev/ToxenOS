# ===== Tools =====
CC = gcc
AS = nasm
LD = ld

# ===== Flags =====
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall -Wextra -I.
LDFLAGS = -m elf_i386 -T linker.ld

# ===== Directories =====
BUILD = build
OBJ   = $(BUILD)/obj

# ===== Kernel =====
KERNEL = $(BUILD)/kernel.bin

# ===== Objects =====
OBJS = \
	$(OBJ)/multiboot.o \
	$(OBJ)/entry.o \
	$(OBJ)/vga.o \
	$(OBJ)/keyboard.o \
	$(OBJ)/idt.o \
	$(OBJ)/isr.o

# ===== Default =====
all: $(KERNEL)

# ===== Multiboot =====
$(OBJ)/multiboot.o: loader/multiboot.asm
	mkdir -p $(OBJ)
	$(AS) -f elf32 $< -o $@

# ===== Kernel Entry =====
$(OBJ)/entry.o: core/entry.c
	mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# ===== VGA Driver =====
$(OBJ)/vga.o: devices/display/vga.c
	$(CC) $(CFLAGS) -c $< -o $@

# ===== Keyboard =====
$(OBJ)/keyboard.o: devices/input/keyboard.c
	$(CC) $(CFLAGS) -c $< -o $@

# ===== IDT =====
$(OBJ)/idt.o: cpu/interrupts/idt.c
	$(CC) $(CFLAGS) -c $< -o $@

# ===== ISR =====
$(OBJ)/isr.o: cpu/interrupts/isr.asm
	$(AS) -f elf32 $< -o $@

# ===== Link =====
$(KERNEL): $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $^

# ===== Run =====
run: $(KERNEL)
	qemu-system-x86_64 -kernel $(KERNEL)

# ===== Clean =====
clean:
	rm -rf build
