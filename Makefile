all:
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

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
	build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o build/switch.o build/pic.o build/irq.o build/timer.o build/mm.o build/process.o build/syscall.o build/paging.o build/tss.o build/ring3.o build/vfs.o build/tmpfs.o build/ata.o build/txfs.o

	cp build/kernel.bin iso/boot/kernel.bin

	grub2-mkrescue -o build/ToxenOS.iso iso

run: all
	qemu-system-i386 -cdrom build/ToxenOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk

disk:
	mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

clean:
	rm -rf build/*.o build/*.bin build/*.iso
	mkdir -p build