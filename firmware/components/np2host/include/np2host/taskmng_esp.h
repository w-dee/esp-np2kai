#pragma once

#include <compiler.h>

#ifdef __cplusplus
extern "C" {
#endif

void np2_host_taskmng_reset(void);
BOOL np2_host_taskmng_exit_requested(void);

#ifdef __cplusplus
}
#endif
