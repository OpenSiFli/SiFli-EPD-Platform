/* The cache suites retain their filesystem doubles; ownership is production code. */
struct dfs_filesystem_ops { const char *name; };
struct dfs_filesystem { const struct dfs_filesystem_ops *ops; };
static struct dfs_filesystem *dfs_filesystem_lookup(const char *path)
{
    (void)path;
    static const struct dfs_filesystem_ops ops = {"elm"};
    static struct dfs_filesystem fs = {&ops};
    return &fs;
}
static bool system_reset_write_allowed(void) { return true; }
#include "owned_actual.inc"
