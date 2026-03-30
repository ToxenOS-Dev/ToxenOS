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
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/fat.c -o build/fat.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/ext2.c -o build/ext2.o

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
		build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o \
		build/switch.o build/pic.o build/irq.o build/timer.o build/mm.o \
		build/process.o build/syscall.o build/paging.o build/tss.o build/ring3.o \
		build/vfs.o build/tmpfs.o build/ata.o build/txfs.o build/fat.o build/elf.o \
		build/tty.o \
		build/user/shell_blob.o build/user/init_blob.o build/framebuffer.o build/font.o build/fbterm.o build/ext2.o

	cp build/kernel.bin iso/boot/kernel.bin
	grub2-mkrescue --modules="part_gpt part_msdos all_video" \
    --locales="" --themes="" \
    -o build/ToxenOS.iso iso

user:
	mkdir -p build/user build/user/bin
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x10000000 \
		-no-pie -static \
		user/shell.c -o build/user/shell.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/shell.elf build/user/shell_blob.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x10000000 \
		-no-pie -static \
		user/init.c -o build/user/init.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/init.elf build/user/init_blob.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-nostdlib -nostartfiles \
		-Ttext=0x10000000 \
		-no-pie -static \
		user/hello.c -o build/user/hello.elf
	for cmd in ls shw mkef mkd rm echo pcd uname file help; do \
		gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
			-nostdlib -nostartfiles -Ttext=0x10000000 -no-pie -static \
			user/bin/$$cmd.c -o build/user/bin/$$cmd.elf || exit 1; \
	done
	
run: all populate
	qemu-system-i386 -cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,if=ide,index=0 \
		-drive file=build/fat_disk.img,format=raw,if=ide,index=1 \
		-drive file=build/ext2_disk.img,format=raw,if=ide,index=3
disk:
	mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

tools/txfs_write: tools/txfs_write.c
	gcc -O2 -o tools/txfs_write tools/txfs_write.c

populate: tools/txfs_write
	dd if=/dev/zero of=build/disk.img bs=4096 count=25600
	tools/txfs_write build/disk.img build/user/bin/ls.elf /bin/ls.elf
	tools/txfs_write build/disk.img build/user/bin/shw.elf /bin/shw.elf
	tools/txfs_write build/disk.img build/user/bin/mkef.elf /bin/mkef.elf
	tools/txfs_write build/disk.img build/user/bin/mkd.elf /bin/mkd.elf
	tools/txfs_write build/disk.img build/user/bin/rm.elf /bin/rm.elf
	tools/txfs_write build/disk.img build/user/bin/echo.elf /bin/echo.elf
	tools/txfs_write build/disk.img build/user/bin/pcd.elf /bin/pcd.elf
	tools/txfs_write build/disk.img build/user/bin/uname.elf /bin/uname.elf
	tools/txfs_write build/disk.img build/user/bin/file.elf /bin/file.elf
	tools/txfs_write build/disk.img build/user/bin/help.elf /bin/help.elf
	tools/txfs_write build/disk.img build/user/hello.elf /hello.elf
	tools/txfs_write build/disk.img build/user/shell.elf /shell.elf
	tools/txfs_write build/disk.img build/user/init.elf /init.elf
	@if [ ! -f build/fat_disk.img ]; then \
			rm -f /tmp/fat_raw.img; \
			mkfs.fat -F 32 -C /tmp/fat_raw.img 65024; \
			python3 -c "open('build/fat_disk.img','wb').write(b'\x00'*512+open('/tmp/fat_raw.img','rb').read())"; \
		fi
		@if [ ! -f build/ext2_disk.img ]; then \
			dd if=/dev/zero of=build/ext2_disk.img bs=1M count=64; \
			mkfs.ext2 build/ext2_disk.img; \
		fi
clean:
	rm -rf build/*.o build/*.bin build/*.iso
	mkdir -p build
