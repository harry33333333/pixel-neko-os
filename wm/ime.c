#include "ime.h"
#include "ime_core.h"
#include "wm.h"
#include "font.h"
#include "utf8.h"
#include "vfs.h"
#include "pmm.h"
#include "mm.h"
#include "settings.h"
#include "string.h"
#include "log.h"
#include "input.h"
#include "neko_abi.h"

#define NKPY_MAGIC          0x59504B4Eu /* "NKPY" */
#define NKPY_VERSION        1u
#define IME_DICT_MAX_BYTES  (4u * 1024u * 1024u)
#define IME_MAX_RAW         40
#define IME_MAX_CANDS       64
#define IME_MAX_CAND_BYTES  64

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t syl_count;
    uint32_t single_count;
    uint32_t phrase_count;
    uint32_t syl_table_off;
    uint32_t single_idx_off;
    uint32_t single_off;
    uint32_t phrase_idx_off;
    uint32_t phrase_off;
    uint32_t syl_pool_off;
    uint32_t utf8_pool_off;
    uint32_t total_size;
} __attribute__((packed)) nkpy_header_t;

typedef struct {
    uint32_t utf8_offset;
    uint32_t weight;
    uint16_t syl_id;
    uint8_t  utf8_len;
    uint8_t  reserved;
} __attribute__((packed)) nkpy_single_t;

typedef struct {
    uint32_t syl_offset;
    uint32_t utf8_offset;
    uint32_t weight;
    uint8_t  syl_len;
    uint8_t  utf8_len;
    uint16_t second_syl;
} __attribute__((packed)) nkpy_phrase_t;

typedef struct {
    char    text[IME_MAX_CAND_BYTES];
    uint8_t consumed; /* bytes of g_raw consumed */
} ime_cand_t;

static phys_addr_t g_dict_phys = 0;
static size_t      g_dict_pages = 0;
static const uint8_t* g_dict_ptr = NULL;
static uint32_t    g_dict_phrase_count = 0;
static uint32_t    g_dict_single_count = 0;

static bool        g_ime_enabled = false;
static char        g_raw[IME_MAX_RAW + 1];
static int         g_raw_len = 0;
static ime_cand_t  g_cands[IME_MAX_CANDS];
static int         g_cand_count = 0;
static int         g_cand_page = 0;
static int         g_cand_sel = 0;
static bool        g_quote_double_open = false;
static bool        g_quote_single_open = false;

static int         g_popup_x = 0;
static int         g_popup_y = 0;
static int         g_popup_w = 0;
static int         g_popup_h = 0;
static int         g_cand_hit_x0[IME_PAGE_SIZE];
static int         g_cand_hit_x1[IME_PAGE_SIZE];

static bool nkpy_validate(const nkpy_header_t* h, uint32_t max_bytes) {
    if (!h || max_bytes < sizeof(nkpy_header_t)) return false;
    if (h->magic != NKPY_MAGIC || h->version != NKPY_VERSION) return false;
    if (h->total_size < sizeof(nkpy_header_t) || h->total_size > max_bytes) return false;
    if (h->syl_count == 0 || h->syl_count > 1024) return false;
    uint64_t sc = (uint64_t)h->syl_count;
    if ((uint64_t)h->syl_table_off + sc * 8ULL > h->total_size) return false;
    if ((uint64_t)h->single_idx_off + (sc + 1ULL) * 4ULL > h->total_size) return false;
    if ((uint64_t)h->single_off + (uint64_t)h->single_count * sizeof(nkpy_single_t) > h->total_size) return false;
    if ((uint64_t)h->phrase_idx_off + (sc + 1ULL) * 4ULL > h->total_size) return false;
    if ((uint64_t)h->phrase_off + (uint64_t)h->phrase_count * sizeof(nkpy_phrase_t) > h->total_size) return false;
    if (h->utf8_pool_off >= h->total_size) return false;
    return true;
}

static bool try_load_ime_file(const char* path) {
    vfs_dirent_t ent;
    if (!path || vfs_find_file(path, &ent) != VFS_OK || ent.is_dir) return false;
    if (ent.size < (uint32_t)sizeof(nkpy_header_t) || ent.size > IME_DICT_MAX_BYTES) {
        return false;
    }
    size_t pages = ((size_t)ent.size + PAGE_SIZE - 1u) / PAGE_SIZE;
    phys_addr_t phys = pmm_alloc_pages(pages);
    if (!phys) return false;
    void* virt = phys_to_virt(phys);
    memset(virt, 0, pages * PAGE_SIZE);
    int n = vfs_read_file(path, (uint8_t*)virt, ent.size);
    if (n != (int)ent.size || !nkpy_validate((const nkpy_header_t*)virt, ent.size)) {
        pmm_free_pages(phys, pages);
        return false;
    }
    g_dict_phys = phys;
    g_dict_pages = pages;
    g_dict_ptr = (const uint8_t*)virt;
    const nkpy_header_t* h = (const nkpy_header_t*)g_dict_ptr;
    g_dict_phrase_count = h->phrase_count;
    g_dict_single_count = h->single_count;
    log_printf("[IME ] Loaded Rime lexicon %s (%u bytes, %u syls, %u singles, %u phrases)\n",
               path, (unsigned)ent.size, (unsigned)h->syl_count,
               (unsigned)h->single_count, (unsigned)h->phrase_count);
    return true;
}

bool ime_has_full_lexicon(void) {
    return g_dict_ptr != NULL;
}

uint32_t ime_get_phrase_count(void) {
    return g_dict_ptr ? g_dict_phrase_count : 0u;
}

uint32_t ime_get_single_count(void) {
    return g_dict_ptr ? g_dict_single_count : 0u;
}

void ime_init(void) {
    if (g_dict_ptr) return;
    char path[64];
    char drv = settings_get_system_drive();
    if (drv) {
        path[0] = drv; path[1] = ':';
        strcpy(path + 2, "\\system\\IME.PNY");
        if (try_load_ime_file(path)) return;
    }
    drv = vfs_get_boot_cdrom_letter();
    if (drv) {
        path[0] = drv; path[1] = ':';
        strcpy(path + 2, "\\system\\IME.PNY");
        if (try_load_ime_file(path)) return;
    }
    for (char c = 'C'; c <= 'Z'; c++) {
        vfs_drive_t* d = vfs_get_drive(c);
        if (!d || !d->present || !d->mounted) continue;
        path[0] = c; path[1] = ':';
        strcpy(path + 2, "\\system\\IME.PNY");
        if (try_load_ime_file(path)) return;
    }
    log_printf("[IME ] Using built-in core fallback lexicon\n");
}

static int ascii_cmp_n(const char* a, const char* b, int b_len) {
    for (int i = 0; i < b_len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == 0) return -1;
        if (ca != cb) return (int)ca - (int)cb;
    }
    return ((unsigned char)a[b_len] == 0) ? 0 : 1;
}

static int find_syl_id(const char* s, int len) {
    if (!s || len <= 0 || len > 7) return -1;
    for (int i = 0; i < len; i++) {
        if (s[i] < 'a' || s[i] > 'z') return -1;
    }
    if (g_dict_ptr) {
        const nkpy_header_t* h = (const nkpy_header_t*)g_dict_ptr;
        const char* table = (const char*)(g_dict_ptr + h->syl_table_off);
        int lo = 0, hi = (int)h->syl_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            const char* entry = table + mid * 8;
            int cmp = ascii_cmp_n(entry, s, len);
            if (cmp == 0) return mid;
            if (cmp < 0) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }
    int count = (int)(sizeof(g_ime_core_syllables) / sizeof(g_ime_core_syllables[0]));
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = ascii_cmp_n(g_ime_core_syllables[mid], s, len);
        if (cmp == 0) return mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static const char* get_syl_str(int sid) {
    if (g_dict_ptr) {
        const nkpy_header_t* h = (const nkpy_header_t*)g_dict_ptr;
        if (sid < 0 || sid >= (int)h->syl_count) return "";
        return (const char*)(g_dict_ptr + h->syl_table_off + (size_t)sid * 8u);
    }
    int count = (int)(sizeof(g_ime_core_syllables) / sizeof(g_ime_core_syllables[0]));
    if (sid < 0 || sid >= count) return "";
    return g_ime_core_syllables[sid];
}

static uint32_t ilog2_score(uint32_t w) {
    uint32_t bits = 0;
    uint32_t v = (w == 0) ? 1u : w;
    while (v > 1u) {
        v >>= 1;
        bits++;
    }
    return bits * 8u;
}

static void add_candidate(const char* txt, int len, uint8_t consumed) {
    if (!txt || len <= 0 || g_cand_count >= IME_MAX_CANDS) return;
    if (len >= IME_MAX_CAND_BYTES) return;
    for (int i = 0; i < g_cand_count; i++) {
        if (strncmp(g_cands[i].text, txt, (size_t)len) == 0 &&
            g_cands[i].text[len] == '\0') {
            return;
        }
    }
    memcpy(g_cands[g_cand_count].text, txt, (size_t)len);
    g_cands[g_cand_count].text[len] = '\0';
    g_cands[g_cand_count].consumed = consumed;
    g_cand_count++;
}

static void add_candidate_cstr(const char* txt, uint8_t consumed) {
    if (!txt || !txt[0]) return;
    add_candidate(txt, (int)strlen(txt), consumed);
}

/* Match remaining phrase syllables against raw[pos..] with optional apostrophes. Returns end_pos or -1. */
static int match_phrase_syls(const uint16_t* sids, int syl_len, int start_after_first, int* out_syl_ends) {
    (void)out_syl_ends;
    int pos = start_after_first;
    for (int s = 1; s < syl_len; s++) {
        while (pos < g_raw_len && g_raw[pos] == '\'') pos++;
        const char* target = get_syl_str((int)sids[s]);
        int k = 0;
        while (target[k]) {
            if (pos >= g_raw_len || g_raw[pos] != target[k]) return -1;
            pos++;
            k++;
        }
    }
    return pos;
}

typedef struct {
    int32_t  score;
    int16_t  prev_pos;
    uint32_t utf8_off;
    uint8_t  utf8_len;
    uint8_t  segs;
} viterbi_node_t;

static void rebuild_candidates_from_pny(void) {
    const nkpy_header_t* h = (const nkpy_header_t*)g_dict_ptr;
    const uint32_t* single_idx = (const uint32_t*)(g_dict_ptr + h->single_idx_off);
    const nkpy_single_t* singles = (const nkpy_single_t*)(g_dict_ptr + h->single_off);
    const uint32_t* phrase_idx = (const uint32_t*)(g_dict_ptr + h->phrase_idx_off);
    const nkpy_phrase_t* phrases = (const nkpy_phrase_t*)(g_dict_ptr + h->phrase_off);
    const uint16_t* syl_pool = (const uint16_t*)(g_dict_ptr + h->syl_pool_off);
    const char* utf8_pool = (const char*)(g_dict_ptr + h->utf8_pool_off);
    uint32_t pool_max = h->total_size - h->utf8_pool_off;

    /* 1. Precompute valid single syllables starting at each position in g_raw */
    viterbi_node_t dp[IME_MAX_RAW + 1];
    for (int i = 0; i <= g_raw_len; i++) {
        dp[i].score = -1;
        dp[i].prev_pos = -1;
        dp[i].utf8_off = 0;
        dp[i].utf8_len = 0;
        dp[i].segs = 0;
    }
    dp[0].score = 0;

    for (int i = 0; i < g_raw_len; i++) {
        if (dp[i].score < 0) continue;
        if (g_raw[i] == '\'') {
            if (dp[i].score > dp[i + 1].score) {
                dp[i + 1] = dp[i];
                dp[i + 1].prev_pos = (int16_t)i;
                dp[i + 1].utf8_len = 0;
            }
            continue;
        }
        /* Precompute possible second syllables starting right after i+slen */
        for (int slen = 6; slen >= 1; slen--) {
            if (i + slen > g_raw_len) continue;
            int sid0 = find_syl_id(g_raw + i, slen);
            if (sid0 < 0) continue;

            /* Single-char edge i -> i + slen using top weighted char for sid0 */
            uint32_t s_beg = single_idx[sid0];
            uint32_t s_end = single_idx[sid0 + 1];
            if (s_beg < s_end) {
                const nkpy_single_t* best_c = &singles[s_beg];
                int32_t edge = dp[i].score + 60 + (int32_t)ilog2_score(best_c->weight) + slen * 24;
                int next_pos = i + slen;
                if (edge > dp[next_pos].score &&
                    best_c->utf8_offset + best_c->utf8_len <= pool_max) {
                    dp[next_pos].score = edge;
                    dp[next_pos].prev_pos = (int16_t)i;
                    dp[next_pos].utf8_off = best_c->utf8_offset;
                    dp[next_pos].utf8_len = best_c->utf8_len;
                    dp[next_pos].segs = (uint8_t)(dp[i].segs + 1u);
                }
            }

            /* Multi-syllable phrase edges starting at i */
            int j = i + slen;
            while (j < g_raw_len && g_raw[j] == '\'') j++;
            if (j >= g_raw_len) continue;

            uint16_t valid_next_sids[6];
            int valid_next_cnt = 0;
            for (int l2 = 6; l2 >= 1; l2--) {
                if (j + l2 <= g_raw_len) {
                    int sid1 = find_syl_id(g_raw + j, l2);
                    if (sid1 >= 0) valid_next_sids[valid_next_cnt++] = (uint16_t)sid1;
                }
            }
            if (valid_next_cnt == 0) continue;

            uint32_t p_beg = phrase_idx[sid0];
            uint32_t p_end = phrase_idx[sid0 + 1];
            for (uint32_t pi = p_beg; pi < p_end; pi++) {
                const nkpy_phrase_t* ph = &phrases[pi];
                bool sec_ok = false;
                for (int vn = 0; vn < valid_next_cnt; vn++) {
                    if (ph->second_syl == valid_next_sids[vn]) { sec_ok = true; break; }
                }
                if (!sec_ok) continue;

                const uint16_t* sids = syl_pool + ph->syl_offset;
                int end_pos = match_phrase_syls(sids, (int)ph->syl_len, j, NULL);
                if (end_pos < 0) continue;

                int32_t edge = dp[i].score + (int32_t)ph->syl_len * 200 +
                               (int32_t)ilog2_score(ph->weight) + (end_pos - i) * 28;
                if (edge > dp[end_pos].score &&
                    ph->utf8_offset + ph->utf8_len <= pool_max) {
                    dp[end_pos].score = edge;
                    dp[end_pos].prev_pos = (int16_t)i;
                    dp[end_pos].utf8_off = ph->utf8_offset;
                    dp[end_pos].utf8_len = ph->utf8_len;
                    dp[end_pos].segs = (uint8_t)(dp[i].segs + 1u);
                }
            }
        }
    }

    /* 2. Exact phrases matching from 0..g_raw_len (all lengths), put top exact match/sentence first */
    int exact_end = g_raw_len;
    while (exact_end > 0 && g_raw[exact_end - 1] == '\'') exact_end--;

    char sent_buf[IME_MAX_CAND_BYTES];
    int sent_len = 0;
    if (dp[g_raw_len].score >= 0 && dp[g_raw_len].segs > 1) {
        /* Reconstruct reverse segments */
        int stack_pos[IME_MAX_RAW + 1];
        int sp = 0;
        int cur = g_raw_len;
        while (cur > 0 && sp <= IME_MAX_RAW) {
            stack_pos[sp++] = cur;
            cur = dp[cur].prev_pos;
        }
        if (cur == 0) {
            for (int t = sp - 1; t >= 0; t--) {
                const viterbi_node_t* n = &dp[stack_pos[t]];
                if (n->utf8_len > 0 && sent_len + n->utf8_len < IME_MAX_CAND_BYTES) {
                    memcpy(sent_buf + sent_len, utf8_pool + n->utf8_off, n->utf8_len);
                    sent_len += n->utf8_len;
                }
            }
            sent_buf[sent_len] = '\0';
        }
    }

    /* Find longest/all first syllables at position 0 */
    int first_sids[6];
    int first_slens[6];
    int first_cnt = 0;
    for (int slen = 6; slen >= 1; slen--) {
        if (slen <= g_raw_len) {
            int sid = find_syl_id(g_raw, slen);
            if (sid >= 0) {
                first_sids[first_cnt] = sid;
                first_slens[first_cnt] = slen;
                first_cnt++;
            }
        }
    }

    /* Add single-phrase exact match first if higher/equal quality or equal to sentence */
    for (int fc = 0; fc < first_cnt; fc++) {
        int sid0 = first_sids[fc];
        int j = first_slens[fc];
        while (j < g_raw_len && g_raw[j] == '\'') j++;
        if (j >= g_raw_len) continue;
        uint32_t p_beg = phrase_idx[sid0];
        uint32_t p_end = phrase_idx[sid0 + 1];
        for (uint32_t pi = p_beg; pi < p_end && g_cand_count < 16; pi++) {
            const nkpy_phrase_t* ph = &phrases[pi];
            if (ph->utf8_offset + ph->utf8_len > pool_max) continue;
            int end_pos = match_phrase_syls(syl_pool + ph->syl_offset, (int)ph->syl_len, j, NULL);
            if (end_pos >= exact_end) {
                add_candidate(utf8_pool + ph->utf8_offset, (int)ph->utf8_len, (uint8_t)g_raw_len);
            }
        }
    }

    /* Add reconstructed Viterbi whole sentence if available */
    if (sent_len > 0) {
        add_candidate(sent_buf, sent_len, (uint8_t)g_raw_len);
    }

    /* 3. Jianpin / initial abbreviation matching (e.g. "nh" -> "你好", "xjwjj" -> "新建文件夹") */
    if (g_raw_len >= 2 && g_raw_len <= 6) {
        bool all_alpha = true;
        for (int i = 0; i < g_raw_len; i++) {
            if (g_raw[i] < 'a' || g_raw[i] > 'z') { all_alpha = false; break; }
        }
        if (all_alpha) {
            for (uint16_t sid = 0; sid < h->syl_count && g_cand_count < 18; sid++) {
                const char* s0 = get_syl_str((int)sid);
                if (s0[0] != g_raw[0]) continue;
                uint32_t p_beg = phrase_idx[sid];
                uint32_t p_end = phrase_idx[sid + 1];
                uint32_t limit = p_beg + 80u;
                if (limit > p_end) limit = p_end;
                for (uint32_t pi = p_beg; pi < limit && g_cand_count < 18; pi++) {
                    const nkpy_phrase_t* ph = &phrases[pi];
                    if ((int)ph->syl_len != g_raw_len) continue;
                    if (ph->weight < 200u) continue;
                    const uint16_t* sids = syl_pool + ph->syl_offset;
                    bool ok = true;
                    for (int t = 1; t < g_raw_len; t++) {
                        if (get_syl_str((int)sids[t])[0] != g_raw[t]) { ok = false; break; }
                    }
                    if (ok && ph->utf8_offset + ph->utf8_len <= pool_max) {
                        add_candidate(utf8_pool + ph->utf8_offset, (int)ph->utf8_len, (uint8_t)g_raw_len);
                    }
                }
            }
        }
    }

    /* 4. Prefix phrases from position 0 (partial multi-char words) */
    for (int fc = 0; fc < first_cnt && g_cand_count < 28; fc++) {
        int sid0 = first_sids[fc];
        int j = first_slens[fc];
        while (j < g_raw_len && g_raw[j] == '\'') j++;
        if (j >= g_raw_len) continue;
        uint32_t p_beg = phrase_idx[sid0];
        uint32_t p_end = phrase_idx[sid0 + 1];
        for (uint32_t pi = p_beg; pi < p_end && g_cand_count < 28; pi++) {
            const nkpy_phrase_t* ph = &phrases[pi];
            if (ph->utf8_offset + ph->utf8_len > pool_max) continue;
            int end_pos = match_phrase_syls(syl_pool + ph->syl_offset, (int)ph->syl_len, j, NULL);
            if (end_pos > 0 && end_pos < exact_end) {
                add_candidate(utf8_pool + ph->utf8_offset, (int)ph->utf8_len, (uint8_t)end_pos);
            }
        }
    }

    /* 5. Single characters matching first syllable (or prefix match when incomplete syllable like "zh") */
    if (first_cnt > 0) {
        int sid0 = first_sids[0];
        int slen0 = first_slens[0];
        uint32_t s_beg = single_idx[sid0];
        uint32_t s_end = single_idx[sid0 + 1];
        for (uint32_t si = s_beg; si < s_end && g_cand_count < IME_MAX_CANDS; si++) {
            const nkpy_single_t* ch = &singles[si];
            if (ch->utf8_offset + ch->utf8_len <= pool_max) {
                add_candidate(utf8_pool + ch->utf8_offset, (int)ch->utf8_len, (uint8_t)slen0);
            }
        }
    }
    if (g_cand_count < IME_MAX_CANDS && (first_cnt == 0 || first_slens[0] < g_raw_len)) {
        int plen = (g_raw_len < 6) ? g_raw_len : 6;
        for (int sid = 0; sid < (int)h->syl_count && g_cand_count < IME_MAX_CANDS; sid++) {
            const char* syl = get_syl_str(sid);
            if (strncmp(syl, g_raw, (size_t)plen) == 0) {
                uint32_t s_beg = single_idx[sid];
                uint32_t s_end = single_idx[sid + 1];
                uint32_t lim = s_beg + 6u;
                if (lim > s_end) lim = s_end;
                for (uint32_t si = s_beg; si < lim && g_cand_count < IME_MAX_CANDS; si++) {
                    const nkpy_single_t* ch = &singles[si];
                    if (ch->utf8_offset + ch->utf8_len <= pool_max) {
                        add_candidate(utf8_pool + ch->utf8_offset, (int)ch->utf8_len, (uint8_t)g_raw_len);
                    }
                }
            }
        }
    }
}

static void rebuild_candidates_fallback(void) {
    int count = (int)(sizeof(g_ime_core_entries) / sizeof(g_ime_core_entries[0]));
    /* Strip apostrophes for compact compare */
    char clean[IME_MAX_RAW + 1];
    int clen = 0;
    for (int i = 0; i < g_raw_len; i++) {
        if (g_raw[i] != '\'') clean[clen++] = g_raw[i];
    }
    clean[clen] = '\0';

    for (int i = 0; i < count && g_cand_count < IME_MAX_CANDS; i++) {
        if (strcmp(g_ime_core_entries[i].pinyin, clean) == 0) {
            add_candidate_cstr(g_ime_core_entries[i].text, (uint8_t)g_raw_len);
        }
    }
    for (int sl = 6; sl >= 1 && g_cand_count < IME_MAX_CANDS; sl--) {
        if (sl >= clen) continue;
        int sid = find_syl_id(clean, sl);
        if (sid < 0) continue;
        for (int i = 0; i < count && g_cand_count < IME_MAX_CANDS; i++) {
            if (strncmp(g_ime_core_entries[i].pinyin, clean, (size_t)sl) == 0 &&
                g_ime_core_entries[i].pinyin[sl] == '\0') {
                add_candidate_cstr(g_ime_core_entries[i].text, (uint8_t)sl);
            }
        }
        break;
    }
}

static void rebuild_candidates(void) {
    g_cand_count = 0;
    g_cand_page = 0;
    g_cand_sel = 0;
    if (g_raw_len <= 0) return;
    if (g_dict_ptr) {
        rebuild_candidates_from_pny();
    } else {
        rebuild_candidates_fallback();
    }
    if (g_cand_count == 0) {
        add_candidate(g_raw, g_raw_len, (uint8_t)g_raw_len);
    }
}

bool ime_is_enabled(void) {
    return g_ime_enabled;
}

void ime_set_enabled(bool enabled) {
    if (g_ime_enabled == enabled) return;
    g_ime_enabled = enabled;
    if (!g_ime_enabled) {
        ime_clear_composition();
    }
}

void ime_toggle(void) {
    ime_set_enabled(!g_ime_enabled);
}

bool ime_has_composition(void) {
    return g_raw_len > 0;
}

void ime_clear_composition(void) {
    g_raw[0] = '\0';
    g_raw_len = 0;
    g_cand_count = 0;
    g_cand_page = 0;
    g_cand_sel = 0;
    g_popup_w = 0;
    g_popup_h = 0;
}

static bool map_zh_punct(int key, char* out, size_t cap) {
    const char* s = NULL;
    switch (key) {
        case ',':  s = "\xEF\xBC\x8C"; break; /* ， */
        case '.':  s = "\xE3\x80\x82"; break; /* 。 */
        case '?':  s = "\xEF\xBC\x9F"; break; /* ？ */
        case '!':  s = "\xEF\xBC\x81"; break; /* ！ */
        case ':':  s = "\xEF\xBC\x9A"; break; /* ： */
        case ';':  s = "\xEF\xBC\x9B"; break; /* ； */
        case '(':  s = "\xEF\xBC\x88"; break; /* （ */
        case ')':  s = "\xEF\xBC\x89"; break; /* ） */
        case '<':  s = "\xE3\x80\x8A"; break; /* 《 */
        case '>':  s = "\xE3\x80\x8B"; break; /* 》 */
        case '\\': s = "\xE3\x80\x81"; break; /* 、 */
        case '"':
            g_quote_double_open = !g_quote_double_open;
            s = g_quote_double_open ? "\xE2\x80\x9C" : "\xE2\x80\x9D"; /* “ ” */
            break;
        case '\'':
            g_quote_single_open = !g_quote_single_open;
            s = g_quote_single_open ? "\xE2\x80\x98" : "\xE2\x80\x99"; /* ‘ ’ */
            break;
        default:
            return false;
    }
    if (!out || cap < 4) return false;
    strcpy(out, s);
    return true;
}

static void commit_candidate_idx(int idx, char* out_commit, size_t out_cap) {
    if (idx < 0 || idx >= g_cand_count) {
        ime_clear_composition();
        return;
    }
    const ime_cand_t* c = &g_cands[idx];
    if (out_commit && out_cap > 0) {
        size_t n = strlen(c->text);
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out_commit, c->text, n);
        out_commit[n] = '\0';
    }
    int cons = (int)c->consumed;
    while (cons < g_raw_len && g_raw[cons] == '\'') cons++;
    if (cons >= g_raw_len) {
        ime_clear_composition();
    } else {
        int rem = g_raw_len - cons;
        memmove(g_raw, g_raw + cons, (size_t)rem);
        g_raw_len = rem;
        g_raw[g_raw_len] = '\0';
        rebuild_candidates();
    }
}

int ime_process_key(int key, int modifiers, char* out_commit, size_t out_cap) {
    if (out_commit && out_cap > 0) out_commit[0] = '\0';
    if (!g_ime_enabled) return 0;
    if (modifiers & PXE_MOD_CTRL) return 0;

    if (g_raw_len == 0) {
        if (key >= 'a' && key <= 'z') {
            g_raw[0] = (char)key;
            g_raw[1] = '\0';
            g_raw_len = 1;
            rebuild_candidates();
            return 1;
        }
        if (map_zh_punct(key, out_commit, out_cap)) {
            return 1;
        }
        return 0;
    }

    /* Active composition state (g_raw_len > 0) */
    if (key >= 'a' && key <= 'z') {
        if (g_raw_len < IME_MAX_RAW) {
            g_raw[g_raw_len++] = (char)key;
            g_raw[g_raw_len] = '\0';
            rebuild_candidates();
        }
        return 1;
    }
    if (key == '\'') {
        if (g_raw_len < IME_MAX_RAW && g_raw[g_raw_len - 1] != '\'') {
            g_raw[g_raw_len++] = '\'';
            g_raw[g_raw_len] = '\0';
            rebuild_candidates();
        }
        return 1;
    }
    if (key == '\b' || key == INPUT_CODE_BACKSPACE) {
        g_raw_len--;
        g_raw[g_raw_len] = '\0';
        rebuild_candidates();
        return 1;
    }
    if (key == INPUT_CODE_ESCAPE || key == PXE_KEY_ESC) {
        ime_clear_composition();
        return 1;
    }
    if (key == '\n' || key == '\r' || key == INPUT_CODE_ENTER) {
        if (out_commit && out_cap > 0) {
            size_t n = (size_t)g_raw_len;
            if (n >= out_cap) n = out_cap - 1;
            memcpy(out_commit, g_raw, n);
            out_commit[n] = '\0';
        }
        ime_clear_composition();
        return 1;
    }
    if (key == ' ') {
        int abs_idx = g_cand_page * IME_PAGE_SIZE + g_cand_sel;
        if (abs_idx >= g_cand_count) abs_idx = 0;
        commit_candidate_idx(abs_idx, out_commit, out_cap);
        return 1;
    }
    if (key >= '1' && key <= '7') {
        int slot = key - '1';
        int abs_idx = g_cand_page * IME_PAGE_SIZE + slot;
        if (abs_idx < g_cand_count) {
            commit_candidate_idx(abs_idx, out_commit, out_cap);
        }
        return 1;
    }

    int total_pages = (g_cand_count + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;

    int page_start = g_cand_page * IME_PAGE_SIZE;
    int on_page = g_cand_count - page_start;
    if (on_page > IME_PAGE_SIZE) on_page = IME_PAGE_SIZE;

    if (key == '-' || key == '[' || key == ',' || key == INPUT_CODE_PAGE_UP) {
        if (g_cand_page > 0) {
            g_cand_page--;
            g_cand_sel = 0;
        }
        return 1;
    }
    if (key == '=' || key == ']' || key == '.' || key == INPUT_CODE_PAGE_DOWN) {
        if (g_cand_page + 1 < total_pages) {
            g_cand_page++;
            g_cand_sel = 0;
        }
        return 1;
    }
    if (key == INPUT_CODE_LEFT || key == INPUT_CODE_UP) {
        if (g_cand_sel > 0) {
            g_cand_sel--;
        } else if (g_cand_page > 0) {
            g_cand_page--;
            g_cand_sel = IME_PAGE_SIZE - 1;
        }
        return 1;
    }
    if (key == INPUT_CODE_RIGHT || key == INPUT_CODE_DOWN) {
        if (g_cand_sel + 1 < on_page) {
            g_cand_sel++;
        } else if (g_cand_page + 1 < total_pages) {
            g_cand_page++;
            g_cand_sel = 0;
        }
        return 1;
    }

    /* Any other symbol commits selection + appends punctuation/char */
    int abs_idx = g_cand_page * IME_PAGE_SIZE + g_cand_sel;
    if (abs_idx >= g_cand_count) abs_idx = 0;
    commit_candidate_idx(abs_idx, out_commit, out_cap);
    char extra[8] = {0};
    if (!map_zh_punct(key, extra, sizeof(extra))) {
        if (key >= 32 && key <= 126) {
            extra[0] = (char)key;
            extra[1] = '\0';
        }
    }
    if (extra[0] && out_commit && out_cap > 0) {
        size_t cur = strlen(out_commit);
        size_t elen = strlen(extra);
        if (cur + elen < out_cap) {
            memcpy(out_commit + cur, extra, elen + 1);
        }
    }
    return 1;
}

int ime_handle_mouse(int mx, int my, int mb_press, char* out_commit, size_t out_cap) {
    if (out_commit && out_cap > 0) out_commit[0] = '\0';
    if (!g_ime_enabled || g_raw_len == 0 || g_popup_w <= 0) return 0;
    if (!mb_press) return 0;
    if (mx < g_popup_x || mx >= g_popup_x + g_popup_w ||
        my < g_popup_y || my >= g_popup_y + g_popup_h) {
        return 0;
    }
    /* Check bottom row candidate hitboxes */
    if (my >= g_popup_y + 22) {
        int page_start = g_cand_page * IME_PAGE_SIZE;
        int on_page = g_cand_count - page_start;
        if (on_page > IME_PAGE_SIZE) on_page = IME_PAGE_SIZE;
        for (int i = 0; i < on_page; i++) {
            if (mx >= g_cand_hit_x0[i] && mx < g_cand_hit_x1[i]) {
                commit_candidate_idx(page_start + i, out_commit, out_cap);
                return 1;
            }
        }
    } else {
        /* Top bar page buttons on right side */
        int total_pages = (g_cand_count + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
        if (mx >= g_popup_x + g_popup_w - 44 && mx < g_popup_x + g_popup_w - 22) {
            if (g_cand_page > 0) { g_cand_page--; g_cand_sel = 0; }
        } else if (mx >= g_popup_x + g_popup_w - 22) {
            if (g_cand_page + 1 < total_pages) { g_cand_page++; g_cand_sel = 0; }
        }
    }
    return 1;
}

static int append_uint_str(char* dst, int val) {
    char tmp[12];
    int t = 0;
    if (val <= 0) {
        dst[0] = '0';
        return 1;
    }
    while (val > 0 && t < 11) {
        tmp[t++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int i = 0; i < t; i++) {
        dst[i] = tmp[t - 1 - i];
    }
    return t;
}

void ime_draw_popup(unsigned char* v, int anchor_x, int anchor_y) {
    if (!v || !g_ime_enabled || g_raw_len == 0) {
        g_popup_w = 0;
        g_popup_h = 0;
        return;
    }

    for (int i = 0; i < IME_PAGE_SIZE; i++) {
        g_cand_hit_x0[i] = 0;
        g_cand_hit_x1[i] = 0;
    }

    int page_start = g_cand_page * IME_PAGE_SIZE;
    int on_page = g_cand_count - page_start;
    if (on_page > IME_PAGE_SIZE) on_page = IME_PAGE_SIZE;
    if (on_page < 0) on_page = 0;

    /* Compute required popup width */
    char labels[IME_PAGE_SIZE][IME_MAX_CAND_BYTES + 4];
    int row_w = 14;
    for (int i = 0; i < on_page; i++) {
        const ime_cand_t* c = &g_cands[page_start + i];
        labels[i][0] = (char)('1' + i);
        labels[i][1] = '.';
        size_t cl = strlen(c->text);
        if (cl > sizeof(labels[i]) - 3) cl = sizeof(labels[i]) - 3;
        memcpy(labels[i] + 2, c->text, cl);
        labels[i][2 + cl] = '\0';
        row_w += font_text_width(labels[i]) + 10;
    }
    int top_w = font_text_width(g_raw) + 84;
    int pw = (row_w > top_w) ? row_w : top_w;
    if (pw < 230) pw = 230;
    if (pw > screen_w - 16) pw = screen_w - 16;
    int ph = 46;

    int px = anchor_x;
    int py = anchor_y;
    if (px + pw > screen_w - 4) px = screen_w - 4 - pw;
    if (px < 4) px = 4;
    if (py + ph > screen_h - 4) py = screen_h - 4 - ph;
    if (py < 22) py = 22;

    g_popup_x = px;
    g_popup_y = py;
    g_popup_w = pw;
    g_popup_h = ph;

    /* Drop shadow + beveled white card */
    for (int y = py + 2; y < py + ph + 2; y++) {
        for (int x = px + 2; x < px + pw + 2; x++) PUT_PIXEL(v, x, y, 0);
    }
    for (int y = py; y < py + ph; y++) {
        for (int x = px; x < px + pw; x++) {
            unsigned char col = (x == px || x == px + pw - 1 || y == py || y == py + ph - 1) ? 0 : 15;
            PUT_PIXEL(v, x, y, col);
        }
    }
    for (int x = px + 1; x < px + pw - 1; x++) {
        PUT_PIXEL(v, x, py + 22, 8);
    }

    /* Top bar: pinyin raw string + page controls */
    char raw_disp[48];
    utf8_clip_by_pixels(g_raw, pw - 68, 0, raw_disp, sizeof(raw_disp));
    font_draw_text(v, px + 8, py + 3, raw_disp, 1);
    int cur_x = px + 8 + font_text_width(raw_disp);
    for (int y = py + 4; y < py + 18; y++) PUT_PIXEL(v, cur_x + 1, y, 1);

    int total_pages = (g_cand_count + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;
    char pind[24];
    int pi = 0;
    pind[pi++] = '<';
    pind[pi++] = ' ';
    pi += append_uint_str(pind + pi, g_cand_page + 1);
    pind[pi++] = '/';
    pi += append_uint_str(pind + pi, total_pages);
    pind[pi++] = ' ';
    pind[pi++] = '>';
    pind[pi] = '\0';
    int piw = font_text_width_small(pind);
    font_draw_text_small(v, px + pw - piw - 8, py + 7, pind, 8);

    /* Candidate strip */
    int cx = px + 6;
    for (int i = 0; i < on_page; i++) {
        int lw = font_text_width(labels[i]);
        if (cx + lw + 6 > px + pw - 4) break;
        g_cand_hit_x0[i] = cx - 2;
        g_cand_hit_x1[i] = cx + lw + 4;
        if (i == g_cand_sel) {
            for (int y = py + 24; y < py + 43; y++) {
                for (int x = cx - 2; x < cx + lw + 2; x++) PUT_PIXEL(v, x, y, 1);
            }
            font_draw_text(v, cx, py + 25, labels[i], 15);
        } else {
            font_draw_text(v, cx, py + 25, labels[i], 0);
        }
        cx += lw + 10;
    }
}
