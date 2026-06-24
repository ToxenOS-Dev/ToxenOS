#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

void     acpi_init(void);
void     acpi_shutdown(void);   // graceful S5 via ACPI; falls back to KBC reset

#endif
