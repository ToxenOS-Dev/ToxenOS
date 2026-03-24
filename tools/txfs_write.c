// txfs_write.c — Host-side tool to write files into a TXFS disk image.
// Usage: txfs_write <disk.img> <local_file> <txfs_path> [<local_file> <txfs_path>...]
// Example: txfs_write build/disk.img build/user/hello.elf /hello.elf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TXFS_MAGIC         0x54584653
#define TXFS_BLOCK_SIZE    4096
#define TXFS_MAX_INODES    128
#define TXFS_DIRECT_BLOCKS 12
#define TXFS_BLOCK_SUPER   1
#define TXFS_BLOCK_IBITMAP 2
#define TXFS_BLOCK_BBITMAP 3
#define TXFS_BLOCK_INODES  4
#define TXFS_BLOCK_DATA    36
#define TXFS_TYPE_FILE     0x1
#define TXFS_TYPE_DIR      0x2

typedef struct {
    uint32_t magic,version,block_size,total_blocks,free_blocks;
    uint32_t total_inodes,free_inodes,root_inode;
    uint8_t  pad[4096-32];
} __attribute__((packed)) sb_t;

typedef struct {
    uint32_t mode,uid,size,created,modified,links;
    uint32_t blocks[TXFS_DIRECT_BLOCKS];
    uint32_t indirect,dindirect;
    uint8_t  pad[256-(6+TXFS_DIRECT_BLOCKS+2)*4];
} __attribute__((packed)) inode_t;

typedef struct {
    uint32_t inode; uint16_t name_len; uint8_t type; char name[256];
} __attribute__((packed)) dirent_t;

static FILE* disk;
static sb_t sb;
static uint8_t imap[TXFS_BLOCK_SIZE], bmap[TXFS_BLOCK_SIZE];

static void rd(uint32_t b,void* buf){fseek(disk,b*TXFS_BLOCK_SIZE,SEEK_SET);fread(buf,TXFS_BLOCK_SIZE,1,disk);}
static void wr(uint32_t b,const void* buf){fseek(disk,b*TXFS_BLOCK_SIZE,SEEK_SET);fwrite(buf,TXFS_BLOCK_SIZE,1,disk);fflush(disk);}
static void rdsb(){rd(TXFS_BLOCK_SUPER,&sb);}
static void wrsb(){wr(TXFS_BLOCK_SUPER,&sb);}

static int btest(uint8_t* m,int i){return(m[i/8]>>(i%8))&1;}
static void bset(uint8_t* m,int i){m[i/8]|=(1<<(i%8));}
static int balloc(uint8_t* m,int max){for(int i=0;i<max;i++)if(!btest(m,i)){bset(m,i);return i;}return -1;}

static void rdinode(uint32_t n,inode_t* v){uint8_t buf[TXFS_BLOCK_SIZE];rd(TXFS_BLOCK_INODES+(n/16),buf);memcpy(v,buf+(n%16)*sizeof(inode_t),sizeof(inode_t));}
static void wrinode(uint32_t n,const inode_t* v){uint8_t buf[TXFS_BLOCK_SIZE];rd(TXFS_BLOCK_INODES+(n/16),buf);memcpy(buf+(n%16)*sizeof(inode_t),v,sizeof(inode_t));wr(TXFS_BLOCK_INODES+(n/16),buf);}

static int ablock(){rd(TXFS_BLOCK_BBITMAP,bmap);int b=balloc(bmap,sb.total_blocks);if(b<0)return -1;wr(TXFS_BLOCK_BBITMAP,bmap);sb.free_blocks--;wrsb();return b+TXFS_BLOCK_DATA;}
static int ainode(){rd(TXFS_BLOCK_IBITMAP,imap);int i=balloc(imap,sb.total_inodes);if(i<0)return -1;wr(TXFS_BLOCK_IBITMAP,imap);sb.free_inodes--;wrsb();return i;}

static void format(uint32_t total){
    printf("Formatting TXFS (%u blocks)...\n",total);
    memset(&sb,0,sizeof(sb));
    sb.magic=TXFS_MAGIC;sb.version=1;sb.block_size=TXFS_BLOCK_SIZE;
    sb.total_blocks=total-TXFS_BLOCK_DATA;sb.free_blocks=sb.total_blocks;
    sb.total_inodes=TXFS_MAX_INODES;sb.free_inodes=TXFS_MAX_INODES-1;
    wrsb();
    uint8_t z[TXFS_BLOCK_SIZE]={0};
    wr(TXFS_BLOCK_IBITMAP,z);wr(TXFS_BLOCK_BBITMAP,z);
    rd(TXFS_BLOCK_IBITMAP,imap);bset(imap,0);wr(TXFS_BLOCK_IBITMAP,imap);
    inode_t root={0};root.mode=(TXFS_TYPE_DIR<<12)|0x1C0;root.links=1;
    wrinode(0,&root);printf("Done.\n");
}

static int write_file(const char* txpath, const char* lpath){
    FILE* f=fopen(lpath,"rb");
    if(!f){fprintf(stderr,"Cannot open %s\n",lpath);return -1;}
    fseek(f,0,SEEK_END);long fsz=ftell(f);rewind(f);
    uint8_t* data=malloc(fsz);fread(data,1,fsz,f);fclose(f);

    // find parent dir and filename
    char path[256];strncpy(path,txpath,255);
    char* p=path;if(*p=='/')p++;
    int dir_ino=0;
    char* fname=p;
    char* sl=strrchr(p,'/');
    if(sl){
        *sl=0;fname=sl+1;
        // navigate path
        char* c=p;
        while(c&&*c){
            char* nx=strchr(c,'/');if(nx)*nx=0;
            // find c in dir_ino
            inode_t dir;rdinode(dir_ino,&dir);
            uint8_t buf[TXFS_BLOCK_SIZE];
            int found=-1;
            if(dir.blocks[0]){
                rd(dir.blocks[0],buf);
                for(uint32_t o=0;o+sizeof(dirent_t)<=TXFS_BLOCK_SIZE;o+=sizeof(dirent_t)){
                    dirent_t* de=(dirent_t*)(buf+o);
                    if(de->inode&&strcmp(de->name,c)==0){found=de->inode;break;}
                }
            }
            if(found<0){
                // create subdir
                int ni=ainode();
                inode_t nd={0};nd.mode=(TXFS_TYPE_DIR<<12)|0x1C0;nd.links=1;wrinode(ni,&nd);
                if(!dir.blocks[0]){int blk=ablock();dir.blocks[0]=blk;uint8_t z[TXFS_BLOCK_SIZE]={0};wr(blk,z);}
                rd(dir.blocks[0],buf);
                uint32_t sl2=dir.size/sizeof(dirent_t);
                dirent_t* de=(dirent_t*)(buf+sl2*sizeof(dirent_t));
                de->inode=ni;de->name_len=strlen(c);de->type=TXFS_TYPE_DIR;strncpy(de->name,c,255);
                wr(dir.blocks[0],buf);dir.size+=sizeof(dirent_t);wrinode(dir_ino,&dir);
                found=ni;
            }
            dir_ino=found;
            c=nx?nx+1:NULL;
        }
    }

    // remove existing file with same name
    inode_t dir;rdinode(dir_ino,&dir);
    uint8_t buf[TXFS_BLOCK_SIZE];
    if(dir.blocks[0]){
        rd(dir.blocks[0],buf);
        for(uint32_t o=0;o+sizeof(dirent_t)<=TXFS_BLOCK_SIZE;o+=sizeof(dirent_t)){
            dirent_t* de=(dirent_t*)(buf+o);
            if(de->inode&&strcmp(de->name,fname)==0){
                rd(TXFS_BLOCK_IBITMAP,imap);imap[de->inode/8]&=~(1<<(de->inode%8));
                wr(TXFS_BLOCK_IBITMAP,imap);sb.free_inodes++;
                de->inode=0;wr(dir.blocks[0],buf);break;
            }
        }
    }

    // allocate inode and write data
    int ni=ainode();
    inode_t ino={0};ino.mode=(TXFS_TYPE_FILE<<12)|0x1C0;ino.links=1;ino.size=fsz;
    uint32_t done=0;int bi=0;
    while(done<(uint32_t)fsz&&bi<TXFS_DIRECT_BLOCKS){
        int blk=ablock();ino.blocks[bi++]=blk;
        uint8_t blkbuf[TXFS_BLOCK_SIZE]={0};
        uint32_t chunk=fsz-done;if(chunk>TXFS_BLOCK_SIZE)chunk=TXFS_BLOCK_SIZE;
        memcpy(blkbuf,data+done,chunk);wr(blk,blkbuf);done+=chunk;
    }
    wrinode(ni,&ino);

    // add dirent
    rdinode(dir_ino,&dir);
    if(!dir.blocks[0]){int blk=ablock();dir.blocks[0]=blk;uint8_t z[TXFS_BLOCK_SIZE]={0};wr(blk,z);}
    rd(dir.blocks[0],buf);
    uint32_t slot=dir.size/sizeof(dirent_t);
    dirent_t* de=(dirent_t*)(buf+slot*sizeof(dirent_t));
    de->inode=ni;de->name_len=strlen(fname);de->type=TXFS_TYPE_FILE;strncpy(de->name,fname,255);
    wr(dir.blocks[0],buf);dir.size+=sizeof(dirent_t);wrinode(dir_ino,&dir);

    free(data);
    printf("  %s -> %s (%ld bytes)\n",lpath,txpath,fsz);
    return 0;
}

int main(int argc,char** argv){
    if(argc<2){fprintf(stderr,"Usage: %s <disk.img> [<local> <txfspath>]...\n",argv[0]);return 1;}
    disk=fopen(argv[1],"r+b");
    if(!disk){
        disk=fopen(argv[1],"w+b");
        if(!disk){perror("open");return 1;}
        uint8_t z[TXFS_BLOCK_SIZE]={0};
        for(int i=0;i<25600;i++)fwrite(z,TXFS_BLOCK_SIZE,1,disk);
    }
    rdsb();
    if(sb.magic!=TXFS_MAGIC){
        fseek(disk,0,SEEK_END);long dsz=ftell(disk);
        format(dsz/TXFS_BLOCK_SIZE);rdsb();
    }
    for(int i=2;i+1<argc;i+=2)
        write_file(argv[i+1],argv[i]);
    fclose(disk);return 0;
}
