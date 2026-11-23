#include "procfs.h"
#include "apic.h"
#include "cpu.h"
#include "heap.h"
#include "log.h"
#include "pci.h"
#include "pdv.h"
#include "pmm.h"
#include "process.h"
#include "string.h"
#include "wm.h"

static char proc_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool proc_ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (proc_lower(*a) != proc_lower(*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool procfs_match_path(const char* path, char* rel_out, size_t rel_cap) {
    if (!path) return false;
    const char* p = path;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':') {
        p += 2;
    }
    while (*p == '\\' || *p == '/') p++;
    if (proc_lower(p[0]) != 'p' || proc_lower(p[1]) != 'r' ||
        proc_lower(p[2]) != 'o' || proc_lower(p[3]) != 'c') {
        return false;
    }
    if (p[4] != '\0' && p[4] != '\\' && p[4] != '/') {
        return false;
    }
    p += 4;
    while (*p == '\\' || *p == '/') p++;

    if (rel_out && rel_cap > 0) {
        size_t i = 0;
        while (*p && i + 1 < rel_cap) {
            rel_out[i++] = (*p == '/') ? '\\' : *p;
            p++;
        }
        while (i > 0 && rel_out[i - 1] == '\\') i--;
        rel_out[i] = '\0';
    }
    return true;
}

static bool parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) return false;
    uint32_t v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

static const char* proc_state_name(process_state_t st) {
    switch (st) {
        case PROCESS_READY:   return "READY";
        case PROCESS_RUNNING: return "RUN";
        case PROCESS_BLOCKED: return "BLOCK";
        case PROCESS_ZOMBIE:  return "ZOMBIE";
        default:              return "UNK";
    }
}

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} proc_buf_t;

static void pb_append(proc_buf_t* pb, const char* s) {
    if (!pb || !pb->data || !s) return;
    size_t sl = strlen(s);
    if (pb->len >= pb->cap - 1) return;
    size_t avail = pb->cap - 1 - pb->len;
    if (sl > avail) sl = avail;
    memcpy(pb->data + pb->len, s, sl);
    pb->len += sl;
    pb->data[pb->len] = '\0';
}

static const char* proc_pdv_class_name(uint16_t cls) {
    switch (cls) {
        case 1: return "VIDEO";
        case 2: return "AUDIO";
        case 3: return "STORAGE";
        case 4: return "INPUT";
        default: return "OTHER";
    }
}

char* procfs_generate_node(const char* rel_path, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!rel_path || !rel_path[0]) return NULL;

    size_t cap = proc_ieq(rel_path, "klog") ? 32768u : 8192u;
    char* mem = (char*)kmalloc(cap);
    if (!mem) return NULL;
    mem[0] = '\0';
    proc_buf_t pb = { mem, 0, cap };
    char line[320];

    if (proc_ieq(rel_path, "list")) {
        pb_append(&pb, "PID   PPID  RING  STATE   MEM_KB    TICKS        NAME\n");
        for (process_t* p = process_head(); p; p = p->next) {
            uint64_t mem_kb = (p->mem_pages_used * 4096ULL + p->r0_heap_bytes) / 1024ULL;
            snprintf(line, sizeof(line), "%-5u %-5u R%-4u %-7s %-9llu %-12llu %s\n",
                     p->pid, p->ppid, (unsigned)p->exec_ring,
                     proc_state_name(p->state),
                     (unsigned long long)mem_kb,
                     (unsigned long long)p->cpu_ticks_total,
                     p->name);
            pb_append(&pb, line);
        }
    } else if (proc_ieq(rel_path, "mem")) {
        uint64_t tot_pg = pmm_get_total_page_count();
        uint64_t fre_pg = pmm_get_free_page_count();
        uint64_t used_pg = (tot_pg >= fre_pg) ? (tot_pg - fre_pg) : 0;
        size_t h_tot = 0, h_used = 0, h_free = 0;
        kheap_get_stats(&h_tot, &h_used, &h_free);
        int pcnt = 0;
        for (process_t* p = process_head(); p; p = p->next) {
            if (p->state != PROCESS_ZOMBIE) pcnt++;
        }
        snprintf(line, sizeof(line),
                 "MemTotalKB:     %llu\n"
                 "MemUsedKB:      %llu\n"
                 "MemFreeKB:      %llu\n"
                 "TotalPages:     %llu\n"
                 "FreePages:      %llu\n"
                 "HeapTotalKB:    %llu\n"
                 "HeapUsedKB:     %llu\n"
                 "HeapFreeKB:     %llu\n"
                 "ActiveProcs:    %d\n"
                 "LoadedPDVs:     %d\n",
                 (unsigned long long)(tot_pg * 4ULL),
                 (unsigned long long)(used_pg * 4ULL),
                 (unsigned long long)(fre_pg * 4ULL),
                 (unsigned long long)tot_pg,
                 (unsigned long long)fre_pg,
                 (unsigned long long)(h_tot / 1024ULL),
                 (unsigned long long)(h_used / 1024ULL),
                 (unsigned long long)(h_free / 1024ULL),
                 pcnt, pdv_get_loaded_count());
        pb_append(&pb, line);
    } else if (proc_ieq(rel_path, "cpu")) {
        uint64_t ticks = apic_get_ticks();
        snprintf(line, sizeof(line),
                 "Vendor:         %s\n"
                 "Model:          %s\n"
                 "Family:         %u\n"
                 "ModelNum:       %u\n"
                 "Stepping:       %u\n"
                 "ClockMHz:       %u\n"
                 "Features:       %s%s%s%s%s%s%s\n"
                 "ApicTicks:      %llu\n"
                 "UptimeSec:      %llu\n",
                 g_cpu_info.vendor[0] ? g_cpu_info.vendor : "Unknown",
                 g_cpu_info.brand[0] ? g_cpu_info.brand : cpu_model_string,
                 g_cpu_info.family, g_cpu_info.model, g_cpu_info.stepping,
                 g_cpu_info.mhz,
                 g_cpu_info.has_lm ? "LM " : "",
                 g_cpu_info.has_nx ? "NX " : "",
                 g_cpu_info.has_apic ? "APIC " : "",
                 g_cpu_info.has_syscall ? "SYSCALL " : "",
                 g_cpu_info.has_sse2 ? "SSE2 " : "",
                 g_cpu_info.has_sse3 ? "SSE3 " : "",
                 g_cpu_info.has_cx16 ? "CX16" : "",
                 (unsigned long long)ticks,
                 (unsigned long long)(ticks / 100ULL));
        pb_append(&pb, line);
    } else if (proc_ieq(rel_path, "pci")) {
        int n = pci_get_device_count();
        pb_append(&pb, "BDF       VEN:DEV   CLS/SUB IRQ BAR0             CLASS_NAME\n");
        for (int i = 0; i < n; i++) {
            const pci_device_t* d = pci_get_device(i);
            if (!d) continue;
            const char* cname = pci_class_name(d->class_code, d->subclass);
            snprintf(line, sizeof(line),
                     "%02x:%02x.%u   %04x:%04x %02x/%02x   %-3u 0x%014llx %s\n",
                     d->bus, d->slot, d->function, d->vendor, d->device,
                     d->class_code, d->subclass, d->irq_line,
                     (unsigned long long)d->bar[0].address,
                     cname ? cname : "Unknown");
            pb_append(&pb, line);
        }
    } else if (proc_ieq(rel_path, "pdv")) {
        int n = pdv_get_loaded_count();
        pb_append(&pb, "FILE             NAME                     VER     CLASS   BASE               PAGES  PROVIDER\n");
        for (int i = 0; i < n; i++) {
            const pdv_module_info_t* inf = pdv_get_loaded_info(i);
            if (!inf) continue;
            size_t tot_pg = inf->rx_pages + inf->ro_pages + inf->rw_pages;
            snprintf(line, sizeof(line),
                     "%-16s %-24s %-7s %-7s 0x%016llx %-6llu %s\n",
                     inf->filename, inf->driver_name, inf->version_str,
                     proc_pdv_class_name(inf->driver_class),
                     (unsigned long long)inf->base_virt,
                     (unsigned long long)tot_pg,
                     inf->provider);
            pb_append(&pb, line);
        }
    } else if (proc_ieq(rel_path, "klog")) {
        int cnt = log_get_count();
        for (int i = 0; i < cnt; i++) {
            const char* ln = log_get_line(i);
            if (!ln) continue;
            pb_append(&pb, ln);
            size_t sl = strlen(ln);
            if (sl == 0 || ln[sl - 1] != '\n') pb_append(&pb, "\n");
        }
    } else if (proc_ieq(rel_path, "mounts")) {
        pb_append(&pb, "DRV TYPE   FS       TOTAL_KB    FREE_KB     ACCESS LABEL\n");
        for (char c = 'A'; c <= 'Z'; c++) {
            vfs_drive_t* d = vfs_get_drive(c);
            if (!d || !d->present) continue;
            uint64_t tot_kb = d->total_kb;
            uint64_t fre_kb = d->free_kb;
            int known = d->is_free_known;
            (void)vfs_get_drive_space(c, &tot_kb, &fre_kb, &known);
            const char* dtype = "OTHER";
            if (d->drv_type == VFS_DRV_FLOPPY) dtype = "FLOPPY";
            else if (d->drv_type == VFS_DRV_HDD) dtype = "HDD";
            else if (d->drv_type == VFS_DRV_CDROM) dtype = "CDROM";
            else if (d->drv_type == VFS_DRV_USB) dtype = "USB";
            const char* fsn = vfs_get_fs_name(d->fs_type);
            if (!fsn || d->fs_type == PART_FS_UNKNOWN) {
                if (!d->mounted) fsn = "NONE";
                else if (d->drv_type == VFS_DRV_CDROM) fsn = "ISO/CDDA";
                else fsn = "UNKNOWN";
            }
            snprintf(line, sizeof(line),
                     "%c:  %-6s %-8s %-11llu %-11llu %-6s %s\n",
                     c, dtype, fsn,
                     (unsigned long long)tot_kb,
                     known ? (unsigned long long)fre_kb : 0ULL,
                     d->is_readonly ? "RO" : "RW",
                     d->label);
            pb_append(&pb, line);
        }
    } else {
        /* Check <pid>\stat */
        const char* slash = NULL;
        for (const char* q = rel_path; *q; q++) {
            if (*q == '\\') { slash = q; break; }
        }
        uint32_t pid = 0;
        if (!slash || !proc_ieq(slash + 1, "stat")) {
            kfree(mem);
            return NULL;
        }
        char pid_buf[16];
        size_t plen = (size_t)(slash - rel_path);
        if (plen == 0 || plen >= sizeof(pid_buf)) {
            kfree(mem);
            return NULL;
        }
        memcpy(pid_buf, rel_path, plen);
        pid_buf[plen] = '\0';
        if (!parse_u32(pid_buf, &pid)) {
            kfree(mem);
            return NULL;
        }
        process_t* target = NULL;
        for (process_t* p = process_head(); p; p = p->next) {
            if (p->pid == pid) { target = p; break; }
        }
        if (!target) {
            kfree(mem);
            return NULL;
        }
        int open_streams = 0;
        for (int i = 0; i < PROCESS_MAX_STREAMS; i++) {
            if (target->streams[i]) open_streams++;
        }
        snprintf(line, sizeof(line),
                 "PID:          %u\n"
                 "PPID:         %u\n"
                 "Name:         %s\n"
                 "ExePath:      %s\n"
                 "Ring:         %u\n"
                 "State:        %s\n"
                 "MemPages:     %llu\n"
                 "MemKB:        %llu\n"
                 "R0HeapBytes:  %llu\n"
                 "CpuTicks:     %llu\n"
                 "Windows:      %d\n"
                 "OpenStreams:  %d\n"
                 "PML4Phys:     0x%016llx\n",
                 target->pid, target->ppid, target->name, target->exe_path,
                 (unsigned)target->exec_ring, proc_state_name(target->state),
                 (unsigned long long)target->mem_pages_used,
                 (unsigned long long)((target->mem_pages_used * 4096ULL + target->r0_heap_bytes) / 1024ULL),
                 (unsigned long long)target->r0_heap_bytes,
                 (unsigned long long)target->cpu_ticks_total,
                 wm_pxe_count_windows_for_pid(target->pid),
                 open_streams,
                 (unsigned long long)target->pml4_phys);
        pb_append(&pb, line);
    }

    if (out_len) *out_len = pb.len;
    return mem;
}

static void emit_proc_file(const char* name, size_t size,
                           void (*callback)(const vfs_dirent_t*)) {
    if (!callback) return;
    vfs_dirent_t ent;
    memset(&ent, 0, sizeof(ent));
    size_t i = 0;
    while (name[i] && i + 1 < sizeof(ent.name)) {
        ent.name[i] = name[i];
        i++;
    }
    ent.name[i] = '\0';
    ent.is_dir = 0;
    ent.size = size;
    callback(&ent);
}

int procfs_list_dir(const char* rel_path, void (*callback)(const vfs_dirent_t*)) {
    if (!rel_path || !rel_path[0]) {
        static const char* k_nodes[] = {
            "list", "mem", "cpu", "pci", "pdv", "klog", "mounts"
        };
        for (size_t i = 0; i < sizeof(k_nodes) / sizeof(k_nodes[0]); i++) {
            size_t sz = 0;
            char* buf = procfs_generate_node(k_nodes[i], &sz);
            if (buf) kfree(buf);
            emit_proc_file(k_nodes[i], sz, callback);
        }
        for (process_t* p = process_head(); p; p = p->next) {
            vfs_dirent_t ent;
            memset(&ent, 0, sizeof(ent));
            snprintf(ent.name, sizeof(ent.name), "%u", p->pid);
            ent.is_dir = 1;
            if (callback) callback(&ent);
        }
        return VFS_OK;
    }

    uint32_t pid = 0;
    if (parse_u32(rel_path, &pid)) {
        for (process_t* p = process_head(); p; p = p->next) {
            if (p->pid == pid) {
                char node_rel[32];
                snprintf(node_rel, sizeof(node_rel), "%u\\stat", pid);
                size_t sz = 0;
                char* buf = procfs_generate_node(node_rel, &sz);
                if (buf) kfree(buf);
                emit_proc_file("stat", sz, callback);
                return VFS_OK;
            }
        }
    }
    return VFS_ERR_NOT_FOUND;
}

int procfs_find_file(const char* rel_path, vfs_dirent_t* out_entry) {
    if (!out_entry) return VFS_ERR_NOT_FOUND;
    memset(out_entry, 0, sizeof(*out_entry));
    if (!rel_path || !rel_path[0]) {
        strcpy(out_entry->name, "PROC");
        out_entry->is_dir = 1;
        return VFS_OK;
    }
    uint32_t pid = 0;
    if (parse_u32(rel_path, &pid)) {
        for (process_t* p = process_head(); p; p = p->next) {
            if (p->pid == pid) {
                snprintf(out_entry->name, sizeof(out_entry->name), "%u", pid);
                out_entry->is_dir = 1;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOT_FOUND;
    }
    size_t sz = 0;
    char* buf = procfs_generate_node(rel_path, &sz);
    if (!buf) return VFS_ERR_NOT_FOUND;
    kfree(buf);

    const char* base = rel_path;
    for (const char* q = rel_path; *q; q++) {
        if (*q == '\\') base = q + 1;
    }
    strncpy(out_entry->name, base, sizeof(out_entry->name) - 1);
    out_entry->is_dir = 0;
    out_entry->size = sz;
    return VFS_OK;
}

