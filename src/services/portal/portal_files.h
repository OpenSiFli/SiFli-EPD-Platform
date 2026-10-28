#ifndef EPD_PORTAL_FILES_H
#define EPD_PORTAL_FILES_H
#include <stdbool.h>
/* Caller holds the storage lock and validates the card session. All paths
 * belong to the same directory. backup must be a fresh, private path. */
int portal_upload_publish(const char *temporary, const char *target,
                          const char *backup, bool replace);
/* Caller holds the storage lock and validates the card session. Removes a
 * regular file or an empty directory; relative is rooted at the TF card. */
int portal_file_remove(const char *relative);
#endif
