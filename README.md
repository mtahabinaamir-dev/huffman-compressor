# huffman

A Huffman file compressor in C. Command-line tool, standard C only, no
external libraries.

## Build

```sh
gcc -O2 -o huffman huffman.c
```

On Windows (MinGW) the same command produces `huffman.exe` — see
`BUILD-WINDOWS.txt` for the full setup.

## Usage

```sh
huffman compress <input> <output.huff>
huffman decompress <input.huff> <output>
```

Example:

```sh
$ huffman compress story.txt story.huff
Original: 88000 bytes
Compressed: 51815 bytes
Ratio: 58.88%
```

## How it works

- Real Huffman coding: byte frequency table, tree build,
  variable-length codes, bit-level output.
- Self-contained `.huff` format: a 2065-byte header (magic, version,
  original size, full frequency table, CRC32) followed by the bitstream,
  so a `.huff` file decompresses on its own with no sidecar files.
- Lossless: decompressing a compressed file reproduces the original
  byte for byte.
- CRC32 integrity check: a damaged file fails with a checksum error
  instead of silently decoding to wrong bytes.
- Reports original size, compressed size, and ratio on every run. A
  ratio above 100% means the file grew (random data does this) — that
  is reported, not hidden, and not treated as a failure.

See `DESIGN.md` for the full format specification.
