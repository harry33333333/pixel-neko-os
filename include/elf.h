#ifndef _ELF_H
#define _ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "process.h"

bool elf_validate(const uint8_t* image, size_t size);
process_t* elf_load_process(const uint8_t* image, size_t size,
                            const char* name, const char* arg0,
                            const char* arg1, process_t* parent);

#endif /* _ELF_H */
