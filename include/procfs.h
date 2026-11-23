#ifndef PROCFS_H
#define PROCFS_H

#include <stdbool.h>
#include <stddef.h>
#include "vfs.h"

bool  procfs_match_path(const char* path, char* rel_out, size_t rel_cap);
int   procfs_list_dir(const char* rel_path, void (*callback)(const vfs_dirent_t*));
int   procfs_find_file(const char* rel_path, vfs_dirent_t* out_entry);
char* procfs_generate_node(const char* rel_path, size_t* out_len);

#endif

