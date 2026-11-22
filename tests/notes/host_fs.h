#include <stddef.h>
#include <stdint.h>
void host_reset(void);
void host_fail_after(long bytes);
void host_new_card(void);
const unsigned char *host_contents(const char *path, size_t *size);
int host_exists(const char *path);

typedef struct {
    unsigned imports,session_imports,session_rejections;
    uint32_t expected_session;
    int expected_volume,import_lock_depth,fail_import,swap_on_import;
} host_io_t;
extern host_io_t host_io;
void host_watch_io(void);
