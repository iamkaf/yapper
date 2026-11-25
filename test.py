"""Yapper's tests. Run from anywhere: python3 test.py [--bless]

Builds the CLI and the C checks with AddressSanitizer and
UndefinedBehaviorSanitizer, using CC (cc by default), and checks:

- the samples of each line in each voice, from letters and from the phrase
  table, against the SHA-256s blessed in tests/blessed.txt;
- the same samples from GCC and Clang, each at -O0 and -O2 (COMPILERS names
  them, "gcc clang" by default);
- that reached climbs to the whole text, and that the mouth opens and shuts;
- that yapper.c compiles freestanding and needs nothing from outside;
- the C API's promises (tests/api.c);
- truncated, damaged and malformed phrase tables (tests/tables.c), and that a
  malformed record is read from its letters instead;
- compile.py, if flite and espeak-ng are installed.

--bless writes tests/blessed.txt afresh from the sanitized build."""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
import common  # noqa: E402
from compile import table as make_table, fnv  # noqa: E402

CC = os.environ.get("CC", "cc")
WARN = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
SAN = ["-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
BLESSED = ROOT / "tests" / "blessed.txt"
TABLE = common.table_path()
tmp = Path(tempfile.mkdtemp(prefix="yapper-test-"))


def cc(out, sources, flags, compiler=CC):
    subprocess.run([compiler, *WARN, *flags, "-o", str(out), *map(str, sources)], check=True)
    return out


def cases():
    """(name, line, CLI arguments) for every blessed render."""
    out = []
    for voice in common.PRESETS:
        for source in ("letters", "table"):
            for i, line in enumerate(common.LINES):
                args = ["--voice", voice] + (["--phrases", str(TABLE)] if source == "table" else [])
                out.append((f"{voice} {source} {i}", line, args))
    for radio in (1, 2, 3):
        for i, line in enumerate(common.LINES):
            out.append((f"man+radio{radio} letters {i}", line, ["--set", f"radio={radio}"]))
    return out


def render(yapper, case, where):
    name, line, args = case
    d = where / name.replace(" ", "-")
    d.mkdir(parents=True, exist_ok=True)
    pcm, timeline, total = common.say(yapper, line, args, d)
    return name, hashlib.sha256(pcm).hexdigest(), timeline, total, line


def renders(yapper, label):
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        return list(ex.map(lambda c: render(yapper, c, tmp / label), cases()))


def sane(name, timeline, total, line):
    """reached climbs to the whole text and the mouth opens and shuts."""
    whole = len(line.encode())
    assert timeline[0][0] == 0, name
    last = 0
    for _, reached, _ in timeline:
        assert last <= reached <= whole, f"{name}: reached went {last} to {reached}"
        last = reached
    assert timeline[-1] == (total, whole, 0), f"{name}: ends {timeline[-1]}"
    assert any(m for _, _, m in timeline) and total > YAP_RATE // 2, f"{name}: never opened its mouth"


YAP_RATE = 44100


def blessed():
    if not BLESSED.exists():
        return {}
    return {ln[65:]: ln[:64] for ln in BLESSED.read_text().splitlines() if ln}


def check_renders():
    yapper = cc(tmp / "yapper-san", [ROOT / "cli.c", ROOT / "yapper.c"], SAN)
    got = renders(yapper, "san")
    for name, _, timeline, total, line in got:
        sane(name, timeline, total, line)
    if "--bless" in sys.argv:
        BLESSED.write_text("".join(f"{h} {name}\n" for name, h, *_ in got))
        print(f"blessed {len(got)} renders")
    want = blessed()
    for name, h, *_ in got:
        assert want.get(name) == h, f"{name}: the samples changed ({h})"
    print(f"renders: {len(got)} match their blessed hashes, reached and mouth sane")
    built = []
    for compiler in os.environ.get("COMPILERS", "gcc clang").split():
        if not shutil.which(compiler):
            print(f"renders: {compiler} is not installed, skipped")
            continue
        for opt in ("-O0", "-O2"):
            exe = cc(tmp / f"yapper-{compiler}{opt}", [ROOT / "cli.c", ROOT / "yapper.c"], [opt], compiler)
            for name, h, *_ in renders(exe, f"{compiler}{opt}"):
                assert want[name] == h, f"{name}: {compiler} {opt} differs"
            built.append(f"{compiler} {opt}")
    print(f"renders: identical from {', '.join(built)}")
    return yapper


def check_freestanding():
    obj = tmp / "yapper.o"
    cc(obj, [ROOT / "yapper.c"], ["-c", "-Os", "-ffreestanding", "-nostdlib", "-pedantic"])
    if shutil.which("nm"):
        undefined = common.run(["nm", "-u", obj]).split()
        assert not undefined, f"yapper.o needs {undefined}"
    print("freestanding: yapper.c compiles with -ffreestanding -nostdlib and needs nothing from outside")


def check_api():
    exe = cc(tmp / "api", [ROOT / "tests" / "api.c", ROOT / "yapper.c"], SAN)
    print(common.run([exe, TABLE]).strip())


def parse(data):
    """A phrase table's records, {hash: record}."""
    n = struct.unpack_from("<I", data, 4)[0]
    hs = struct.unpack_from(f"<{n}I", data, 8)
    offs = struct.unpack_from(f"<{n + 1}I", data, 8 + 4 * n)
    base = 12 + 8 * n
    return {h: data[base + offs[i]:base + offs[i + 1]] for i, h in enumerate(hs)}


def check_tables(yapper):
    good = TABLE.read_bytes()
    rng = random.Random(1701)
    out = bytearray()

    def case(data, verdict):
        out.extend(struct.pack("<II", len(data), verdict) + data)

    def header(n, hashes, offs, records=b"", tag=b"YPH1"):
        return tag + struct.pack(f"<I{len(hashes)}I{len(offs)}I", n, *hashes, *offs) + records

    case(good, 0)
    case(make_table({}), 0)
    for i in range(len(good)):                   # every truncation
        case(good[:i], 1)
    for i in range(len(good)):                   # every byte damaged, twice
        for x in (1, rng.randrange(2, 256)):
            bad = bytearray(good)
            bad[i] ^= x
            case(bytes(bad), 2)
    for _ in range(3000):                        # a few bytes damaged at once
        bad = bytearray(good)
        for _ in range(rng.randrange(2, 9)):
            bad[rng.randrange(len(bad))] = rng.randrange(256)
        case(bytes(bad), 2)
    case(good + b"\0", 1)
    case(b"YPH2" + good[4:], 1)
    case(header(0xFFFFFFFF, [], [0]), 1)
    case(header(0x20000000, [], [0]), 1)         # 12 + 8n wraps a u32
    case(header(1, [5], [1, 2], b"\xff\xff"), 1)  # the first offset is not 0
    case(header(1, [5], [0, 3], b"\xff\xff"), 1)  # the last offset is not the records' size
    case(header(2, [5, 5], [0, 1, 2], b"\xff\xff"), 1)
    case(header(2, [6, 5], [0, 1, 2], b"\xff\xff"), 1)
    case(header(2, [5, 6], [0, 2, 1], b"\xff\xff"), 1)
    case(header(2, [5, 6], [0, 2, 2], b"\xff\xff"), 0)
    for size in range(12):
        case((b"YPH1" + bytes(8))[:size], 1)
    # Malformed records behind a sound header: accepted, and the phrase read
    # from its letters. Each stands for "hello there".
    h = fnv(b"hello there")
    records = [b"", b"\xfe\xfe", b"\x2a\x10\xfe\xfe", b"\x40\x10\x80\xfe\xfe", b"\x2a\x10\xfe\xfe\xff\x00",
               b"\x28\x10\xfe\xfe\xff", b"\x00\x10\xfe\xff", b"\x00\x10\xfe\x00\x10\xfe\x00\x10\xfe\xff",
               b"\x40\x10\xfe\xfe\xff\xff", b"\x00\xfe\xff", b"\x40\x10"]
    for rec in records:
        case(make_table({h: rec}), 0)
    cases_file, lines_file = tmp / "cases.bin", tmp / "lines.txt"
    cases_file.write_bytes(out)
    lines_file.write_text("\n".join(common.LINES + ["Hello there.", "hello THERE, hello there"]) + "\n")
    exe = cc(tmp / "tables", [ROOT / "tests" / "tables.c", ROOT / "yapper.c"], SAN)
    print(common.run([exe, cases_file, lines_file]).strip())
    letters, _, _ = common.say(yapper, "Hello there.", [], tmp)
    for i, rec in enumerate(records):
        path = tmp / f"broken{i}.yph"
        path.write_bytes(make_table({h: rec}))
        pcm, _, _ = common.say(yapper, "Hello there.", ["--phrases", str(path)], tmp)
        assert pcm == letters, f"the malformed record {rec.hex()} was used"
    print(f"tables: {len(records)} malformed records each read from letters instead")


def check_compiler():
    if not (shutil.which("flite") and shutil.which("espeak-ng")):
        print("compile.py: skipped (flite and espeak-ng are not both installed)")
        return
    out = tmp / "lines.yph"
    common.run([sys.executable, ROOT / "compile.py", ROOT / "tests" / "lines.txt", "-o", out])
    data = out.read_bytes()
    records = parse(data)
    wanted = {fnv(b" ".join(bytes(c | 32 if 65 <= c <= 90 else c for c in page[a:b] if c != 39) for a, b in words))
              for page in map(blank, common.LINES) for words in phrases(page) if words}
    assert set(records) == wanted, "compile.py left out a phrase, or added one"
    assert make_table(records) == data, "compile.py's table is not laid out as FORMAT.md says"
    same = data == TABLE.read_bytes()
    print(f"compile.py: {len(records)} phrases, every one well made"
          + ("; identical to tests/lines.yph" if same else
             "; differs from tests/lines.yph (another version of flite or espeak-ng?)"))


from compile import blank, phrases  # noqa: E402


def main():
    try:
        yapper = check_renders()
        check_freestanding()
        check_api()
        check_tables(yapper)
        check_compiler()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("all passed")


if __name__ == "__main__":
    main()
