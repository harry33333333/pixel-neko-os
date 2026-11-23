#!/usr/bin/env python3
"""
Generate Rime-derived binary Pinyin lexicon (system/IME.PNY) and built-in core
header (include/ime_core.h) for Pixel Neko OS.

Sources (license notices recorded in THIRD_PARTY_LICENSES.md):
  - rime/rime-luna-pinyin (luna_pinyin.dict.yaml, LGPL-3.0)
  - rime/rime-essay (essay.txt, LGPL-3.0)
  - BYVoid/OpenCC (TSCharacters.txt & TSPhrases.txt, Apache-2.0)
"""

import argparse
import os
import struct
import sys

NKPY_MAGIC = 0x59504B4E  # "NKPY"
NKPY_VERSION = 1

DEFAULT_DICT_DIR = (
    r"C:\Users\29744\.gemini\antigravity\brain\b3de5e59-6e51-487c-ad14-2914dfe78eef\scratch"
)


def load_supported_cps(fnt_path):
    with open(fnt_path, "rb") as f:
        data = f.read()
    magic, ver, flags, cnt, cp_off, g16_off, g12_off, tot, res = struct.unpack(
        "<IHHIIIIII", data[:32]
    )
    if magic != 0x54464B4E:
        raise RuntimeError(f"Unexpected FONT_CJK.FNT magic: {magic:#x}")
    return set(struct.unpack(f"<{cnt}H", data[cp_off : cp_off + cnt * 2]))


def load_opencc_t2s(dict_dir, supported_cps):
    ts_phrases = {}
    ts_chars = {}

    phr_path = os.path.join(dict_dir, "TSPhrases.txt")
    if os.path.exists(phr_path):
        with open(phr_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) < 2:
                    continue
                cands = parts[1].split(" ")
                chosen = None
                for c in cands:
                    if all(ord(ch) in supported_cps for ch in c):
                        chosen = c
                        break
                if chosen:
                    ts_phrases[parts[0]] = chosen

    chr_path = os.path.join(dict_dir, "TSCharacters.txt")
    if os.path.exists(chr_path):
        with open(chr_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) < 2:
                    continue
                cands = parts[1].split(" ")
                chosen = None
                for c in cands:
                    if len(c) == 1 and ord(c) in supported_cps:
                        chosen = c
                        break
                if chosen:
                    ts_chars[parts[0]] = chosen

    return ts_phrases, ts_chars


def to_simplified(word, ts_phrases, ts_chars):
    if word in ts_phrases:
        return ts_phrases[word]
    if len(word) <= 1:
        return ts_chars.get(word, word)
    # Longest-match phrase replacement (up to 4 chars), then char-by-char
    out = []
    i = 0
    n = len(word)
    while i < n:
        matched = False
        for k in (4, 3, 2):
            if i + k <= n:
                sub = word[i : i + k]
                if sub in ts_phrases:
                    out.append(ts_phrases[sub])
                    i += k
                    matched = True
                    break
        if not matched:
            ch = word[i]
            out.append(ts_chars.get(ch, ch))
            i += 1
    return "".join(out)


def is_all_cjk_supported(word, supported_cps):
    for ch in word:
        cp = ord(ch)
        if cp < 0x4E00 or cp > 0x9FFF:
            return False
        if cp not in supported_cps:
            return False
    return len(word) > 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".")
    parser.add_argument("--dict-dir", default=DEFAULT_DICT_DIR)
    parser.add_argument("--max-phrases", type=int, default=48000)
    args = parser.parse_args()

    fnt_path = os.path.join(args.repo, "system", "FONT_CJK.FNT")
    supported_cps = load_supported_cps(fnt_path)
    ts_phrases, ts_chars = load_opencc_t2s(args.dict_dir, supported_cps)

    # 1. Load essay.txt frequencies (Trad->Simp merged)
    essay_freq = {}
    essay_path = os.path.join(args.dict_dir, "essay.txt")
    with open(essay_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            word = parts[0]
            try:
                freq = int(parts[1])
            except ValueError:
                continue
            simp = to_simplified(word, ts_phrases, ts_chars)
            if not is_all_cjk_supported(simp, supported_cps):
                continue
            prev = essay_freq.get(simp, 0)
            essay_freq[simp] = max(prev, freq)

    # 2. Load luna_pinyin.dict.yaml readings
    luna_singles = {}  # (ch, syl) -> float pct weight
    luna_phrases = {}  # (word, tuple(syls)) -> int bonus
    luna_path = os.path.join(args.dict_dir, "luna_pinyin.dict.yaml")
    with open(luna_path, encoding="utf-8") as f:
        in_body = False
        for line in f:
            s = line.strip()
            if not in_body:
                if s == "...":
                    in_body = True
                continue
            if not s or s.startswith("#"):
                continue
            parts = s.split("\t")
            if len(parts) < 2:
                continue
            word_raw, py_raw = parts[0], parts[1].strip()
            pct = 1.0
            if len(parts) >= 3:
                wcol = parts[2].strip()
                if wcol.endswith("%"):
                    try:
                        pct = max(0.01, float(wcol[:-1]) / 100.0)
                    except ValueError:
                        pct = 1.0
            syls = tuple(t.lower() for t in py_raw.split(" ") if t)
            if not syls or any((not t.isalpha() or len(t) > 7) for t in syls):
                continue

            if len(word_raw) == 1:
                # Only keep native Simplified/shared char directly, or mapped char if missing
                if is_all_cjk_supported(word_raw, supported_cps):
                    k = (word_raw, syls[0])
                    luna_singles[k] = max(luna_singles.get(k, 0.0), pct)
                simp_ch = ts_chars.get(word_raw, word_raw)
                if simp_ch != word_raw and is_all_cjk_supported(simp_ch, supported_cps):
                    # Inherit Traditional reading as fallback if not already natively recorded
                    k2 = (simp_ch, syls[0])
                    if k2 not in luna_singles:
                        luna_singles[k2] = min(pct, 0.25)
            else:
                simp_w = to_simplified(word_raw, ts_phrases, ts_chars)
                if (
                    2 <= len(simp_w) <= 6
                    and len(simp_w) == len(syls)
                    and is_all_cjk_supported(simp_w, supported_cps)
                ):
                    k = (simp_w, syls)
                    luna_phrases[k] = max(luna_phrases.get(k, 0), 1)

    # Boost essential modern high-frequency Simplified characters so ordering feels natural
    common_boost = {
        "的": ("de", 500000), "一": ("yi", 450000), "是": ("shi", 450000),
        "了": ("le", 420000), "我": ("wo", 420000), "不": ("bu", 400000),
        "在": ("zai", 400000), "人": ("ren", 380000), "有": ("you", 380000),
        "他": ("ta", 360000), "这": ("zhe", 360000), "个": ("ge", 350000),
        "上": ("shang", 340000), "们": ("men", 340000), "来": ("lai", 340000),
        "到": ("dao", 330000), "时": ("shi", 320000), "大": ("da", 320000),
        "地": ("di", 310000), "为": ("wei", 310000), "子": ("zi", 300000),
        "中": ("zhong", 320000), "你": ("ni", 320000), "说": ("shuo", 300000),
        "生": ("sheng", 290000), "国": ("guo", 290000), "年": ("nian", 290000),
        "着": ("zhe", 280000), "就": ("jiu", 280000), "那": ("na", 280000),
        "和": ("he", 280000), "要": ("yao", 280000), "她": ("ta", 270000),
        "出": ("chu", 270000), "也": ("ye", 270000), "得": ("de", 260000),
        "里": ("li", 260000), "后": ("hou", 260000), "自": ("zi", 260000),
        "以": ("yi", 260000), "会": ("hui", 260000), "家": ("jia", 250000),
        "可": ("ke", 250000), "下": ("xia", 250000), "而": ("er", 250000),
        "过": ("guo", 250000), "天": ("tian", 250000), "去": ("qu", 250000),
        "能": ("neng", 250000), "对": ("dui", 250000), "小": ("xiao", 250000),
        "多": ("duo", 240000), "然": ("ran", 240000), "于": ("yu", 240000),
        "心": ("xin", 240000), "学": ("xue", 240000), "么": ("me", 240000),
        "之": ("zhi", 240000), "都": ("dou", 240000), "好": ("hao", 280000),
        "看": ("kan", 240000), "起": ("qi", 240000), "发": ("fa", 240000),
        "当": ("dang", 240000), "没": ("mei", 240000), "成": ("cheng", 230000),
        "只": ("zhi", 230000), "如": ("ru", 230000), "事": ("shi", 230000),
        "把": ("ba", 230000), "还": ("hai", 230000), "用": ("yong", 230000),
        "第": ("di", 230000), "样": ("yang", 230000), "道": ("dao", 230000),
        "想": ("xiang", 230000), "作": ("zuo", 230000), "种": ("zhong", 230000),
        "开": ("kai", 230000), "美": ("mei", 220000), "总": ("zong", 220000),
        "从": ("cong", 220000), "无": ("wu", 220000), "情": ("qing", 220000),
        "己": ("ji", 220000), "面": ("mian", 220000), "最": ("zui", 220000),
        "女": ("nv", 220000), "但": ("dan", 220000), "现": ("xian", 220000),
        "前": ("qian", 220000), "些": ("xie", 220000), "所": ("suo", 220000),
        "同": ("tong", 220000), "日": ("ri", 220000), "手": ("shou", 220000),
        "又": ("you", 220000), "行": ("xing", 220000), "意": ("yi", 220000),
        "动": ("dong", 220000), "方": ("fang", 220000), "期": ("qi", 210000),
        "它": ("ta", 210000), "头": ("tou", 210000), "经": ("jing", 210000),
        "长": ("chang", 210000), "儿": ("er", 210000), "回": ("hui", 210000),
        "位": ("wei", 210000), "分": ("fen", 210000), "爱": ("ai", 210000),
        "老": ("lao", 210000), "因": ("yin", 210000), "很": ("hen", 210000),
        "给": ("gei", 210000), "名": ("ming", 210000), "法": ("fa", 210000),
        "间": ("jian", 210000), "斯": ("si", 200000), "知": ("zhi", 210000),
        "世": ("shi", 210000), "什": ("shen", 210000), "两": ("liang", 210000),
        "次": ("ci", 200000), "使": ("shi", 200000), "身": ("shen", 200000),
        "者": ("zhe", 200000), "被": ("bei", 200000), "高": ("gao", 200000),
        "已": ("yi", 200000), "亲": ("qin", 200000), "其": ("qi", 200000),
        "进": ("jin", 200000), "此": ("ci", 200000), "话": ("hua", 200000),
        "常": ("chang", 200000), "与": ("yu", 200000), "活": ("huo", 200000),
        "正": ("zheng", 200000), "感": ("gan", 200000), "见": ("jian", 200000),
        "明": ("ming", 200000), "问": ("wen", 200000), "力": ("li", 200000),
        "理": ("li", 200000), "尔": ("er", 200000), "点": ("dian", 200000),
        "文": ("wen", 200000), "几": ("ji", 200000), "定": ("ding", 200000),
        "本": ("ben", 200000), "公": ("gong", 200000), "特": ("te", 200000),
        "做": ("zuo", 200000), "外": ("wai", 200000), "孩": ("hai", 200000),
        "相": ("xiang", 200000), "西": ("xi", 200000), "果": ("guo", 200000),
        "走": ("zou", 200000), "将": ("jiang", 200000), "月": ("yue", 200000),
        "十": ("shi", 200000), "实": ("shi", 200000), "向": ("xiang", 200000),
        "声": ("sheng", 200000), "车": ("che", 200000), "全": ("quan", 200000),
        "信": ("xin", 200000), "重": ("zhong", 200000), "三": ("san", 200000),
        "机": ("ji", 200000), "工": ("gong", 200000), "物": ("wu", 200000),
        "气": ("qi", 200000), "每": ("mei", 200000), "并": ("bing", 200000),
        "别": ("bie", 200000), "真": ("zhen", 200000), "打": ("da", 200000),
        "太": ("tai", 200000), "新": ("xin", 200000), "比": ("bi", 200000),
        "才": ("cai", 200000), "便": ("bian", 200000), "夫": ("fu", 200000),
        "再": ("zai", 200000), "书": ("shu", 200000), "部": ("bu", 200000),
        "水": ("shui", 200000), "像": ("xiang", 200000), "眼": ("yan", 200000),
        "等": ("deng", 200000), "体": ("ti", 200000), "却": ("que", 200000),
        "加": ("jia", 200000), "电": ("dian", 200000), "主": ("zhu", 200000),
        "界": ("jie", 200000), "门": ("men", 200000), "利": ("li", 200000),
        "海": ("hai", 200000), "受": ("shou", 200000), "听": ("ting", 200000),
        "表": ("biao", 200000), "德": ("de", 200000), "少": ("shao", 200000),
        "克": ("ke", 200000), "代": ("dai", 200000), "员": ("yuan", 200000),
        "许": ("xu", 200000), "稜": ("leng", 100), "先": ("xian", 200000),
        "口": ("kou", 200000), "由": ("you", 200000), "死": ("si", 200000),
        "安": ("an", 200000), "写": ("xie", 200000), "性": ("xing", 200000),
        "马": ("ma", 200000), "光": ("guang", 200000), "白": ("bai", 200000),
        "或": ("huo", 200000), "住": ("zhu", 200000), "难": ("nan", 200000),
        "望": ("wang", 200000), "教": ("jiao", 200000), "命": ("ming", 200000),
        "花": ("hua", 200000), "结": ("jie", 200000), "乐": ("le", 200000),
        "色": ("se", 200000), "更": ("geng", 200000), "拉": ("la", 200000),
        "东": ("dong", 200000), "神": ("shen", 200000), "记": ("ji", 200000),
        "处": ("chu", 200000), "让": ("rang", 200000), "母": ("mu", 200000),
        "父": ("fu", 200000), "应": ("ying", 200000), "直": ("zhi", 200000),
        "字": ("zi", 200000), "场": ("chang", 200000), "平": ("ping", 200000),
        "报": ("bao", 200000), "友": ("you", 200000), "关": ("guan", 200000),
        "放": ("fang", 200000), "至": ("zhi", 200000), "张": ("zhang", 200000),
        "认": ("ren", 200000), "接": ("jie", 200000), "告": ("gao", 200000),
        "入": ("ru", 200000), "笑": ("xiao", 200000), "内": ("nei", 200000),
        "英": ("ying", 200000), "军": ("jun", 200000), "候": ("hou", 200000),
        "民": ("min", 200000), "岁": ("sui", 200000), "往": ("wang", 200000),
        "何": ("he", 200000), "度": ("du", 200000), "山": ("shan", 200000),
        "觉": ("jue", 200000), "路": ("lu", 200000), "带": ("dai", 200000),
        "万": ("wan", 200000), "男": ("nan", 200000), "边": ("bian", 200000),
        "风": ("feng", 200000), "解": ("jie", 200000), "叫": ("jiao", 200000),
        "任": ("ren", 200000), "金": ("jin", 200000), "快": ("kuai", 200000),
        "原": ("yuan", 200000), "吃": ("chi", 200000), "妈": ("ma", 200000),
        "变": ("bian", 200000), "通": ("tong", 200000), "师": ("shi", 200000),
        "立": ("li", 200000), "象": ("xiang", 200000), "数": ("shu", 200000),
        "四": ("si", 200000), "失": ("shi", 200000), "满": ("man", 200000),
        "战": ("zhan", 200000), "远": ("yuan", 200000), "格": ("ge", 200000),
        "士": ("shi", 200000), "音": ("yin", 200000), "轻": ("qing", 200000),
        "目": ("mu", 200000), "条": ("tiao", 200000), "呢": ("ne", 200000),
        "女": ("nv", 220000), "绿": ("lv", 180000), "旅": ("lv", 180000),
        "率": ("lv", 180000), "略": ("lve", 180000), "虐": ("nve", 180000),
        "件": ("jian", 220000), "夹": ("jia", 180000), "设": ("she", 200000),
        "置": ("zhi", 200000), "系": ("xi", 210000), "统": ("tong", 210000),
        "盘": ("pan", 180000), "删": ("shan", 180000), "编": ("bian", 180000),
        "辑": ("ji", 180000), "保": ("bao", 190000), "存": ("cun", 190000),
        "测": ("ce", 180000), "试": ("shi", 190000), "猫": ("mao", 190000),
        "树": ("shu", 190000), "朴": ("pu", 180000), "凡": ("fan", 180000),
        "吧": ("ba", 220000), "吗": ("ma", 220000), "啊": ("a", 210000),
    }

    # Build weighted single-char map & best reading per char
    char_best_syl = {}  # ch -> (weight, syl)
    single_records = []  # (syl, ch, weight)
    for (ch, syl), pct in luna_singles.items():
        base_f = essay_freq.get(ch, 10)
        w = int(base_f * pct)
        if w < 1:
            w = 1
        cb = common_boost.get(ch)
        if cb and cb[0] == syl:
            w = max(w, cb[1])
        single_records.append((syl, ch, w))
        prev = char_best_syl.get(ch)
        if prev is None or w > prev[0]:
            char_best_syl[ch] = (w, syl)

    # Deduplicate single_records cleanly
    single_map = {}
    for syl, ch, w in single_records:
        k = (syl, ch)
        single_map[k] = max(single_map.get(k, 0), w)

    # 3. Collect multi-character phrases (2..6 chars) from luna_phrases + essay_freq
    phrase_map = {}  # (tuple(syls), word) -> weight
    for (word, syls), _ in luna_phrases.items():
        freq = essay_freq.get(word, 25)
        w = max(freq, 25)
        k = (syls, word)
        phrase_map[k] = max(phrase_map.get(k, 0), w)

    for word, freq in essay_freq.items():
        if not (2 <= len(word) <= 6):
            continue
        if freq < 5:
            continue
        syls = []
        ok = True
        for ch in word:
            if ch not in char_best_syl:
                ok = False
                break
            syls.append(char_best_syl[ch][1])
        if not ok:
            continue
        ts = tuple(syls)
        # Only add if explicit luna phrase reading wasn't already recorded for this word
        if any(pword == word for (_, pword) in [] if False):
            pass
        k = (ts, word)
        if k not in phrase_map:
            phrase_map[k] = freq

    # Avoid ambiguous duplicate homographs from essay if luna_pinyin already supplied a specific reading for the same word
    words_in_luna = set(w for (w, _) in luna_phrases.keys())
    filtered_phrases = []
    for (syls, word), w in phrase_map.items():
        if word in words_in_luna and (word, syls) not in luna_phrases:
            continue
        filtered_phrases.append((syls, word, w))

    # Ensure essential UI / OS / modern Simplified phrases are strongly weighted
    essential_phrases = [
        (("xin", "jian"), "新建", 120000),
        (("xin", "jian", "wen", "jian", "jia"), "新建文件夹", 120000),
        (("wen", "jian"), "文件", 150000),
        (("wen", "jian", "jia"), "文件夹", 140000),
        (("wen", "jian", "guan", "li"), "文件管理", 110000),
        (("kong", "zhi", "mian", "ban"), "控制面板", 110000),
        (("ji", "shi", "ben"), "记事本", 110000),
        (("ji", "suan", "qi"), "计算器", 110000),
        (("bo", "fang", "qi"), "播放器", 110000),
        (("tu", "pian", "cha", "kan"), "图片查看", 100000),
        (("xi", "tong", "xin", "xi"), "系统信息", 110000),
        (("ren", "wu", "guan", "li", "qi"), "任务管理器", 110000),
        (("shu", "ru", "fa"), "输入法", 130000),
        (("zhong", "wen"), "中文", 160000),
        (("ying", "wen"), "英文", 140000),
        (("jian", "ti", "zhong", "wen"), "简体中文", 140000),
        (("cao", "zuo", "xi", "tong"), "操作系统", 130000),
        (("ni", "hao"), "你好", 180000),
        (("shi", "jie"), "世界", 150000),
        (("ni", "hao", "shi", "jie"), "你好世界", 150000),
        (("xie", "xie"), "谢谢", 140000),
        (("ping", "fan", "zhi", "lu"), "平凡之路", 90000),
        (("pu", "shu"), "朴树", 80000),
        (("you", "xi"), "游戏", 120000),
        (("dian", "nao"), "电脑", 140000),
        (("she", "zhi"), "设置", 150000),
        (("yin", "le"), "音乐", 130000),
        (("yin", "liang"), "音量", 120000),
        (("fen", "bian", "lv"), "分辨率", 110000),
        (("bi", "zhi"), "壁纸", 110000),
        (("chong", "qi"), "重启", 110000),
        (("guan", "ji"), "关机", 110000),
        (("bao", "cun"), "保存", 130000),
        (("da", "kai"), "打开", 130000),
        (("shan", "chu"), "删除", 130000),
        (("fu", "zhi"), "复制", 130000),
        (("zhan", "tie"), "粘贴", 130000),
        (("jian", "qie"), "剪切", 120000),
        (("chong", "ming", "ming"), "重命名", 120000),
        (("shu", "xing"), "属性", 120000),
        (("que", "ding"), "确定", 140000),
        (("qu", "xiao"), "取消", 140000),
        (("tui", "chu"), "退出", 130000),
        (("an", "zhuang"), "安装", 130000),
        (("ci", "pan"), "磁盘", 120000),
        (("fen", "qu"), "分区", 120000),
        (("ce", "shi"), "测试", 140000),
    ]
    existing_phrase_idx = {(s, w): i for i, (s, w, _) in enumerate(filtered_phrases)}
    for es, ew, eweight in essential_phrases:
        if not is_all_cjk_supported(ew, supported_cps):
            continue
        k = (es, ew)
        if k in existing_phrase_idx:
            i = existing_phrase_idx[k]
            _, _, oldw = filtered_phrases[i]
            if eweight > oldw:
                filtered_phrases[i] = (es, ew, eweight)
        else:
            filtered_phrases.append((es, ew, eweight))

    # Sort phrases globally by descending weight and keep top N
    filtered_phrases.sort(key=lambda x: (-x[2], len(x[0]), x[1]))
    filtered_phrases = filtered_phrases[: args.max_phrases]

    # Collect sorted syllable list (filter out non-Mandarin zhuyin artifacts by checking usable singles)
    used_syls = sorted(
        set(s for (s, _) in single_map.keys())
        | set(s for (syls, _, _) in filtered_phrases for s in syls)
    )
    syl_to_id = {s: idx for idx, s in enumerate(used_syls)}
    syl_count = len(used_syls)

    # Filter & group singles by syl_id sorted by (-weight, utf8)
    singles_by_id = [[] for _ in range(syl_count)]
    for (syl, ch), w in single_map.items():
        sid = syl_to_id[syl]
        singles_by_id[sid].append((w, ch.encode("utf-8")))
    for sid in range(syl_count):
        singles_by_id[sid].sort(key=lambda x: (-x[0], x[1]))

    # Group phrases by first_syl_id sorted by (-weight, len(syls))
    phrases_by_id = [[] for _ in range(syl_count)]
    for syls, word, w in filtered_phrases:
        sids = [syl_to_id[s] for s in syls]
        phrases_by_id[sids[0]].append((sids, word.encode("utf-8"), w))
    for sid in range(syl_count):
        phrases_by_id[sid].sort(key=lambda x: (-x[2], len(x[0]), x[1]))

    # Build pools & tables
    utf8_pool = bytearray()
    utf8_cache = {}

    def intern_utf8(b):
        off = utf8_cache.get(b)
        if off is not None:
            return off
        off = len(utf8_pool)
        utf8_pool.extend(b)
        utf8_pool.append(0)
        utf8_cache[b] = off
        return off

    syl_table = bytearray()
    for s in used_syls:
        b = s.encode("ascii")[:7]
        syl_table.extend(b + b"\x00" * (8 - len(b)))

    single_idx = [0] * (syl_count + 1)
    single_entries = bytearray()
    cur = 0
    for sid in range(syl_count):
        single_idx[sid] = cur
        for w, ubytes in singles_by_id[sid]:
            uoff = intern_utf8(ubytes)
            single_entries.extend(
                struct.pack("<IIHBB", uoff, w & 0xFFFFFFFF, sid, len(ubytes), 0)
            )
            cur += 1
    single_idx[syl_count] = cur
    single_count = cur

    phrase_idx = [0] * (syl_count + 1)
    phrase_entries = bytearray()
    syl_pool = bytearray()
    cur = 0
    syl_pool_cnt = 0
    for sid in range(syl_count):
        phrase_idx[sid] = cur
        for sids, ubytes, w in phrases_by_id[sid]:
            uoff = intern_utf8(ubytes)
            soff = syl_pool_cnt
            for x in sids:
                syl_pool.extend(struct.pack("<H", x))
                syl_pool_cnt += 1
            phrase_entries.extend(
                struct.pack(
                    "<IIIBBH",
                    soff,
                    uoff,
                    w & 0xFFFFFFFF,
                    len(sids),
                    len(ubytes),
                    sids[1],
                )
            )
            cur += 1
    phrase_idx[syl_count] = cur
    phrase_count = cur

    # Pack binary IME.PNY
    hdr_size = 48  # <IHHIIIIIIIIII (4 + 2 + 2 + 10*4 = 48)
    syl_table_off = hdr_size
    single_idx_off = syl_table_off + len(syl_table)
    single_off = single_idx_off + (syl_count + 1) * 4
    phrase_idx_off = single_off + len(single_entries)
    phrase_off = phrase_idx_off + (syl_count + 1) * 4
    syl_pool_off = phrase_off + len(phrase_entries)
    utf8_pool_off = syl_pool_off + len(syl_pool)
    total_size = utf8_pool_off + len(utf8_pool)

    hdr = struct.pack(
        "<IHHIIIIIIIIII",
        NKPY_MAGIC,
        NKPY_VERSION,
        syl_count,
        single_count,
        phrase_count,
        syl_table_off,
        single_idx_off,
        single_off,
        phrase_idx_off,
        phrase_off,
        syl_pool_off,
        utf8_pool_off,
        total_size,
    )

    out_pny = os.path.join(args.repo, "system", "IME.PNY")
    with open(out_pny, "wb") as f:
        f.write(hdr)
        f.write(syl_table)
        f.write(struct.pack(f"<{syl_count + 1}I", *single_idx))
        f.write(single_entries)
        f.write(struct.pack(f"<{syl_count + 1}I", *phrase_idx))
        f.write(phrase_entries)
        f.write(syl_pool)
        f.write(utf8_pool)

    # Emit compact fallback header include/ime_core.h
    # Top ~3 characters per syllable + core 500 phrases + sorted syllable list
    core_singles = []
    for sid in range(syl_count):
        for w, ubytes in singles_by_id[sid][:4]:
            core_singles.append((used_syls[sid], ubytes.decode("utf-8")))
    core_phrases = []
    all_ph_flat = []
    for sid in range(syl_count):
        for sids, ubytes, w in phrases_by_id[sid]:
            all_ph_flat.append((w, "".join(used_syls[x] for x in sids), ubytes.decode("utf-8")))
    all_ph_flat.sort(key=lambda x: (-x[0], x[1]))
    core_phrases = [(p, w) for (_, p, w) in all_ph_flat[:350]]

    out_h = os.path.join(args.repo, "include", "ime_core.h")
    with open(out_h, "w", encoding="utf-8", newline="\n") as f:
        f.write("#ifndef _IME_CORE_H\n#define _IME_CORE_H\n\n")
        f.write("/*\n * Auto-generated fallback core Pinyin lexicon from Rime (LGPL-3.0) +\n")
        f.write(" * OpenCC (Apache-2.0) data. Full lexicon lives in system/IME.PNY.\n */\n")
        f.write("typedef struct {\n    const char* pinyin;\n    const char* text;\n} ime_core_entry_t;\n\n")
        f.write("static const char* const g_ime_core_syllables[] = {\n")
        for i in range(0, len(used_syls), 12):
            chunk = ", ".join(f'"{s}"' for s in used_syls[i : i + 12])
            f.write(f"    {chunk},\n")
        f.write("};\n\n")
        f.write("static const ime_core_entry_t g_ime_core_entries[] = {\n")
        for py, txt in core_phrases + core_singles:
            esc = txt.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{ "{py}", "{esc}" }},\n')
        f.write("};\n\n#endif /* _IME_CORE_H */\n")

    print(
        f"Generated {out_pny}: {total_size:,} bytes | syls={syl_count} "
        f"| singles={single_count:,} | phrases={phrase_count:,}"
    )
    print(f"Generated {out_h}: {len(core_phrases)} core phrases + {len(core_singles)} core chars")


if __name__ == "__main__":
    main()
