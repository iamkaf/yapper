"""compile.py: a phrase table (FORMAT.md) from a text file of lines.

    python3 compile.py LINES.txt -o TABLE.yph

LINES.txt holds one line of speech a line, UTF-8; blank lines and lines
that start with "#" are skipped. Each line is split into phrases at
, . ; : ! ? as the runtime splits it, and each phrase gets a record: its
words' sounds, each sound's length, and each vowel's stress and pitch.

It runs two speech programs, which it needs installed and which never ship
with the runtime: flite says the whole line in its slt voice, and gives
each word's sounds as its lexicon says them in context, and each sound's
length; espeak-ng says which syllable of each word is stressed. pitch.c,
built here with the C compiler (CC, or cc), reads each vowel's pitch off
flite's recording."""
import argparse
import difflib
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
TAG = b"YPH1"
TEXT = 1024                     # yapper.h's YAP_TEXT: what is past it is never said
# The sounds a record names, by number: yapper.c's cph[].
PHONES = ["iy", "ih", "ey", "eh", "ae", "aa", "ao", "ow", "uh", "uw", "ah", "ax", "er", "ay", "aw", "oy",
          "b", "d", "g", "p", "t", "k", "f", "v", "th", "dh", "s", "z", "sh", "zh", "hh", "ch", "jh",
          "m", "n", "ng", "l", "r", "w", "y"]
VOWELS = set(PHONES[:16])
# Words said unstressed whatever espeak-ng says.
FUNCTION = set("""a an the and or but of to in on at by for with from as is was were are be been am will would shall
should can could do does did have has had i you he she it we they me him her us them my your his its our their
that this these those there not no so if than then up out""".split())
IPA_V = set("aeiouyæɑɒɔəɚɛɜɝɪʊʌɐøœɘɵɤʉɯɨ")


class Bad(Exception):
    pass


# ---------------------------------------------------------------- as the runtime reads a line

def letter(c):
    return 65 <= c <= 90 or 97 <= c <= 122


def blank(text):
    """The line as the runtime holds it: bytes, at most TEXT, what is in
    brackets blanked where it stands."""
    page, n = bytearray(text.encode("utf-8")[:TEXT]), 0
    for i, c in enumerate(page):
        opening, closing = c in b"([", c in b")]"
        n += opening
        if n:
            page[i] = 32
        if closing and n:
            n -= 1
    return bytes(page)


def phrases(page):
    """Each phrase's words as [(start, end)] in page, as the runtime walks them."""
    out, t = [], 0
    while t < len(page):
        words = []
        while t < len(page) and page[t] not in b",.;:!?":
            if not letter(page[t]):
                t += 1
                continue
            w = t
            while t < len(page) and (letter(page[t]) or (page[t] == 39 and t + 1 < len(page) and letter(page[t + 1]))):
                t += 1
            words.append((w, t))
        out.append(words)
        while t < len(page) and page[t] in b",.;:!?":
            t += 1
    return out


def norm(page, words):
    """The words as the runtime hashes them: lower case, no apostrophes, a space between."""
    return b" ".join(bytes(c | 32 if 65 <= c <= 90 else c for c in page[a:b] if c != 39) for a, b in words)


def fnv(b):
    h = 2166136261
    for c in b:
        h = (h ^ c) * 16777619 & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------- flite, espeak-ng and pitch

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw).stdout


def build_pitch(tmp):
    for t in ("flite", "espeak-ng"):
        if not shutil.which(t):
            raise Bad(f"{t} is not installed")
    exe = Path(tmp) / "pitch"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-o", str(exe), str(HERE / "pitch.c"), "-lm"],
                   check=True)
    return exe


def alone(word):
    """flite's sounds for a word said alone."""
    return [p for p in run(["flite", "-ps", "-t", word, "-o", "none"]).split() if p != "pau"]


def stresses(words):
    """{word: (stressed syllable or None, {secondary}, syllables)}, from espeak-ng, one word a line."""
    if not words:
        return {}
    ipa = run(["espeak-ng", "-v", "en-us", "-q", "--ipa"], input="\n".join(words) + "\n").splitlines()
    ipa = [x.strip() for x in ipa if x.strip()]
    if len(ipa) != len(words):
        return {w: stressed(run(["espeak-ng", "-v", "en-us", "-q", "--ipa", w]).strip()) for w in words}
    return {w: stressed(x) for w, x in zip(words, ipa)}


def stressed(ipa):
    syl, prim, sec, inv, mark = 0, None, set(), False, None
    for ch in ipa:
        if ch in "ˈˌ":
            mark = ch
            continue
        if ch in IPA_V:
            if not inv:
                if mark == "ˈ" and prim is None:
                    prim = syl
                elif mark == "ˌ":
                    sec.add(syl)
                mark = None
                syl += 1
            inv = True
        elif ch not in "ː":
            inv = False
    return prim, sec, syl


def said(text, pitch, tmp):
    """flite's slt saying the line: [(sound, start, end)], and [(t, f0)] every 5 ms."""
    wav, lab = os.path.join(tmp, "l.wav"), os.path.join(tmp, "l.lab")
    seg, t0 = [], 0.0
    with open(lab, "w") as f:
        f.write("#\n")
        for s in run(["flite", "-voice", "slt", "-psdur", "-t", text, "-o", wav]).split():
            p, e = s.split(":")[0], float(s.split(":")[1])
            f.write(f"{e:.5f} 125 {p}\n")
            seg.append((p, t0, e))
            t0 = e
    f0 = [tuple(map(float, r.split())) for r in run([str(pitch), wav, lab]).splitlines()]
    return seg, f0


# ---------------------------------------------------------------- one line

def compile_line(text, own, pitch):
    """{hash: (words, record)} for the line's phrases. own: each word's
    sounds alone, and its stress."""
    page = blank(text)
    phr = phrases(page)
    toks = [page[a:b].decode("ascii").lower() for words in phr for a, b in words]
    flat, owner = [], []
    for i, w in enumerate(toks):
        flat += own["sounds"][w]
        owner += [i] * len(own["sounds"][w])
    tmp = tempfile.mkdtemp(prefix="yapper-")
    try:
        seg, f0 = said(page.decode("utf-8", "replace"), pitch, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    seg = [s for s in seg if s[0] != "pau"]
    for p, _, _ in seg:
        if p not in PHONES:
            raise Bad(f"flite said a sound the runtime has not: {p}")
    names = [s[0] for s in seg]
    word_of = [None] * len(seg)
    for a, b, n in difflib.SequenceMatcher(a=names, b=flat, autojunk=False).get_matching_blocks():
        for i in range(n):
            word_of[a + i] = owner[b + i]
    for i in range(len(seg)):                 # a sound flite changed in context: its neighbour's word
        if word_of[i] is None:
            word_of[i] = word_of[i - 1] if i and word_of[i - 1] is not None else next(
                (w for w in word_of[i:] if w is not None), max(0, len(toks) - 1))
    voiced = [x for _, x in f0 if x > 0]
    mean = math.exp(sum(math.log(x) for x in voiced) / len(voiced)) if voiced else 200.0
    sounds = [[] for _ in toks]               # each word's [sound, kind, 5 ms, pitch, start, end]
    for (p, b, e), w in zip(seg, word_of):
        sounds[w].append([p, 0, max(1, min(255, round((e - b) * 200))), 0, b, e])
    for i, w in enumerate(toks):
        nv = sum(1 for s in sounds[i] if s[0] in VOWELS)
        if w.replace("'", "") in FUNCTION:
            prim, sec = None, set()
        else:
            prim, sec, n = own["stress"][w]
            prim, sec = (prim, sec) if prim is not None and n == nv else (0, set())
        k = 0
        for s in sounds[i]:
            if s[0] in VOWELS:
                s[1] = 2 if k == prim else 3 if k in sec else 1
                mid = [x for t, x in f0 if s[4] <= t <= s[5] and x > 0]
                if mid:
                    s[3] = 128 + max(-127, min(127, round(48 * math.log2(mid[len(mid) // 2] / mean))))
                k += 1
    out, at = {}, 0
    for words in phr:
        if not words:
            continue
        mine = range(at, at + len(words))
        at += len(words)
        rec = bytearray()
        for i in mine:
            for p, kind, ln, q, _, _ in sounds[i]:
                rec += bytes([PHONES.index(p) | kind << 6, ln] + ([q] if kind else []))
            rec.append(0xFE)
        rec.append(0xFF)
        key = norm(page, words)
        out[fnv(key)] = (key.decode(), bytes(rec))
    return out


def table(records):
    """A phrase table's bytes from {hash: record}."""
    hs = sorted(records)
    offs, at = [], 0
    for h in hs:
        offs.append(at)
        at += len(records[h])
    offs.append(at)
    return (TAG + struct.pack(f"<I{len(hs)}I{len(hs) + 1}I", len(hs), *hs, *offs)
            + b"".join(records[h] for h in hs))


def compile_each(lines):
    """For each line, {hash: (words, record)} of its phrases."""
    toks = sorted({page[a:b].decode("ascii").lower() for page in map(blank, lines)
                   for words in phrases(page) for a, b in words})
    tmp = tempfile.mkdtemp(prefix="yapper-")
    try:
        pitch = build_pitch(tmp)
        with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            own = {"sounds": dict(zip(toks, ex.map(alone, toks)))}
            own["stress"] = stresses(toks)
            return list(ex.map(lambda ln: compile_line(ln, own, pitch) if any(c.isalpha() for c in ln) else {},
                               lines))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def compile_lines(lines):
    """{hash: (words, record)} for every phrase of every line; refuses two
    different phrases that hash alike. A phrase in more than one line takes
    its first line's record."""
    out = {}
    for got in compile_each(lines):
        for h, (words, rec) in got.items():
            if h in out and out[h][0] != words:
                raise Bad(f"the phrases \"{words}\" and \"{out[h][0]}\" hash alike: reword one")
            out.setdefault(h, (words, rec))
    return out


def main():
    ap = argparse.ArgumentParser(description="Compile lines of speech to a Yapper phrase table.")
    ap.add_argument("lines", help="a text file, one line of speech a line")
    ap.add_argument("-o", "--output", required=True, help="the phrase table to write")
    a = ap.parse_args()
    lines = [ln.strip() for ln in open(a.lines, encoding="utf-8")]
    lines = [ln for ln in lines if ln and not ln.startswith("#")]
    try:
        records = compile_lines(lines)
    except (Bad, subprocess.CalledProcessError, OSError) as e:
        sys.exit(f"compile.py: {e}")
    Path(a.output).write_bytes(table({h: rec for h, (_, rec) in records.items()}))
    print(f"{a.output}: {len(records)} phrases from {len(lines)} lines")


if __name__ == "__main__":
    main()
