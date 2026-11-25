"""Parity with the synthesizer Yapper was taken from.

Yapper began as the voice of a game. Point YAPPER_ORIGIN (or the first
argument) at the directory holding that game's voice.c, and this script
builds the original with a small harness, says the same lines in the same
voices through both, with and without a phrase table, and checks that every
sample, and the text reached and the mouth at every 1/60 s, are the same.
Without the original it skips.

YAPPER_ORIGIN_PHRASES, if set, names the original phrase compiler's output:
JSON lines, each a line of text ("t") and its phrases ("c": hash to words
and record), with "row" on a line compiled with more than a record holds.
compile.py must make the same record for every phrase of the lines without
one, leaving out lines the original said louder (a word in capitals, or
"!!") or that hold an "@". The original took at most 640 bytes of a line,
where Yapper takes 1024, so this part compiles as it did."""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
import common  # noqa: E402

TICK = 735
HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void vo_start(void);
void vo_say(const uint8_t *row, const char *text, int n);
void vo_mix(int32_t *mix, int32_t gain);
uint32_t vo_reached(void);
int vo_open(void);
int tk_pain(void) { return 0; }
uint8_t sd_deaf;
static uint8_t *spok;
static uint32_t nspok;
const uint8_t *pn_section(uint32_t tag, uint32_t *size);
const uint8_t *pn_section(uint32_t tag, uint32_t *size)
{
    if (tag != 0x4b4f5053u || !spok) { *size = 0; return 0; }
    *size = nspok;
    return spok;
}
/* harness ROW(16 numbers, comma separated) TEXT SPOK|- : int16 samples on
   stdout, a tick at a time, until 20 ticks after the voice is done; on
   stderr, each tick's reached and mouth as the tick begins. */
int main(int argc, char **argv)
{
    uint8_t row[32] = { 0 };
    char *p = argv[1];
    int i, tail = 0;
    for (i = 0; i < 16; i++) { row[i] = (uint8_t)strtol(p, &p, 10); if (*p) p++; }
    if (strcmp(argv[3], "-")) {
        FILE *f = fopen(argv[3], "rb");
        spok = malloc(1 << 24);
        nspok = (uint32_t)fread(spok, 1, 1 << 24, f);
        fclose(f);
    }
    vo_start();
    vo_say(row, argv[2], (int)strlen(argv[2]));
    while (tail < 20) {
        int32_t mix[735];
        int16_t out[735];
        fprintf(stderr, "%u %d\n", vo_reached(), vo_open());
        memset(mix, 0, sizeof mix);
        vo_mix(mix, 256);
        for (i = 0; i < 735; i++) {
            int32_t q = mix[i] * 4;
            out[i] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
        }
        fwrite(out, 2, 735, stdout);
        if (vo_reached() == 0xffff) tail++;
    }
    return 0;
}
"""
FIELDS = ["pitch", "range", "pace", "tract", "oq", "tilt", "breath", "flutter", "tremor",
          "volume", "b1", "soft", "style", "pause", "radio"]


def row(v):
    """The original's 16-byte row for a voice: pitch and b1 in 2 Hz steps,
    the radio its "over", and a last byte it alone has."""
    r = [v[f] for f in FIELDS] + [0]
    r[0] //= 2
    r[10] //= 2
    return ",".join(map(str, r))


def section(table):
    """A Yapper phrase table in the original's layout: a u16 count, the
    hashes, u16 lengths, and each record after a flag byte of 0."""
    n = struct.unpack_from("<I", table, 4)[0]
    hashes = struct.unpack_from(f"<{n}I", table, 8)
    offs = struct.unpack_from(f"<{n + 1}I", table, 8 + 4 * n)
    base = 12 + 8 * n
    recs = [b"\0" + table[base + offs[i]:base + offs[i + 1]] for i in range(n)]
    return struct.pack(f"<H{n}I{n}H", n, *hashes, *map(len, recs)) + b"".join(recs)


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("YAPPER_ORIGIN", ""))
    if not str(src) or not (src / "voice.c").is_file():
        print("parity: skipped (set YAPPER_ORIGIN to the original voice.c's directory)")
        return
    tmp = Path(tempfile.mkdtemp(prefix="yapper-parity-"))
    (tmp / "harness.c").write_text(HARNESS)
    origin = tmp / "origin"
    sources = [src / "voice.c"] + sorted(src.glob("woman*.c"))
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-w", f"-I{src.parent}", "-o", origin,
                    tmp / "harness.c", *sources, "-lm"], check=True)
    yapper = common.build_cli(tmp / "yapper")
    tables = [None]
    if common.table_path().exists():
        spok = tmp / "spok.bin"
        spok.write_bytes(section(common.table_path().read_bytes()))
        tables.append(spok)
    checked = 0
    for name in common.PRESETS:
        voice = common.voice(yapper, name)
        for radio in ([0, 1, 2, 3] if name == "man" else [voice["radio"]]):
            v = dict(voice, radio=radio)
            sets = ["--set", f"radio={radio}"]
            for spok in tables:
                for line in common.LINES:
                    got = subprocess.run([origin, row(v), line, str(spok or "-")], capture_output=True, check=True)
                    pcm, cues = got.stdout, [tuple(map(int, c.split())) for c in got.stderr.decode().splitlines()]
                    args = ["--voice", name, *sets] + (["--phrases", str(common.table_path())] if spok else [])
                    mine, timeline, total = common.say(yapper, line, args, tmp)
                    where = f"{name} radio={radio} {'table' if spok else 'letters'}: {line!r}"
                    assert len(pcm) >= len(mine), f"{where}: the original stopped first"
                    assert pcm[:len(mine)] == mine, f"{where}: the samples differ at {first_difference(pcm, mine)}"
                    assert not pcm[len(mine):].strip(b"\0"), f"{where}: the original goes on"
                    for k, (reached, mouth) in enumerate(cues):
                        r, m = common.at(timeline, k * TICK)
                        want = len(line.encode()) if reached == 0xFFFF else reached
                        assert (r, m) == (want, mouth), f"{where}: tick {k}: {(r, m)} != {(want, mouth)}"
                    checked += 1
    print(f"parity: {checked} renders identical to the original")
    phrases = os.environ.get("YAPPER_ORIGIN_PHRASES")
    if phrases:
        compiled(Path(phrases))


def compiled(path):
    import compile as yc
    yc.TEXT = 640                       # the original took 640 bytes of a line, at most
    lines = [json.loads(x) for x in path.read_text().splitlines() if x.strip()]
    louder = re.compile(r"[A-Za-z']+")
    lines = [e for e in lines if "row" not in e and "@" not in e["t"] and "!!" not in e["t"]
             and not any(len(w.replace("'", "")) >= 2 and w.isupper() for w in louder.findall(e["t"]))]
    same, long = 0, 0
    for e, got in zip(lines, yc.compile_each([e["t"] for e in lines])):
        if any(got.get(int(h, 16), (0, 0))[1] != bytes.fromhex(rec)[1:] for h, (_, rec) in e["c"].items()):
            # The original read pitch from the first 20 s of a line alone.
            took = float(yc.run(["flite", "-voice", "slt", "-psdur", "-t", yc.blank(e["t"]).decode(errors="replace"),
                                 "-o", "none"]).split()[-1].split(":")[1])
            assert took > 20, f"{e['t']!r} compiled differently"
            long += 1
            continue
        same += len(e["c"])
    if long:
        print(f"parity: {long} lines longer than 20 s, past which the original found no pitch, left out")
    print(f"parity: {same} phrases of {len(lines)} lines compiled as the original compiled them")


def first_difference(a, b):
    return next(i // 2 for i in range(min(len(a), len(b))) if a[i] != b[i])


if __name__ == "__main__":
    main()
