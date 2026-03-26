.PHONY: all user install run disk clean populate

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
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/font.c -o build/font.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/fbterm.c -o build/fbterm.o

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
		build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o \
		build/switch.o build/pic.o build/irq.o build/timer.o build/mm.o \
		build/process.o build/syscall.o build/paging.o build/tss.o build/ring3.o \
		build/vfs.o build/tmpfs.o build/ata.o build/txfs.o build/elf.o \
		build/tty.o \
		build/user/shell_blob.o build/user/init_blob.o build/framebuffer.o build/font.o build/fbterm.o

	cp build/kernel.bin iso/boot/kernel.bin
	grub2-mkrescue --modules="part_gpt part_msdos all_video" \
    --locales="" --themes="" \
    -o build/ToxenOS.iso iso

user:
	mkdir -p build/user
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x400000 \
		-no-pie -static \
		user/shell.c -o build/user/shell.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/shell.elf build/user/shell_blob.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x400000 \
		-no-pie -static \
		user/init.c -o build/user/init.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/init.elf build/user/init_blob.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x400000 \
		-no-pie -static \
		user/hello.c -o build/user/hello.elf
	mkdir -p build/user/bin
	# External command programs
	for cmd in ls shw mkef mkd rm echo pcd uname file; do \
		gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
			-nostdlib -nostartfiles -Ttext=0x400000 -no-pie -static \
			user/bin/$$cmd.c -o build/user/bin/$$cmd.elf || exit 1; \
	done

install: user
	dd if=build/user/shell.elf of=build/disk.img bs=512 seek=2048 conv=notrunc

run: all populate
	qemu-system-i386 -cdrom build/ToxenOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk

disk:
	mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

tools/txfs_write: tools/txfs_write.c
	gcc -O2 -o tools/txfs_write tools/txfs_write.c

populate: tools/txfs_write user
	dd if=/dev/zero of=build/disk.img bs=4096 count=25600
	@for pair in \
		"build/user/hello.elf /hello.elf" \
		"build/user/shell.elf /shell.elf" \
		"build/user/init.elf /init.elf" \
		"build/user/bin/ls.elf /bin/ls.elf" \
		"build/user/bin/shw.elf /bin/shw.elf" \
		"build/user/bin/mkef.elf /bin/mkef.elf" \
		"build/user/bin/mkd.elf /bin/mkd.elf" \
		"build/user/bin/rm.elf /bin/rm.elf" \
		"build/user/bin/echo.elf /bin/echo.elf" \
		"build/user/bin/pcd.elf /bin/pcd.elf" \
		"build/user/bin/uname.elf /bin/uname.elf" \
		"build/user/bin/file.elf /bin/file.elf"; do \
		src=$$(echo $$pair | cut -d' ' -f1); \
		dst=$$(echo $$pair | cut -d' ' -f2); \
		if [ -f "$$src" ]; then \
			tools/txfs_write build/disk.img $$src $$dst; \
		else \
			echo "WARNING: $$src not found, skipping"; \
		fi; \
	done

clean:
	rm -rf build/*.o build/*.bin build/*.iso
	mkdir -p build