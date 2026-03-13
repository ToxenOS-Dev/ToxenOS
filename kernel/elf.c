#include <stdint.h>
#include "../include/elf.h"
#include "../include/vfs.h"
#include "../include/mm.h"
#include "../include/paging.h"
#include "../include/vga.h"

int elf_load(const char* path, uint32_t* entry_out)
{
    // open the file
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;

    // read ELF header
    elf_header_t header;
    int bytes = vfs_read(fd, (uint8_t*)&header, sizeof(elf_header_t));
    if (bytes < (int)sizeof(elf_header_t))
    {
        vfs_close(fd);
        return -1;
    }

    // validate
    if (header.magic != ELF_MAGIC)
    {
        vfs_close(fd);
        return -1;
    }

    if (header.bits != 1 || header.machine != EM_386)
    {
        vfs_close(fd);
        return -1;
    }

    if (header.type != ET_EXEC)
    {
        vfs_close(fd);
        return -1;
    }

    // read program headers
    for (int i = 0; i < header.phnum; i++)
    {
        elf_phdr_t phdr;

        // seek to program header — we fake seek by reading
        // re-open for each phdr (simple approach)
        int pfd = vfs_open(path, VFS_O_READ);
        if (pfd < 0) { vfs_close(fd); return -1; }

        // skip to phdr offset
        uint32_t target = header.phoff + i * header.phentsize;
        uint8_t  skip_buf[64];
        uint32_t skipped = 0;
        while (skipped < target)
        {
            uint32_t to_skip = target - skipped;
            if (to_skip > 64) to_skip = 64;
            int r = vfs_read(pfd, skip_buf, to_skip);
            if (r <= 0) break;
            skipped += r;
        }

        vfs_read(pfd, (uint8_t*)&phdr, sizeof(elf_phdr_t));
        vfs_close(pfd);

        if (phdr.type != PT_LOAD) continue;
        if (phdr.memsz == 0)      continue;

        // allocate memory for this segment
        uint8_t* seg = (uint8_t*)kmalloc(phdr.memsz);
        if (!seg) { vfs_close(fd); return -1; }

        // zero it out first (handles BSS)
        for (uint32_t j = 0; j < phdr.memsz; j++)
            seg[j] = 0;

        // read segment data from file
        if (phdr.filesz > 0)
        {
            int sfd = vfs_open(path, VFS_O_READ);
            if (sfd < 0) { vfs_close(fd); return -1; }

            // skip to segment offset
            uint32_t target2 = phdr.offset;
            uint32_t skipped2 = 0;
            while (skipped2 < target2)
            {
                uint32_t to_skip = target2 - skipped2;
                if (to_skip > 64) to_skip = 64;
                int r = vfs_read(sfd, skip_buf, to_skip);
                if (r <= 0) break;
                skipped2 += r;
            }

            vfs_read(sfd, seg, phdr.filesz);
            vfs_close(sfd);
        }

        // map segment into memory at vaddr
        // for now: copy to the physical address directly
        // (proper per-process paging comes with process spawning)
        uint8_t* dst = (uint8_t*)phdr.vaddr;
        for (uint32_t j = 0; j < phdr.memsz; j++)
            dst[j] = seg[j];
    }

    vfs_close(fd);

    *entry_out = header.entry;
    return 0;
}