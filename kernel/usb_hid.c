// ToxenOS/kernel/usb_hid.c
// Minimal xHCI + USB HID Boot Protocol keyboard driver.
// Supports one keyboard, polling-based (called from timer ISR).
#include <stdint.h>
#include "../include/pci.h"
#include "../include/klog.h"
#include "../include/paging.h"
#include "../include/memmap.h"
#include "../include/keyboard.h"
#include "../include/timer.h"

// ── xHCI Capability register offsets (BAR0) ──────────────────────────────
#define CAPLENGTH   0x00  // byte: operational register base = BAR0 + this
#define HCSPARAMS1  0x04  // max_slots[7:0], max_ports[31:24]
#define HCSPARAMS2  0x08  // scratchpad bufs hi[31:27], lo[25:21]; ERST max[7:4]
#define HCCPARAMS1  0x10  // XECP (extended cap ptr) at [15:8]
#define DBOFF       0x14  // doorbell array offset from BAR0
#define RTSOFF      0x18  // runtime register space offset from BAR0

// ── xHCI Operational register offsets (op_base = BAR0 + CAPLENGTH) ───────
#define USBCMD      0x00
#define USBSTS      0x04
#define OP_PAGESIZE 0x08  // supported page sizes (bit 0 = 4KB)
#define OP_DNCTRL   0x14  // device notification control
#define CRCR_LO     0x18
#define CRCR_HI     0x1C
#define DCBAAP      0x30
#define DCBAAP_H    0x34
#define CONFIG      0x38

#define CMD_RUN   (1u<<0)
#define CMD_HCRST (1u<<1)
#define STS_HCH   (1u<<0)
#define STS_CNR   (1u<<11)

// PORTSC bits (op_base + 0x400 + port*0x10)
#define PORTSC_CCS  (1u<<0)
#define PORTSC_PED  (1u<<1)
#define PORTSC_PR   (1u<<4)
#define PORTSC_PP   (1u<<9)
#define PORTSC_PRC  (1u<<21)  // Port Reset Change (RW1C)
#define PORTSC_WRC  (1u<<19)  // Warm Reset Change (RW1C, USB3)
#define PORTSC_WPR  (1u<<31)  // Warm Port Reset (USB3)

// TRB types
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12

// Event TRB types
#define EV_TRANSFER  32
#define EV_CMD_COMPL 33
#define EV_PORT_SC   34

// TRB control field helpers
#define TRB_C       (1u<<0)
#define TRB_TC      (1u<<1)
#define TRB_IDT     (1u<<6)
#define TRB_IOC     (1u<<5)
#define TRB_DIR_IN  (1u<<16)
#define TRB_TYPE(t) ((uint32_t)(t)<<10)
#define TRB_SLOT(s) ((uint32_t)(s)<<24)

// Completion codes
#define CC_SUCCESS 1
#define CC_STOPPED 26

// USB descriptor types
#define DESC_IFACE 4
#define DESC_EP    5

// USB HID class/subclass/protocol
#define USB_CLASS_HID      0x03
#define USB_SUBCLASS_BOOT  0x01
#define USB_PROTO_KBD      0x01


// ── Structures ────────────────────────────────────────────────────────────

typedef struct __attribute__((packed)) {
    uint32_t p0, p1, status, ctrl;
} xhci_trb_t;

typedef struct __attribute__((packed)) {
    uint32_t ep_info1;  // Interval[23:16], Mult[9:8], EPState[2:0]
    uint32_t ep_info2;  // MaxPS[31:16], MaxBurst[15:8], HID[7], EPType[5:3], CErr[2:1]
    uint32_t deq_lo;    // TR Dequeue Ptr | DCS[0]
    uint32_t deq_hi;
    uint32_t tx_info;   // AvgTRBLen[15:0]
    uint32_t rsvd[3];
} xhci_ep_ctx_t;

typedef struct __attribute__((packed)) {
    uint32_t info1;  // CtxEnt[31:27], Hub[26], MTT[25], Speed[23:20], Route[19:0]
    uint32_t info2;  // HubPorts[31:24], RHPortNum[23:16], MaxExitLat[15:0]
    uint32_t tt;
    uint32_t state;
    uint32_t rsvd[4];
} xhci_slot_ctx_t;

typedef struct __attribute__((packed,aligned(64))) {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t   ep[31];  // ep[n] = DCI n+1
} xhci_dev_ctx_t;

typedef struct __attribute__((packed,aligned(64))) {
    uint32_t drop, add;
    uint32_t rsvd[6];
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t   ep[31];
} xhci_input_ctx_t;

typedef struct __attribute__((packed,aligned(64))) {
    uint64_t base;
    uint16_t size;
    uint16_t rsvd[3];
} xhci_erst_t;

// ── Static buffers ────────────────────────────────────────────────────────
#define RING_SZ     16
#define MAX_SCRATCH 32   // upper bound for scratchpad buffer array entries

static uint64_t         dcbaa[256]                      __attribute__((aligned(64)));
// Scratchpad buffer array + backing pages (required when HCSPARAMS2 says so).
// dcbaa[0] points to scratch_arr; scratch_arr[i] points to scratch_pages[i].
static uint64_t         scratch_arr[MAX_SCRATCH]        __attribute__((aligned(64)));
static uint8_t          scratch_pages[MAX_SCRATCH][4096]__attribute__((aligned(4096)));

static xhci_dev_ctx_t   dev_ctx                         __attribute__((aligned(64)));
static xhci_input_ctx_t inp_ctx                         __attribute__((aligned(64)));
static xhci_trb_t       cmd_ring[RING_SZ]               __attribute__((aligned(64)));
static xhci_trb_t       evt_ring[RING_SZ]               __attribute__((aligned(64)));
static xhci_erst_t      erst[1]                         __attribute__((aligned(64)));
static xhci_trb_t       ctrl_ring[RING_SZ]              __attribute__((aligned(64)));
static xhci_trb_t       hid_ring[RING_SZ]               __attribute__((aligned(64)));
static uint8_t          ubuf[512]                       __attribute__((aligned(64)));

// Ring cycle/enqueue state
static uint8_t  cmd_c=1; static uint16_t cmd_eq=0;
static uint8_t  evt_c=1; static uint16_t evt_dq=0;
static uint8_t  ctl_c=1; static uint16_t ctl_eq=0;
static uint8_t  hid_c=1; static uint16_t hid_eq=0;

// Controller base addresses
static int      xhci_ok=0;
static uint32_t cap_b=0, op_b=0, rt_b=0, db_b=0;

// Keyboard device state
static uint8_t  kbd_slot=0;
static uint8_t  kbd_ep_dci=0;
static uint8_t  prev_keys[6]={0};

// ── Register macros ───────────────────────────────────────────────────────
#define C8(o)     (*((volatile uint8_t*) (cap_b+(o))))
#define C32(o)    (*((volatile uint32_t*)(cap_b+(o))))
#define O32(o)    (*((volatile uint32_t*)(op_b +(o))))
#define R32(o)    (*((volatile uint32_t*)(rt_b +(o))))
#define DB(s)     (*((volatile uint32_t*)(db_b +(s)*4)))
#define PORT(p)   (*((volatile uint32_t*)(op_b +0x400+(p)*0x10)))
#define INTR(n,o) (*((volatile uint32_t*)(rt_b+0x20+(n)*0x20+(o))))

static uint32_t v2p(void *v){ return (uint32_t)(uintptr_t)v - KERNEL_VIRT_BASE; }

static int wait0(volatile uint32_t *r,uint32_t m){
    for(int i=0;i<2000000;i++) if(!(*r&m)) return 0; return -1; }
static int wait1(volatile uint32_t *r,uint32_t m){
    for(int i=0;i<2000000;i++) if( (*r&m)) return 0; return -1; }

// Wait at least `ms` milliseconds using the 100Hz PIT timer (10ms/tick).
// Safety: if timer ticks don't advance (shouldn't happen), falls out after
// a bounded spin so we don't hang the boot.
static void wait_ms(uint32_t ms){
    uint32_t ticks = (ms + 9) / 10;
    if(!ticks) ticks = 1;
    uint32_t start = timer_getticks();
    for(uint32_t spin = 0; spin < 50000000u; spin++)
        if(timer_getticks() - start >= ticks) break;
}



// ── Command ring ──────────────────────────────────────────────────────────
static void cmd_init(void){
    for(int i=0;i<RING_SZ;i++) cmd_ring[i].p0=cmd_ring[i].p1=cmd_ring[i].status=cmd_ring[i].ctrl=0;
    cmd_ring[RING_SZ-1].p0   = v2p(cmd_ring);
    cmd_ring[RING_SZ-1].ctrl = TRB_TYPE(TRB_LINK)|TRB_TC|TRB_C;
    cmd_eq=0; cmd_c=1;
}
static void cmd_submit(uint32_t p0,uint32_t p1,uint32_t st,uint32_t ct){
    uint16_t i=cmd_eq;
    cmd_ring[i].p0=p0; cmd_ring[i].p1=p1; cmd_ring[i].status=st;
    cmd_ring[i].ctrl=ct|(cmd_c?TRB_C:0);
    cmd_eq=(cmd_eq+1)%(RING_SZ-1);
    if(cmd_eq==0){ cmd_ring[RING_SZ-1].ctrl^=TRB_C; cmd_c^=1; }
    DB(0)=0;
}

// ── Event ring ────────────────────────────────────────────────────────────
// Returns 0 and fills out params if an event is waiting, else -1.
static int evt_try(uint32_t *op0,uint32_t *op1,uint32_t *ost,uint32_t *oct){
    xhci_trb_t *e=&evt_ring[evt_dq];
    if((e->ctrl&1)!=(uint32_t)evt_c) return -1;
    *op0=e->p0; *op1=e->p1; *ost=e->status; *oct=e->ctrl;
    evt_dq=(evt_dq+1)%RING_SZ;
    if(evt_dq==0) evt_c^=1;
    INTR(0,0x18)=v2p(&evt_ring[evt_dq])|(1<<3);  // ERDP: advance + clear EHB
    INTR(0,0x1C)=0;
    return 0;
}
// Blocking wait — used during init only
static int evt_wait(uint32_t *op0,uint32_t *op1,uint32_t *ost,uint32_t *oct){
    for(uint32_t t=0;t<8000000;t++) if(evt_try(op0,op1,ost,oct)==0) return 0;
    return -1;
}
// Wait for a command completion, skipping port-status events
static int cmd_wait(uint8_t *slot_out){
    uint32_t p0,p1,st,ct;
    for(int tries=0;tries<64;tries++){
        if(evt_wait(&p0,&p1,&st,&ct)<0) return -1;
        uint32_t type=(ct>>10)&0x3F;
        if(type==EV_CMD_COMPL){
            if(slot_out) *slot_out=(uint8_t)(ct>>24);
            return ((st>>24)==CC_SUCCESS)?0:-1;
        }
    }
    return -1;
}

// ── Control transfer (EP0) ────────────────────────────────────────────────
static void ctrl_ring_init(void){
    for(int i=0;i<RING_SZ;i++) ctrl_ring[i].p0=ctrl_ring[i].p1=ctrl_ring[i].status=ctrl_ring[i].ctrl=0;
    ctrl_ring[RING_SZ-1].p0   = v2p(ctrl_ring);
    ctrl_ring[RING_SZ-1].ctrl = TRB_TYPE(TRB_LINK)|TRB_TC|TRB_C;
    ctl_eq=0; ctl_c=1;
}
static int ctrl_xfer(uint8_t bmt,uint8_t bReq,uint16_t wVal,uint16_t wIdx,
                     uint8_t *buf,uint16_t len){
    int dir_in=(bmt&0x80)!=0;

    // Setup Stage
    uint16_t si=ctl_eq;
    ctrl_ring[si].p0=(uint32_t)bmt|((uint32_t)bReq<<8)|((uint32_t)wVal<<16);
    ctrl_ring[si].p1=(uint32_t)wIdx|((uint32_t)len<<16);
    ctrl_ring[si].status=8;
    uint32_t trt=len?(dir_in?(3u<<16):(2u<<16)):0;
    ctrl_ring[si].ctrl=TRB_TYPE(TRB_SETUP)|TRB_IDT|trt|(ctl_c?TRB_C:0);
    ctl_eq=(ctl_eq+1)%(RING_SZ-1);

    // Data Stage (if any)
    if(len&&buf){
        uint16_t di=ctl_eq;
        ctrl_ring[di].p0=v2p(buf); ctrl_ring[di].p1=0;
        ctrl_ring[di].status=len;
        ctrl_ring[di].ctrl=TRB_TYPE(TRB_DATA)|(dir_in?TRB_DIR_IN:0)|(ctl_c?TRB_C:0);
        ctl_eq=(ctl_eq+1)%(RING_SZ-1);
    }

    // Status Stage
    uint16_t xi=ctl_eq;
    uint32_t sdir=(len&&!dir_in)?TRB_DIR_IN:0;
    ctrl_ring[xi].p0=0; ctrl_ring[xi].p1=0; ctrl_ring[xi].status=0;
    ctrl_ring[xi].ctrl=TRB_TYPE(TRB_STATUS)|sdir|TRB_IOC|(ctl_c?TRB_C:0);
    ctl_eq=(ctl_eq+1)%(RING_SZ-1);
    if(ctl_eq==0){ ctrl_ring[RING_SZ-1].ctrl^=TRB_C; ctl_c^=1; }

    DB(kbd_slot)=1;  // ring EP0 doorbell

    uint32_t p0,p1,st,ct;
    for(int tries=0;tries<32;tries++){
        if(evt_wait(&p0,&p1,&st,&ct)<0) return -1;
        uint32_t type=(ct>>10)&0x3F;
        if(type==EV_TRANSFER){
            uint8_t cc=(uint8_t)(st>>24);
            return (cc==CC_SUCCESS||cc==CC_STOPPED)?0:-1;
        }
    }
    return -1;
}

// ── HID interrupt ring ────────────────────────────────────────────────────
static void hid_ring_init(void){
    for(int i=0;i<RING_SZ;i++) hid_ring[i].p0=hid_ring[i].p1=hid_ring[i].status=hid_ring[i].ctrl=0;
    hid_ring[RING_SZ-1].p0   = v2p(hid_ring);
    hid_ring[RING_SZ-1].ctrl = TRB_TYPE(TRB_LINK)|TRB_TC|TRB_C;
    hid_eq=0; hid_c=1;
}
static void hid_queue(void){
    uint16_t i=hid_eq;
    hid_ring[i].p0=v2p(ubuf); hid_ring[i].p1=0;
    hid_ring[i].status=8;
    hid_ring[i].ctrl=TRB_TYPE(TRB_NORMAL)|TRB_IOC|(hid_c?TRB_C:0);
    hid_eq=(hid_eq+1)%(RING_SZ-1);
    if(hid_eq==0){
        // Set link TRB cycle = current PCS so hardware can follow it.
        // (XOR would be wrong — hardware sees the link while cycle == hid_c,
        // so the link must carry that same bit.  We toggle hid_c AFTER.)
        hid_ring[RING_SZ-1].ctrl &= ~TRB_C;
        hid_ring[RING_SZ-1].ctrl |= (hid_c ? TRB_C : 0);
        hid_c ^= 1;
    }
    DB(kbd_slot)=kbd_ep_dci;
}

// ── HID Boot Protocol keycode tables ─────────────────────────────────────
static const char hid_ascii[128]={
    0,0,0,0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',    // 0x04-0x10
    'n','o','p','q','r','s','t','u','v','w','x','y','z',    // 0x11-0x1D
    '1','2','3','4','5','6','7','8','9','0',                 // 0x1E-0x27
    '\n',27,8,'\t',' ','-','=','[',']','\\',0,';','\'','`', // 0x28-0x35
    ',','.','/',                                             // 0x36-0x38
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,         // 0x39-0x4E
    0x04,0x03,0x02,0x01,                                     // 0x4F-0x52: R/L/D/U arrows
};
static const char hid_shift[128]={
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n',27,8,'\t',' ','_','+','{','}','|',0,':','"','~',
    '<','>','?',
};

// ── Polling — called from timer ISR every tick (non-blocking) ─────────────
void usb_hid_poll(void){
    if(!xhci_ok) return;
    uint32_t p0,p1,st,ct;
    // Drain ALL pending events — a PORT_SC or CMD_COMPL event before
    // EV_TRANSFER must not prevent us from re-queuing the receive TRB.
    while(evt_try(&p0,&p1,&st,&ct)==0){
        uint32_t type=(ct>>10)&0x3F;
        if(type!=EV_TRANSFER) continue;
        uint8_t cc=(uint8_t)(st>>24);
        if(cc==CC_SUCCESS){
            uint8_t mod=ubuf[0];
            int shift=(mod&0x22)!=0;
            int ctrl =(mod&0x11)!=0;
            for(int k=0;k<6;k++){
                uint8_t kc=ubuf[2+k]; if(!kc) continue;
                int dup=0; for(int j=0;j<6;j++) if(prev_keys[j]==kc){dup=1;break;}
                if(dup) continue;
                if(kc<128){
                    char c=shift?hid_shift[kc]:hid_ascii[kc];
                    if(ctrl&&c>='a'&&c<='z') c=(char)(c-'a'+1);
                    else if(ctrl&&c>='A'&&c<='Z') c=(char)(c-'A'+1);
                    if(c) keyboard_inject(c);
                }
            }
            for(int k=0;k<6;k++) prev_keys[k]=ubuf[2+k];
        }
        hid_queue();
    }
}

// ── Controller setup ──────────────────────────────────────────────────────
static int setup_controller(pci_device_t *dev){
    uint32_t bar0=dev->bar[0];
    int bar_type=(bar0>>1)&3;
    uint32_t base=bar0&~0xFu;
    if(bar_type==2&&dev->bar[1]){ klog("xHCI: BAR>4GB\n"); return -1; }
    if(!base){ klog("xHCI: no BAR0\n"); return -1; }
    pci_enable(dev);

    // Map xHCI MMIO as cache-disable (PAGE_CD = PTE bit 4).
    // MTRRs normally mark PCIe BAR regions UC on real HW, but we set PCD
    // explicitly so our reads/writes bypass the CPU cache unconditionally.
    extern uint32_t kernel_directory[];
    for(uint32_t off=0;off<0x10000;off+=0x1000)
        paging_map(kernel_directory,base+off,base+off,
                   PAGE_PRESENT|PAGE_WRITABLE|PAGE_CD);

    cap_b=base;
    op_b=cap_b+(uint32_t)C8(CAPLENGTH);
    rt_b=cap_b+C32(RTSOFF);
    db_b=cap_b+C32(DBOFF);

    // ── BIOS handoff via EECP/LEGSUP ──────────────────────────────────────
    // Walk the extended capability list looking for cap-ID 1 (USB Legacy Support).
    // Set OS-owns bit [24] and wait up to ~500ms for BIOS-owns [16] to clear.
    // XECP is bits[31:16] of HCCPARAMS1, a DWORD offset from BAR0.
    uint32_t hccp=C32(HCCPARAMS1);
    uint32_t xecp=((hccp>>16)&0xFFFF)*4;
    klog_hex("xHCI HCCPARAMS1=", hccp);
    klog_hex("xHCI XECP=",       xecp);
    if(xecp){
        uint32_t ptr=cap_b+xecp;
        for(int i=0;i<16;i++){
            volatile uint32_t *r=(volatile uint32_t*)ptr;
            if((*r&0xFF)==1){
                *r|=(1u<<24);  // request OS ownership
                // Wait up to ~500ms for BIOS to release (5M MMIO reads ~= 500ms)
                for(int t=0;t<5000000;t++) if(!(*r&(1u<<16))) break;
                break;
            }
            uint8_t next=(uint8_t)((*r>>8)&0xFF);
            if(!next) break;
            ptr+=(uint32_t)next*4;
        }
    }
    // Brief settle after handoff before touching USBCMD (~20ms)
    wait_ms(20);

    // ── Stop ──────────────────────────────────────────────────────────────
    O32(USBCMD)&=~CMD_RUN;
    if(wait1((volatile uint32_t*)(op_b+USBSTS),STS_HCH)<0){
        klog("xHCI: stop timeout\n"); return -1; }

    // ── Reset ─────────────────────────────────────────────────────────────
    O32(USBCMD)|=CMD_HCRST;
    if(wait0((volatile uint32_t*)(op_b+USBCMD),CMD_HCRST)<0){
        klog("xHCI: reset timeout\n"); return -1; }
    if(wait0((volatile uint32_t*)(op_b+USBSTS),STS_CNR)<0){
        klog("xHCI: CNR timeout\n"); return -1; }

    // ── Verify 4KB page support ───────────────────────────────────────────
    if(!(O32(OP_PAGESIZE)&1)){ klog("xHCI: no 4KB page\n"); return -1; }

    // ── Disable device notifications ──────────────────────────────────────
    O32(OP_DNCTRL)=0;

    // ── Log controller parameters for debugging ───────────────────────────
    klog_hex("xHCI HCSPARAMS1=", C32(HCSPARAMS1));
    klog_hex("xHCI HCSPARAMS2=", C32(HCSPARAMS2));
    klog_hex("xHCI BAR0=",       cap_b);

    // ── CONFIG: max device slots ──────────────────────────────────────────
    uint32_t max_slots=C32(HCSPARAMS1)&0xFF;
    if(!max_slots||max_slots>16) max_slots=16;
    O32(CONFIG)=max_slots;

    // ── DCBAA + scratchpad buffers ────────────────────────────────────────
    // Per xHCI spec §4.20: if HCSPARAMS2 indicates non-zero Max Scratchpad
    // Buffers, dcbaa[0] must point to a scratchpad buffer array before the
    // first command ring execution.  AMD controllers commonly require this.
    for(int i=0;i<256;i++) dcbaa[i]=0;

    uint32_t hcs2=C32(HCSPARAMS2);
    // Linux kernel formula (drivers/usb/host/xhci.h HCS_MAX_SCRATCHPAD):
    // bits[31:27] are the LOW 5 bits of the count (×1),
    // bits[25:21] are the HIGH 5 bits of the count (×32).
    // ((p>>27)&0x1f) | ((p>>16)&0x3e0)  where 0x3e0 extracts bits[9:5] from (p>>16)
    uint32_t max_scratch=((hcs2>>27)&0x1Fu)|((hcs2>>16)&0x3E0u);
    if(max_scratch>MAX_SCRATCH) max_scratch=MAX_SCRATCH;
    if(max_scratch>0){
        for(uint32_t s=0;s<max_scratch;s++){
            for(int j=0;j<4096;j++) scratch_pages[s][j]=0;
            scratch_arr[s]=(uint64_t)v2p(scratch_pages[s]);
        }
        dcbaa[0]=(uint64_t)v2p(scratch_arr);
    }

    O32(DCBAAP)=v2p(dcbaa); O32(DCBAAP_H)=0;

    // ── Command ring ──────────────────────────────────────────────────────
    cmd_init();
    O32(CRCR_LO)=v2p(cmd_ring)|1;  // RCS=1
    O32(CRCR_HI)=0;

    // ── Event ring ────────────────────────────────────────────────────────
    for(int i=0;i<RING_SZ;i++)
        evt_ring[i].p0=evt_ring[i].p1=evt_ring[i].status=evt_ring[i].ctrl=0;
    erst[0].base=v2p(evt_ring); erst[0].size=RING_SZ;
    INTR(0,0x08)=1;                                // ERSTSZ = 1 segment
    INTR(0,0x10)=v2p(erst); INTR(0,0x14)=0;       // ERSTBA
    INTR(0,0x18)=v2p(evt_ring); INTR(0,0x1C)=0;   // ERDP
    evt_dq=0; evt_c=1;

    // ── Start ─────────────────────────────────────────────────────────────
    O32(USBCMD)=CMD_RUN;
    if(wait0((volatile uint32_t*)(op_b+USBSTS),STS_HCH)<0){
        klog("xHCI: start timeout\n"); return -1; }

    // Wait for root-hub ports to settle and USB3 links to train (~200ms).
    // USB3 SS link training from reset takes up to 170ms per spec.
    wait_ms(200);
    return 0;
}

// ── Device enumeration ────────────────────────────────────────────────────
static int find_keyboard(void){
    uint32_t num_ports=(C32(HCSPARAMS1)>>24)&0xFF;
    klog_hex("xHCI num_ports=",num_ports);

    // Retry for up to 2 seconds.  USB3 link re-training after controller
    // reset can take 170ms+; a single pass after 200ms sometimes misses it.
    uint32_t deadline=timer_getticks()+200;  // 200 ticks = 2000ms
    while(1){
    for(uint32_t p=0;p<num_ports;p++){
        uint32_t psc=PORT(p);
        if(!(psc&PORTSC_CCS)) continue;  // no device on this port
        klog_hex("xHCI port CCS=1 PORTSC=",psc);

        // ── Port reset ────────────────────────────────────────────────────
        // USB3 SuperSpeed ports may already have PED=1 (link trained during
        // detect).  USB2 ports need an explicit USB2 Reset (PR).  If PR doesn't
        // enable the port, fall back to a USB3 Warm Reset (WPR).
        if(!(PORT(p)&PORTSC_PED)){
            // Attempt USB2-style port reset
            PORT(p)=PORTSC_PP|PORTSC_PR;
            // Wait for PR to clear (≤50ms per spec; MMIO reads add ~100ns each)
            for(int t=0;t<2000000;t++) if(!(PORT(p)&PORTSC_PR)) break;
            // Clear Port Reset Change (write 1 to RW1C bit)
            PORT(p)=PORTSC_PP|PORTSC_PRC;
            wait_ms(20);  // TRSTRCY: USB 2.0 spec ≥10ms after reset before transfers

            if(!(PORT(p)&PORTSC_PED)){
                // USB2 reset didn't enable port — try USB3 Warm Port Reset
                PORT(p)=PORTSC_PP|PORTSC_WPR;
                for(int t=0;t<2000000;t++) if(PORT(p)&PORTSC_WRC) break;
                PORT(p)=PORTSC_PP|PORTSC_WRC;
                wait_ms(20);
            }
        }
        if(!(PORT(p)&PORTSC_PED)) continue;  // port still not enabled

        uint8_t speed=(uint8_t)((PORT(p)>>10)&0xF);
        if(!speed) speed=3;  // default to HighSpeed if unknown
        kbd_slot=0;

        // ── Enable Slot ───────────────────────────────────────────────────
        cmd_submit(0,0,0,TRB_TYPE(TRB_ENABLE_SLOT));
        uint8_t slot=0;
        if(cmd_wait(&slot)<0||!slot){
            klog("xHCI: Enable Slot fail\n"); continue; }

        // ── Address Device (slot + EP0) ────────────────────────────────────
        uint8_t *ictx=(uint8_t*)&inp_ctx;
        for(int i=0;i<(int)sizeof(inp_ctx);i++) ictx[i]=0;
        inp_ctx.add=(1u<<0)|(1u<<1);  // A0=slot, A1=EP0

        inp_ctx.slot.info1=((uint32_t)speed<<20)|(1u<<27);  // ctx_entries=1
        inp_ctx.slot.info2=(p+1)<<16;                        // root hub port number

        // EP0 max packet size: 512 for SS, 64 for HS, 8 for FS/LS
        uint16_t mps=(speed>=4)?512:(speed==3)?64:8;
        inp_ctx.ep[0].ep_info2=(3u<<1)|(4u<<3)|((uint32_t)mps<<16); // CErr=3, Control EP
        ctrl_ring_init();
        inp_ctx.ep[0].deq_lo=v2p(ctrl_ring)|1;  // DCS=1
        inp_ctx.ep[0].tx_info=8;

        dcbaa[slot]=v2p(&dev_ctx);
        kbd_slot=slot;

        cmd_submit(v2p(&inp_ctx),0,0,TRB_TYPE(TRB_ADDRESS_DEV)|TRB_SLOT(slot));
        if(cmd_wait(0)<0){ klog("xHCI: Address Device fail\n"); continue; }

        // SET_ADDRESS recovery time: USB 2.0 spec requires ≥2ms
        wait_ms(10);

        // ── GET_DESCRIPTOR Device (18 bytes) ──────────────────────────────
        for(int i=0;i<512;i++) ubuf[i]=0;
        if(ctrl_xfer(0x80,6,0x0100,0,ubuf,18)<0) continue;

        // ── GET_DESCRIPTOR Configuration (full, up to 255 bytes) ──────────
        if(ctrl_xfer(0x80,6,0x0200,0,ubuf,255)<0) continue;

        // ── Parse descriptors: find HID keyboard interface + interrupt IN EP
        uint16_t total=(uint16_t)(ubuf[2]|((uint16_t)ubuf[3]<<8));
        if(total>255) total=255;
        uint8_t config_val=ubuf[5];
        uint8_t if_num=0, ep_addr=0;
        uint16_t ep_mps=8;
        uint8_t  ep_intv=3;

        uint16_t off=0;
        while(off<total){
            uint8_t dlen=ubuf[off]; if(!dlen) break;
            uint8_t dtype=ubuf[off+1];
            if(dtype==DESC_IFACE&&dlen>=9){
                if(ubuf[off+5]==USB_CLASS_HID&&ubuf[off+6]==USB_SUBCLASS_BOOT
                   &&ubuf[off+7]==USB_PROTO_KBD)
                    if_num=ubuf[off+2];
            }
            if(dtype==DESC_EP&&dlen>=7&&ep_addr==0){
                uint8_t ea=ubuf[off+2];
                if((ea&0x80)&&(ubuf[off+3]&3)==3){ // IN + Interrupt
                    ep_addr=ea;
                    ep_mps=(uint16_t)(ubuf[off+4]|((uint16_t)ubuf[off+5]<<8));
                    ep_intv=ubuf[off+6];
                }
            }
            off+=dlen;
        }
        if(!ep_addr){ klog("xHCI: no HID EP found\n"); continue; }

        // DCI = endpoint number * 2 + direction (1=IN, 0=OUT)
        uint8_t dci=(uint8_t)((ep_addr&0x0F)*2+((ep_addr>>7)&1));
        kbd_ep_dci=dci;

        // ── SET_CONFIGURATION ─────────────────────────────────────────────
        ctrl_xfer(0x00,9,config_val,0,0,0);

        // ── SET_PROTOCOL = 0 (Boot Protocol) ─────────────────────────────
        ctrl_xfer(0x21,0x0B,0,if_num,0,0);

        // ── SET_IDLE = 0 (report only on change) ──────────────────────────
        ctrl_xfer(0x21,0x0A,0,if_num,0,0);

        // ── Configure Endpoint (add HID interrupt IN) ─────────────────────
        for(int i=0;i<(int)sizeof(inp_ctx);i++) ictx[i]=0;
        inp_ctx.add=(1u<<0)|(1u<<dci);
        inp_ctx.slot.info1=((uint32_t)speed<<20)|((uint32_t)dci<<27); // ctx_entries=dci
        inp_ctx.slot.info2=(p+1)<<16;
        hid_ring_init();
        inp_ctx.ep[dci-1].ep_info1=(uint32_t)ep_intv<<16;
        inp_ctx.ep[dci-1].ep_info2=(3u<<1)|(7u<<3)|((uint32_t)ep_mps<<16); // CErr=3, Interrupt IN
        inp_ctx.ep[dci-1].deq_lo=v2p(hid_ring)|1;  // DCS=1
        inp_ctx.ep[dci-1].tx_info=8;

        cmd_submit(v2p(&inp_ctx),0,0,TRB_TYPE(TRB_CONFIG_EP)|TRB_SLOT(slot));
        if(cmd_wait(0)<0){ klog("xHCI: Configure EP fail\n"); continue; }

        klog("xHCI: USB keyboard ready\n");
        hid_queue();  // prime the receive TRB
        xhci_ok=1;
        return 0;
    }
    // End of port scan — check if deadline passed
    if(timer_getticks()>=deadline) break;
    wait_ms(100);  // wait 100ms then rescan all ports
    }  // end retry loop
    return -1;
}

// ── Public init ───────────────────────────────────────────────────────────
// AMD Ryzen laptops commonly have 2–4 separate xHCI controllers (Gen1 × 2,
// Gen2 × 1, USB4 × 1…).  Try every one until we find a keyboard.
int usb_hid_init(void){
    int found_any=0;
    for(int i=0;i<pci_device_count;i++){
        if(pci_devices[i].class_code!=0x0C||pci_devices[i].subclass!=0x03) continue;
        uint8_t pi=(uint8_t)pci_read(pci_devices[i].bus,
                                     pci_devices[i].slot,
                                     pci_devices[i].func,0x09);
        if(pi!=0x30) continue;
        found_any=1;

        // Reset all ring state before attempting this controller
        cmd_c=1; cmd_eq=0;
        evt_c=1; evt_dq=0;
        ctl_c=1; ctl_eq=0;
        hid_c=1; hid_eq=0;
        kbd_slot=0; kbd_ep_dci=0;
        xhci_ok=0;

        klog("xHCI: found controller\n");
        if(setup_controller(&pci_devices[i])<0){
            klog("xHCI: setup failed\n"); continue;
        }
        if(find_keyboard()==0) return 0;  // keyboard found on this controller

        // No keyboard here — stop this controller before trying the next one
        O32(USBCMD)&=~CMD_RUN;
        for(int t=0;t<2000000;t++) if(O32(USBSTS)&STS_HCH) break;
        klog("xHCI: no keyboard on this controller\n");
    }
    if(!found_any) klog("xHCI: no controller\n");
    else           klog("xHCI: no USB keyboard on any controller\n");
    return -1;
}
