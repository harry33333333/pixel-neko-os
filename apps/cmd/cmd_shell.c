#include "cmd.h"

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char upper_ch(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

void shell_set_env(cmd_state_t* st, const char* name, const char* val) {
    if (!st || !name || !name[0]) return;
    for (int i = 0; i < st->env_count; i++) {
        if (strcasecmp(st->envs[i].name, name) == 0) {
            snprintf(st->envs[i].val, sizeof(st->envs[i].val), "%s", val ? val : "");
            return;
        }
    }
    if (st->env_count < CMD_MAX_ENV) {
        snprintf(st->envs[st->env_count].name, sizeof(st->envs[0].name), "%s", name);
        snprintf(st->envs[st->env_count].val, sizeof(st->envs[0].val), "%s", val ? val : "");
        st->env_count++;
    }
}

const char* shell_get_env(cmd_state_t* st, const char* name) {
    if (!st || !name) return NULL;
    if (strcasecmp(name, "CD") == 0) return st->cwd;
    for (int i = 0; i < st->env_count; i++) {
        if (strcasecmp(st->envs[i].name, name) == 0) return st->envs[i].val;
    }
    return NULL;
}

void shell_sync_cwd_env(cmd_state_t* st) {
    char err_s[16];
    snprintf(err_s, sizeof(err_s), "%d", st->last_errorlevel);
    shell_set_env(st, "ERRORLEVEL", err_s);
    shell_set_env(st, "CD", st->cwd);
}

void shell_init_env(cmd_state_t* st) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);

    char sysd = (info.boot_mode == 2 && info.system_drive)
                    ? info.system_drive
                    : (info.cdrom_drive ? info.cdrom_drive
                                        : (info.system_drive ? info.system_drive : 'C'));
    if (sysd < 'C' || sysd > 'Z') sysd = 'C';

    char drv_root[8], desk[32], pathv[128];
    snprintf(drv_root, sizeof(drv_root), "%c:", sysd);
    snprintf(desk, sizeof(desk), "%c:\\Desktop", sysd);
    snprintf(pathv, sizeof(pathv), "%c:\\APPS;%c:\\GAME;%c:\\", sysd, sysd, sysd);

    snprintf(st->cwd, sizeof(st->cwd), "%c:\\", sysd);
    shell_set_env(st, "SYSDRIVE", drv_root);
    shell_set_env(st, "DESKTOP", desk);
    shell_set_env(st, "PATH", pathv);
    shell_set_env(st, "LANG", st->zh ? "ZH" : "EN");
    shell_set_env(st, "ERRORLEVEL", "0");
    shell_set_env(st, "PROMPT", "$P$G");
}

void shell_expand_vars(cmd_state_t* st, const char* in, char* out, size_t cap) {
    if (!in || !out || cap == 0) return;
    size_t pos = 0;
    for (size_t i = 0; in[i] && pos + 1 < cap; ) {
        if (in[i] == '%') {
            if (in[i + 1] == '%') {
                out[pos++] = '%';
                i += 2;
                continue;
            }
            if (in[i + 1] >= '0' && in[i + 1] <= '9') {
                int idx = in[i + 1] - '0';
                if (idx < st->pat_arg_count) {
                    const char* a = st->pat_args[idx];
                    while (*a && pos + 1 < cap) out[pos++] = *a++;
                }
                i += 2;
                continue;
            }
            const char* end = strchr(in + i + 1, '%');
            if (end && end > in + i + 1 && (size_t)(end - (in + i + 1)) < 48u) {
                char key[48];
                size_t kl = (size_t)(end - (in + i + 1));
                memcpy(key, in + i + 1, kl);
                key[kl] = '\0';
                const char* val = shell_get_env(st, key);
                if (val) {
                    while (*val && pos + 1 < cap) out[pos++] = *val++;
                }
                i = (size_t)(end - in) + 1u;
                continue;
            }
        }
        out[pos++] = in[i++];
    }
    out[pos] = '\0';
}

int shell_resolve_path(cmd_state_t* st, const char* spec, char* out, size_t cap) {
    if (!st || !out || cap < 4) return -1;
    if (!spec || !spec[0]) {
        snprintf(out, cap, "%s", st->cwd);
        return 0;
    }

    /* Keep \PROC paths intact */
    if ((spec[0] == '\\' || spec[0] == '/') &&
        (lower_ch(spec[1]) == 'p' && lower_ch(spec[2]) == 'r' &&
         lower_ch(spec[3]) == 'o' && lower_ch(spec[4]) == 'c')) {
        snprintf(out, cap, "%s", spec);
        for (int i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
        return 0;
    }

    char raw[256];
    char norm[256];
    for (int i = 0; i < 255 && spec[i]; i++) {
        norm[i] = (spec[i] == '/') ? '\\' : spec[i];
        norm[i + 1] = '\0';
    }

    char drv = st->cwd[0] ? upper_ch(st->cwd[0]) : 'C';
    const char* rel = norm;
    if (((norm[0] >= 'A' && norm[0] <= 'Z') || (norm[0] >= 'a' && norm[0] <= 'z')) &&
        norm[1] == ':') {
        drv = upper_ch(norm[0]);
        rel = norm + 2;
    }

    if (rel[0] == '\\') {
        snprintf(raw, sizeof(raw), "%c:%s", drv, rel);
    } else if (drv != upper_ch(st->cwd[0])) {
        snprintf(raw, sizeof(raw), "%c:\\%s", drv, rel);
    } else {
        size_t cl = strlen(st->cwd);
        if (cl > 0 && st->cwd[cl - 1] == '\\') snprintf(raw, sizeof(raw), "%s%s", st->cwd, rel);
        else snprintf(raw, sizeof(raw), "%s\\%s", st->cwd, rel);
    }

    /* Normalize . and .. components */
    char* parts[32];
    int pcnt = 0;
    char work[256];
    snprintf(work, sizeof(work), "%s", raw + 2);
    char* cur = work;
    while (*cur) {
        while (*cur == '\\') cur++;
        if (!*cur) break;
        char* seg = cur;
        while (*cur && *cur != '\\') cur++;
        if (*cur == '\\') *cur++ = '\0';
        if (strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) {
            if (pcnt > 0) pcnt--;
        } else if (pcnt < 32) {
            parts[pcnt++] = seg;
        }
    }

    size_t pos = 0;
    out[pos++] = drv;
    out[pos++] = ':';
    if (pcnt == 0) {
        out[pos++] = '\\';
    } else {
        for (int i = 0; i < pcnt; i++) {
            out[pos++] = '\\';
            size_t sl = strlen(parts[i]);
            if (pos + sl + 1 >= cap) break;
            memcpy(out + pos, parts[i], sl);
            pos += sl;
        }
    }
    out[pos] = '\0';
    return 0;
}

static int parse_args(const char* cmd, char* buf, size_t buf_cap,
                      char** argv, int max_args, const char** raw_tail) {
    int argc = 0;
    size_t pos = 0;
    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (raw_tail) *raw_tail = p;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc == 1 && raw_tail) *raw_tail = p;

        argv[argc++] = buf + pos;
        int in_quote = 0;
        while (*p) {
            if (!in_quote && (*p == ' ' || *p == '\t')) break;
            if (*p == '"') {
                in_quote = !in_quote;
                p++;
                continue;
            }
            if (pos + 1 < buf_cap) buf[pos++] = *p;
            p++;
        }
        if (pos < buf_cap) buf[pos++] = '\0';
    }
    return argc;
}

static int split_pipeline(char* line, char** stages, int max_stages) {
    int count = 0;
    int in_q = 0;
    char* start = line;
    for (char* p = line; *p; p++) {
        if (*p == '"') in_q = !in_q;
        else if (!in_q && *p == '|') {
            *p = '\0';
            if (count < max_stages) stages[count++] = start;
            start = p + 1;
        }
    }
    if (count < max_stages) stages[count++] = start;
    return count;
}

static void strip_redirect(char* stage, char* redir_path, size_t rcap, int* redir_mode) {
    *redir_mode = 0;
    redir_path[0] = '\0';
    int in_q = 0;
    for (char* p = stage; *p; p++) {
        if (*p == '"') in_q = !in_q;
        else if (!in_q && *p == '>') {
            *p = '\0';
            p++;
            if (*p == '>') {
                *redir_mode = 2;
                p++;
            } else {
                *redir_mode = 1;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                size_t ri = 0;
                while (*p && *p != '"' && ri + 1 < rcap) redir_path[ri++] = *p++;
                redir_path[ri] = '\0';
            } else {
                size_t ri = 0;
                while (*p && *p != ' ' && *p != '\t' && ri + 1 < rcap) redir_path[ri++] = *p++;
                redir_path[ri] = '\0';
            }
            return;
        }
    }
}

static int exec_external(cmd_state_t* st, int argc, char** argv) {
    const char* name = argv[0];
    char cand[256];
    pxe_dirent_t de;
    int found = 0;

    int has_slash = (strchr(name, ':') || strchr(name, '\\') || strchr(name, '/'));
    const char* dot = strrchr(name, '.');

    if (has_slash) {
        if (shell_resolve_path(st, name, cand, sizeof(cand)) == 0) {
            if (pxe_stat(cand, &de) == 0 && !de.is_dir) found = 1;
            else if (!dot) {
                char alt[256];
                snprintf(alt, sizeof(alt), "%s.pxe", cand);
                if (pxe_stat(alt, &de) == 0 && !de.is_dir) {
                    snprintf(cand, sizeof(cand), "%s", alt);
                    found = 1;
                } else {
                    snprintf(alt, sizeof(alt), "%s.pat", cand);
                    if (pxe_stat(alt, &de) == 0 && !de.is_dir) {
                        snprintf(cand, sizeof(cand), "%s", alt);
                        found = 1;
                    }
                }
            }
        }
    } else {
        static const char* k_exts[] = { "", ".pxe", ".pat", ".pnk" };
        char dirs[12][256];
        int ndirs = 0;
        snprintf(dirs[ndirs++], 256, "%s", st->cwd);
        const char* pv = shell_get_env(st, "PATH");
        if (pv) {
            char copy[256];
            snprintf(copy, sizeof(copy), "%s", pv);
            char* cur = copy;
            while (*cur && ndirs < 12) {
                char* sep = strchr(cur, ';');
                if (sep) *sep = '\0';
                if (cur[0]) snprintf(dirs[ndirs++], 256, "%s", cur);
                if (!sep) break;
                cur = sep + 1;
            }
        }
        for (int d = 0; d < ndirs && !found; d++) {
            size_t dl = strlen(dirs[d]);
            const char* slash = (dl > 0 && dirs[d][dl - 1] == '\\') ? "" : "\\";
            for (int e = 0; e < 4; e++) {
                if (e == 0 && !dot) continue;
                snprintf(cand, sizeof(cand), "%s%s%s%s", dirs[d], slash, name, k_exts[e]);
                if (pxe_stat(cand, &de) == 0 && !de.is_dir) {
                    found = 1;
                    break;
                }
            }
        }
    }

    if (!found) {
        if (cmd_resolve_app_path(st, name, cand, sizeof(cand)) == 0) found = 1;
    }

    if (!found) {
        term_printf(st, CMD_TR(st, "'%s' 不是内部或外部命令，也不是可运行的程序。\n",
                               "'%s' is not recognized as an internal or external command.\n"),
                    name);
        st->last_errorlevel = 9009;
        return 9009;
    }

    const char* cdot = strrchr(cand, '.');
    if (cdot && strcasecmp(cdot, ".pat") == 0) {
        return shell_run_pat_file(st, cand, argc, (const char**)argv);
    }
    if (cdot && strcasecmp(cdot, ".pnk") == 0) {
        return cmd_open_with_assoc(st, cand);
    }
    if (cdot && strcasecmp(cdot, ".pxe") == 0) {
        const char* spawn_argv[16];
        int sa = (argc > 15) ? 15 : argc;
        spawn_argv[0] = cand;
        for (int i = 1; i < sa; i++) spawn_argv[i] = argv[i];
        int pid = pxe_spawn(cand, sa, spawn_argv);
        if (pid < 0) {
            term_printf(st, CMD_TR(st, "启动程序失败: %s\n", "Failed to launch: %s\n"), cand);
            st->last_errorlevel = 1;
            return 1;
        }
        st->last_errorlevel = 0;
        return 0;
    }

    return cmd_open_with_assoc(st, cand);
}

static int exec_single_stage(cmd_state_t* st, const char* stage_str) {
    char work[CMD_MAX_LINE];
    snprintf(work, sizeof(work), "%s", stage_str);

    /* Expand wildcards or keep raw for built-ins */
    char parse_buf[1024];
    char* argv[32];
    const char* raw_tail = "";
    int argc = parse_args(work, parse_buf, sizeof(parse_buf), argv, 32, &raw_tail);
    if (argc == 0) return 0;

    /* Handle drive switch e.g. "D:" */
    if (argc == 1 && ((argv[0][0] >= 'A' && argv[0][0] <= 'Z') ||
                      (argv[0][0] >= 'a' && argv[0][0] <= 'z')) &&
        argv[0][1] == ':' && argv[0][2] == '\0') {
        char target[8];
        snprintf(target, sizeof(target), "%c:\\", upper_ch(argv[0][0]));
        pxe_drive_info_t drvs[24];
        int dn = pxe_get_drives(drvs, 24);
        int ok = 0;
        for (int i = 0; i < dn; i++) {
            if (drvs[i].present && drvs[i].mounted &&
                upper_ch(drvs[i].letter) == upper_ch(argv[0][0])) {
                ok = 1;
                break;
            }
        }
        if (ok) {
            snprintf(st->cwd, sizeof(st->cwd), "%s", target);
            shell_sync_cwd_env(st);
            st->last_errorlevel = 0;
        } else {
            term_printf(st, CMD_TR(st, "系统找不到指定的驱动器: %c:\n",
                                   "The system cannot find the drive specified: %c:\n"),
                        upper_ch(argv[0][0]));
            st->last_errorlevel = 1;
        }
        return st->last_errorlevel;
    }

    int rc = cmd_dispatch_builtin(st, argc, argv, raw_tail);
    if (rc == -999) {
        return exec_external(st, argc, argv);
    }
    st->last_errorlevel = rc;
    return rc;
}

static int write_redirect_output(cmd_state_t* st, const char* path_spec, int mode,
                                 const char* data, size_t len) {
    char dst[256];
    if (shell_resolve_path(st, path_spec, dst, sizeof(dst)) != 0) return -1;

    if (mode == 2) {
        pxe_dirent_t de;
        if (pxe_stat(dst, &de) == 0 && !de.is_dir && de.size > 0 && de.size < 262144u) {
            size_t old_sz = (size_t)de.size;
            char* merged = (char*)malloc(old_sz + len + 1);
            if (!merged) return -1;
            int r = pxe_read_file(dst, merged, old_sz);
            size_t base = (r > 0) ? (size_t)r : 0u;
            if (len > 0) memcpy(merged + base, data, len);
            int w = pxe_write_file(dst, merged, base + len);
            free(merged);
            return (w >= 0) ? 0 : -1;
        }
    }
    return (pxe_write_file(dst, data ? data : "", len) >= 0) ? 0 : -1;
}

int shell_exec_line(cmd_state_t* st, const char* raw_line) {
    if (!st || !raw_line) return 0;
    char expanded[CMD_MAX_LINE];
    shell_expand_vars(st, raw_line, expanded, sizeof(expanded));

    char* p = expanded;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '@') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    if (p[0] == ':') {
        if (p[1] == ':') return 0; /* Comment :: */
        return 0; /* Label */
    }

    char* stages[4];
    int nstages = split_pipeline(p, stages, 4);
    char* pipe_buf = NULL;
    size_t pipe_len = 0;
    int last_rc = 0;

    for (int s = 0; s < nstages; s++) {
        char redir_path[256];
        int redir_mode = 0;
        strip_redirect(stages[s], redir_path, sizeof(redir_path), &redir_mode);

        int need_capture = (s + 1 < nstages) || (redir_mode != 0);
        st->stdin_data = pipe_buf;
        st->stdin_len = pipe_len;

        if (need_capture) {
            if (st->cap_buf) { free(st->cap_buf); st->cap_buf = NULL; }
            st->cap_len = 0;
            st->cap_cap = 0;
            st->capturing = 1;
            last_rc = exec_single_stage(st, stages[s]);
            st->capturing = 0;

            if (pipe_buf) { free(pipe_buf); pipe_buf = NULL; pipe_len = 0; }

            if (redir_mode != 0) {
                if (write_redirect_output(st, redir_path, redir_mode,
                                          st->cap_buf ? st->cap_buf : "", st->cap_len) != 0) {
                    term_printf(st, CMD_TR(st, "无法写入重定向文件: %s\n",
                                           "Cannot write redirection target: %s\n"),
                                redir_path);
                    last_rc = 1;
                }
                if (st->cap_buf) { free(st->cap_buf); st->cap_buf = NULL; }
                st->cap_len = 0;
            } else {
                pipe_buf = st->cap_buf;
                pipe_len = st->cap_len;
                st->cap_buf = NULL;
                st->cap_len = 0;
                st->cap_cap = 0;
            }
        } else {
            last_rc = exec_single_stage(st, stages[s]);
            if (pipe_buf) { free(pipe_buf); pipe_buf = NULL; pipe_len = 0; }
        }
        st->stdin_data = NULL;
        st->stdin_len = 0;
        if (st->interrupted || !st->running) break;
    }

    if (pipe_buf) free(pipe_buf);
    shell_sync_cwd_env(st);
    return last_rc;
}

static int pat_match_var_line(cmd_state_t* st, char* line) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '@') p++;
    while (*p == ' ' || *p == '\t') p++;

    /* IF [NOT] EXIST <file> <cmd> | IF [NOT] "a"=="b" <cmd> */
    if ((lower_ch(p[0]) == 'i' && lower_ch(p[1]) == 'f') &&
        (p[2] == ' ' || p[2] == '\t')) {
        char exp[CMD_MAX_LINE];
        shell_expand_vars(st, p + 3, exp, sizeof(exp));
        char* q = exp;
        while (*q == ' ' || *q == '\t') q++;
        int negate = 0;
        if (lower_ch(q[0]) == 'n' && lower_ch(q[1]) == 'o' &&
            lower_ch(q[2]) == 't' && (q[3] == ' ' || q[3] == '\t')) {
            negate = 1;
            q += 3;
            while (*q == ' ' || *q == '\t') q++;
        }
        int cond = 0;
        if (lower_ch(q[0]) == 'e' && lower_ch(q[1]) == 'x' &&
            lower_ch(q[2]) == 'i' && lower_ch(q[3]) == 's' &&
            lower_ch(q[4]) == 't' && (q[5] == ' ' || q[5] == '\t')) {
            q += 5;
            while (*q == ' ' || *q == '\t') q++;
            char fspec[256];
            size_t fi = 0;
            if (*q == '"') {
                q++;
                while (*q && *q != '"' && fi + 1 < sizeof(fspec)) fspec[fi++] = *q++;
                if (*q == '"') q++;
            } else {
                while (*q && *q != ' ' && *q != '\t' && fi + 1 < sizeof(fspec)) fspec[fi++] = *q++;
            }
            fspec[fi] = '\0';
            char rpath[256];
            pxe_dirent_t de;
            if (shell_resolve_path(st, fspec, rpath, sizeof(rpath)) == 0 &&
                pxe_stat(rpath, &de) == 0) {
                cond = 1;
            }
        } else {
            char lhs[128], rhs[128];
            char* eq = strchr(q, '=');
            if (eq && eq[1] == '=') {
                size_t ll = (size_t)(eq - q);
                if (ll >= sizeof(lhs)) ll = sizeof(lhs) - 1;
                memcpy(lhs, q, ll);
                lhs[ll] = '\0';
                while (ll > 0 && (lhs[ll - 1] == ' ' || lhs[ll - 1] == '\t')) lhs[--ll] = '\0';
                q = eq + 2;
                while (*q == ' ' || *q == '\t') q++;
                size_t ri = 0;
                if (*q == '"') {
                    rhs[ri++] = *q++;
                    while (*q && *q != '"' && ri + 2 < sizeof(rhs)) rhs[ri++] = *q++;
                    if (*q == '"') rhs[ri++] = *q++;
                } else {
                    while (*q && *q != ' ' && *q != '\t' && ri + 1 < sizeof(rhs)) rhs[ri++] = *q++;
                }
                rhs[ri] = '\0';
                cond = (strcmp(lhs, rhs) == 0);
            }
        }
        if (negate) cond = !cond;
        while (*q == ' ' || *q == '\t') q++;
        if (cond && *q) return shell_exec_line(st, q);
        return 0;
    }

    /* FOR %%f IN (pattern) DO cmd */
    if (lower_ch(p[0]) == 'f' && lower_ch(p[1]) == 'o' &&
        lower_ch(p[2]) == 'r' && (p[3] == ' ' || p[3] == '\t')) {
        char exp[CMD_MAX_LINE];
        shell_expand_vars(st, p + 4, exp, sizeof(exp));
        char* q = exp;
        while (*q == ' ' || *q == '\t') q++;
        char var_ch = 0;
        while (*q == '%') q++;
        if (*q) var_ch = *q++;
        while (*q && !(lower_ch(q[0]) == 'i' && lower_ch(q[1]) == 'n' && (q[2] == ' ' || q[2] == '('))) q++;
        if (q[0]) q += 2;
        char* lp = strchr(q, '(');
        char* rp = lp ? strchr(lp + 1, ')') : NULL;
        if (!lp || !rp || !var_ch) return 1;
        *rp = '\0';
        char* pat = lp + 1;
        char* dptr = rp + 1;
        while (*dptr == ' ' || *dptr == '\t') dptr++;
        if (lower_ch(dptr[0]) == 'd' && lower_ch(dptr[1]) == 'o' && (dptr[2] == ' ' || dptr[2] == '\t')) {
            dptr += 3;
            while (*dptr == ' ' || *dptr == '\t') dptr++;
        } else {
            return 1;
        }
        pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
        if (!ents) return 1;
        int n = pxe_listdir(st->cwd, ents, 256);
        char tok[128];
        char* tp = pat;
        while (*tp) {
            while (*tp == ' ' || *tp == '\t' || *tp == ',') tp++;
            if (!*tp) break;
            size_t ti = 0;
            while (*tp && *tp != ' ' && *tp != '\t' && *tp != ',' && ti + 1 < sizeof(tok)) tok[ti++] = *tp++;
            tok[ti] = '\0';
            if (strchr(tok, '*') || strchr(tok, '?')) {
                for (int i = 0; i < n; i++) {
                    if (ents[i].is_dir) continue;
                    if (cmd_wildcard_match(tok, ents[i].name)) {
                        char run_l[CMD_MAX_LINE];
                        size_t ri = 0;
                        for (int k = 0; dptr[k] && ri + 1 < sizeof(run_l); k++) {
                            if (dptr[k] == '%' && (dptr[k + 1] == var_ch ||
                                (dptr[k + 1] == '%' && dptr[k + 2] == var_ch))) {
                                int skip = (dptr[k + 1] == '%') ? 2 : 1;
                                size_t nl = strlen(ents[i].name);
                                if (ri + nl < sizeof(run_l)) {
                                    memcpy(run_l + ri, ents[i].name, nl);
                                    ri += nl;
                                }
                                k += skip;
                            } else {
                                run_l[ri++] = dptr[k];
                            }
                        }
                        run_l[ri] = '\0';
                        (void)shell_exec_line(st, run_l);
                    }
                }
            } else {
                char run_l[CMD_MAX_LINE];
                size_t ri = 0;
                for (int k = 0; dptr[k] && ri + 1 < sizeof(run_l); k++) {
                    if (dptr[k] == '%' && (dptr[k + 1] == var_ch ||
                        (dptr[k + 1] == '%' && dptr[k + 2] == var_ch))) {
                        int skip = (dptr[k + 1] == '%') ? 2 : 1;
                        size_t nl = strlen(tok);
                        if (ri + nl < sizeof(run_l)) {
                            memcpy(run_l + ri, tok, nl);
                            ri += nl;
                        }
                        k += skip;
                    } else {
                        run_l[ri++] = dptr[k];
                    }
                }
                run_l[ri] = '\0';
                (void)shell_exec_line(st, run_l);
            }
            if (st->interrupted || !st->running) break;
        }
        free(ents);
        return 0;
    }

    if (st->echo_on && line[0] != '@' && p[0] != ':' && p[0] != '\0') {
        char exp[CMD_MAX_LINE];
        shell_expand_vars(st, p, exp, sizeof(exp));
        term_printf(st, "%s>%s\n", st->cwd, exp);
    }
    return shell_exec_line(st, p);
}

int shell_run_pat_file(cmd_state_t* st, const char* path, int argc, const char** argv) {
    if (st->pat_depth >= CMD_MAX_PAT_DEPTH) {
        term_puts(st, CMD_TR(st, "批处理嵌套过深。\n", "Batch recursion limit exceeded.\n"));
        return 1;
    }
    pxe_dirent_t de;
    if (pxe_stat(path, &de) != 0 || de.is_dir || de.size > 65536u) {
        term_printf(st, CMD_TR(st, "找不到批处理文件: %s\n", "Batch file not found: %s\n"), path);
        return 1;
    }
    char* text = (char*)malloc((size_t)de.size + 1);
    if (!text) return 1;
    int rd = pxe_read_file(path, text, (size_t)de.size);
    if (rd < 0) { free(text); return 1; }
    text[rd] = '\0';

    /* Save previous %0..%9 and echo state */
    char saved_args[10][128];
    int saved_argc = st->pat_arg_count;
    int saved_echo = st->echo_on;
    memcpy(saved_args, st->pat_args, sizeof(saved_args));

    st->pat_arg_count = (argc > 10) ? 10 : argc;
    for (int i = 0; i < 10; i++) {
        if (i < argc && argv && argv[i]) snprintf(st->pat_args[i], 128, "%s", argv[i]);
        else st->pat_args[i][0] = '\0';
    }
    st->pat_depth++;

    /* Split lines */
    char* lines[256];
    int nlines = 0;
    char* cur = text;
    while (*cur && nlines < 256) {
        lines[nlines++] = cur;
        while (*cur && *cur != '\r' && *cur != '\n') cur++;
        while (*cur == '\r' || *cur == '\n') *cur++ = '\0';
    }

    int pc = 0;
    int rc = 0;
    while (pc < nlines && st->running && !st->interrupted) {
        char* ln = lines[pc];
        char* t = ln;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '@') t++;
        while (*t == ' ' || *t == '\t') t++;

        if ((lower_ch(t[0]) == 'g' && lower_ch(t[1]) == 'o' &&
             lower_ch(t[2]) == 't' && lower_ch(t[3]) == 'o') &&
            (t[4] == ' ' || t[4] == '\t')) {
            char* lbl = t + 4;
            while (*lbl == ' ' || *lbl == '\t' || *lbl == ':') lbl++;
            char lbuf[64];
            size_t li = 0;
            while (*lbl && *lbl != ' ' && *lbl != '\t' && li + 1 < sizeof(lbuf)) lbuf[li++] = *lbl++;
            lbuf[li] = '\0';
            if (strcasecmp(lbuf, "eof") == 0) break;
            int jumped = 0;
            for (int k = 0; k < nlines; k++) {
                char* s = lines[k];
                while (*s == ' ' || *s == '\t') s++;
                if (s[0] == ':' && s[1] != ':') {
                    char cand[64];
                    size_t ci = 0;
                    s++;
                    while (*s && *s != ' ' && *s != '\t' && ci + 1 < sizeof(cand)) cand[ci++] = *s++;
                    cand[ci] = '\0';
                    if (strcasecmp(cand, lbuf) == 0) {
                        pc = k + 1;
                        jumped = 1;
                        break;
                    }
                }
            }
            if (jumped) continue;
            term_printf(st, CMD_TR(st, "找不到标签: %s\n", "Label not found: %s\n"), lbuf);
            rc = 1;
            break;
        }

        if (lower_ch(t[0]) == 'c' && lower_ch(t[1]) == 'a' &&
            lower_ch(t[2]) == 'l' && lower_ch(t[3]) == 'l' &&
            (t[4] == ' ' || t[4] == '\t')) {
            t += 4;
            while (*t == ' ' || *t == '\t') t++;
            rc = shell_exec_line(st, t);
            term_flush_if_due(st, 1);
            (void)term_poll_interrupt(st);
            pc++;
            continue;
        }

        rc = pat_match_var_line(st, ln);
        term_flush_if_due(st, 1);
        (void)term_poll_interrupt(st);
        pc++;
    }

    st->pat_depth--;
    memcpy(st->pat_args, saved_args, sizeof(saved_args));
    st->pat_arg_count = saved_argc;
    st->echo_on = saved_echo;
    free(text);
    st->last_errorlevel = rc;
    return rc;
}

void shell_tab_reset(cmd_state_t* st) {
    st->tab_active = 0;
    st->tab_match_count = 0;
    st->tab_match_idx = 0;
}

static int starts_with_ci(const char* s, const char* pre) {
    while (*pre) {
        if (lower_ch(*s) != lower_ch(*pre)) return 0;
        s++; pre++;
    }
    return 1;
}

void shell_tab_complete(cmd_state_t* st) {
    if (!st) return;
    if (!st->tab_active) {
        int start = st->line_cursor;
        int in_q = 0;
        for (int i = 0; i < st->line_cursor; i++) {
            if (st->line[i] == '"') in_q = !in_q;
        }
        while (start > 0) {
            char c = st->line[start - 1];
            if (!in_q && (c == ' ' || c == '\t')) break;
            if (c == '"') { start--; break; }
            start--;
        }
        st->tab_token_start = start;
        int len = st->line_cursor - start;
        if (len < 0) len = 0;
        if (len >= (int)sizeof(st->tab_prefix)) len = (int)sizeof(st->tab_prefix) - 1;
        memcpy(st->tab_prefix, st->line + start, (size_t)len);
        st->tab_prefix[len] = '\0';

        const char* p = st->tab_prefix;
        if (*p == '"') p++;
        const char* last_sep = strrchr(p, '\\');
        const char* last_sep2 = strrchr(p, '/');
        if (last_sep2 && (!last_sep || last_sep2 > last_sep)) last_sep = last_sep2;

        char search_dir[256];
        char dir_prefix[256];
        char name_prefix[128];
        if (last_sep) {
            size_t dlen = (size_t)(last_sep - p + 1);
            if (dlen >= sizeof(dir_prefix)) dlen = sizeof(dir_prefix) - 1;
            memcpy(dir_prefix, p, dlen);
            dir_prefix[dlen] = '\0';
            snprintf(name_prefix, sizeof(name_prefix), "%s", last_sep + 1);
            if (shell_resolve_path(st, dir_prefix, search_dir, sizeof(search_dir)) != 0) return;
        } else {
            dir_prefix[0] = '\0';
            snprintf(name_prefix, sizeof(name_prefix), "%s", p);
            snprintf(search_dir, sizeof(search_dir), "%s", st->cwd);
        }

        pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 128);
        if (!ents) return;
        int n = pxe_listdir(search_dir, ents, 128);
        st->tab_match_count = 0;
        for (int i = 0; i < n && st->tab_match_count < 64; i++) {
            if (!name_prefix[0] || starts_with_ci(ents[i].name, name_prefix)) {
                char combined[256];
                snprintf(combined, sizeof(combined), "%s%s%s",
                         dir_prefix, ents[i].name, ents[i].is_dir ? "\\" : "");
                if (strchr(combined, ' ')) {
                    snprintf(st->tab_matches[st->tab_match_count++], 128, "\"%s\"", combined);
                } else {
                    snprintf(st->tab_matches[st->tab_match_count++], 128, "%s", combined);
                }
            }
        }
        free(ents);
        if (st->tab_match_count == 0) return;
        st->tab_active = 1;
        st->tab_match_idx = 0;
    } else {
        st->tab_match_idx = (st->tab_match_idx + 1) % st->tab_match_count;
    }

    const char* rep = st->tab_matches[st->tab_match_idx];
    size_t rlen = strlen(rep);
    size_t tail_len = strlen(st->line + st->line_cursor);
    if ((size_t)st->tab_token_start + rlen + tail_len < CMD_MAX_LINE) {
        memmove(st->line + st->tab_token_start + rlen,
                st->line + st->line_cursor, tail_len + 1);
        memcpy(st->line + st->tab_token_start, rep, rlen);
        st->line_cursor = st->tab_token_start + (int)rlen;
        st->line_len = (int)strlen(st->line);
    }
}

