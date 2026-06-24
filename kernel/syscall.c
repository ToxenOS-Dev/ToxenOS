// ToxenOS/kernel/syscall.c
#include <stdint.h>
#include "../include/syscall.h"
#include "../include/uaccess.h"
#include "../include/klog.h"
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"
#include "../include/paging.h"
#include "../include/memmap.h"
#include "../include/pipe.h"
#include "../include/net.h"
#include "../include/tcp.h"
#include "../include/tls.h"
#include "../include/tty.h"
#include "../include/fbterm.h"
#include "../include/timer.h"
#include "../include/env.h"
#include "../include/ata.h"
#include "../include/crypto.h"
#include "../include/pmm.h"
#include "../include/txfs.h"
#include "../include/acpi.h"

// Embedded boot-time shell, packaged as native .nex bytes. This is safe
// because process_create_elf() -> load_binary_into_dir() already sniffs the
// ELF/NEX magic rather than assuming a format, so handing it NEX bytes here
// just works without any kernel changes.
extern uint8_t _binary_build_user_shell_nex_start[];
extern uint8_t _binary_build_user_shell_nex_end[];

// Bootable disk.img stored in RAM (set when ramdisk is a GRUB-prefixed image).
// SYS_INSTALL_CHUNK reads from here so the full bootable image is copied to disk.
extern uint8_t*  install_img_buf;
extern uint32_t  install_img_size;

static uint32_t kstrlen(const char* s) { uint32_t i=0; while(s[i]) i++; return i; }
static int kstreq(const char* a, const char* b) {
    int i = 0; while (a[i] && b[i] && a[i]==b[i]) i++; return a[i]==b[i];
}
static int kstarts(const char* s, const char* p) {
    int i = 0; while (p[i] && s[i]==p[i]) i++; return p[i]==0;
}

// Returns 1 if path is inside /BSM/SystemT/ and process is NOT admin
static int path_is_system_protected(const char* path) {
    if (!kstarts(path, "/C:/BSM/SystemT/")) return 0;
    return !process_current()->is_admin;  // elevated processes can write
}

// Returns 1 if path IS a core system directory that cannot be deleted
static int path_is_system_dir(const char* path) {
    return kstreq(path, "/C:/BSM") || kstreq(path, "/C:/BSM/") ||
           kstreq(path, "/C:/etc") || kstreq(path, "/C:/etc/");
}

uint32_t __attribute__((cdecl)) syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    switch (eax)
    {
        case SYS_EXIT:
            sys_exit((int)ebx);
            return 0;

        case SYS_PRINT:
        {
            CHECK_USER_STR(ebx);
            const char* s = (const char*)ebx;
            process_t* cur = process_current();
            if (cur->stdout_fd >= 0 && cur->stdout_fd < VFS_MAX_FDS) {
                uint32_t len = 0;
                while (s[len]) len++;
                vfs_write(cur->stdout_fd, (const uint8_t*)s, len);
            } else {
                sys_print(s);
            }
            return 0;
        }

        case SYS_GETCHAR:
        {
            process_t* cur = process_current();
            if (cur->stdin_fd >= 0 && cur->stdin_fd < VFS_MAX_FDS) {
                uint8_t ch = 0;
                int r = vfs_read(cur->stdin_fd, &ch, 1);
                return (r > 0) ? ch : 0;
            }
            if (keyboard_available())
                return keyboard_getchar();
            return 0;
        }

        case SYS_SETCOLOR:
            set_color((uint8_t)ebx);
            return 0;

        case SYS_ERASE:
            erase_char();
            return 0;

        case SYS_CLEAR:
            clear_screen();
            return 0;

        case SYS_REBOOT:
            __asm__ volatile("cli");
            // Keyboard controller CPU reset line (works on virtually all x86 hardware)
            __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
            // Fallback: triple-fault via null IDT
            { volatile struct { uint16_t limit; uint32_t base; } idt = {0, 0};
              __asm__ volatile("lidt (%0); int $3" :: "r"(&idt)); }
            while(1) __asm__ volatile("hlt");
            return 0;

        case SYS_SHUTDOWN:
            acpi_shutdown();
            return 0;

        case SYS_READDIR:
            CHECK_USER_STR(ebx);
            CHECK_USER_PTR(ecx, VFS_NAME_MAX);
            return vfs_readdir((const char*)ebx, (char*)ecx, edx);

        case SYS_OPEN:
            CHECK_USER_STR(ebx);
            if ((ecx & (VFS_O_WRITE | VFS_O_CREATE)) && path_is_system_protected((const char*)ebx))
                return (uint32_t)-1;
            return vfs_open((const char*)ebx, ecx);

        case SYS_READ:
            CHECK_USER_PTR(ecx, edx);
            return vfs_read(ebx, (uint8_t*)ecx, edx);

        case SYS_WRITE:
            CHECK_USER_PTR(ecx, edx);
            return vfs_write(ebx, (const uint8_t*)ecx, edx);

        case SYS_CLOSE:
            return vfs_close(ebx);

        case SYS_STAT:
        {
            CHECK_USER_STR(ebx);
            uint32_t size = 0;
            if (vfs_stat((const char*)ebx, &size) < 0) return (uint32_t)-1;
            return size;
        }

        case SYS_ISDIR:
            CHECK_USER_STR(ebx);
            return vfs_isdir((const char*)ebx);

        case SYS_MKDIR:
            CHECK_USER_STR(ebx);
            if (path_is_system_protected((const char*)ebx)) return (uint32_t)-1;
            return vfs_mkdir((const char*)ebx);

        case SYS_REMOVE:
            CHECK_USER_STR(ebx);
            if (path_is_system_protected((const char*)ebx)) return (uint32_t)-1;
            if (path_is_system_dir((const char*)ebx)) return (uint32_t)-1;
            return vfs_remove((const char*)ebx);

        case SYS_YIELD:
            scheduler();
            return 0;

        case SYS_GETPID:
            return process_current()->pid;

        case SYS_GET_TTY:
            return tty_current();

        case SYS_MY_TTY:
            return process_current()->tty;

        case SYS_EXEC:
            CHECK_USER_STR(ebx);
            return sys_exec((const char*)ebx);

        /* Helper: propagate is_admin from parent to child on every spawn */
        #define INHERIT_ADMIN(pid) do { \
            process_t* _ch = process_get_by_pid(pid); \
            if (_ch) _ch->is_admin = process_current()->is_admin; \
        } while(0)

        case SYS_SPAWN:
        { CHECK_USER_STR(ebx); int _p=sys_spawn((const char*)ebx); if(_p>=0) INHERIT_ADMIN(_p); return _p; }

        case SYS_WAIT:
            sys_wait((int)ebx);
            return 0;

        case SYS_SPAWN_TTY:
        { CHECK_USER_STR(ebx); int _p=sys_spawn_tty((const char*)ebx,(int)ecx); if(_p>=0) INHERIT_ADMIN(_p); return _p; }

        case SYS_GET_ARGS:
        {
            CHECK_USER_PTR(ebx, 256);
            char* buf = (char*)ebx;
            const char* src = process_current()->args;
            int i = 0;
            while (src[i] && i < 255) { buf[i] = src[i]; i++; }
            buf[i] = 0;
            return i;
        }

        case SYS_SPAWN_ARGS:
        {
            CHECK_USER_STR(ebx); if(edx){CHECK_USER_STR(edx);}
            int _p=sys_spawn_tty_args((const char*)ebx,(int)ecx,edx?(const char*)edx:"");
            if(_p>=0) { INHERIT_ADMIN(_p); }
            return _p;
        }

        case SYS_IS_ALIVE:
            return process_is_alive((int)ebx);

        case SYS_KEYAVAIL:
            return keyboard_available();

        case SYS_SPAWN_EMBEDDED:
        {
            uint8_t* buf  = _binary_build_user_shell_nex_start;
            uint32_t size = (uint32_t)(_binary_build_user_shell_nex_end
                                      - _binary_build_user_shell_nex_start);
            int tty = (int)ebx;
            int pid = process_create_elf("shell", buf, size);
            if (pid < 0) return (uint32_t)-1;
            tty_assign_pid(pid, tty);
            INHERIT_ADMIN(pid);
            return pid;
        }

        case SYS_BMSG:
        {
            if (ecx == 0) return 0;
            CHECK_USER_PTR(ebx, ecx);
            char* out    = (char*)ebx;
            int   sz     = (int)(uint32_t)ecx;
            return klog_read(out, sz);
        }

        case SYS_PROC_LIST:
        {
            if (ecx == 0) return 0;
            CHECK_USER_PTR(ebx, ecx);
            uint8_t* buf  = (uint8_t*)ebx;
            uint32_t size = (uint32_t)ecx;
            uint32_t written = 0;
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state == PROCESS_DEAD) continue;
                if (written + 40 > size) break;
                buf[written+0] = processes[i].pid & 0xFF;
                buf[written+1] = (processes[i].pid >> 8) & 0xFF;
                buf[written+2] = 0; buf[written+3] = 0;
                buf[written+4] = (uint8_t)processes[i].state;
                buf[written+5] = 0; buf[written+6] = 0; buf[written+7] = 0;
                for (int j = 0; j < 32; j++)
                    buf[written+8+j] = processes[i].name[j];
                written += 40;
            }
            return (int)written;
        }

        case SYS_KILL:
        {
            int pid = (int)ebx;
            if (pid <= 0) return (uint32_t)-1;
            process_t* target = process_get_by_pid(pid);
            if (!target) return (uint32_t)-1;
            target->state = PROCESS_DEAD;
            // Wake any process waiting on this pid
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state == PROCESS_WAITING &&
                    processes[i].waiting_for == pid) {
                    processes[i].state       = PROCESS_READY;
                    processes[i].waiting_for = -1;
                }
            }
            return 0;
        }

        case SYS_SIGINT_TARGET:
            keyboard_set_sigint_target((int)ebx);
            return 0;

        case SYS_PIPE:
        {
            CHECK_USER_PTR(ebx, sizeof(int));
            CHECK_USER_PTR(ecx, sizeof(int));
            int* rfd_ptr = (int*)ebx;
            int* wfd_ptr = (int*)ecx;
            int rfd, wfd;
            if (vfs_pipe(&rfd, &wfd) < 0) return (uint32_t)-1;
            *rfd_ptr = rfd;
            *wfd_ptr = wfd;
            return 0;
        }

        case SYS_SPAWN_PIPE:
        {
            CHECK_USER_STR(ebx);
            if (ecx) { CHECK_USER_STR(ecx); }
            const char* path    = (const char*)ebx;
            const char* args    = ecx ? (const char*)ecx : "";
            int         stdin_f = (int)(int16_t)(edx & 0xFFFF);
            int         stdout_f= (int)(int16_t)((edx >> 16) & 0xFFFF);
            int tty = process_current()->tty;
            if (tty < 0) tty = 0;
            int pid = sys_spawn_tty_args(path, tty, args);
            if (pid < 0) return (uint32_t)-1;
            process_t* child = process_get_by_pid(pid);
            if (child) {
                child->stdin_fd  = stdin_f;
                child->stdout_fd = stdout_f;
            }
            INHERIT_ADMIN(pid);
            return pid;
        }

        case SYS_SLEEP:
            sys_sleep((uint32_t)ebx);
            return 0;

        case SYS_SBRK:
            return (int)sys_sbrk((int32_t)ebx);

        case SYS_SPAWN_INHERIT:
        {
            CHECK_USER_STR(ebx);
            if (ecx) { CHECK_USER_STR(ecx); }
            const char* path  = (const char*)ebx;
            const char* args  = ecx ? (const char*)ecx : "";
            const int*  ilist = (const int*)edx;
            if (ilist) { CHECK_USER_PTR(ilist, sizeof(int)); }
            int tty = process_current()->tty;
            if (tty < 0) tty = 0;
            int pid = sys_spawn_tty_args(path, tty, args);
            if (pid < 0) return (uint32_t)-1;
            process_t* child = process_get_by_pid(pid);
            if (child && ilist) {
                for (int i = 0; ; i += 2) {
                    if (!uaccess_ok(&ilist[i], sizeof(int) * 2)) break;
                    if (ilist[i] < 0) break;
                    int child_local = ilist[i];
                    int parent_gfd  = ilist[i+1];
                    if (child_local < VFS_PROC_FDS && parent_gfd >= 0)
                        child->fds.local_fds[child_local] = parent_gfd;
                }
            }
            INHERIT_ADMIN(pid);
            return pid;
        }

        case SYS_NET_SEND_UDP:
        {
            CHECK_USER_PTR(edx, sizeof(uint8_t*) + sizeof(uint16_t));
            uint32_t  dst_ip   = (uint32_t)ebx;
            uint16_t  src_port = (uint16_t)(ecx >> 16);
            uint16_t  dst_port = (uint16_t)(ecx & 0xFFFF);
            uint8_t*  data_ptr = *(uint8_t**)edx;
            uint16_t  data_len = *(uint16_t*)((uint8_t*)edx + sizeof(uint8_t*));
            if (data_len > 0) { CHECK_USER_PTR(data_ptr, data_len); }
            return net_udp_send(dst_ip, src_port, dst_port, data_ptr, data_len);
        }

        case SYS_NET_POLL:
            net_poll();
            return 0;

        case SYS_NET_GET_IP:
            return (int)net_ip;

        case SYS_NET_UDP_RECV:
        {
            CHECK_USER_PTR(edx, sizeof(uint16_t) * 2 + sizeof(uint32_t));
            uint16_t  port    = (uint16_t)ebx;
            uint16_t  maxlen  = *(uint16_t*)edx;
            uint32_t  timeout = *(uint32_t*)((uint8_t*)edx + sizeof(uint16_t) * 2);
            if (maxlen > 0) { CHECK_USER_PTR(ecx, maxlen); }
            net_udp_open(port);
            return net_udp_recv(port, (uint8_t*)ecx, maxlen, 0, timeout);
        }

        case SYS_TCP_CONNECT:
            return tcp_connect((uint32_t)ebx, (uint16_t)ecx);

        case SYS_TCP_SEND:
            if (edx > 0) { CHECK_USER_PTR(ecx, edx); }
            return tcp_send((int)ebx, (const uint8_t*)ecx, (uint32_t)edx);

        case SYS_TCP_RECV:
        {
            CHECK_USER_PTR(edx, sizeof(uint16_t) * 2 + sizeof(uint32_t));
            int      sock    = (int)ebx;
            uint16_t maxlen  = *(uint16_t*)edx;
            uint32_t timeout = *(uint32_t*)((uint8_t*)edx + sizeof(uint16_t) * 2);
            if (maxlen > 0) { CHECK_USER_PTR(ecx, maxlen); }
            return tcp_recv(sock, (uint8_t*)ecx, maxlen, timeout);
        }

        case SYS_TCP_CLOSE:
            tcp_close((int)ebx);
            return 0;

        case SYS_PING:
            return net_ping((uint32_t)ebx, (uint16_t)ecx, (uint32_t)edx);

        case SYS_TLS_CONNECT:
            CHECK_USER_STR(edx);
            return tls_connect((uint32_t)ebx, (uint16_t)ecx, (const char*)edx);

        case SYS_TLS_SEND:
            if (edx > 0) { CHECK_USER_PTR(ecx, edx); }
            return tls_send((int)ebx, (const uint8_t*)ecx, (uint32_t)edx);

        case SYS_TLS_RECV:
        {
            CHECK_USER_PTR(edx, sizeof(uint16_t) * 2 + sizeof(uint32_t));
            uint16_t maxlen  = *(uint16_t*)edx;
            uint32_t timeout = *(uint32_t*)((uint8_t*)edx + sizeof(uint16_t) * 2);
            if (maxlen > 65535) return (uint32_t)-1;
            if (maxlen > 0) { CHECK_USER_PTR(ecx, maxlen); }
            return tls_recv((int)ebx, (uint8_t*)ecx, maxlen, timeout);
        }

        case SYS_TLS_CLOSE:
            tls_close((int)ebx);
            return 0;

        case SYS_PAGE_FLAGS: {
            // Walk the current process's page directory and return the flags
            // for the given virtual address. Returns 0 if not mapped.
            // Bit 0 = present, bit 1 = writable, bit 2 = user-accessible.
            uint32_t vaddr    = (uint32_t)ebx;
            uint32_t dir_idx  = vaddr >> 22;
            uint32_t page_idx = (vaddr >> 12) & 0x3FF;
            process_t* cp     = process_current();
            uint32_t* dir     = cp->page_directory;
            if (!(dir[dir_idx] & PAGE_PRESENT)) return 0;
            uint32_t* tbl = (uint32_t*)KPHYS_TO_VIRT(dir[dir_idx] & ~0xFFFu);
            uint32_t   pte = tbl[page_idx];
            if (!(pte & PAGE_PRESENT)) return 0;
            return pte & 0x7;  // present + writable + user bits
        }

        case SYS_SYSCTL:
        {
            if (!ebx || !ecx || !edx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            CHECK_USER_PTR(ecx, edx);
            const char* key = (const char*)ebx;
            char*       out = (char*)ecx;
            uint32_t    maxl = (uint32_t)edx;

            // copy string into user buffer, return length
            #define SC_STR(s) do { \
                const char* _s=(s); uint32_t _i=0; \
                while(_s[_i]&&_i<maxl-1){out[_i]=_s[_i];_i++;} \
                out[_i]=0; return (int)_i; } while(0)

            // write uint32 as decimal into user buffer
            #define SC_NUM(v) do { \
                char _t[12]; int _i=11; _t[11]=0; uint32_t _v=(v); \
                if(!_v){_t[--_i]='0';}else{while(_v){_t[--_i]='0'+_v%10;_v/=10;}} \
                SC_STR(_t+_i); } while(0)

            if (kstreq(key, "version"))  SC_STR("ToxenOS 1.0 (i386)");
            if (kstreq(key, "hostname")) {
                char _hn[64];
                if (env_get("hostname", _hn, 64) >= 0) SC_STR(_hn);
                SC_STR("toxenos");
            }
            if (kstreq(key, "uptime"))   SC_NUM(timer_getticks() / 100);
            if (kstreq(key, "procs")) {
                int n = 0;
                for (int i = 0; i < MAX_PROCESSES; i++)
                    if (processes[i].state != PROCESS_DEAD) n++;
                SC_NUM((uint32_t)n);
            }
            if (kstreq(key, "net.ip"))   SC_NUM(net_ip);
            if (kstreq(key, "net.gw"))   SC_NUM(net_gateway);
            if (kstreq(key, "net.mask")) SC_NUM(net_mask);
            if (kstreq(key, "mem.total")) SC_NUM(pmm_total_frames() * 4);
            if (kstreq(key, "mem.free"))  SC_NUM(pmm_free_frames()  * 4);
            if (kstreq(key, "disk.total")) {
                uint32_t t = 0, f = 0; txfs_diskstats(&t, &f); SC_NUM(t);
            }
            if (kstreq(key, "disk.free")) {
                uint32_t t = 0, f = 0; txfs_diskstats(&t, &f); SC_NUM(f);
            }
            if (kstreq(key, "term.cols")) SC_NUM((uint32_t)fbterm_cols());
            if (kstreq(key, "term.rows")) SC_NUM((uint32_t)fbterm_rows());

            #undef SC_STR
            #undef SC_NUM
            return (uint32_t)-1;
        }

        case SYS_IS_ADMIN:
            return process_current()->is_admin;

        case SYS_SET_ADMIN:
            process_current()->is_admin = (uint8_t)ebx;
            return 0;

        case SYS_INSTALL_DRIVE:
        {
            // Block-copy TxFS filesystem from drive 0 to target drive
            // ebx = target drive number (1-3)
            uint8_t target = (uint8_t)ebx;
            if (target == 0 || target > 3) return (uint32_t)-1;
            uint32_t sectors_per_block = TXFS_BLOCK_SIZE / 512;
            uint32_t total_blocks = 25600; // standard TxFS disk size
            static uint8_t copy_buf[TXFS_BLOCK_SIZE];
            for (uint32_t b = 0; b < total_blocks; b++) {
                ata_read_drive(0, b * sectors_per_block, copy_buf, sectors_per_block);
                ata_write_drive(target, b * sectors_per_block, copy_buf, sectors_per_block);
            }
            return 0;
        }

        case SYS_INSTALL_CHUNK:
        {
            // ebx=target drive, ecx=start_block, edx=num_blocks
            // When install_img_buf is set, reads from the bootable disk.img in RAM
            // (includes GRUB prefix + kernel.bin + TxFS) so the target disk is bootable.
            uint8_t tdrv = (uint8_t)ebx;
            if (tdrv == 0 || tdrv > 3) return (uint32_t)-1;
            uint32_t sects = TXFS_BLOCK_SIZE / 512;
            static uint8_t cbuf[TXFS_BLOCK_SIZE];
            for (uint32_t b = ecx; b < ecx + edx; b++) {
                if (install_img_buf) {
                    uint32_t off = b * TXFS_BLOCK_SIZE;
                    if (off < install_img_size) {
                        uint32_t len = install_img_size - off;
                        if (len > TXFS_BLOCK_SIZE) len = TXFS_BLOCK_SIZE;
                        const uint8_t* src = install_img_buf + off;
                        for (uint32_t i = 0; i < len; i++) cbuf[i] = src[i];
                        for (uint32_t i = len; i < TXFS_BLOCK_SIZE; i++) cbuf[i] = 0;
                    } else {
                        for (uint32_t i = 0; i < TXFS_BLOCK_SIZE; i++) cbuf[i] = 0;
                    }
                } else {
                    ata_read_drive(0, b * sects, cbuf, sects);
                }
                ata_write_drive(tdrv, b * sects, cbuf, sects);
            }
            return 0;
        }

        case SYS_DISK_SECTORS:
            return ata_get_sectors((uint8_t)ebx);

        case SYS_DISK_READ:
        {
            // ebx=drive, ecx=lba, edx=ptr to {uint8_t* buf, uint32_t sectors}
            if (!edx) return (uint32_t)-1;
            CHECK_USER_PTR(edx, 8);
            uint32_t* p = (uint32_t*)edx;
            uint8_t* buf = (uint8_t*)p[0]; uint32_t sec = p[1];
            if (!buf || !sec || sec > 128) return (uint32_t)-1;
            CHECK_USER_PTR(buf, sec * 512);
            return (uint32_t)ata_read_drive((uint8_t)ebx, ecx, buf, sec);
        }

        case SYS_DISK_WRITE:
        {
            // ebx = drive, ecx = lba, edx = ptr to {uint8_t* buf, uint32_t sectors}
            if (!edx) return (uint32_t)-1;
            CHECK_USER_PTR(edx, 8);
            uint32_t* params = (uint32_t*)edx;
            uint8_t*  buf     = (uint8_t*)params[0];
            uint32_t  sectors = params[1];
            if (!buf || !sectors || sectors > 128) return (uint32_t)-1;
            CHECK_USER_PTR(buf, sectors * 512);
            return (uint32_t)ata_write_drive((uint8_t)ebx, ecx, buf, sectors);
        }

        case SYS_SNAP_CREATE:
        {
            if (!ebx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            return (uint32_t)txfs_snap_create((const char*)ebx, timer_getticks());
        }
        case SYS_SNAP_RESTORE:
        {
            if (!ebx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            return (uint32_t)txfs_snap_restore((const char*)ebx);
        }
        case SYS_SNAP_DELETE:
        {
            if (!ebx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            return (uint32_t)txfs_snap_delete((const char*)ebx);
        }
        case SYS_SNAP_LIST:
        {
            // ebx = char[10][64] buffer, ecx = uint32_t[10] timestamps, edx = max
            if (!ebx || !edx) return 0;
            CHECK_USER_PTR(ebx, (uint32_t)(edx * 64));
            return (uint32_t)txfs_snap_list((char(*)[64])ebx,
                                            ecx ? (uint32_t*)ecx : 0, (int)edx);
        }

        case SYS_PBKDF2:
        {
            // ebx = ptr to: [pwd_ptr(4) pwd_len(4) salt_ptr(4) salt_len(4)
            //                iterations(4) out_ptr(4) out_len(4)]  = 28 bytes
            if (!ebx) return (uint32_t)-1;
            CHECK_USER_PTR(ebx, 28);
            uint32_t* p       = (uint32_t*)ebx;
            const uint8_t* pwd  = (const uint8_t*)p[0];
            uint32_t       plen = p[1];
            const uint8_t* salt = (const uint8_t*)p[2];
            uint32_t       slen = p[3];
            uint32_t       iter = p[4];
            uint8_t*       out  = (uint8_t*)p[5];
            uint32_t       olen = p[6];
            if (plen > 256 || slen > 64 || olen > 64 || iter > 500000) return (uint32_t)-1;
            if (plen) { CHECK_USER_PTR(pwd,  plen); }
            if (slen) { CHECK_USER_PTR(salt, slen); }
            CHECK_USER_PTR(out, olen);
            return (uint32_t)kernel_pbkdf2_sha256(pwd, plen, salt, slen, iter, out, olen);
        }

        case SYS_ELEVATE:
        {
            // Prompt is handled in USER SPACE (tox_pkg.c) to avoid
            // blocking with interrupts disabled in the syscall handler.
            // This syscall just grants elevation and logs it.
            if (ebx) { CHECK_USER_STR(ebx); }
            process_t* cur = process_current();
            const char* reason = ebx ? (const char*)ebx : "privileged action";
            cur->is_admin = 1;
            // Log to /C:/etc/priv.log
            {
                int lfd = vfs_open("/C:/etc/priv.log",
                                   VFS_O_WRITE | VFS_O_CREATE | VFS_O_APPEND);
                if (lfd >= 0) {
                    vfs_write(lfd, (const uint8_t*)"[ALLOW] ", 8);
                    vfs_write(lfd, (const uint8_t*)cur->name, kstrlen(cur->name));
                    vfs_write(lfd, (const uint8_t*)" -> ", 4);
                    vfs_write(lfd, (const uint8_t*)reason, kstrlen(reason));
                    vfs_write(lfd, (const uint8_t*)"\n", 1);
                    vfs_close(lfd);
                }
            }
            return 0;
        }

        case SYS_CHMOD:
            CHECK_USER_STR(ebx);
            return vfs_chmod((const char*)ebx, (uint32_t)ecx);

        case SYS_GETMODE:
            CHECK_USER_STR(ebx);
            return vfs_getmode((const char*)ebx);

        case SYS_GETENV:
        {
            if (!ebx || !ecx || !edx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            CHECK_USER_PTR(ecx, edx);
            return env_get((const char*)ebx, (char*)ecx, (uint32_t)edx);
        }

        case SYS_SETENV:
        {
            if (!ebx) return (uint32_t)-1;
            CHECK_USER_STR(ebx);
            if (ecx) { CHECK_USER_STR(ecx); }
            return env_set((const char*)ebx, ecx ? (const char*)ecx : "");
        }

        case SYS_GETTIME:
        {
            if (!ebx || !ecx) return (uint32_t)-1;
            CHECK_USER_PTR(ebx, ecx);
            char* buf = (char*)ebx;
            uint32_t maxl = (uint32_t)ecx;
            if (maxl < 20) return (uint32_t)-1;

            // Read CMOS RTC via I/O ports 0x70/0x71
            // Wait until RTC update is not in progress (bit 7 of reg 0x0A)
            uint8_t a;
            do {
                __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x0A), "Nd"((uint16_t)0x70));
                __asm__ volatile("inb %1, %0" : "=a"(a) : "Nd"((uint16_t)0x71));
            } while (a & 0x80);

            #define CMOS_READ(reg, out) do { \
                __asm__ volatile("outb %0,%1"::"a"((uint8_t)(reg)),"Nd"((uint16_t)0x70)); \
                __asm__ volatile("inb %1,%0":"=a"(out):"Nd"((uint16_t)0x71)); \
            } while(0)

            uint8_t sec, min, hour, day, mon, yr, cent = 20;
            CMOS_READ(0x00, sec);  CMOS_READ(0x02, min);
            CMOS_READ(0x04, hour); CMOS_READ(0x07, day);
            CMOS_READ(0x08, mon);  CMOS_READ(0x09, yr);
            CMOS_READ(0x32, cent);
            #undef CMOS_READ

            // Check if values are BCD (bit 2 of status reg B = 0 means BCD)
            uint8_t statb;
            __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x0B),"Nd"((uint16_t)0x70));
            __asm__ volatile("inb %1,%0":"=a"(statb):"Nd"((uint16_t)0x71));
            if (!(statb & 0x04)) {
                sec  = (uint8_t)((sec  >> 4) * 10 + (sec  & 0xF));
                min  = (uint8_t)((min  >> 4) * 10 + (min  & 0xF));
                hour = (uint8_t)((hour >> 4) * 10 + (hour & 0xF));
                day  = (uint8_t)((day  >> 4) * 10 + (day  & 0xF));
                mon  = (uint8_t)((mon  >> 4) * 10 + (mon  & 0xF));
                yr   = (uint8_t)((yr   >> 4) * 10 + (yr   & 0xF));
                cent = (uint8_t)((cent >> 4) * 10 + (cent & 0xF));
            }
            if (cent == 0) cent = 20;
            uint16_t year = (uint16_t)(cent * 100 + yr);

            // Format: "YYYY-MM-DD HH:MM:SS"
            #define D2(buf, i, v) do { (buf)[i]='0'+(v)/10; (buf)[i+1]='0'+(v)%10; } while(0)
            D2(buf, 0, year/100); D2(buf, 2, year%100);
            buf[4]='-'; D2(buf,5,mon);  buf[7]='-'; D2(buf,8,day);
            buf[10]=' '; D2(buf,11,hour); buf[13]=':';
            D2(buf,14,min); buf[16]=':'; D2(buf,17,sec);
            buf[19]=0;
            #undef D2
            return 19;
        }

        case SYS_GETUID:   // 75
            return process_current()->uid;

        case SYS_SETUID:   // 76
            process_current()->uid = (uint32_t)ebx;
            return 0;

        case SYS_WAIT_STATUS:  // 78
        {
            extern int sys_wait_status(int pid);
            return (uint32_t)sys_wait_status((int)ebx);
        }

        case SYS_ENV_LIST:  // 79
        {
            // ebx=index, ecx=key_buf (32 bytes), edx=val_buf (128 bytes)
            if (!ecx || !edx) return (uint32_t)-1;
            CHECK_USER_PTR(ecx, 32);
            CHECK_USER_PTR(edx, 128);
            return (uint32_t)env_list((int)ebx, (char*)ecx, (char*)edx);
        }

        case SYS_WHOAMI:
        {
            if (!ebx || !ecx) return (uint32_t)-1;
            CHECK_USER_PTR(ebx, ecx);
            char ubuf[64];
            if (env_get("USER", ubuf, sizeof(ubuf)) < 0)
                ubuf[0] = 0;
            char* dst = (char*)ebx;
            uint32_t max = (uint32_t)ecx;
            uint32_t i = 0;
            while (ubuf[i] && i < max - 1) { dst[i] = ubuf[i]; i++; }
            dst[i] = 0;
            return (uint32_t)i;
        }

        default:
            return (uint32_t)-1;
    }
}

void sys_exit(int code)         { process_current()->exit_code = code; process_exit(); }
void sys_print(const char* str) { print(str); }
char sys_getchar()              { return keyboard_getchar(); }
int  sys_getpid()               { return process_current()->pid; }

void syscall_init()
{
    extern void isr128();
    idt_set_gate_user(0x80, (uint32_t)isr128);
}
