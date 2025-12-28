CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall -Wextra -I.
LDFLAGS = -m elf_i386 -T linker.ld

OBJ = build/obj
BIN = build/kernel.bin

all: $(BIN)

$(BIN): $(OBJ)/multiboot.o $(OBJ)/entry.o $(OBJ)/vga.o
	$(LD) $(LDFLAGS) -o $@ $^

$(OBJ)/multiboot.o: loader/multiboot.asm
	mkdir -p $(OBJ)
	$(AS) -f elf32 $< -o $@

$(OBJ)/entry.o: core/entry.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/vga.o: devices/display/vga.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

run: $(BIN)
	qemu-system-x86_64 -kernel $(BIN)
