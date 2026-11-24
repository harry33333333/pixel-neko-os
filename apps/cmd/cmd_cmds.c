#include "cmd.h"

static char lower_c(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int parse_int_str(const char* s, int* out) {
    if (!s || !*s) return -1;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int cmd_wildcard_match(const char* pat, const char* str) {
    const char* p = pat;
    const char* s = str;
    const char* star = NULL;
    const char* ss = NULL;
    while (*s) {
        if (*p == '*') { star = p++; ss = s; }
        else if (*p == '?' || lower_c(*p) == lower_c(*s)) { p++; s++; }
        else if (star) { p = star + 1; s = ++ss; }
        else return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

int cmd_resolve_app_path(cmd_state_t* st, const char* app_name, char* out, size_t cap) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);
    pxe_drive_info_t drvs[24];
    int dn = pxe_get_drives(drvs, 24);

    char drives[26];
    int cnt = 0;
    if (st->cwd[0] >= 'C' && st->cwd[0] <= 'Z') drives[cnt++] = st->cwd[0];
    if (info.boot_mode == 2 && info.system_drive) drives[cnt++] = info.system_drive;
    else if (info.cdrom_drive) drives[cnt++] = info.cdrom_drive;
    if (info.system_drive) drives[cnt++] = info.system_drive;
    if (info.cdrom_drive) drives[cnt++] = info.cdrom_drive;
    for (int i = 0; i < dn && cnt < 26; i++) {
        if (drvs[i].present && drvs[i].mounted) drives[cnt++] = drvs[i].letter;
    }

    char fn[96];
    if (strrchr(app_name, '.')) snprintf(fn, sizeof(fn), "%s", app_name);
    else snprintf(fn, sizeof(fn), "%s.pxe", app_name);

    pxe_dirent_t de;
    for (int i = 0; i < cnt; i++) {
        snprintf(out, cap, "%c:\\APPS\\%s", drives[i], fn);
        if (pxe_stat(out, &de) == 0 && !de.is_dir) return 0;
        snprintf(out, cap, "%c:\\GAME\\%s", drives[i], fn);
        if (pxe_stat(out, &de) == 0 && !de.is_dir) return 0;
    }
    return -1;
}

int cmd_open_with_assoc(cmd_state_t* st, const char* target_path) {
    pxe_dirent_t de;
    if (pxe_stat(target_path, &de) != 0) {
        term_printf(st, CMD_TR(st, "找不到: %s\n", "Not found: %s\n"), target_path);
        return 1;
    }
    if (de.is_dir) {
        char fpath[128];
        if (cmd_resolve_app_path(st, "files.pxe", fpath, sizeof(fpath)) == 0) {
            const char* argv[2] = { fpath, target_path };
            return (pxe_spawn(fpath, 2, argv) >= 0) ? 0 : 1;
        }
        return 1;
    }

    const char* dot = strrchr(target_path, '.');
    if (dot && strcasecmp(dot, ".pat") == 0) {
        const char* argv[1] = { target_path };
        return shell_run_pat_file(st, target_path, 1, argv);
    }
    if (dot && strcasecmp(dot, ".pxe") == 0) {
        const char* argv[1] = { target_path };
        return (pxe_spawn(target_path, 1, argv) >= 0) ? 0 : 1;
    }
    if (dot && strcasecmp(dot, ".pnk") == 0) {
        char buf[512];
        int n = pxe_read_file(target_path, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char* tp = strchr(buf, 't');
            for (char* q = buf; *q; q++) {
                if ((q == buf || q[-1] == '\n' || q[-1] == '\r') &&
                    strncmp(q, "target=", 7) == 0) {
                    tp = q + 7;
                    char tbuf[256];
                    size_t ti = 0;
                    while (*tp && *tp != '\r' && *tp != '\n' && ti + 1 < sizeof(tbuf)) {
                        tbuf[ti++] = *tp++;
                    }
                    tbuf[ti] = '\0';
                    if (tbuf[0] && tbuf[1] != ':') {
                        char sysd = st->cwd[0] ? st->cwd[0] : 'C';
                        char full[256];
                        snprintf(full, sizeof(full), "%c:%s", sysd, tbuf);
                        return cmd_open_with_assoc(st, full);
                    }
                    return cmd_open_with_assoc(st, tbuf);
                }
            }
        }
        return 1;
    }

    const char* app = "notepad.pxe";
    if (dot) {
        if (strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".png") == 0 ||
            strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            app = "viewer.pxe";
        } else if (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
                   strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".cda") == 0) {
            app = "player.pxe";
        }
    }
    char apath[128];
    if (cmd_resolve_app_path(st, app, apath, sizeof(apath)) == 0) {
        const char* argv[2] = { apath, target_path };
        return (pxe_spawn(apath, 2, argv) >= 0) ? 0 : 1;
    }
    return 1;
}

static void print_stream_paged(cmd_state_t* st, const char* text, size_t len) {
    int line_cnt = 0;
    int max_lines = st->vis_rows - 2;
    if (max_lines < 4) max_lines = 4;
    for (size_t i = 0; i < len; ) {
        if (text[i] == '\n') {
            term_putc_cp(st, '\n');
            i++;
            line_cnt++;
            if (line_cnt >= max_lines) {
                if (term_more_prompt(st) == -1) return;
                line_cnt = 0;
            }
            continue;
        }
        const char* p = text + i;
        uint32_t cp = utf8_decode_next(&p);
        i = (size_t)(p - text);
        if (!cp) break;
        term_putc_cp(st, cp);
    }
}

static int rmdir_recursive(cmd_state_t* st, const char* dir_path) {
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 128);
    if (!ents) return -1;
    int n = pxe_listdir(dir_path, ents, 128);
    if (n > 0) {
        size_t dl = strlen(dir_path);
        const char* sep = (dl > 0 && dir_path[dl - 1] == '\\') ? "" : "\\";
        for (int i = 0; i < n; i++) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s%s%s", dir_path, sep, ents[i].name);
            if (ents[i].is_dir) rmdir_recursive(st, sub);
            else pxe_unlink(sub);
            if (term_poll_interrupt(st)) { free(ents); return -1; }
        }
    }
    free(ents);
    return pxe_unlink(dir_path);
}

static int xcopy_recursive(cmd_state_t* st, const char* src_dir, const char* dst_dir,
                           int recursive, int* copied_cnt) {
    (void)pxe_mkdir(dst_dir);
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 128);
    if (!ents) return -1;
    int n = pxe_listdir(src_dir, ents, 128);
    if (n > 0) {
        size_t sl = strlen(src_dir), dl = strlen(dst_dir);
        const char* ssep = (sl > 0 && src_dir[sl - 1] == '\\') ? "" : "\\";
        const char* dsep = (dl > 0 && dst_dir[dl - 1] == '\\') ? "" : "\\";
        for (int i = 0; i < n; i++) {
            char sp[256], dp[256];
            snprintf(sp, sizeof(sp), "%s%s%s", src_dir, ssep, ents[i].name);
            snprintf(dp, sizeof(dp), "%s%s%s", dst_dir, dsep, ents[i].name);
            if (ents[i].is_dir) {
                if (recursive) xcopy_recursive(st, sp, dp, recursive, copied_cnt);
            } else {
                if (pxe_copy_file(sp, dp, NULL) == 0) {
                    (*copied_cnt)++;
                    term_printf(st, "%s -> %s\n", sp, dp);
                }
            }
            if (term_poll_interrupt(st)) { free(ents); return -1; }
        }
    }
    free(ents);
    return 0;
}

static void tree_walk(cmd_state_t* st, const char* dir_path, const char* prefix, int depth) {
    if (depth > 8 || term_poll_interrupt(st)) return;
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 128);
    if (!ents) return;
    int n = pxe_listdir(dir_path, ents, 128);
    if (n > 0) {
        int subdirs = 0;
        for (int i = 0; i < n; i++) if (ents[i].is_dir) subdirs++;
        int seen = 0;
        size_t dl = strlen(dir_path);
        const char* sep = (dl > 0 && dir_path[dl - 1] == '\\') ? "" : "\\";
        for (int i = 0; i < n; i++) {
            if (!ents[i].is_dir) continue;
            seen++;
            int last = (seen == subdirs);
            term_printf(st, "%s%s%s\n", prefix, last ? "\\---" : "+---", ents[i].name);
            if (term_poll_interrupt(st)) break;
            char next_dir[256], next_pre[128];
            snprintf(next_dir, sizeof(next_dir), "%s%s%s", dir_path, sep, ents[i].name);
            snprintf(next_pre, sizeof(next_pre), "%s%s", prefix, last ? "    " : "|   ");
            tree_walk(st, next_dir, next_pre, depth + 1);
            if (term_poll_interrupt(st)) break;
        }
    }
    free(ents);
}

static void cmd_dir(cmd_state_t* st, const char* arg) {
    char spec_dir[256];
    char pat[128] = "*";
    if (!arg || !arg[0]) {
        snprintf(spec_dir, sizeof(spec_dir), "%s", st->cwd);
    } else if (strchr(arg, '*') || strchr(arg, '?')) {
        const char* sep = strrchr(arg, '\\');
        if (!sep) sep = strrchr(arg, '/');
        if (sep) {
            char raw_d[256];
            size_t dl = (size_t)(sep - arg + 1);
            if (dl >= sizeof(raw_d)) dl = sizeof(raw_d) - 1;
            memcpy(raw_d, arg, dl);
            raw_d[dl] = '\0';
            shell_resolve_path(st, raw_d, spec_dir, sizeof(spec_dir));
            snprintf(pat, sizeof(pat), "%s", sep + 1);
        } else {
            snprintf(spec_dir, sizeof(spec_dir), "%s", st->cwd);
            snprintf(pat, sizeof(pat), "%s", arg);
        }
    } else {
        shell_resolve_path(st, arg, spec_dir, sizeof(spec_dir));
    }

    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
    if (!ents) return;
    int n = pxe_listdir(spec_dir, ents, 256);
    if (n < 0) {
        free(ents);
        term_printf(st, CMD_TR(st, "找不到路径: %s\n", "File Not Found: %s\n"), spec_dir);
        st->last_errorlevel = 1;
        return;
    }

    term_printf(st, CMD_TR(st, "\n %s 的目录\n\n", "\n Directory of %s\n\n"), spec_dir);
    int files = 0, dirs = 0;
    uint64_t total_bytes = 0;
    for (int i = 0; i < n; i++) {
        if (!cmd_wildcard_match(pat, ents[i].name)) continue;
        if (ents[i].is_dir) {
            dirs++;
            term_printf(st, "    <DIR>          %s\n", ents[i].name);
        } else {
            files++;
            total_bytes += ents[i].size;
            term_printf(st, "    %13llu  %s\n", (unsigned long long)ents[i].size, ents[i].name);
        }
    }
    free(ents);

    uint64_t free_bytes = 0;
    pxe_drive_info_t drvs[24];
    int dn = pxe_get_drives(drvs, 24);
    for (int i = 0; i < dn; i++) {
        if (drvs[i].present && drvs[i].letter == spec_dir[0] && drvs[i].is_free_known) {
            free_bytes = drvs[i].free_kb * 1024ULL;
        }
    }
    term_printf(st, CMD_TR(st, "    %4d 个文件 %13llu 字节\n    %4d 个目录 %13llu 可用字节\n\n",
                           "    %4d File(s) %13llu bytes\n    %4d Dir(s)  %13llu bytes free\n\n"),
                files, (unsigned long long)total_bytes, dirs, (unsigned long long)free_bytes);
}

static void print_help_all(cmd_state_t* st, const char* topic) {
    if (topic && topic[0]) {
        term_printf(st, CMD_TR(st, "命令 [%s]: 请参阅下方内建命令分类列表。\n",
                               "Command [%s]: see categorized built-ins below.\n"), topic);
    }
    term_puts(st, CMD_TR(st,
        "Pixel Neko OS 命令提示符内建命令:\n"
        "  [文件/目录]  DIR/LS, CD, MD/MKDIR, RD/RMDIR [/S], DEL/RM, COPY,\n"
        "               XCOPY [/S], MOVE, REN, TYPE/CAT, MORE, FIND, TREE,\n"
        "               START/EXPLORER\n"
        "  [系统诊断]   VER, SYSINFO, DATE, TIME, FREE/MEM, CPU, PS/TASKLIST,\n"
        "               KILL/TASKKILL, TASKMGR, LSPCI, PDV, DMESG, LANG [ZH|EN],\n"
        "               BEEP [HZ] [MS], COLOR [ATTR], CLS/CLEAR\n"
        "  [存储/卷]    DRIVES/MOUNT, VOL, LABEL, FORMAT, EJECT\n"
        "  [批处理]     ECHO, SET, REM, PAUSE, IF, GOTO, FOR, CALL (.pat),\n"
        "               HELP, EXIT, SHUTDOWN, REBOOT\n"
        "  [增强特性]   支持中英文输入与路径, | 管道, > / >> 重定向, Tab 补全, * 通配\n\n",
        "Pixel Neko OS Command Prompt Built-in Commands:\n"
        "  [Files]      DIR/LS, CD, MD/MKDIR, RD/RMDIR [/S], DEL/RM, COPY,\n"
        "               XCOPY [/S], MOVE, REN, TYPE/CAT, MORE, FIND, TREE,\n"
        "               START/EXPLORER\n"
        "  [System]     VER, SYSINFO, DATE, TIME, FREE/MEM, CPU, PS/TASKLIST,\n"
        "               KILL/TASKKILL, TASKMGR, LSPCI, PDV, DMESG, LANG [ZH|EN],\n"
        "               BEEP [HZ] [MS], COLOR [ATTR], CLS/CLEAR\n"
        "  [Storage]    DRIVES/MOUNT, VOL, LABEL, FORMAT, EJECT\n"
        "  [Batch]      ECHO, SET, REM, PAUSE, IF, GOTO, FOR, CALL (.pat),\n"
        "               HELP, EXIT, SHUTDOWN, REBOOT\n"
        "  [Features]   UTF-8 CJK IME & paths, | pipes, > / >> redirect, Tab, *\n\n"));
}

int cmd_dispatch_builtin(cmd_state_t* st, int argc, char** argv, const char* raw_tail) {
    const char* c = argv[0];

    if (strcasecmp(c, "cls") == 0 || strcasecmp(c, "clear") == 0) {
        term_clear(st);
        return 0;
    }
    if (strcasecmp(c, "exit") == 0) {
        st->running = 0;
        return 0;
    }
    if (strcasecmp(c, "rem") == 0) {
        return 0;
    }
    if (strcasecmp(c, "help") == 0 || strcmp(c, "?") == 0) {
        print_help_all(st, argc > 1 ? argv[1] : NULL);
        return 0;
    }
    if (strcasecmp(c, "echo") == 0) {
        if (argc == 1) {
            term_printf(st, "ECHO is %s\n", st->echo_on ? "ON" : "OFF");
        } else if (argc == 2 && strcasecmp(argv[1], "on") == 0) {
            st->echo_on = 1;
        } else if (argc == 2 && strcasecmp(argv[1], "off") == 0) {
            st->echo_on = 0;
        } else {
            term_puts(st, raw_tail ? raw_tail : "");
            term_putc_cp(st, '\n');
        }
        return 0;
    }
    if (strcasecmp(c, "set") == 0) {
        if (argc == 1) {
            for (int i = 0; i < st->env_count; i++) {
                term_printf(st, "%s=%s\n", st->envs[i].name, st->envs[i].val);
            }
        } else {
            const char* eq = strchr(raw_tail, '=');
            if (eq && eq > raw_tail) {
                char k[48];
                size_t kl = (size_t)(eq - raw_tail);
                if (kl >= sizeof(k)) kl = sizeof(k) - 1;
                memcpy(k, raw_tail, kl);
                k[kl] = '\0';
                shell_set_env(st, k, eq + 1);
            } else {
                const char* v = shell_get_env(st, argv[1]);
                if (v) term_printf(st, "%s=%s\n", argv[1], v);
                else {
                    term_printf(st, CMD_TR(st, "环境变量未定义: %s\n", "Environment variable %s not defined\n"), argv[1]);
                    return 1;
                }
            }
        }
        return 0;
    }
    if (strcasecmp(c, "pause") == 0) {
        term_puts(st, CMD_TR(st, "请按任意键继续...\n", "Press any key to continue...\n"));
        term_render_now(st);
        pxe_event_t ev;
        (void)term_wait_key_blocking(st, &ev);
        return 0;
    }
    if (strcasecmp(c, "color") == 0) {
        if (argc < 2) {
            term_set_attr(st, 0, 7);
        } else if (strlen(argv[1]) == 2) {
            int bg = hex_digit(argv[1][0]);
            int fg = hex_digit(argv[1][1]);
            if (bg >= 0 && fg >= 0 && bg != fg) {
                term_set_attr(st, (uint8_t)bg, (uint8_t)fg);
            } else {
                return 1;
            }
        }
        return 0;
    }
    if (strcasecmp(c, "cd") == 0 || strcasecmp(c, "chdir") == 0) {
        if (argc < 2) {
            term_printf(st, "%s\n", st->cwd);
            return 0;
        }
        char dst[256];
        pxe_dirent_t de;
        if (shell_resolve_path(st, argv[1], dst, sizeof(dst)) == 0 &&
            ((strlen(dst) == 3 && dst[1] == ':' && dst[2] == '\\') ||
             (pxe_stat(dst, &de) == 0 && de.is_dir))) {
            snprintf(st->cwd, sizeof(st->cwd), "%s", dst);
            shell_sync_cwd_env(st);
            return 0;
        }
        term_printf(st, CMD_TR(st, "系统找不到指定的路径: %s\n", "The system cannot find the path specified: %s\n"), argv[1]);
        return 1;
    }
    if (strcasecmp(c, "dir") == 0 || strcasecmp(c, "ls") == 0) {
        cmd_dir(st, argc > 1 ? argv[1] : NULL);
        return 0;
    }
    if (strcasecmp(c, "md") == 0 || strcasecmp(c, "mkdir") == 0) {
        if (argc < 2) return 1;
        char dst[256];
        shell_resolve_path(st, argv[1], dst, sizeof(dst));
        if (pxe_mkdir(dst) != 0) {
            term_printf(st, CMD_TR(st, "无法创建目录: %s\n", "Unable to create directory: %s\n"), dst);
            return 1;
        }
        pxe_notify_desktop();
        return 0;
    }
    if (strcasecmp(c, "rd") == 0 || strcasecmp(c, "rmdir") == 0) {
        if (argc < 2) return 1;
        int rec = 0;
        const char* target = NULL;
        for (int i = 1; i < argc; i++) {
            if (strcasecmp(argv[i], "/s") == 0 || strcasecmp(argv[i], "-r") == 0) rec = 1;
            else target = argv[i];
        }
        if (!target) return 1;
        char dst[256];
        shell_resolve_path(st, target, dst, sizeof(dst));
        int rc = rec ? rmdir_recursive(st, dst) : pxe_unlink(dst);
        if (rc != 0) {
            term_printf(st, CMD_TR(st, "无法删除目录: %s\n", "Unable to remove directory: %s\n"), dst);
            return 1;
        }
        pxe_notify_desktop();
        return 0;
    }
    if (strcasecmp(c, "del") == 0 || strcasecmp(c, "erase") == 0 || strcasecmp(c, "rm") == 0) {
        if (argc < 2) return 1;
        const char* pat_arg = argv[1];
        if (strchr(pat_arg, '*') || strchr(pat_arg, '?')) {
            pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
            if (!ents) return 1;
            int n = pxe_listdir(st->cwd, ents, 256);
            size_t cl = strlen(st->cwd);
            const char* sep = (cl > 0 && st->cwd[cl - 1] == '\\') ? "" : "\\";
            for (int i = 0; i < n; i++) {
                if (!ents[i].is_dir && cmd_wildcard_match(pat_arg, ents[i].name)) {
                    char full[256];
                    snprintf(full, sizeof(full), "%s%s%s", st->cwd, sep, ents[i].name);
                    (void)pxe_unlink(full);
                }
            }
            free(ents);
            pxe_notify_desktop();
            return 0;
        }
        char dst[256];
        shell_resolve_path(st, pat_arg, dst, sizeof(dst));
        if (pxe_unlink(dst) != 0) {
            term_printf(st, CMD_TR(st, "删除失败: %s\n", "Could not delete: %s\n"), dst);
            return 1;
        }
        pxe_notify_desktop();
        return 0;
    }
    if (strcasecmp(c, "copy") == 0 || strcasecmp(c, "cp") == 0) {
        if (argc < 3) return 1;
        char dst[256];
        shell_resolve_path(st, argv[2], dst, sizeof(dst));
        pxe_dirent_t d_st;
        int dst_is_dir = (pxe_stat(dst, &d_st) == 0 && d_st.is_dir);

        if (strchr(argv[1], '*') || strchr(argv[1], '?')) {
            pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
            if (!ents) return 1;
            int n = pxe_listdir(st->cwd, ents, 256);
            int cnt = 0;
            size_t cl = strlen(st->cwd), dl = strlen(dst);
            const char* ssep = (cl > 0 && st->cwd[cl - 1] == '\\') ? "" : "\\";
            const char* dsep = (dl > 0 && dst[dl - 1] == '\\') ? "" : "\\";
            for (int i = 0; i < n; i++) {
                if (!ents[i].is_dir && cmd_wildcard_match(argv[1], ents[i].name)) {
                    char sfull[256], dfull[256];
                    snprintf(sfull, sizeof(sfull), "%s%s%s", st->cwd, ssep, ents[i].name);
                    snprintf(dfull, sizeof(dfull), "%s%s%s", dst, dsep, ents[i].name);
                    if (pxe_copy_file(sfull, dfull, NULL) == 0) cnt++;
                }
            }
            free(ents);
            pxe_notify_desktop();
            term_printf(st, CMD_TR(st, "已复制 %d 个文件。\n", "        %d file(s) copied.\n"), cnt);
            return 0;
        }

        char src[256];
        shell_resolve_path(st, argv[1], src, sizeof(src));
        if (dst_is_dir) {
            const char* bn = strrchr(src, '\\');
            bn = bn ? (bn + 1) : src;
            char combined[256];
            size_t dl = strlen(dst);
            snprintf(combined, sizeof(combined), "%s%s%s", dst, (dl > 0 && dst[dl - 1] == '\\') ? "" : "\\", bn);
            snprintf(dst, sizeof(dst), "%s", combined);
        }
        if (pxe_copy_file(src, dst, NULL) != 0) {
            term_printf(st, CMD_TR(st, "复制失败。\n", "Copy failed.\n"));
            return 1;
        }
        pxe_notify_desktop();
        term_puts(st, CMD_TR(st, "已复制 1 个文件。\n", "        1 file(s) copied.\n"));
        return 0;
    }
    if (strcasecmp(c, "xcopy") == 0) {
        if (argc < 3) return 1;
        int rec = 0;
        for (int i = 1; i < argc; i++) if (strcasecmp(argv[i], "/s") == 0) rec = 1;
        char src[256], dst[256];
        shell_resolve_path(st, argv[1], src, sizeof(src));
        shell_resolve_path(st, argv[2], dst, sizeof(dst));
        int cnt = 0;
        xcopy_recursive(st, src, dst, rec, &cnt);
        pxe_notify_desktop();
        term_printf(st, CMD_TR(st, "已复制 %d 个文件。\n", "%d File(s) copied\n"), cnt);
        return 0;
    }
    if (strcasecmp(c, "move") == 0 || strcasecmp(c, "ren") == 0 || strcasecmp(c, "rename") == 0) {
        if (argc < 3) return 1;
        char src[256], dst[256];
        shell_resolve_path(st, argv[1], src, sizeof(src));
        shell_resolve_path(st, argv[2], dst, sizeof(dst));
        if (pxe_rename(src, dst) == 0) {
            pxe_notify_desktop();
            return 0;
        }
        if (strcasecmp(c, "move") == 0 && pxe_copy_file(src, dst, NULL) == 0 && pxe_unlink(src) == 0) {
            pxe_notify_desktop();
            return 0;
        }
        term_puts(st, CMD_TR(st, "移动/重命名失败。\n", "Move/Rename failed.\n"));
        return 1;
    }
    if (strcasecmp(c, "type") == 0 || strcasecmp(c, "cat") == 0) {
        if (argc < 2) return 1;
        char src[256];
        shell_resolve_path(st, argv[1], src, sizeof(src));
        pxe_dirent_t de;
        if (pxe_stat(src, &de) != 0 || de.is_dir) {
            term_printf(st, CMD_TR(st, "找不到文件: %s\n", "File not found: %s\n"), src);
            return 1;
        }
        size_t sz = (de.size > 65536u) ? 65536u : (size_t)de.size;
        char* buf = (char*)malloc(sz + 1);
        if (!buf) return 1;
        int n = pxe_read_file(src, buf, sz);
        if (n > 0) {
            buf[n] = '\0';
            term_puts(st, buf);
            if (buf[n - 1] != '\n') term_putc_cp(st, '\n');
        }
        free(buf);
        return 0;
    }
    if (strcasecmp(c, "more") == 0) {
        if (argc >= 2) {
            char src[256];
            shell_resolve_path(st, argv[1], src, sizeof(src));
            pxe_dirent_t de;
            if (pxe_stat(src, &de) != 0 || de.is_dir) return 1;
            size_t sz = (de.size > 65536u) ? 65536u : (size_t)de.size;
            char* buf = (char*)malloc(sz + 1);
            if (!buf) return 1;
            int n = pxe_read_file(src, buf, sz);
            if (n > 0) { buf[n] = '\0'; print_stream_paged(st, buf, (size_t)n); }
            free(buf);
        } else if (st->stdin_data && st->stdin_len > 0) {
            print_stream_paged(st, st->stdin_data, st->stdin_len);
        }
        return 0;
    }
    if (strcasecmp(c, "find") == 0) {
        if (argc < 2) return 1;
        const char* needle = argv[1];
        const char* data = st->stdin_data;
        size_t dlen = st->stdin_len;
        char* fbuf = NULL;
        if (argc >= 3) {
            char src[256];
            shell_resolve_path(st, argv[2], src, sizeof(src));
            pxe_dirent_t de;
            if (pxe_stat(src, &de) != 0 || de.is_dir) return 1;
            size_t sz = (de.size > 65536u) ? 65536u : (size_t)de.size;
            fbuf = (char*)malloc(sz + 1);
            if (!fbuf) return 1;
            int n = pxe_read_file(src, fbuf, sz);
            if (n > 0) { fbuf[n] = '\0'; data = fbuf; dlen = (size_t)n; }
            else dlen = 0;
        }
        int matches = 0;
        if (data && dlen > 0) {
            const char* p = data;
            while (*p) {
                const char* eol = strchr(p, '\n');
                size_t ll = eol ? (size_t)(eol - p) : strlen(p);
                char line[512];
                if (ll >= sizeof(line)) ll = sizeof(line) - 1;
                memcpy(line, p, ll);
                line[ll] = '\0';
                /* Substring check */
                size_t nl = strlen(needle);
                int hit = 0;
                for (size_t i = 0; nl > 0 && i + nl <= ll; i++) {
                    if (memcmp(line + i, needle, nl) == 0) { hit = 1; break; }
                }
                if (hit) {
                    term_printf(st, "%s\n", line);
                    matches++;
                }
                if (!eol) break;
                p = eol + 1;
            }
        }
        if (fbuf) free(fbuf);
        return matches > 0 ? 0 : 1;
    }
    if (strcasecmp(c, "tree") == 0) {
        char dir[256];
        if (argc > 1) shell_resolve_path(st, argv[1], dir, sizeof(dir));
        else snprintf(dir, sizeof(dir), "%s", st->cwd);
        term_printf(st, "%s\n", dir);
        tree_walk(st, dir, "", 0);
        return 0;
    }
    if (strcasecmp(c, "start") == 0 || strcasecmp(c, "explorer") == 0) {
        if (strcasecmp(c, "explorer") == 0 ||
            (argc >= 2 && (strcasecmp(argv[1], "file") == 0 || strcasecmp(argv[1], "files") == 0))) {
            if (!pxe_desktop_shell_ctl(0)) {
                pxe_desktop_shell_ctl(1);
                term_puts(st, CMD_TR(st, "已恢复桌面外壳。\n", "Desktop Shell restored.\n"));
                return 0;
            }
        }
        const char* t = (argc >= 2) ? argv[1] : ".";
        char resolved[256];
        if (strcasecmp(t, ".") == 0) snprintf(resolved, sizeof(resolved), "%s", st->cwd);
        else shell_resolve_path(st, t, resolved, sizeof(resolved));
        return cmd_open_with_assoc(st, resolved);
    }
    if (strcasecmp(c, "ver") == 0) {
        term_puts(st, "\nPixel Neko OS [Version Darwin 26A300b - Snow Neko x86_64]\n\n");
        return 0;
    }
    if (strcasecmp(c, "date") == 0 || strcasecmp(c, "time") == 0) {
        pxe_rtc_time_t rt;
        if (pxe_get_rtc_time(&rt) == 0) {
            term_printf(st, "%04u-%02u-%02u %02u:%02u:%02u\n",
                        rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second);
        }
        return 0;
    }
    if (strcasecmp(c, "sysinfo") == 0) {
        pxe_basic_sysinfo_t info;
        if (pxe_sys_get_basic_info(&info) == 0) {
            term_printf(st, "OS:      Pixel Neko OS (Darwin %s x86_64)\n",
                        info.os_build[0] ? info.os_build : "26A300b");
            term_printf(st, "Display: %dx%d @ %dbpp (%s)\n",
                        info.screen_w, info.screen_h, info.screen_bpp, info.gfx_backend);
            term_printf(st, "Audio:   %s (%s)\n", info.audio_backend,
                        info.has_audio_hw ? "Active" : "None");
            term_printf(st, "Lang:    %s | System Drive: %c: | Live Drive: %c:\n",
                        info.ui_lang == PXE_LANG_ZH ? "ZH" : "EN",
                        info.system_drive ? info.system_drive : '-',
                        info.cdrom_drive ? info.cdrom_drive : '-');
        }
        return 0;
    }
    if (strcasecmp(c, "free") == 0 || strcasecmp(c, "mem") == 0 ||
        strcasecmp(c, "cpu") == 0 || strcasecmp(c, "ps") == 0 ||
        strcasecmp(c, "tasklist") == 0 || strcasecmp(c, "lspci") == 0 ||
        strcasecmp(c, "pdv") == 0 || strcasecmp(c, "dmesg") == 0 ||
        strcasecmp(c, "drives") == 0 || strcasecmp(c, "mount") == 0) {
        const char* node = "\\PROC\\mem";
        if (strcasecmp(c, "cpu") == 0) node = "\\PROC\\cpu";
        else if (strcasecmp(c, "ps") == 0 || strcasecmp(c, "tasklist") == 0) node = "\\PROC\\list";
        else if (strcasecmp(c, "lspci") == 0) node = "\\PROC\\pci";
        else if (strcasecmp(c, "pdv") == 0) node = "\\PROC\\pdv";
        else if (strcasecmp(c, "dmesg") == 0) node = "\\PROC\\klog";
        else if (strcasecmp(c, "drives") == 0 || strcasecmp(c, "mount") == 0) node = "\\PROC\\mounts";

        pxe_dirent_t de;
        if (pxe_stat(node, &de) != 0) return 1;
        size_t sz = (de.size > 65536u) ? 65536u : (size_t)de.size;
        char* buf = (char*)malloc(sz + 1);
        if (!buf) return 1;
        int n = pxe_read_file(node, buf, sz);
        if (n > 0) { buf[n] = '\0'; term_puts(st, buf); }
        free(buf);
        return 0;
    }
    if (strcasecmp(c, "kill") == 0 || strcasecmp(c, "taskkill") == 0) {
        const char* target = NULL;
        int force_mode = 0; /* 0=auto, 1=pid, 2=im */
        for (int i = 1; i < argc; i++) {
            if (!argv[i] || !argv[i][0]) continue;
            if (strcasecmp(argv[i], "/f") == 0) continue;
            if (strcasecmp(argv[i], "/pid") == 0) {
                force_mode = 1;
                if (i + 1 < argc) target = argv[++i];
            } else if (strcasecmp(argv[i], "/im") == 0) {
                force_mode = 2;
                if (i + 1 < argc) target = argv[++i];
            } else if (!target) {
                target = argv[i];
            }
        }
        if (!target || !target[0]) {
            term_puts(st, CMD_TR(st,
                "用法: taskkill [/PID <进程号> | /IM <进程名>] 或 taskkill <PID|程序名>\n",
                "Usage: taskkill [/PID <pid> | /IM <name>] or taskkill <pid|name>\n"));
            return 1;
        }

        int pid = -1;
        if (force_mode != 2 && parse_int_str(target, &pid) == 0 && pid >= 0) {
            if (pid == 0) {
                term_puts(st, CMD_TR(st,
                    "错误: 拒绝终止系统关键进程 PID 0 (idle)。\n",
                    "ERROR: Refusing to terminate critical process PID 0 (idle).\n"));
                return 1;
            }
            if (pxe_kill_pid((uint32_t)pid) == 0) {
                term_printf(st, CMD_TR(st, "成功: 已终止进程 PID %d。\n",
                                       "SUCCESS: Terminated process PID %d.\n"), pid);
                return 0;
            }
            term_printf(st, CMD_TR(st,
                "错误: 找不到 PID %d 或该进程受系统保护无法终止。\n",
                "ERROR: Process PID %d not found or protected.\n"), pid);
            return 1;
        }
        if (force_mode == 1) {
            term_printf(st, CMD_TR(st, "错误: 无效的进程号 \"%s\"。\n",
                                   "ERROR: Invalid PID \"%s\".\n"), target);
            return 1;
        }

        char* pbuf = (char*)malloc(8192);
        if (!pbuf) return 1;
        int pn = pxe_read_file("\\PROC\\list", pbuf, 8191);
        if (pn <= 0) {
            free(pbuf);
            term_puts(st, CMD_TR(st, "错误: 无法读取 \\PROC\\list。\n",
                                 "ERROR: Cannot read \\PROC\\list.\n"));
            return 1;
        }
        pbuf[pn] = '\0';

        char target_stem[128];
        snprintf(target_stem, sizeof(target_stem), "%s", target);
        char* tdot = strrchr(target_stem, '.');
        if (tdot && strcasecmp(tdot, ".pxe") == 0) *tdot = '\0';

        int self_pid = pxe_getpid();
        int matched = 0;
        int killed = 0;
        int header_done = 0;
        char* cur = pbuf;
        while (*cur) {
            char* line = cur;
            while (*cur && *cur != '\r' && *cur != '\n') cur++;
            while (*cur == '\r' || *cur == '\n') *cur++ = '\0';
            if (!header_done) { header_done = 1; continue; }
            if (!line[0]) continue;

            char* lp = line;
            while (*lp == ' ') lp++;
            int row_pid = 0;
            if (*lp < '0' || *lp > '9') continue;
            while (*lp >= '0' && *lp <= '9') row_pid = row_pid * 10 + (*lp++ - '0');

            /* Extract columns: PPID RING STATE MEM_KB TICKS NAME */
            char cols[6][64];
            int nc = 0;
            while (*lp && nc < 6) {
                while (*lp == ' ' || *lp == '\t') lp++;
                if (!*lp) break;
                size_t ci = 0;
                if (nc == 5) {
                    while (*lp && ci + 1 < sizeof(cols[0])) cols[nc][ci++] = *lp++;
                } else {
                    while (*lp && *lp != ' ' && *lp != '\t' && ci + 1 < sizeof(cols[0])) {
                        cols[nc][ci++] = *lp++;
                    }
                }
                cols[nc][ci] = '\0';
                nc++;
            }
            if (nc < 6) continue;
            if (strcasecmp(cols[2], "ZOMBIE") == 0) continue;

            const char* pname = cols[5];
            char pstem[64];
            snprintf(pstem, sizeof(pstem), "%s", pname);
            char* pdot = strrchr(pstem, '.');
            if (pdot && strcasecmp(pdot, ".pxe") == 0) *pdot = '\0';

            if (strcasecmp(pname, target) == 0 || strcasecmp(pstem, target_stem) == 0) {
                matched++;
                if (row_pid == self_pid) {
                    term_printf(st, CMD_TR(st,
                        "警告: 跳过当前终端自身 (PID %d)。\n",
                        "Warning: Skipping current terminal (PID %d).\n"), row_pid);
                    continue;
                }
                if (pxe_kill_pid((uint32_t)row_pid) == 0) {
                    killed++;
                    term_printf(st, CMD_TR(st,
                        "成功: 已终止进程 \"%s\" (PID %d)。\n",
                        "SUCCESS: Terminated process \"%s\" (PID %d).\n"), pname, row_pid);
                } else {
                    term_printf(st, CMD_TR(st,
                        "错误: 无法终止关键或受保护进程 \"%s\" (PID %d)。\n",
                        "ERROR: Cannot terminate protected process \"%s\" (PID %d).\n"),
                        pname, row_pid);
                }
            }
        }
        free(pbuf);
        if (matched == 0) {
            term_printf(st, CMD_TR(st, "错误: 找不到进程 \"%s\"。\n",
                                   "ERROR: Process \"%s\" not found.\n"), target);
            return 1;
        }
        return (killed > 0) ? 0 : 1;
    }
    if (strcasecmp(c, "taskmgr") == 0) {
        char tp[128];
        if (cmd_resolve_app_path(st, "taskmgr.pxe", tp, sizeof(tp)) == 0) {
            const char* av[1] = { tp };
            return (pxe_spawn(tp, 1, av) >= 0) ? 0 : 1;
        }
        return 1;
    }
    if (strcasecmp(c, "lang") == 0) {
        if (argc < 2) {
            term_printf(st, "LANG=%s\n", st->zh ? "ZH" : "EN");
            return 0;
        }
        int z = (strcasecmp(argv[1], "zh") == 0) ? 1 : 0;
        pxe_set_language(z);
        st->zh = z;
        shell_set_env(st, "LANG", z ? "ZH" : "EN");
        pxe_win_set_title(st->win_id, z ? "命令提示符" : "Command Prompt");
        return 0;
    }
    if (strcasecmp(c, "beep") == 0) {
        int hz = 880, ms = 120;
        if (argc >= 2) (void)parse_int_str(argv[1], &hz);
        if (argc >= 3) (void)parse_int_str(argv[2], &ms);
        return (pxe_beep_tone(hz, ms) == 0) ? 0 : 1;
    }
    if (strcasecmp(c, "vol") == 0 || strcasecmp(c, "label") == 0) {
        char dlet = (argc >= 2 && argv[1][0]) ? (char)(argv[1][0] >= 'a' ? argv[1][0] - 32 : argv[1][0])
                                              : (st->cwd[0] ? st->cwd[0] : 'C');
        pxe_drive_info_t drvs[24];
        int dn = pxe_get_drives(drvs, 24);
        for (int i = 0; i < dn; i++) {
            if (drvs[i].present && drvs[i].letter == dlet) {
                term_printf(st, CMD_TR(st, " 驱动器 %c 中的卷是 %s (%s, %llu KB)\n",
                                       " Volume in drive %c is %s (%s, %llu KB)\n"),
                            dlet, drvs[i].label[0] ? drvs[i].label : "No Label",
                            drvs[i].fs_name, (unsigned long long)drvs[i].total_kb);
                return 0;
            }
        }
        return 1;
    }
    if (strcasecmp(c, "eject") == 0) {
        char dlet = (argc >= 2 && argv[1][0]) ? argv[1][0] : (st->cwd[0] ? st->cwd[0] : 'D');
        return (pxe_eject_drive(dlet) == 0) ? 0 : 1;
    }
    if (strcasecmp(c, "format") == 0) {
        term_puts(st, CMD_TR(st, "为安全起见，请在文件管理器右键驱动器图标执行格式化操作。\n",
                             "For safety, please right-click the drive in File Manager to format.\n"));
        return 0;
    }
    if (strcasecmp(c, "reboot") == 0) {
        pxe_system_reboot();
        return 0;
    }
    if (strcasecmp(c, "shutdown") == 0) {
        pxe_system_shutdown();
        return 0;
    }

    return -999; /* Not a built-in command */
}

