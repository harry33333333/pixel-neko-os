#ifndef _SMP_H
#define _SMP_H

#include <stdint.h>
#include <stdbool.h>
#include "percpu.h"

typedef struct {
    uint32_t logical_cpus;
    uint32_t physical_cores;
    uint32_t packages;
    bool     smt_enabled;
    bool     x2apic_active;
} smp_topology_t;

void smp_init(void);
void smp_detect_this_cpu_topology(void);
uint32_t smp_get_online_cpus(void);
uint32_t smp_get_cpu_count(void);
void smp_get_topology(smp_topology_t* out);
per_cpu_t* smp_get_cpu(uint32_t cpu_id);

#endif /* _SMP_H */
