build:
	mkdir -p build
	mkdir -p iso/boot

	nasm -f elf32 kernel/boot.asm -o build/boot.o
	nasm -f elf32 kernel/isr.asm -o build/isr.o

	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/kernel.c -o build/kernel.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/keyboard.c -o build/keyboard.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/idt.c -o build/idt.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/pic.c -o build/pic.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/irq.c -o build/irq.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/timer.c -o build/timer.o

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
	build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o build/pic.o build/irq.o build/timer.o

	cp build/kernel.bin iso/boot/kernel.bin

	grub2-mkrescue -o build/ToxenOS.iso iso

run: build
	qemu-system-x86_64 build/ToxenOS.iso

clean:
	rm -rf build