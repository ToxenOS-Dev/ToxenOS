#pragma once
int dhcp_run(void);  // Run DHCP client. Returns 1 if IP assigned, 0 if fallback to static.
