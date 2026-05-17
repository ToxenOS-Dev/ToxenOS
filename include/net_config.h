#pragma once
// ── Network configuration ─────────────────────────────────────────────────────
// Change these to match your network. With QEMU user-mode networking the
// defaults below work out of the box.  On real hardware you need values
// matching your LAN, or implement DHCP.
//
// IP4(a,b,c,d) is defined in net.h.

#define NET_IP_A      10
#define NET_IP_B       0
#define NET_IP_C       2
#define NET_IP_D      15

#define NET_GW_A      10
#define NET_GW_B       0
#define NET_GW_C       2
#define NET_GW_D       2

#define NET_MASK_A   255
#define NET_MASK_B   255
#define NET_MASK_C   255
#define NET_MASK_D     0
