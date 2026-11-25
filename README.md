<p align="center"><img alt="Yapper: a speech waveform drawn in red ink on chart paper, with the words hello, there, how are you under it" src=".github/banner.png"></p>

<p align="center">
  <a href="https://github.com/iamkaf/yapper/actions/workflows/ci.yml"><picture><source media="(prefers-color-scheme: dark)" srcset="https://shieldcn.dev/github/ci/iamkaf/yapper.svg?workflow=ci.yml&amp;branch=main&amp;variant=outline&amp;size=sm&amp;font=geist&amp;mode=dark"><img alt="CI status" src="https://shieldcn.dev/github/ci/iamkaf/yapper.svg?workflow=ci.yml&amp;branch=main&amp;variant=outline&amp;size=sm&amp;font=geist&amp;mode=light"></picture></a>
  <a href="https://github.com/iamkaf/yapper/releases"><picture><source media="(prefers-color-scheme: dark)" srcset="https://shieldcn.dev/github/release/iamkaf/yapper.svg?variant=outline&amp;size=sm&amp;font=geist&amp;mode=dark"><img alt="Latest release" src="https://shieldcn.dev/github/release/iamkaf/yapper.svg?variant=outline&amp;size=sm&amp;font=geist&amp;mode=light"></picture></a>
  <a href="LICENSE"><picture><source media="(prefers-color-scheme: dark)" srcset="https://shieldcn.dev/badge/license-MIT.svg?variant=outline&amp;size=sm&amp;font=geist&amp;mode=dark"><img alt="MIT license" src="https://shieldcn.dev/badge/license-MIT.svg?variant=outline&amp;size=sm&amp;font=geist&amp;mode=light"></picture></a>
</p>

Yapper is a tiny speech synthesizer in freestanding C. Give it text, and it gives back 44.1 kHz audio, how far through the text the voice has got, and whether the mouth is open. A game can type out dialogue in step with the voice and move a portrait's mouth on each vowel. It uses only integer math, so the same text gives the same samples on every machine. It needs no C library, never allocates, and compiles to about 9 KB of code and tables.

Yapper was built for Pinion, a game that has to fit on one floppy disk. It reads words from their letters with a few rules of English spelling, and that needs no data at all. For lines known ahead of time, an optional phrase table says them better. A phrase table holds each phrase's sounds, their lengths, its stress and its tune, taken from flite and espeak-ng when the table is built. Neither program ships with the runtime.

Listen to the man's voice saying "Hello there." [from its letters](docs/letters.wav) and [from a phrase table](docs/table.wav).

## Try it

The command is `cli.c` and the synthesizer `yapper.c`, both C11 with no dependencies. Build them with GCC or Clang on Linux or macOS:

```sh
cc -O2 cli.c yapper.c -o yapper
./yapper say "Hello there. How are you today?" -o hello.wav
./yapper say "Is it raining outside?" --voice woman -o rain.wav
./yapper say "Come in, base. Over." --set radio=2 -o radio.wav
./yapper voices
```

On Windows, build it with MSVC from a Developer Command Prompt:

```bat
cl /O2 cli.c yapper.c /Fe:yapper.exe
```

Pre-built binaries for Linux (x86-64 and arm64), macOS and Windows are attached to each [release](https://github.com/iamkaf/yapper/releases).

`yapper say TEXT` writes a 16-bit mono WAV with `-o`. `TEXT` can be `-` to read it from standard input. `--voice` picks a preset, and `--set KEY=VALUE` changes one of its fields, which can be repeated. `yapper voices` lists the presets and their fields. `--phrases TABLE` uses a phrase table. `--cues` prints a line each time the text reached or the mouth changes: the sample, the bytes of text reached, and 1 or 0 for the mouth.

```
$ ./yapper say "Hello there." --cues
0 0 0
7491 5 0
9661 5 1
13441 5 0
15611 5 1
19986 12 0
22366 12 1
28036 12 0
44101 12 0
```

The voice starts with a breath. It reaches "Hello" at sample 7,491 and opens its mouth for the "e" at 9,661, and it reaches "there." at 19,986. The last line is where the audio ends.

## Use the library

Copy `yapper.h` and `yapper.c` into your project. Each voice that talks needs a `yapper`, about 15 KB of state that you own, so several voices can talk at once.

```c
#include "yapper.h"

static yapper voice;

void start(void)
{
    yap_reset(&voice);
    yap_say(&voice, &yap_woman, "Is it raining outside?", 22);
}

/* Each audio callback: add the voice to the mix at half volume. */
void audio(int32_t *mix, uint32_t samples)
{
    yap_mix(&voice, mix, samples, 128);
}

/* Each frame: show the text as far as the voice has got, and open the mouth. */
void frame(void)
{
    draw_text(text, yap_reached(&voice));
    draw_mouth(yap_mouth(&voice));
}
```

The API, from `yapper.h`:

- `yap_reset(y)` silences a voice and sets all of its state as new. Call it once before anything else.
- `yap_say(y, voice, text, n)` starts saying the first `n` bytes of `text`, or up to a NUL, at most 1024. It replaces whatever was being said. Text in brackets, `( )` or `[ ]`, isn't said, so "(quietly)" can say how a line is spoken.
- `yap_mix(y, mix, n, gain)` adds the next `n` samples into an `int32_t` buffer at `gain/256`, where 256 is full scale. `yap_render(y, out, n)` writes them to an `int16_t` buffer instead, clipped. Once the voice is done, both give silence.
- `yap_reached(y)` returns how many bytes of the text the voice has begun. Each word counts with the punctuation after it. Once the voice is done, it returns the whole text's length.
- `yap_mouth(y)` returns 1 while a vowel sounds.
- `yap_busy(y)` returns 1 while there is sound to come.
- `yap_hush(y)` fades the voice out over about 20 ms. The text then counts as reached.
- `yap_phrases(y, table, size)` uses a phrase table from now on, or none if `table` is 0. The table isn't copied, so keep it alive while it is in use. It returns 0, or -1 for a malformed table, which is then not used.

A voice is a plain struct, `yap_voice`, and these are its fields:

| Field | Meaning |
|---|---|
| `pitch` | Speaking pitch in Hz, 20 to 1000 |
| `range` | How wide the tune is, in 256ths of the pitch |
| `pace` | Each sound's length in percent. Smaller is quicker |
| `tract` | The formants' scale. 128 is a grown man's, about 150 a woman's |
| `oq` | Open quotient: how much of each period the glottis is open, in 256ths |
| `tilt` | The pulse's brightness, in 256ths. Lower is darker |
| `breath` | Breath in the voice, in 256ths |
| `flutter` | How much the pitch wanders, in 256ths |
| `tremor` | An old voice's 5.5 Hz shake, in 256ths |
| `volume` | Loudness, in 256ths |
| `b1` | Extra width for the first formant, in Hz |
| `soft` | A second pole on the pulse, in 256ths. It takes the buzz off the upper harmonics |
| `style` | Bits: `YAP_WOMAN` women's vowel formants, `YAP_ASPIRATE` air through the glottis in time with it, `YAP_PARALLEL` formants side by side instead of in a cascade, `YAP_SMOOTH` a glottis that closes smoothly |
| `pause` | Pauses between phrases in percent, with a breath drawn now and then. 0 gives short, even pauses |
| `radio` | 0, or 1 to 3 to come over a radio channel, each stronger than the last |

The presets are `yap_man`, `yap_woman`, `yap_child`, `yap_elder`, `yap_giant` and `yap_robot`. Copy one and change its fields to make your own.

The code is freestanding C11 and needs only `<stdint.h>`. It uses 64-bit integers, so on a 32-bit target the compiler may call its own helpers for 64-bit division. It assumes that right shifts of negative numbers are arithmetic, as they are on GCC, Clang and MSVC. On x86-64 with GCC 16.2, `-Os -ffreestanding -fno-asynchronous-unwind-tables`, it compiles to 7,301 bytes of code and 1,678 bytes of read-only data, 8,979 bytes in all. With Clang 22.1 it is 8,616 and 1,592, 10,208 in all. On x86-64 it makes no calls outside itself.

It is fast. On one core of a Ryzen 9 7900X, built with `-O2`, it speaks between 1,500 and 2,400 times faster than real time, depending on the voice. `tests/bench.c` measures it:

```sh
cc -O2 tests/bench.c yapper.c -o bench && ./bench
```

## Phrase tables

The letter rules get a lot of English wrong. A phrase table fixes that for the lines you know ahead of time. Put them in a text file, one line of speech per line, and compile it:

```sh
python3 compile.py lines.txt -o lines.yph
./yapper say "Is it raining outside?" --phrases lines.yph -o rain.wav
```

`compile.py` needs Python 3, [flite](http://cmuflite.org) and [espeak-ng](https://github.com/espeak-ng/espeak-ng) installed, and a C compiler for `pitch.c`. It splits each line into phrases at punctuation, as the runtime does. For each phrase it records each word's sounds as flite's lexicon says them in context, each sound's length as flite's slt voice says the line, which syllable espeak-ng stresses, and each vowel's pitch, which `pitch.c` reads off flite's recording. The runtime finds a phrase by a hash of its words, so a phrase is said from the table wherever it appears, in any line. Any phrase the table doesn't hold is read from its letters. [FORMAT.md](FORMAT.md) documents the table.

## How it works

The sound starts at the glottis: a pulse each period, shaped as the derivative of the airflow through the vocal folds (Rosenberg's pulse, as in Klatt's KLGLOTT88), or as a smooth rise and fall for a softer voice. Noise rides on it as breath. A one-pole filter tilts the pulse's spectrum, and the pitch wanders with two slow random walks, as a real voice does, plus jitter and shimmer from one period to the next. That source goes through four two-pole resonators in a cascade, one for each formant, after Klatt 1980. Fricatives come from noise through a fifth resonator beside the tract, and "h" from noise through the tract itself. The resonators' coefficients are computed in Q30 fixed point, and the voice updates its controls every 35 samples, which is 0.8 ms.

The sounds come from the text. Each letter has formant targets for a grown man, from Peterson and Barney 1952 for the vowels, and from the loci the formants move toward for the consonants. The formants glide toward each sound's targets, and the `tract` field scales them for a smaller or larger speaker. `YAP_WOMAN` uses women's vowel formants from Hillenbrand et al. 1995 instead, since a woman's vowels are not a man's scaled by one number. The letter rules handle silent final e's, doubled letters, "sh", "ch", "th", "ph", "ng", "ck", "qu", "x", and a soft or hard c. A word of more than three letters, or a capitalised one, gets its first syllable stressed.

The prosody comes from the phrase. Syllables run at about four a second. The pitch starts high and falls through each phrase, and each stressed syllable is lifted. The last syllable is longer and falls for a statement. It rises for a yes-or-no question, and lifts a little before a comma. A question that starts with "wh" or "how" falls. Commas, semicolons and full stops give pauses of their own lengths, and an ellipsis gives a longer one. A voice with a `pause` setting draws a short breath before a new sentence once it has spoken for a second and a half.

The radio option band-limits the voice to 400 Hz to 3.4 kHz, drives it into a soft limit, and adds a carrier hiss and a burst of squelch as the channel opens and closes. Stronger settings narrow the band and add a small speaker's honk and crackle.

## Sources

- Dennis Klatt, "Software for a cascade/parallel formant synthesizer", JASA 67, 1980: the resonators and the cascade.
- Dennis and Laura Klatt, "Analysis, synthesis, and perception of voice quality variations among female and male talkers", JASA 87, 1990: the glottal pulse, breath and aspiration.
- Gordon Peterson and Harold Barney, "Control methods used in a study of the vowels", JASA 24, 1952: the men's vowel formants.
- James Hillenbrand et al., "Acoustic characteristics of American English vowels", JASA 97, 1995: the women's vowel formants.
- Alain de Cheveigné and Hideki Kawahara, "YIN, a fundamental frequency estimator for speech and music", JASA 111, 2002: `pitch.c`.
- [flite](http://cmuflite.org) and [espeak-ng](https://github.com/espeak-ng/espeak-ng), run by `compile.py` at build time only. Nothing from either is in the runtime.

## Run the tests

`CC=clang python3 test.py` builds the command and the C checks with AddressSanitizer and UndefinedBehaviorSanitizer, and checks:

- the SHA-256 of the samples for seven lines in every preset, from letters and from a phrase table, plus the radio's three strengths, against the hashes in `tests/blessed.txt`;
- that GCC and Clang, each at `-O0` and `-O2`, give exactly those samples;
- that the text reached only climbs and ends at the whole text, and that the mouth opens and shuts;
- that `yapper.c` compiles with `-ffreestanding -nostdlib` and needs no symbol from outside;
- the API's promises in `tests/api.c`: the same samples in any chunk size, two voices at once, mixing, hushing, cut-off text, every byte value, and random voices at the extremes;
- every truncation of a phrase table, every byte of one damaged, thousands of random damages and malformed headers, each in a buffer of exactly its size, and that a malformed record is read from its letters instead;
- `compile.py` on `tests/lines.txt`, if flite and espeak-ng are installed.

It runs in about 10 seconds. `python3 test.py --bless` writes `tests/blessed.txt` afresh, for a change that is meant to change the sound. `COMPILERS` names the compilers to compare, `gcc clang` by default.

`tests/parity.py` checks Yapper against the code it was taken from, when that code is present. Set `YAPPER_ORIGIN` to the directory holding the original `voice.c`. Every preset, and the radio's three strengths, must give the original's samples exactly, from letters and from a phrase table, along with its text reached and mouth at every 1/60 s. With `YAPPER_ORIGIN_PHRASES` set to the original phrase compiler's output, it also checks that `compile.py` makes the same record for every phrase. Without the original, it skips.

## Contributing and security

Bug reports and fixes are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) says how to make a change and what CI runs. Use the repository's [security policy](SECURITY.md) instead of a public issue for suspected vulnerabilities.

## License

MIT. See [LICENSE](LICENSE).
