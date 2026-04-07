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
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/fat.c -o build/fat.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/ext2.c -o build/ext2.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/elf.c -o build/elf.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/tty.c -o build/tty.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/pipe.c -o build/pipe.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/waitqueue.c -o build/waitqueue.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/pci.c -o build/pci.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/e1000.c -o build/e1000.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/net.c -o build/net.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/tcp.c -o build/tcp.o
	# Build mbedTLS
	@mkdir -p build/mbedtls
	@for f in mbedtls/library/*.c; do \
		base=$$(basename $$f .c); \
		case $$base in net_sockets|timing|pkcs7|ssl_tls13*|mps_*|platform) continue ;; esac; \
		gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
			-DMBEDTLS_CONFIG_FILE='"../toxenos_config.h"' \
			-I mbedtls/include -I mbedtls \
			-c $$f -o build/mbedtls/$$base.o 2>/dev/null; \
	done
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-DMBEDTLS_CONFIG_FILE='"../mbedtls/toxenos_config.h"' \
		-I mbedtls/include -I mbedtls \
		-c mbedtls/toxenos_platform.c -o build/mbedtls/toxenos_platform.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
		-DMBEDTLS_CONFIG_FILE='"../mbedtls/toxenos_config.h"' \
		-I mbedtls/include -I mbedtls \
		-I include \
		-c kernel/tls.c -o build/tls.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/framebuffer.c -o build/framebuffer.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/font.c -o build/font.o
	gcc -ffreestanding -fno-stack-protector -fno-pic -m32 -c kernel/fbterm.c -o build/fbterm.o

	ld -m elf_i386 -T linker.ld -o build/kernel.bin \
		build/boot.o build/kernel.o build/keyboard.o build/idt.o build/isr.o \
		build/switch.o build/pic.o build/irq.o build/timer.o build/mm.o \
		build/process.o build/syscall.o build/paging.o build/tss.o build/ring3.o \
		build/vfs.o build/tmpfs.o build/ata.o build/txfs.o build/fat.o build/ext2.o build/elf.o \
		build/tty.o build/pipe.o build/waitqueue.o build/pci.o build/e1000.o build/net.o build/tcp.o build/tls.o \
		build/mbedtls/aes.o build/mbedtls/aesce.o build/mbedtls/aesni.o build/mbedtls/aria.o build/mbedtls/asn1parse.o build/mbedtls/asn1write.o build/mbedtls/base64.o build/mbedtls/bignum.o build/mbedtls/bignum_core.o build/mbedtls/bignum_mod.o build/mbedtls/bignum_mod_raw.o build/mbedtls/block_cipher.o build/mbedtls/camellia.o build/mbedtls/ccm.o build/mbedtls/chacha20.o build/mbedtls/chachapoly.o build/mbedtls/cipher.o build/mbedtls/cipher_wrap.o build/mbedtls/cmac.o build/mbedtls/constant_time.o build/mbedtls/ctr_drbg.o build/mbedtls/debug.o build/mbedtls/des.o build/mbedtls/dhm.o build/mbedtls/ecdh.o build/mbedtls/ecdsa.o build/mbedtls/ecjpake.o build/mbedtls/ecp.o build/mbedtls/ecp_curves.o build/mbedtls/ecp_curves_new.o build/mbedtls/entropy.o build/mbedtls/entropy_poll.o build/mbedtls/error.o build/mbedtls/gcm.o build/mbedtls/hkdf.o build/mbedtls/hmac_drbg.o build/mbedtls/lmots.o build/mbedtls/lms.o build/mbedtls/md.o build/mbedtls/md5.o build/mbedtls/memory_buffer_alloc.o build/mbedtls/nist_kw.o build/mbedtls/oid.o build/mbedtls/padlock.o build/mbedtls/pem.o build/mbedtls/pk.o build/mbedtls/pk_ecc.o build/mbedtls/pk_wrap.o build/mbedtls/pkcs12.o build/mbedtls/pkcs5.o build/mbedtls/pkparse.o build/mbedtls/pkwrite.o build/mbedtls/platform_util.o build/mbedtls/poly1305.o build/mbedtls/psa_crypto.o build/mbedtls/psa_crypto_aead.o build/mbedtls/psa_crypto_cipher.o build/mbedtls/psa_crypto_client.o build/mbedtls/psa_crypto_driver_wrappers_no_static.o build/mbedtls/psa_crypto_ecp.o build/mbedtls/psa_crypto_ffdh.o build/mbedtls/psa_crypto_hash.o build/mbedtls/psa_crypto_mac.o build/mbedtls/psa_crypto_pake.o build/mbedtls/psa_crypto_rsa.o build/mbedtls/psa_crypto_se.o build/mbedtls/psa_crypto_slot_management.o build/mbedtls/psa_crypto_storage.o build/mbedtls/psa_its_file.o build/mbedtls/psa_util.o build/mbedtls/ripemd160.o build/mbedtls/rsa.o build/mbedtls/rsa_alt_helpers.o build/mbedtls/sha1.o build/mbedtls/sha256.o build/mbedtls/sha3.o build/mbedtls/sha512.o build/mbedtls/ssl_cache.o build/mbedtls/ssl_ciphersuites.o build/mbedtls/ssl_client.o build/mbedtls/ssl_cookie.o build/mbedtls/ssl_debug_helpers_generated.o build/mbedtls/ssl_msg.o build/mbedtls/ssl_ticket.o build/mbedtls/ssl_tls.o build/mbedtls/ssl_tls12_client.o build/mbedtls/ssl_tls12_server.o build/mbedtls/threading.o build/mbedtls/version.o build/mbedtls/version_features.o build/mbedtls/x509.o build/mbedtls/x509_create.o build/mbedtls/x509_crl.o build/mbedtls/x509_crt.o build/mbedtls/x509_csr.o build/mbedtls/x509write.o build/mbedtls/x509write_crt.o build/mbedtls/x509write_csr.o build/mbedtls/toxenos_platform.o \
		build/user/shell_blob.o build/user/init_blob.o build/framebuffer.o build/font.o build/fbterm.o

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
	for cmd in ls shw mkef mkd rm echo pcd uname file help cp tree hex mv rname sif find bmsg proc end top sleeptest memtest pipetest nettest dns http ping https; do \
		gcc -ffreestanding -fno-stack-protector -fno-pic -m32 \
			-nostdlib -nostartfiles -Ttext=0x10000000 -no-pie -static \
			user/bin/$$cmd.c -o build/user/bin/$$cmd.elf || exit 1; \
	done

run: all populate
	qemu-system-i386 \
		-cdrom build/ToxenOS.iso \
		-drive file=build/disk.img,format=raw,index=0,media=disk \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-object filter-dump,id=f0,netdev=net0,file=/tmp/toxenos_net.pcap

disk:
	mkdir -p build
	dd if=/dev/zero of=build/disk.img bs=512 count=204800

tools/txfs_write: tools/txfs_write.c
	gcc -O2 -o tools/txfs_write tools/txfs_write.c

populate: tools/txfs_write
	dd if=/dev/zero of=build/disk.img bs=4096 count=25600
	tools/txfs_write build/disk.img build/user/bin/ls.elf /Programs/ls.elf
	tools/txfs_write build/disk.img build/user/bin/shw.elf /Programs/shw.elf
	tools/txfs_write build/disk.img build/user/bin/mkef.elf /Programs/mkef.elf
	tools/txfs_write build/disk.img build/user/bin/mkd.elf /Programs/mkd.elf
	tools/txfs_write build/disk.img build/user/bin/rm.elf /Programs/rm.elf
	tools/txfs_write build/disk.img build/user/bin/echo.elf /Programs/echo.elf
	tools/txfs_write build/disk.img build/user/bin/pcd.elf /Programs/pcd.elf
	tools/txfs_write build/disk.img build/user/bin/uname.elf /Programs/uname.elf
	tools/txfs_write build/disk.img build/user/bin/file.elf /Programs/file.elf
	tools/txfs_write build/disk.img build/user/bin/help.elf /Programs/help.elf
	tools/txfs_write build/disk.img build/user/bin/cp.elf /Programs/cp.elf
	tools/txfs_write build/disk.img build/user/bin/tree.elf /Programs/tree.elf
	tools/txfs_write build/disk.img build/user/bin/hex.elf /Programs/hex.elf
	tools/txfs_write build/disk.img build/user/bin/mv.elf /Programs/mv.elf
	tools/txfs_write build/disk.img build/user/bin/rname.elf /Programs/rname.elf
	tools/txfs_write build/disk.img build/user/bin/sif.elf /Programs/sif.elf
	tools/txfs_write build/disk.img build/user/bin/find.elf /Programs/find.elf
	tools/txfs_write build/disk.img build/user/bin/bmsg.elf /Programs/bmsg.elf
	tools/txfs_write build/disk.img build/user/bin/proc.elf /Programs/proc.elf
	tools/txfs_write build/disk.img build/user/bin/end.elf /Programs/end.elf
	tools/txfs_write build/disk.img build/user/bin/top.elf /Programs/top.elf
	tools/txfs_write build/disk.img build/user/bin/sleeptest.elf /Programs/sleeptest.elf
	tools/txfs_write build/disk.img build/user/bin/memtest.elf /Programs/memtest.elf
	tools/txfs_write build/disk.img build/user/bin/pipetest.elf /Programs/pipetest.elf
	tools/txfs_write build/disk.img build/user/bin/nettest.elf /Programs/nettest.elf
	tools/txfs_write build/disk.img build/user/bin/dns.elf /Programs/dns.elf
	tools/txfs_write build/disk.img build/user/bin/http.elf /Programs/http.elf
	tools/txfs_write build/disk.img build/user/bin/ping.elf /Programs/ping.elf
	tools/txfs_write build/disk.img build/user/bin/https.elf /Programs/https.elf
	tools/txfs_write build/disk.img build/user/hello.elf /hello.elf
	tools/txfs_write build/disk.img build/user/shell.elf /shell.elf
	tools/txfs_write build/disk.img build/user/init.elf /init.elf
	# Create /etc directory and tinit.cfg
	tools/txfs_write build/disk.img /dev/null /etc/.keep 2>/dev/null || true
	printf "# Tinit configuration\n# Add services like:\n# service myservice restart\n# shell\n" > /tmp/tinit.cfg
	tools/txfs_write build/disk.img /tmp/tinit.cfg /etc/tinit.cfg

clean:
	rm -rf build/*.o build/*.bin build/*.iso
	mkdir -p build
