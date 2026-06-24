#pragma once
#include <stdint.h>

int env_get(const char* name, char* buf, uint32_t maxl);
int env_set(const char* name, const char* val);
int env_list(int i, char* key_out, char* val_out);
