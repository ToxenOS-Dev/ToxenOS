.PHONY: all user install run disk clean

all: user
	mkdir -p build
	mkdir -p iso/boot

	nasm -f elf32 kernel/boot.asm -o build/boot.o
	nasm -f elf32 kernel/isr.asm -o build/isr.o
	nasm -f elf32 kernel/switch.asm -o build/switch.o

	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/kernel.c -o build/kernel.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/keyboard.c -o build/keyboard.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/idt.c -o build/idt.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/pic.c -o build/pic.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/irq.c -o build/irq.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/timer.c -o build/timer.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/mm.c -o build/mm.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/process.c -o build/process.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/syscall.c -o build/syscall.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/paging.c -o build/paging.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/tss.c -o build/tss.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/ring3.c -o build/ring3.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/vfs.c -o build/vfs.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/tmpfs.c -o build/tmpfs.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/ata.c -o build/ata.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/txfs.c -o build/txfs.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/elf.c -o build/elf.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/tty.c -o build/tty.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/framebuffer.c -o build/framebuffer.o

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
		build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o \
		build/switch.o build/pic.o build/irq.o build/timer.o build/mm.o \
		build/process.o build/syscall.o build/paging.o build/tss.o build/ring3.o \
		build/vfs.o build/tmpfs.o build/ata.o build/txfs.o build/elf.o \
		build/tty.o \
		build/user/shell_blob.o build/framebuffer.o

	cp build/kernel.bin iso/boot/kernel.bin
	grub2-mkrescue --modules="part_gpt part_msdos all_video" \
    --locales="" --themes="" \
    -o build/ToxenOS.iso iso

user:
	mkdir -p build/user
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x400000 \
		user/shell.c -o build/user/shell.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/shell.elf build/user/shell_blob.o

install: user
	dd if=build/user/shell.elf of=build/disk.img bs=512 seek=2048 conv=notrunc

run: all
	qemu-system-i386 -cdrom build/ToxenOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk

disk:
	mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

clean:
	rm -rf build/*.o build/*.bin build/*.iso
	mkdir -p build