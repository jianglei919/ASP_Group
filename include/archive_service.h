#ifndef ARCHIVE_SERVICE_H
#define ARCHIVE_SERVICE_H

int archive_build_from_list(const char *list_path, const char *archive_path);
int archive_cleanup_temp(const char *path);

#endif
