# huffman — Design

A Huffman file compressor in C. Command-line tool, standard C only, no
external libraries. One source file, one gcc command to build. The developer
builds exactly this; anything not listed here is out of scope.

## 1. Overview

- Input: one file. Output: one file. No stdin/stdout streaming, no archives.
- `compress` reads the input twice (pass 1: count byte frequencies; pass 2:
  encode). `decompress` reads the `.huff` file once, streaming.
- Memory is bounded: the tree has at most 511 nodes, I/O is chunked. The
  program never loads a whole file into memory and never allocates based on
  a size claimed in a header.
- Only headers used: `stdio.h`, `stdlib.h`, `stdint.h`, `string.h`.

## 2. CLI syntax

```
huffman compress <input> <output.huff>
huffman decompress <input.huff> <output>
```

Rules:

- Exactly 3 arguments after the program name (`argc == 4`). Fewer or more
  arguments, or a mode other than exactly `compress` or `decompress`
  (case-sensitive), prints the usage text to stderr and exits 1.
- The usage text uses the literal name `huffman`, not `argv[0]`:

```
Usage: huffman compress <input> <output.huff>
       huffman decompress <input.huff> <output>
```

- The `.huff` extension in the usage is conventional only. Neither mode
  checks file extensions; `decompress` validates by magic bytes (§3).
- An existing output file is truncated and overwritten. No prompt, no
  refuse-if-exists behavior.
- If the input and output paths are the same string, the program errors
  out before touching any file (see §7). This check is a plain string
  comparison; the same file reached via two different paths (for example
  `x` vs `./x`) is not detected, and that limitation is accepted.
- Input is assumed to be a stable, seekable, regular file. `compress`
  rewinds the input between passes; if the rewind fails, that is an error
  (see §7). Pipes and special files are not supported.

## 3. The .huff file format

All multi-byte integers are unsigned little-endian, written and read with
explicit byte shifts (portable, not a raw memory dump). Total header size
is fixed: 2065 bytes.

| Bytes | Content |
| ----- | ------- |
| 0–3 | Magic: ASCII `HUFF` (`0x48 0x55 0x46 0x46`) |
| 4 | Version: `1` |
| 5–12 | Original size: `uint64_t`, little-endian |
| 13–2060 | Frequency table: 256 × `uint64_t`, little-endian, index = byte value |
| 2061–2064 | CRC32 of the original data: `uint32_t`, little-endian (see §4.1) |
| 2065+ | Bitstream (see below) |

Bitstream rules:

- Bits are packed MSB-first: the first code bit goes into bit 7 of the
  first bitstream byte, the next into bit 6, and so on.
- Codes are written left to right (first code bit first).
- The final byte is padded with zero bits to a byte boundary.
- The decoder stops after producing exactly `original size` bytes. Padding
  bits are never decoded. Trailing bytes after the final partial byte, if
  any, are ignored.

Why the full 256-entry table instead of a sparse list: the header is
fixed-size, so parsing is trivial (read 2065 bytes, done) and there is no
variable-length header parsing to get wrong. The cost is 2048 bytes of
header on every file, which the size report shows plainly (§6) — small
files will report growth, and that is honest, not hidden.

Tree and code rules (both sides run the identical algorithm, so the trees
match exactly):

- Leaves are created only for byte values with frequency > 0, in byte
  order (0–255). Internal nodes are appended in creation order.
- A node is: `weight` (`uint64_t`), `symbol` (byte value, or -1 for an
  internal node), `left` and `right` child indices. At most 511 nodes.
- Tree build: repeatedly combine the two smallest-weight nodes that have
  no parent yet into a new internal node whose weight is their sum.
  Ties are broken by smaller node index. No heap is needed; a linear scan
  over at most 511 nodes is trivial.
- Code assignment is a depth-first walk from the root: left edge = 0 bit,
  right edge = 1 bit. A code is stored as its length (0–255) plus its bits
  as an array of 0/1 bytes (one array element per bit). Storing codes this
  way, instead of packed into a `uint64_t`, is deliberate: the longest
  possible Huffman code over 256 symbols is 255 bits, which does not fit
  in 64 bits.
- The decoder never materializes codes. It rebuilds the tree from the
  frequency table and walks it bit by bit (see §4).

## 4. Algorithms

### 4.1 Compress

1. Open the input. Count frequencies and compute the CRC32 of the raw
   input bytes in one streaming pass (8 KB read buffer; the exact
   buffer size is an implementation detail; the CRC table is generated
   at startup).
2. Rewind the input (failure here is an error, §7). Build the tree and
   the code table from the frequencies.
3. Open the output. Write the header (§3), including the CRC32 computed
   in pass 1.
4. Second pass: for each input byte, write its code bits through the bit
   writer. Flush the final partial byte with zero padding.
5. Check every write and the final close. Report sizes (§6).

### 4.2 Decompress

1. Open the input. Read and validate the full 2065-byte header, in this
   order: file must be at least 2065 bytes; magic must be `HUFF`; version
   must be `1`; every frequency must be ≤ the original size, and the
   256 frequencies must sum to the original size (the sum is accumulated
   with overflow detection; overflow is an inconsistency). Any failure
   here is an error, and the output file is not created yet. The CRC32
   itself is checked after decoding (step 4), since it covers the
   decoded bytes.
2. If the original size is 0, check the header CRC32 against the CRC32
   of empty input (0): on mismatch, checksum error with no output file;
   otherwise write an empty output file and succeed (no tree is built,
   no bits are read).
3. Otherwise rebuild the tree from the frequencies and decode with this
   loop, which also handles the single-symbol case with no special case:

```
node = root
while (bytes_written < original_size) {
    if (node is a leaf) { write node.symbol; bytes_written++; node = root; continue; }
    bit = read_bit();
    if (bit == EOF) -> truncated bitstream error (§7)
    node = bit ? node.right : node.left;
}
```

4. After the loop, compute the CRC32 of the decoded bytes and compare it
   with the header's CRC32. On mismatch, remove the partial output file
   and exit 1 with the checksum error (§7). No sizes are reported.
5. Check every write and the final close. Report sizes (§6).

### 4.3 Single distinct byte

If the input contains exactly one distinct byte, the tree is a single
leaf. Its code is defined as one `0` bit. The encoder therefore writes
one zero bit per input byte; the decoder's loop (§4.2) emits the symbol
without consuming bits, so those bits are simply never read. Both sides
agree, and the round-trip holds.

### 4.4 Empty file

Frequencies are all zero, original size is 0, bitstream is empty. The
output is exactly the 2065-byte header. Decompression writes an empty
file and succeeds.

## 5. File layout and internal parts

One file: `huffman.c`. No header file, no Makefile. Build command:

```
gcc -O2 -o huffman huffman.c
```

On Windows (MinGW) the same command produces `huffman.exe`:

```
gcc -O2 -o huffman.exe huffman.c
```

Internal parts, in order (all functions `static` except `main`; exact
function names are the developer's choice, responsibilities are not):

1. Constants: magic `HUFF`, version `1`, header size 2065, max nodes 511.
2. Little-endian integer read/write helpers (explicit shifts;
   `uint64_t` for sizes and frequencies, `uint32_t` for the CRC32),
   plus the CRC32 function (table generated at startup, computed over
   raw bytes in order).
3. Bit writer: holds a `FILE *`, a partial byte, and a bit count;
   `write_bit` buffers MSB-first, `flush` pads the last byte with zeros.
4. Bit reader: holds a `FILE *`, a current byte, and bits remaining;
   `read_bit` returns 0, 1, or -1 on end of file.
5. Tree: node array, `build_tree` (two-smallest selection with the
   index tie-break from §3), `build_codes` (DFS, left = 0, right = 1).
6. `compress`: two-pass flow from §4.1.
7. `decompress`: validate-then-decode flow from §4.2.
8. `main`: argument check and usage, same-path check, dispatch,
   success reporting (§6), and failure cleanup (§7).

## 6. Reporting

On success only, print exactly these three lines to stdout, in this
order, for both modes. `Original` is the uncompressed size,
`Compressed` is the `.huff` file size on disk. Sizes are plain numbers
with no thousands separators.

```
Original: 102400 bytes
Compressed: 51206 bytes
Ratio: 50.01%
```

- Ratio = `100.0 * compressed / original`, printed with two decimals
  and a `%` sign. Below 100 means the file shrank; above 100 means it
  grew, and growth is reported plainly, never hidden or clamped.
- If the original size is 0, the third line is exactly:
  `Ratio: n/a (empty input)`
- For `compress`, the sizes come from the real files (input size and
  output size after closing). For `decompress`, `Original` is the
  header's original size (which must equal the bytes written) and
  `Compressed` is the input `.huff` size on disk.
- Nothing else is printed on success. No progress output, no "Done!".

## 7. Error behavior

- Exit codes: `0` on success, `1` on any failure. There is one failure
  code; no distinction between usage, I/O, and corrupt-input failures.
- All error messages go to stderr with the exact style
  `huffman: <message>` (one line, no extra text). Argument problems
  print only the usage text from §2, not an additional error line.
- Validation order is deliberate: for `decompress`, the header is fully
  validated before the output file is opened, so header errors never
  create an output file. For `compress`, the input is fully readable
  (pass 1 complete, rewind successful) before the output is opened.
- If a failure happens after the output file was created (mid-stream
  read/write error, truncated bitstream, checksum mismatch, close failure), the partial
  output file is removed before exiting. Removal is best-effort: if it
  fails, the program still exits 1 with the original error. Note that a
  pre-existing output file is destroyed by truncation and then removed
  on failure, so it ends up not existing at all — do not point the
  output at a file you want to keep.

Error catalog (message after the `huffman: ` prefix):

| Situation | Message |
| --------- | ------- |
| Wrong argument count, unknown mode | usage text only (§2) |
| Input and output paths are the same string | `input and output must be different files` |
| Input cannot be opened | `cannot open input file '<path>'` |
| Output cannot be opened | `cannot open output file '<path>'` |
| Read failure on input (including rewind failure) | `error reading input file '<path>'` |
| Write or close failure on output | `error writing output file '<path>'` |
| File shorter than 2065 bytes | `truncated file (header incomplete)` |
| Magic bytes are not `HUFF` | `not a huffman file (bad magic)` |
| Version byte is not `1` | `unsupported version <n>` (n = the byte read) |
| Frequencies do not sum to the original size, a frequency exceeds the original size, or the sum overflows | `inconsistent header (frequencies do not match original size)` |
| Bitstream ends before `original size` bytes are decoded | `truncated file (bitstream ended early)` |
| CRC32 of decoded bytes does not match the header CRC32 | `checksum mismatch (file is corrupt)` |

Integrity: the CRC32 catches accidental corruption — a flipped bit in
the bitstream (or in the header's CRC field itself) fails the check and
exits 1 with no output file left behind. CRC32 is not cryptographic:
it proves the file was not damaged by accident, not that it was not
tampered with deliberately. A collision crafted on purpose would still
pass, and that is accepted for this tool.

## 8. Edge cases the implementation must handle

- Empty file: 2065-byte header-only output; decompresses to empty;
  ratio line is `n/a` (§6).
- File with a single distinct byte: code is one `0` bit (§4.3);
  output size is 2065 + ceil(original / 8) bytes.
- File containing all 256 byte values: tree has 255 internal nodes;
  code lengths up to 255 bits must work via the 0/1-byte code storage
  (§3), never via a 64-bit code value.
- Already-compressed or random data: output grows (header plus roughly
  one bit per bit of entropy). No fallback "store mode" exists; the
  program reports the growth (§6) and exits 0. Growth is not an error.
- Large files: frequencies and sizes are `uint64_t`; I/O is streamed,
  so a multi-gigabyte file needs no more memory than a small one.
- A `.huff` file with trailing garbage bytes after the final partial
  byte: ignored; decoding stops at the original size.
- Output path in a nonexistent directory, unreadable input file,
  directory passed as input: all surface as the open/read errors in §7
  (a directory open may succeed but its first read fails; read errors
  must be checked, not assumed away).
- A `.huff` file whose bitstream or CRC field was damaged: the CRC32
  check fails after decoding, the program exits 1 with the checksum
  error, and no output file is left behind.

## 9. Test plan

Build once: `gcc -O2 -o huffman huffman.c`

Generate the test files (any method producing the same bytes is fine):

```sh
: > empty.bin
printf 'A' > onebyte.bin
head -c 10000 /dev/zero | tr '\0' 'A' > single.bin
for i in $(seq 1 2000); do echo "the quick brown fox jumps over the lazy dog"; done > text.txt
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256))*10)" > allbytes.bin
head -c 100000 /dev/urandom > random.bin
cat text.txt text.txt text.txt text.txt text.txt > large.txt
```

Round-trip tests — for EACH of `empty.bin`, `onebyte.bin`, `single.bin`,
`text.txt`, `allbytes.bin`, `random.bin`, `large.txt`, all of these must
hold:

1. `huffman compress <in> <in>.huff` exits 0.
2. `huffman decompress <in>.huff <in>.back` exits 0.
3. `cmp <in> <in>.back` exits 0 (byte-for-byte identical).
4. The reported `Original` equals the input's real byte count and the
   reported `Compressed` equals the `.huff` file's real byte count
   (check with `wc -c`).
5. The reported ratio equals `100.0 * compressed / original` to two
   decimals (except `empty.bin`, where it must read `n/a`).

Effectiveness checks (not correctness, but they must hold):

- `text.txt`: compressed size is smaller than the original size (the
  repetitive text must beat the 2065-byte header).
- `random.bin`: compressed size is greater than or equal to the
  original size, and the run still exits 0 (growth is reported, not a
  failure).

Corruption tests — each must exit 1, print exactly one error line to
stderr, and leave NO output file behind (check with `test ! -e out`):

1. Truncated header: `head -c 100 text.txt.huff > cut.huff`,
   then decompress it.
2. Bad magic: flip the first byte of a valid `.huff` file, then
   decompress it.
3. Wrong version: set byte 4 (offset 4) of a valid `.huff` file to `2`,
   then decompress it.
4. Truncated bitstream: `head -c <half the file size> text.txt.huff`
   (a valid header but a cut bitstream), then decompress it.
5. Flipped bitstream bit: flip one bit past the header of a valid
   `.huff` file (read the file with python3, XOR the byte at offset
   3000 with `0x01`, write it back), then decompress it. Must exit 1
   with the checksum error and leave no output file behind.

Error tests — each must exit 1 with the §7 message or usage:

1. Nonexistent input file for both modes.
2. Wrong argument counts: no arguments, one argument, and five
   arguments; unknown mode (`huffman squeeze a b`).
3. Input and output paths identical (`huffman compress x x`): exit 1
   and the input file must be untouched.

## 10. Explicitly out of scope

No stdin/stdout streaming. No multiple files or archives (one input,
one output). No compression levels or flags of any kind. No cryptographic signing or tamper-proofing (CRC32 only). No "store mode" fallback for incompressible data. No progress,
verbose, or quiet output. No GUI. No Makefile (a single gcc command is
the build). No external libraries. No in-place operation. No directory
recursion. No encryption. No threads.

## 11. Example sessions

Successful compression of repetitive text:

```
$ huffman compress story.txt story.huff
Original: 102400 bytes
Compressed: 51206 bytes
Ratio: 50.01%
```

Successful decompression (same three lines, sizes from the other side):

```
$ huffman decompress story.huff story.out
Original: 102400 bytes
Compressed: 51206 bytes
Ratio: 50.01%
```

Empty file (ratio is not defined):

```
$ huffman compress empty.bin empty.huff
Original: 0 bytes
Compressed: 2065 bytes
Ratio: n/a (empty input)
```

Random data grows, and the program says so instead of failing:

```
$ huffman compress random.bin random.huff
Original: 100000 bytes
Compressed: 102065 bytes
Ratio: 102.06%
```

Failures (all to stderr, exit 1, no output file left behind):

```
$ huffman compress missing.txt missing.huff
huffman: cannot open input file 'missing.txt'

$ huffman decompress story.txt story.out
huffman: not a huffman file (bad magic)

$ huffman
Usage: huffman compress <input> <output.huff>
       huffman decompress <input.huff> <output>
```

## 12. Decisions (ambiguity notes — simpler option chosen each time)

- Little-endian header integers (the common desktop byte order), with
  explicit shifts so the format is identical on any machine.
- MSB-first bit packing (the most common textbook convention).
- Full 256-entry frequency table (fixed 2065-byte header) over a sparse
  table: simpler parsing wins over smaller headers for small files.
- One exit code (1) for every failure: simpler than a code per error
  class, and the message text carries the detail.
- Overwrite existing output without asking: standard CLI behavior, and
  refusing would add a flag to override the refusal.
- Usage text uses the literal name `huffman`: stable regardless of
  what the binary is renamed to.
- CRC32 checksum (owner's fix request): 4 bytes in the header,
  computed over the raw input during pass 1, verified after decode.
  Catches accidental bit flips; a table generated at startup keeps
  the code small. Not a cryptographic guarantee (§7).
- Same-file detection by string comparison only: detecting aliases
  would need inode comparison, which is platform-specific; the cheap
  check catches the common mistake.
- Ratio defined as `100 * compressed / original`: below 100 means
  shrank, above 100 means grew, with no clamping or special casing
  beyond the empty-file `n/a`.
