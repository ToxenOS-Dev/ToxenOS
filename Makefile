CC = gcc
LD = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra
LDFLAGS = -m elf_i386

all: kernel.bin

kernel.o: src/kernel.c
	$(CC) $(CFLAGS) -c src/kernel.c -o kernel.o

boot.o: src/boot.s
	$(CC) -m32 -c src/boot.s -o boot.o

kernel.bin: boot.o kernel.o src/linker.ld
	$(LD) $(LDFLAGS) -T src/linker.ld -o kernel.bin boot.o kernel.o

iso: all
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o ToxenOS.iso isodir

clean:
	rm -f *.o kernel.bin
	rm -rf isodir ToxenOS.iso
