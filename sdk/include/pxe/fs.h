#ifndef _PXE_FS_H
#define _PXE_FS_H

#include "abi.h"

int pxe_open(const char* path);
int pxe_read(int fd, void* buf, size_t count);
int pxe_seek(int fd, uint64_t offset);
int pxe_close(int fd);

int pxe_stat(const char* path, pxe_dirent_t* out);
int pxe_read_file(const char* path, void* buf, size_t max_size);
int pxe_write_file(const char* path, const void* buf, size_t size);
int pxe_copy_file(const char* src_path, const char* dst_path,
                  void (*progress_cb)(uint64_t copied, uint64_t total));

int pxe_listdir(const char* path, pxe_dirent_t* out_entries, int max_entries);
int pxe_mkdir(const char* path);
int pxe_unlink(const char* path);
int pxe_rename(const char* old_path, const char* new_path);

int pxe_get_drives(pxe_drive_info_t* out_drives, int max_drives);
int pxe_eject_drive(char drive_letter);

#endif /* _PXE_FS_H */

