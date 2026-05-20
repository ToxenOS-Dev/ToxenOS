.PHONY: all user install run disk clean populate

# ── Address space layout — must match include/memmap.h ───────────────────────
USER_ELF_BASE := 0x10000000

# ── Compiler flags ────────────────────────────────────────────────────────────
# -MMD -MP: generate .d dependency files for incremental builds
# -Wall -Wextra: catch real bugs for free
KFLAGS := -ffreestanding -fno-stack-protector -fno-pic -m32 \
          -Wall -Wextra -Wno-unused-parameter \
          -MMD -MP -I include
UFLAGS := -ffreestanding -fno-stack-protector -fno-pic -m32 \
          -nostdlib -nostartfiles \
          -Ttext=$(USER_ELF_BASE) \
          -no-pie -static \
          -Wall -Wextra -Wno-unused-parameter

MBEDFLAGS := -ffreestanding -fno-stack-protector -fno-pic -m32 \
             -DMBEDTLS_CONFIG_FILE='"../mbedtls/toxenos_config.h"' \
             -I mbedtls/include -I mbedtls

# ── Kernel object files ───────────────────────────────────────────────────────
KOBJS := \
	build/boot.o build/isr.o build/switch.o \
	build/kernel.o build/keyboard.o build/idt.o build/pic.o build/irq.o \
	build/timer.o build/mm.o build/klog.o build/pmm.o build/process.o build/syscall.o \
	build/paging.o build/tss.o build/ring3.o \
	build/vfs.o build/tmpfs.o build/ata.o build/txfs.o build/fat.o build/env.o build/crypto.o build/dhcp.o \
	build/ext2.o build/elf.o build/tty.o build/pipe.o build/waitqueue.o \
	build/pci.o build/e1000.o build/net.o build/tcp.o build/tls.o \
	build/framebuffer.o build/font.o build/fbterm.o \
	build/user/shell_blob.o build/user/init_blob.o

# Include generated dependency files (silently skip if not yet built)
-include $(wildcard build/*.d) $(wildcard build/mbedtls/*.d)

all: user
	@mkdir -p build build/mbedtls iso/boot

	# ── ASM ──────────────────────────────────────────────────────────────────
	nasm -f elf32 kernel/boot.asm   -o build/boot.o
	nasm -f elf32 kernel/isr.asm    -o build/isr.o
	nasm -f elf32 kernel/switch.asm -o build/switch.o

	# ── Kernel C files ───────────────────────────────────────────────────────
	gcc $(KFLAGS) -c kernel/kernel.c     -o build/kernel.o
	gcc $(KFLAGS) -c kernel/keyboard.c   -o build/keyboard.o
	gcc $(KFLAGS) -c kernel/idt.c        -o build/idt.o
	gcc $(KFLAGS) -c kernel/pic.c        -o build/pic.o
	gcc $(KFLAGS) -c kernel/irq.c        -o build/irq.o
	gcc $(KFLAGS) -c kernel/timer.c      -o build/timer.o
	gcc $(KFLAGS) -c kernel/mm.c         -o build/mm.o
	gcc $(KFLAGS) -c kernel/klog.c       -o build/klog.o
	gcc $(KFLAGS) -c kernel/pmm.c        -o build/pmm.o
	gcc $(KFLAGS) -c kernel/process.c    -o build/process.o
	gcc $(KFLAGS) -c kernel/syscall.c    -o build/syscall.o
	gcc $(KFLAGS) -c kernel/paging.c     -o build/paging.o
	gcc $(KFLAGS) -c kernel/tss.c        -o build/tss.o
	gcc $(KFLAGS) -c kernel/ring3.c      -o build/ring3.o
	gcc $(KFLAGS) -c kernel/vfs.c        -o build/vfs.o
	gcc $(KFLAGS) -c kernel/env.c        -o build/env.o
	gcc $(KFLAGS) -c kernel/dhcp.c       -o build/dhcp.o
	gcc $(KFLAGS) -c kernel/tmpfs.c      -o build/tmpfs.o
	gcc $(KFLAGS) -c kernel/ata.c        -o build/ata.o
	gcc $(KFLAGS) -c kernel/txfs.c       -o build/txfs.o
	gcc $(KFLAGS) -c kernel/fat.c        -o build/fat.o
	gcc $(KFLAGS) -c kernel/ext2.c       -o build/ext2.o
	gcc $(KFLAGS) -c kernel/elf.c        -o build/elf.o
	gcc $(KFLAGS) -c kernel/tty.c        -o build/tty.o
	gcc $(KFLAGS) -c kernel/pipe.c       -o build/pipe.o
	gcc $(KFLAGS) -c kernel/waitqueue.c  -o build/waitqueue.o
	gcc $(KFLAGS) -c kernel/pci.c        -o build/pci.o
	gcc $(KFLAGS) -c kernel/e1000.c      -o build/e1000.o
	gcc $(KFLAGS) -c kernel/net.c        -o build/net.o
	gcc $(KFLAGS) -c kernel/tcp.c        -o build/tcp.o
	gcc $(KFLAGS) -c kernel/framebuffer.c -o build/framebuffer.o
	gcc $(KFLAGS) -c kernel/font.c       -o build/font.o
	gcc $(KFLAGS) -c kernel/fbterm.c     -o build/fbterm.o

	# ── mbedTLS ──────────────────────────────────────────────────────────────
	# Preflight: verify 32-bit headers are available (needed for mbedTLS + tls.c)
	@if ! echo '#include <stdint.h>' | gcc -m32 -x c -fsyntax-only - 2>/dev/null; then \
		echo ""; \
		echo "ERROR: 32-bit system headers not found."; \
		echo "  Fedora/RHEL:  sudo dnf install glibc-devel.i686 libgcc.i686"; \
		echo "  Debian/Ubuntu: sudo apt install gcc-multilib"; \
		echo ""; \
		exit 1; \
	fi
	@mkdir -p build/mbedtls
	@for f in mbedtls/library/*.c; do \
		base=$$(basename $$f .c); \
		case $$base in net_sockets|timing|pkcs7|ssl_tls13*|mps_*|platform) continue ;; esac; \
		if ! gcc $(MBEDFLAGS) -MMD -MP -c $$f -o build/mbedtls/$$base.o 2>/dev/null; then \
			echo "WARNING: failed to compile mbedtls/library/$$base.c (skipping)"; \
		fi; \
	done
	gcc $(MBEDFLAGS) -MMD -MP \
		-c mbedtls/toxenos_platform.c -o build/mbedtls/toxenos_platform.o
	gcc $(MBEDFLAGS) -MMD -MP -I include \
		-c kernel/tls.c -o build/tls.o
	gcc $(MBEDFLAGS) -MMD -MP -I include \
		-c kernel/crypto.c -o build/crypto.o
	@if [ $$(ls build/mbedtls/*.o 2>/dev/null | wc -l) -lt 10 ]; then \
		echo "ERROR: mbedTLS build produced too few objects — check 32-bit headers."; \
		exit 1; \
	fi

	# ── Link ─────────────────────────────────────────────────────────────────
	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
		$(KOBJS) \
		$$(ls build/mbedtls/*.o)

	cp build/kernel.bin iso/boot/kernel.bin
	@if command -v grub2-mkrescue >/dev/null 2>&1; then \
		grub2-mkrescue --modules="part_gpt part_msdos all_video" \
			--locales="" --themes="" \
			-o build/ToxenOS.iso iso; \
	elif command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue --modules="part_gpt part_msdos all_video" \
			--locales="" --themes="" \
			-o build/ToxenOS.iso iso; \
	else \
		echo "NOTE: grub2-mkrescue not found — kernel.bin built but ISO not created."; \
		echo "      Install grub2 then run: grub2-mkrescue -o build/ToxenOS.iso iso"; \
	fi

user:
	@mkdir -p build/user build/user/bin
	gcc $(UFLAGS) user/shell.c -o build/user/shell.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/shell.elf build/user/shell_blob.o
	gcc $(UFLAGS) user/init.c -o build/user/init.elf
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/init.elf build/user/init_blob.o
	gcc $(UFLAGS) user/hello.c -o build/user/hello.elf
	for cmd in ls shw mkef mkd rm echo pcd uname file help cp tree hex mv rname sif find bmsg proc end top sleeptest memtest pipetest nettest dns http ping https isolation_test stresstest restore trash rmkd sysctl kill reg syslog wc date chmod where df free hostname adduser passwd usermod; do \
		gcc $(UFLAGS) user/bin/$$cmd.c -o build/user/bin/$$cmd.elf || exit 1; \
	done
	gcc $(UFLAGS) user/bin/tox_pkg.c -o build/user/bin/tox.elf

run: all populate
	qemu-system-i386 \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,index=0,media=disk \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap

disk:
	@mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

tools/txfs_write: tools/txfs_write.c
	gcc -O2 -o tools/txfs_write tools/txfs_write.c

populate: tools/txfs_write
	dd if=/dev/zero of=build/disk.img bs=4096 count=25600
	tools/txfs_write build/disk.img build/user/bin/ls.elf /BSM/SystemT/ls.elf
	tools/txfs_write build/disk.img build/user/bin/shw.elf /BSM/SystemT/shw.elf
	tools/txfs_write build/disk.img build/user/bin/mkef.elf /BSM/SystemT/mkef.elf
	tools/txfs_write build/disk.img build/user/bin/mkd.elf /BSM/SystemT/mkd.elf
	tools/txfs_write build/disk.img build/user/bin/rm.elf /BSM/SystemT/rm.elf
	tools/txfs_write build/disk.img build/user/bin/echo.elf /BSM/SystemT/echo.elf
	tools/txfs_write build/disk.img build/user/bin/pcd.elf /BSM/SystemT/pcd.elf
	tools/txfs_write build/disk.img build/user/bin/uname.elf /BSM/SystemT/uname.elf
	tools/txfs_write build/disk.img build/user/bin/file.elf /BSM/SystemT/file.elf
	tools/txfs_write build/disk.img build/user/bin/help.elf /BSM/SystemT/help.elf
	tools/txfs_write build/disk.img build/user/bin/cp.elf /BSM/SystemT/cp.elf
	tools/txfs_write build/disk.img build/user/bin/tree.elf /BSM/SystemT/tree.elf
	tools/txfs_write build/disk.img build/user/bin/hex.elf /BSM/SystemT/hex.elf
	tools/txfs_write build/disk.img build/user/bin/mv.elf /BSM/SystemT/mv.elf
	tools/txfs_write build/disk.img build/user/bin/rname.elf /BSM/SystemT/rname.elf
	tools/txfs_write build/disk.img build/user/bin/sif.elf /BSM/SystemT/sif.elf
	tools/txfs_write build/disk.img build/user/bin/find.elf /BSM/SystemT/find.elf
	tools/txfs_write build/disk.img build/user/bin/bmsg.elf /BSM/SystemT/bmsg.elf
	tools/txfs_write build/disk.img build/user/bin/proc.elf /BSM/SystemT/proc.elf
	tools/txfs_write build/disk.img build/user/bin/end.elf /BSM/SystemT/end.elf
	tools/txfs_write build/disk.img build/user/bin/top.elf /BSM/SystemT/top.elf
	tools/txfs_write build/disk.img build/user/bin/sleeptest.elf /BSM/SystemT/sleeptest.elf
	tools/txfs_write build/disk.img build/user/bin/memtest.elf /BSM/SystemT/memtest.elf
	tools/txfs_write build/disk.img build/user/bin/pipetest.elf /BSM/SystemT/pipetest.elf
	tools/txfs_write build/disk.img build/user/bin/nettest.elf /BSM/SystemT/nettest.elf
	tools/txfs_write build/disk.img build/user/bin/dns.elf /BSM/SystemT/dns.elf
	tools/txfs_write build/disk.img build/user/bin/http.elf /BSM/SystemT/http.elf
	tools/txfs_write build/disk.img build/user/bin/ping.elf /BSM/SystemT/ping.elf
	tools/txfs_write build/disk.img build/user/bin/https.elf /BSM/SystemT/https.elf
	tools/txfs_write build/disk.img build/user/bin/isolation_test.elf /BSM/SystemT/isolation_test.elf
	tools/txfs_write build/disk.img build/user/bin/stresstest.elf /BSM/SystemT/stresstest.elf
	tools/txfs_write build/disk.img build/user/hello.elf /hello.elf
	tools/txfs_write build/disk.img build/user/shell.elf /shell.elf
	tools/txfs_write build/disk.img build/user/init.elf /init.elf
	tools/txfs_write build/disk.img build/user/bin/rmkd.elf /BSM/SystemT/rmkd.elf
	tools/txfs_write build/disk.img build/user/bin/restore.elf /BSM/SystemT/restore.elf
	tools/txfs_write build/disk.img build/user/bin/sysctl.elf /BSM/SystemT/sysctl.elf
	tools/txfs_write build/disk.img build/user/bin/kill.elf /BSM/SystemT/kill.elf
	tools/txfs_write build/disk.img build/user/bin/reg.elf /BSM/SystemT/reg.elf
	tools/txfs_write build/disk.img build/user/bin/syslog.elf /BSM/SystemT/syslog.elf
	tools/txfs_write build/disk.img build/user/bin/wc.elf /BSM/SystemT/wc.elf
	tools/txfs_write build/disk.img build/user/bin/date.elf /BSM/SystemT/date.elf
	tools/txfs_write build/disk.img build/user/bin/chmod.elf /BSM/SystemT/chmod.elf
	tools/txfs_write build/disk.img build/user/bin/where.elf /BSM/SystemT/where.elf
	tools/txfs_write build/disk.img build/user/bin/df.elf /BSM/SystemT/df.elf
	tools/txfs_write build/disk.img build/user/bin/free.elf /BSM/SystemT/free.elf
	tools/txfs_write build/disk.img build/user/bin/hostname.elf /BSM/SystemT/hostname.elf
	tools/txfs_write build/disk.img build/user/bin/adduser.elf /BSM/SystemT/adduser.elf
	tools/txfs_write build/disk.img build/user/bin/passwd.elf /BSM/SystemT/passwd.elf
	tools/txfs_write build/disk.img build/user/bin/usermod.elf /BSM/SystemT/usermod.elf
	tools/txfs_write build/disk.img user/system/users /etc/users
	tools/txfs_write build/disk.img build/user/bin/trash.elf /BSM/SystemT/trash.elf
	tools/txfs_write build/disk.img build/user/bin/tox.elf /BSM/SystemT/tox.elf
	@tools/txfs_write build/disk.img /dev/null /BSM/usr/lst/.keep 2>/dev/null || true
	@tools/txfs_write build/disk.img /dev/null /Trash/.keep 2>/dev/null || true
	@tools/txfs_write build/disk.img /dev/null /etc/.keep 2>/dev/null || true

clean:
	rm -rf build/*.o build/*.d build/*.bin build/*.iso build/mbedtls build/user
	@mkdir -p build
