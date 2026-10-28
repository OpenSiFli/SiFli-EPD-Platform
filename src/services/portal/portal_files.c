#include "portal_files.h"
#include "portal_protocol.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dfs_posix.h>

static int removal_error(int error)
{
    if (error < 0) error = -error;
    if (error == ENOENT) return 404;
    if (error == EACCES || error == EPERM || error == EROFS) return 403;
    if (error == ENOTEMPTY || error == EEXIST || error == EBUSY) return 409;
    return 500;
}

int portal_file_remove(const char *relative)
{
    char path[PORTAL_PATH_MAX];
    if (!portal_path(relative,path,sizeof(path)) || !strcmp(relative,"/")) return 400;
    if (!portal_path_mutable(relative)) return 403;
    struct stat state;
    if (stat(path,&state) != 0) return removal_error(errno);
    int result;
    if (S_ISREG(state.st_mode)) result = unlink(path);
    else if (S_ISDIR(state.st_mode))
    {
        /* FatFs reports a nonempty directory as EROFS, so inspect its entries
         * before removing it to distinguish contents from write protection. */
        DIR *directory = opendir(path);
        if (!directory) return removal_error(errno);
        int status = 200;
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(directory)) != NULL)
            if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) { status = 409; break; }
        if (status == 200 && errno) status = removal_error(errno);
        if (closedir(directory) != 0 && status == 200) status = removal_error(errno);
        if (status != 200) return status;
        result = rmdir(path);
    }
    else return 403;
    return result == 0 ? 200 : removal_error(errno);
}

int portal_upload_publish(const char *temporary,const char *target,const char *backup,bool replace)
{
    struct stat state;
    bool saved=false;
    int exists=stat(target,&state);
    if (exists!=0 && errno!=ENOENT && errno!=-ENOENT) return 500;
    if (exists==0)
    {
        if (!replace || !S_ISREG(state.st_mode)) return 409;
        if (stat(backup,&state)==0) return 409;
        if (errno!=ENOENT && errno!=-ENOENT) return 500;
        if (rename(target,backup)!=0) return 500;
        saved=true;
    }
    if (rename(temporary,target)!=0)
    {
        if (saved) (void)rename(backup,target);
        return 500;
    }
    if (saved) (void)unlink(backup);
    return 200;
}
