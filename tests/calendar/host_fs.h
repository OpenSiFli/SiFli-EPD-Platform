#include <stddef.h>
#include <stdint.h>
void host_reset(void);
void host_fail_after(long bytes);
void host_stat_error(int error);
void host_new_card(void);
const unsigned char *host_contents(const char *path, size_t *size);
int host_exists(const char *path);

int host_storage_depth(void);
