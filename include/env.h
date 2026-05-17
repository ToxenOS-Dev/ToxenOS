#pragma once
#include <stdint.h>

#define MAX_ENV_VARS  8
#define ENV_KEY_MAX   32
#define ENV_VAL_MAX   128

int  env_get(uint32_t pid, const char* name, char* buf, uint32_t maxl);
int  env_set(uint32_t pid, const char* name, const char* val);
