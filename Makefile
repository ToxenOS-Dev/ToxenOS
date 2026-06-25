.PHONY: all user install run disk clean populate kernel64 run64 user64

# ── Address space layout — must match include/memmap.h ───────────────────────
USER_ELF_BASE := 0x10000000

# Milestone 6: 64-bit user ELF/NEX64 base. = KERNEL_VIRT_BASE64 +
# PD_EXEC_IDX*0x200000 (include/nex64.h) -- must stay in sync with that
# file and with tools/elf2nex64.c's own copy; Make can't evaluate that
# expression, so the literal is duplicated here, same precedent as
# USER_ELF_BASE above.
USER64_ELF_BASE := 0xFFFFFFFF83C00000

# Set PACKAGE_ELF=1 (e.g. `make all PACKAGE_ELF=1`) to also write .elf copies
# of bundled commands into the TxFS image alongside .nex, for dev/debug use.
# Normal images ship .nex only — .elf binaries still exist in build/ as
# compile intermediates, and ELF stays fully loadable for anything a user
# manually installs (e.g. /C:/tools/test.elf).
PACKAGE_ELF ?= 0

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

# ── x86_64 long-mode bring-up (Milestone 1) — additive, parallel build ───────
# Does not touch KFLAGS/KOBJS/the 32-bit `all` target above: the 32-bit
# kernel stays fully buildable as the migration's stable-backup reference
# while this is brought up. See kernel/kernel64.c and kernel/boot64.asm.
# -mcmodel=kernel: required because linker64.ld links the kernel at the
#   canonical higher-half base 0xFFFFFFFF80000000 (top 2GB of canonical
#   address space) — the default/small code model can't address that.
# -fno-pie alongside -fno-pic: freestanding kernel code must not be PIE.
# -mno-red-zone: mandatory for any x86_64 kernel code that can be interrupted.
# -mno-sse/-mno-mmx: avoid the compiler using SSE/MMX regs before FPU/SSE
#   state is set up (not yet relevant with interrupts off, but free to add now).
KFLAGS64 := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 \
            -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -fno-asynchronous-unwind-tables \
            -Wall -Wextra -Wno-unused-parameter -MMD -MP -I include

# Milestone 6: 64-bit userspace test program flags -- the 64-bit
# analogue of UFLAGS above (-m64 plus the same SSE/red-zone-avoidance
# flags KFLAGS64 already needs for freestanding x86_64 code).
UFLAGS64 := -ffreestanding -fno-stack-protector -fno-pic -m64 \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -nostdlib -nostartfiles \
            -Ttext=$(USER64_ELF_BASE) \
            -no-pie -static \
            -Wall -Wextra -Wno-unused-parameter

# ── Kernel object files ───────────────────────────────────────────────────────
KOBJS := \
	build/boot.o build/isr.o build/switch.o \
	build/kernel.o build/keyboard.o build/idt.o build/pic.o build/irq.o \
	build/timer.o build/mm.o build/klog.o build/pmm.o build/process.o build/syscall.o \
	build/paging.o build/tss.o build/ring3.o \
	build/cpu.o build/acpi.o build/vfs.o build/tmpfs.o build/ata.o build/ahci.o build/nvme.o build/virtio_blk.o build/txfs.o build/fat.o build/env.o build/crypto.o build/dhcp.o \
	build/ext2.o build/elf.o build/tty.o build/pipe.o build/waitqueue.o \
	build/usb_hid.o build/pci.o build/e1000.o build/net.o build/tcp.o build/tls.o \
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
	gcc $(KFLAGS) -c kernel/cpu.c        -o build/cpu.o
	gcc $(KFLAGS) -c kernel/acpi.c       -o build/acpi.o
	gcc $(KFLAGS) -c kernel/ahci.c        -o build/ahci.o
	gcc $(KFLAGS) -c kernel/nvme.c        -o build/nvme.o
	gcc $(KFLAGS) -c kernel/virtio_blk.c  -o build/virtio_blk.o
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
	gcc $(KFLAGS) -c kernel/usb_hid.c    -o build/usb_hid.o
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

	# Rebuild disk.img with the freshly-built kernel.bin (installer copies this to NVMe)
	$(MAKE) populate
	cp build/kernel.bin iso/boot/kernel.bin
	cp build/disk.img   iso/boot/disk.img
	@if command -v grub2-mkrescue >/dev/null 2>&1; then \
		grub2-mkrescue --modules="part_gpt part_msdos all_video gfxterm" \
			--locales="" --themes="" \
			-o build/ToxenOS.iso iso; \
	elif command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue --modules="part_gpt part_msdos all_video gfxterm" \
			--locales="" --themes="" \
			-o build/ToxenOS.iso iso; \
	else \
		echo "NOTE: grub2-mkrescue not found — kernel.bin built but ISO not created."; \
		echo "      Install grub2 then run: grub2-mkrescue -o build/ToxenOS.iso iso"; \
	fi

user: tools/elf2nex
	@mkdir -p build/user build/user/bin
	gcc $(UFLAGS) user/shell.c -o build/user/shell.elf
	gcc $(UFLAGS) user/init.c -o build/user/init.elf
	gcc $(UFLAGS) user/login.c -o build/user/login.elf
	gcc $(UFLAGS) user/hello.c -o build/user/hello.elf
	for cmd in ls shw mkef mkd rm echo pcd uname file help cp tree hex mv rname sif find bmsg proc end top sleeptest memtest pipetest nettest dns http ping https isolation_test stresstest rmkd sysctl kill reg syslog wc date chmod where df free hostname adduser passwd usermod ipcfg snap install uptime env alias touch nexinfo; do \
		gcc $(UFLAGS) user/bin/$$cmd.c -o build/user/bin/$$cmd.elf || exit 1; \
	done
	gcc $(UFLAGS) user/bin/tox_pkg.c -o build/user/bin/tox.elf
	gcc $(UFLAGS) user/bin/ts.c -o build/user/bin/ts.elf
	gcc $(UFLAGS) user/bin/edit.c -o build/user/bin/edit.elf

	# ── Convert every ELF binary to ToxenOS's native .nex format ──────────────
	# The .elf files above are build intermediates (GCC/ld only emit ELF);
	# .nex is what actually gets packaged into the TxFS image below.
	for f in build/user/*.elf build/user/bin/*.elf; do \
		tools/elf2nex "$$f" "$${f%.elf}.nex" || exit 1; \
	done

	# init's embedded boot blob stays ELF — see the comment on launch_init()
	# in kernel/kernel.c for why.
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/init.elf build/user/init_blob.o
	# shell's embedded boot blob (SYS_SPAWN_EMBEDDED) goes through
	# process_create_elf()'s magic-sniffing dispatcher, so it can be genuine
	# NEX bytes — see the comment in kernel/syscall.c.
	objcopy -I binary -O elf32-i386 -B i386 \
		build/user/shell.nex build/user/shell_blob.o

build/target.img:
	dd if=/dev/zero of=build/target.img bs=1M count=2048

# ── x86_64 long-mode bring-up (Milestones 1-2) ───────────────────────────────
# No disk image involved — there's no filesystem code yet, just the
# long-mode transition (boot64.asm), VGA/serial proof of life
# (kernel64.c), and now 64-bit IDT/exception/PIC/IRQ bring-up. Lives in
# its own iso64/ tree so it never touches iso/ (used by the 32-bit
# `all`/`populate` targets).
# kernel/pic.c is reused VERBATIM here (compiled a second time under
# KFLAGS64, same precedent as klog.c below) — it's pure port I/O with no
# 32-bit-specific dependency, so there's no need to fork a pic64.c.
kernel64:
	@mkdir -p build iso64/boot
	nasm -f elf64 kernel/boot64.asm        -o build/boot64.o
	nasm -f elf64 kernel/isr64.asm         -o build/isr64.o
	nasm -f elf64 kernel/switch64.asm      -o build/switch64.o
	nasm -f elf64 kernel/ring3_test64.asm  -o build/ring3_test64_asm.o
	nasm -f elf64 kernel/userproc64.asm    -o build/userproc64_asm.o
	# Milestone 5: ring3 syscall test stub -- flat binary, embedded as a
	# blob the same way the 32-bit Makefile embeds init.elf/shell.nex.
	nasm -f bin kernel/ring3_syscall_stub64.asm -o build/ring3_syscall_stub64.bin
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		build/ring3_syscall_stub64.bin build/ring3_syscall_stub64_blob.o
	gcc $(KFLAGS64) -c kernel/kernel64.c     -o build/kernel64.o
	gcc $(KFLAGS64) -c kernel/klog.c         -o build/klog64.o
	gcc $(KFLAGS64) -c kernel/idt64.c        -o build/idt64.o
	gcc $(KFLAGS64) -c kernel/interrupt64.c  -o build/interrupt64.o
	gcc $(KFLAGS64) -c kernel/irq64.c        -o build/irq64.o
	gcc $(KFLAGS64) -c kernel/timer64.c      -o build/timer64.o
	gcc $(KFLAGS64) -c kernel/keyboard64.c   -o build/keyboard64.o
	gcc $(KFLAGS64) -c kernel/pic.c          -o build/pic64.o
	gcc $(KFLAGS64) -c kernel/process64.c    -o build/process64.o
	gcc $(KFLAGS64) -c kernel/tss64.c        -o build/tss64.o
	gcc $(KFLAGS64) -c kernel/ring3_test64.c -o build/ring3_test64.o
	gcc $(KFLAGS64) -c kernel/ata64.c        -o build/ata64.o
	gcc $(KFLAGS64) -c kernel/txfs64.c       -o build/txfs64.o
	gcc $(KFLAGS64) -c kernel/syscall64.c    -o build/syscall64.o
	gcc $(KFLAGS64) -c kernel/exec64.c       -o build/exec64.o
	gcc $(KFLAGS64) -c kernel/userproc64.c   -o build/userproc64.o
	ld -m elf_x86_64 -T linker64.ld -o build/kernel64.bin \
		build/boot64.o build/isr64.o build/switch64.o build/kernel64.o \
		build/klog64.o build/idt64.o build/interrupt64.o build/irq64.o \
		build/timer64.o build/keyboard64.o build/pic64.o build/process64.o \
		build/tss64.o build/ring3_test64.o build/ring3_test64_asm.o \
		build/ata64.o build/txfs64.o build/syscall64.o build/exec64.o \
		build/userproc64.o build/userproc64_asm.o \
		build/ring3_syscall_stub64_blob.o
	cp build/kernel64.bin iso64/boot/kernel64.bin
	@if command -v grub2-mkrescue >/dev/null 2>&1; then \
		grub2-mkrescue --modules="multiboot2" \
			-o build/ToxenOS64.iso iso64; \
	elif command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue --modules="multiboot2" \
			-o build/ToxenOS64.iso iso64; \
	else \
		echo "NOTE: grub2-mkrescue not found — kernel64.bin built but ISO not created."; \
		echo "      Install grub2 then run: grub2-mkrescue -o build/ToxenOS64.iso iso64"; \
	fi

# build/disk.img (GPT + TxFS, containing /hello.ts) comes from the
# existing `populate` pipeline (same image the 32-bit run targets use,
# just attached here as legacy IDE instead of NVMe) -- depends on `user`
# since `populate` writes build/user/*.nex into the image.
run64: kernel64 user populate
	qemu-system-x86_64 -m 256 -boot order=d -cdrom build/ToxenOS64.iso \
		-drive file=build/disk.img,format=raw,if=ide \
		-serial stdio

run: all build/target.img
	qemu-system-i386 \
		-enable-kvm -cpu host,+cmov,+cx8 \
		-m 256 \
		-boot order=d \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,drive=nvme0,serial=toxnvme0 \
		-drive file=build/target.img,format=raw,if=none,id=nvme1 \
		-device nvme,drive=nvme1,serial=toxnvme1 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap \
		-serial stdio

run-virtio: all build/target.img
	qemu-system-i386 \
		-enable-kvm -cpu host,+cmov,+cx8 \
		-m 256 \
		-boot order=d \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,if=virtio \
		-drive file=build/target.img,format=raw,if=virtio \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap \
		-serial stdio

run-usb: all build/target.img
	qemu-system-i386 \
		-enable-kvm -cpu host,+cmov,+cx8 \
		-m 256 \
		-boot order=d \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,drive=nvme0,serial=toxnvme0 \
		-drive file=build/target.img,format=raw,if=none,id=nvme1 \
		-device nvme,drive=nvme1,serial=toxnvme1 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap \
		-serial stdio

run-ata: populate all build/target.img
	qemu-system-i386 \
		-enable-kvm -cpu host,+cmov,+cx8 \
		-m 256 \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,index=0,media=disk \
		-drive file=build/target.img,format=raw,index=1,media=disk \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap

disk:
	@mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

tools/txfs_write: tools/txfs_write.c
	gcc -O2 -o tools/txfs_write tools/txfs_write.c

tools/patch_diskboot: tools/patch_diskboot.c
	gcc -O2 -o tools/patch_diskboot tools/patch_diskboot.c

tools/elf2nex: tools/elf2nex.c
	gcc -O2 -o tools/elf2nex tools/elf2nex.c

# ── Milestone 6: 64-bit user exec test program ───────────────────────────────
tools/elf2nex64: tools/elf2nex64.c
	gcc -O2 -o tools/elf2nex64 tools/elf2nex64.c

user64: tools/elf2nex64
	@mkdir -p build/user64
	gcc $(UFLAGS64) user64/exec_test.c -o build/user64/exec_test.elf64
	tools/elf2nex64 build/user64/exec_test.elf64 build/user64/exec_test.nex64
	gcc $(UFLAGS64) user64/exec_fault_test.c -o build/user64/exec_fault_test.elf64
	tools/elf2nex64 build/user64/exec_fault_test.elf64 build/user64/exec_fault_test.nex64

populate: tools/txfs_write tools/patch_diskboot user64
	dd if=/dev/zero of=build/fs.img bs=4096 count=2048
	# Bundled commands: .nex is the packaged default. Pass PACKAGE_ELF=1 to
	# also ship the .elf copies (dev/debug builds only — see note up top).
	for f in build/user/bin/*.nex; do \
		name=$$(basename $$f); \
		tools/txfs_write build/fs.img $$f /BSM/SystemT/$$name || exit 1; \
	done
	if [ "$(PACKAGE_ELF)" = "1" ]; then \
		for f in build/user/bin/*.elf; do \
			name=$$(basename $$f); \
			tools/txfs_write build/fs.img $$f /BSM/SystemT/$$name || exit 1; \
		done; \
	fi
	tools/txfs_write build/fs.img build/user/hello.nex /hello.nex
	tools/txfs_write build/fs.img build/user/shell.nex /shell.nex
	tools/txfs_write build/fs.img build/user/init.nex /init.nex
	tools/txfs_write build/fs.img build/user/login.nex /BSM/SystemT/login.nex
	if [ "$(PACKAGE_ELF)" = "1" ]; then \
		tools/txfs_write build/fs.img build/user/hello.elf /hello.elf; \
		tools/txfs_write build/fs.img build/user/shell.elf /shell.elf; \
		tools/txfs_write build/fs.img build/user/init.elf /init.elf; \
		tools/txfs_write build/fs.img build/user/login.elf /BSM/SystemT/login.elf; \
	fi
	tools/txfs_write build/fs.img user/system/users /etc/users
	tools/txfs_write build/fs.img user/system/hello.ts /hello.ts
	# Milestone 6: 64-bit exec test binaries -- inert to the 32-bit
	# kernel (just two more files it never looks at), read by
	# kernel/exec64.c via the same shared disk image.
	tools/txfs_write build/fs.img build/user64/exec_test.nex64 /exec64_test.nex64
	tools/txfs_write build/fs.img build/user64/exec_test.elf64 /exec64_test.elf64
	# Milestone 7: deliberate-fault test binary, exercises the pid-aware
	# fault termination path (kernel/interrupt64.c -> kernel/userproc64.c).
	tools/txfs_write build/fs.img build/user64/exec_fault_test.nex64 /exec64_fault_test.nex64
	@tools/txfs_write build/fs.img /dev/null /BSM/usr/lst/.keep 2>/dev/null || true
	@tools/txfs_write build/fs.img /dev/null /etc/.keep 2>/dev/null || true
	# ── Assemble bootable disk.img (GPT — BIOS + UEFI dual-boot) ────────────────
	# Layout: LBA 0:        Protective MBR + boot.img code
	#         LBA 1-33:     GPT header + partition entries
	#         LBA 34-2047:  GRUB core.img (gap, ~1 MB available)
	#         LBA 2048-10239: EFI System Partition gpt1 (FAT, 4 MB)
	#         LBA 10240+:   TxFS filesystem
	#         LBA last-32:  Backup GPT
	# BIOS: boot.img -> core.img at LBA 34 -> GRUB reads (hd0,gpt1)/kernel.bin
	# UEFI: firmware -> /EFI/BOOT/BOOTX64.EFI -> GRUB search --label TOXENOS
	@if command -v grub2-mkimage >/dev/null 2>&1; then \
		printf 'insmod gfxterm\ninsmod vbe\nset gfxmode=1024x768x32,800x600x32,auto\nterminal_output gfxterm\nset gfxpayload=keep\nset root=(hd0,gpt1)\nmultiboot2 /kernel.bin\nboot\n' \
			> build/grub_embed.cfg; \
		printf 'insmod gfxterm\ninsmod efi_gop\nset gfxmode=1024x768x32,800x600x32,auto\nterminal_output gfxterm\nset gfxpayload=keep\nsearch --no-floppy --set=root --label TOXENOS\nmultiboot2 /kernel.bin\nboot\n' \
			> build/grub_efi_embed.cfg; \
		grub2-mkimage --format=i386-pc \
			--output=build/grub_core.img \
			--config=build/grub_embed.cfg \
			--prefix="(hd0,gpt1)" \
			biosdisk part_gpt fat multiboot2 gfxterm video_fb vbe all_video; \
		grub2-mkimage --format=x86_64-efi \
			--output=build/grubx64.efi \
			--config=build/grub_efi_embed.cfg \
			--prefix="(hd0,gpt1)" \
			part_gpt fat multiboot2 search search_label gfxterm video_fb efi_gop all_video; \
		dd if=/dev/zero of=build/grub_fat.img bs=512 count=8192 status=none; \
		mkfs.fat -n TOXENOS build/grub_fat.img; \
		mcopy -i build/grub_fat.img build/kernel.bin ::kernel.bin; \
		mmd   -i build/grub_fat.img ::EFI ::EFI/BOOT; \
		mcopy -i build/grub_fat.img build/grubx64.efi ::EFI/BOOT/BOOTX64.EFI; \
		TOTAL_SECTS=26657; \
		dd if=/dev/zero             of=build/disk.img bs=512 count=$$TOTAL_SECTS status=none; \
		dd if=/usr/lib/grub/i386-pc/boot.img \
		                            of=build/disk.img bs=512 count=1 conv=notrunc status=none; \
		dd if=build/grub_core.img   of=build/disk.img bs=512 seek=34   conv=notrunc status=none; \
		dd if=build/grub_fat.img    of=build/disk.img bs=512 seek=2048 conv=notrunc status=none; \
		dd if=build/fs.img          of=build/disk.img bs=512 seek=10240 conv=notrunc status=none; \
		CORE_SECTS=$$(( ($$(stat -c %s build/grub_core.img) + 511) / 512 )); \
		tools/patch_diskboot build/disk.img $$CORE_SECTS $$TOTAL_SECTS; \
		echo "disk.img: GPT BIOS+UEFI (ESP gpt1 at LBA 2048, TxFS at LBA 10240)"; \
	else \
		echo "WARNING: grub2-mkimage not found — disk.img is raw TxFS (not bootable)"; \
		cp build/fs.img build/disk.img; \
	fi

clean:
	rm -rf build/*.o build/*.d build/*.bin build/*.iso build/mbedtls build/user
	@mkdir -p build

clean-target:
	rm -f build/target.img
