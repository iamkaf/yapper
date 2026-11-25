"""What the tests share: the lines, the voices, and how to build and run the CLI."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESETS = ["man", "woman", "child", "elder", "giant", "robot"]
# Lines that between them reach every rule: questions that rise and fall,
# commas, semicolons, an ellipsis before a word and at the start, brackets,
# digits and symbols, apostrophes, capitals, and a paragraph long enough to
# take a breath.
LINES = [
    "Hello there. How are you today?",
    "The quick brown fox jumps over the lazy dog.",
    "Is it raining outside? Bring an umbrella, just in case.",
    "...Well. I suppose we could try again; nothing else has worked.",
    "(quietly) Keep your voice down. [pause] They're listening.",
    "Room 42 costs $90 a night, which isn't cheap!",
    "Why would anyone build a whole speech synthesizer that fits in a few kilobytes? "
    "Because it's fun, and because small things travel well. Shall we listen to it again?",
]


def run(cmd):
    """A command's standard output; if it fails, its standard error and a stop."""
    got = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if got.returncode:
        raise SystemExit(f"{' '.join(map(str, cmd))} failed ({got.returncode}):\n{got.stdout[-2000:]}{got.stderr[-4000:]}")
    return got.stdout


def table_path():
    return ROOT / "tests" / "lines.yph"


def build_cli(out, cc=None, flags=("-O2",)):
    cc = cc or os.environ.get("CC", "cc")
    sources = [str(ROOT / name) for name in ("cli.c", "yapper.c", "compile.c", "pitch.c")]
    subprocess.run([cc, "-std=c11", *flags, "-o", str(out), *sources, "-lm"], check=True)
    return out


def voice(yapper, name):
    """A preset's fields, as `yapper voices` lists them."""
    rows = [r.split() for r in run([yapper, "voices"]).splitlines()]
    return dict(zip(rows[0], map(int, next(r for r in rows if r[0] == name)[1:])))


def say(yapper, line, args, tmp):
    """(samples as bytes, the cue timeline [(sample, reached, mouth)], its length)."""
    wav = Path(tmp) / "say.wav"
    timeline = [tuple(map(int, c.split())) for c in run([yapper, "say", line, "-o", wav, "--cues", *args]).splitlines()]
    pcm = wav.read_bytes()[44:]
    return pcm, timeline, len(pcm) // 2


def at(timeline, sample):
    """reached and mouth as the given sample begins."""
    r = m = 0
    for s, reached, mouth in timeline:
        if s > sample:
            break
        r, m = reached, mouth
    return r, m
