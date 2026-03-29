#include "archive_service.h"

int archive_build_from_list(const char *list_path, const char *archive_path)
{
    (void)list_path;
    (void)archive_path;
    /* TODO: call tar to build temp.tar.gz from list file. */
    return -1;
}

int archive_cleanup_temp(const char *path)
{
    (void)path;
    /* TODO: remove temporary artifacts. */
    return 0;
}
