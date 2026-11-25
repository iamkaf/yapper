# Yapper phrase table, version 1

A phrase table tells the runtime how to say phrases better than its letter rules can. `compile.py` writes one from a text file of lines, and `yap_phrases` hands one to the runtime. All integers are little-endian.

## Layout

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Tag: `YPH1` |
| 4 | 4 | `n`, the number of phrases |
| 8 | 4 × `n` | Each phrase's hash, strictly ascending |
| 8 + 4`n` | 4 × (`n` + 1) | Offsets into the records: the first is 0, and none is less than the one before |
| 12 + 8`n` | | The records. Record `i` runs from offset `i` to offset `i` + 1 |

The table ends exactly where the records do, so its size is 12 + 8`n` + the last offset. `yap_phrases` refuses a table that breaks any rule in this section, and the runtime then uses none.

## Phrases and their hashes

The runtime splits a text into phrases at `,` `.` `;` `:` `!` and `?`. Before that, it blanks out whatever is in brackets, `( )` or `[ ]`, nested or not, and it reads at most 1024 bytes of a text. A word is a run of ASCII letters, which may hold an apostrophe between two letters. Every other byte, digits and non-ASCII bytes included, separates words.

A phrase's hash is the 32-bit FNV-1a hash of its words, lower case, with the apostrophes left out and one space between words. "Don't (sighs) STOP" hashes as `dont stop`. A text's phrase is said from the table when the table holds its hash and the record is well made. Otherwise it is read from its letters.

## Records

A record holds the phrase's words in order. Each word is its sounds, then the byte `0xFE`. The byte `0xFF` follows the last word and ends the record. A sound is:

1. A byte: the sound's number from the list below, plus its kind × 64. Kind 0 is a consonant. A vowel's kind is 1 when it is unstressed, 2 when stressed and 3 for a secondary stress.
2. Its length, a byte, in 5 ms steps.
3. Unless its kind is 0, its pitch, a byte: 128 plus the vowel's distance from the line's mean pitch, in quarter semitones. 0 means no pitch of its own. A sound without one, consonants included, takes the pitch of the sound before it, or the phrase's first pitch.

A record is well made when its sounds' numbers are all below 40, its `0xFF` is its last byte, and it holds exactly as many words as the phrase it is found for. The runtime checks this each time it looks a phrase up. Pitch and length bytes can take any value, `0xFE` and `0xFF` included.

The sounds, by number:

| | | | | | | | | | |
|---|---|---|---|---|---|---|---|---|---|
| 0 iy | 1 ih | 2 ey | 3 eh | 4 ae | 5 aa | 6 ao | 7 ow | 8 uh | 9 uw |
| 10 ah | 11 ax | 12 er | 13 ay | 14 aw | 15 oy | 16 b | 17 d | 18 g | 19 p |
| 20 t | 21 k | 22 f | 23 v | 24 th | 25 dh | 26 s | 27 z | 28 sh | 29 zh |
| 30 hh | 31 ch | 32 jh | 33 m | 34 n | 35 ng | 36 l | 37 r | 38 w | 39 y |

These are the CMU Pronouncing Dictionary's phones, as flite names them. Numbers 0 to 15 are vowels and diphthongs, and the rest are consonants.

## How a voice uses a record

A voice scales the record's lengths by its pace and its pitches by its range. At a pace of 76, the sounds are as long as the record says. At a range of 135, the tune is as wide as in the recording the record was taken from. The record replaces the letter rules' sounds, lengths and tune, but each syllable's loudness, the pauses and everything after them work as they do for letters.

## Versions

Version 1 is the only version. A change that the version 1 reader would read differently needs a new tag.
