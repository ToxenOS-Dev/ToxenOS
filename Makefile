AS = nasm
CC = gcc
LD = ld

CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib
LDFLAGS = -m elf_i386 -T linker.ld

BUILD = build
OBJ   = $(BUILD)/obj
ISO   = $(BUILD)/iso

all: iso

$(OBJ)/multiboot.o: loader/multiboot.asm
	mkdir -p $(OBJ)
	$(AS) -f elf32 $< -o $@

$(OBJ)/entry.o: core/entry.c
	mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.bin: $(OBJ)/multiboot.o $(OBJ)/entry.o
	$(LD) $(LDFLAGS) $^ -o $@

iso: $(BUILD)/kernel.bin
	mkdir -p $(ISO)/boot/grub
	cp $(BUILD)/kernel.bin $(ISO)/boot/kernel.bin
	cp loader/grub/grub.cfg $(ISO)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD)/toxenos.iso $(ISO)

run: iso
	qemu-system-x86_64 -cdrom $(BUILD)/toxenos.iso

clean:
	rm -rf build
