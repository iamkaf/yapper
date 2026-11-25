# Contributing

Bug reports, fixes and new voices are welcome. Open an [issue](https://github.com/iamkaf/yapper/issues) for a bug or an idea, and a pull request for a change. Report a suspected vulnerability privately, as the [security policy](SECURITY.md) describes, not in a public issue.

## What belongs here

Yapper is the runtime in `yapper.h` and `yapper.c`, the `yapper` command in `cli.c`, and the command's phrase compiler in `compile.c` and `pitch.c`, with its Python reference, `compile.py`. The runtime stays small and freestanding: integer math only, no C library, no allocation, and all state in the caller's `yapper`. A change that needs floating point, a heap or a header beyond `<stdint.h>` in the runtime doesn't fit. The compiler never ships with the runtime, so it can use whatever helps. A change to how a table is compiled goes into both `compile.c` and `compile.py`, and `test.py` checks that they still write the same bytes.

Version 1 of the phrase table is frozen ([FORMAT.md](FORMAT.md)). The runtime must keep reading every table it reads now, the same way. A change to the layout needs a new tag.

## Set up

You need a C compiler and Python 3. The tests use AddressSanitizer and UndefinedBehaviorSanitizer, which Clang has on Linux and macOS. To compile phrase tables, and to run the compiler's test, install flite and espeak-ng too:

```sh
sudo apt install clang gcc python3 flite espeak-ng      # Debian, Ubuntu
sudo dnf install clang gcc python3 flite espeak-ng      # Fedora
```

## Run the checks

CI runs exactly these, on Ubuntu 24.04 with GCC 13 and Clang 18:

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror cli.c yapper.c compile.c pitch.c -o yapper -lm
clang -std=c11 -O2 -Wall -Wextra -Werror cli.c yapper.c compile.c pitch.c -o yapper -lm
CC=clang python3 test.py
python3 tests/parity.py
```

`test.py` builds what it needs, so there is nothing to build first. The README's [Run the tests](README.md#run-the-tests) section lists what it checks. CI doesn't install flite or espeak-ng, so there the compiler's test skips. If you change `compile.c`, `compile.py` or `pitch.c`, run `test.py` with both installed. `tests/parity.py` skips everywhere but on a machine that has the code Yapper was taken from.

A pull request needs CI to pass.

## Write the change

If a change is meant to change the sound, run `CC=clang python3 test.py --bless` and commit the new `tests/blessed.txt` with it, and say in the pull request what changed and why. Listen to a few lines before and after. If a change isn't meant to change the sound, the blessed hashes must not move. If you add a check to the runtime's table reader, add a damaged table to `test.py` that the check refuses.

If a change affects the code size or speed in the README, measure them again the way the README describes, and update the numbers.

Make each commit one change. Write its subject as a short plain sentence, and use its body to say why the change is needed and anything a reader of the diff can't see.
