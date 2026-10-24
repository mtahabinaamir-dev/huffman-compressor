#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// huffman: a Huffman file compressor. Single file, standard C only.
// Usage: huffman compress <input> <output.huff>
//        huffman decompress <input.huff> <output>

#define HEADER_SIZE 2065
#define MAX_NODES 511
#define IO_BUF 8192

// One tree node. symbol >= 0 for a leaf, -1 for an internal node.
typedef struct {
    uint64_t weight;
    int symbol;
    int left;
    int right;
    int parent;
} Node;

// One code: length plus its bits as 0/1 bytes, leftmost bit first.
typedef struct {
    uint8_t len;
    uint8_t bits[256];
} Code;

// Bit writer: packs bits MSB-first into bytes.
typedef struct {
    FILE *f;
    unsigned char buf;
    int nbits;
    uint64_t bytes;
    int error;
} BitWriter;

// Bit reader: unpacks bits MSB-first. error is set on a real read error.
typedef struct {
    FILE *f;
    unsigned char buf;
    int nbits;
    int error;
} BitReader;

// CRC32 (IEEE, polynomial 0xEDB88320), table generated at startup.
static uint32_t crc_table[256];

static void crc_table_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}

// crc is the running state (starts 0xFFFFFFFF); callers apply the final
// XOR with 0xFFFFFFFF once, after all bytes are fed.
static uint32_t crc_update(uint32_t crc, const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Little-endian integer helpers, explicit shifts (portable, no raw dumps).
static void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint32_t get_u32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void bw_put(BitWriter *w, int bit) {
    w->buf = (unsigned char)((w->buf << 1) | (bit & 1));
    w->nbits++;
    if (w->nbits == 8) {
        if (fputc(w->buf, w->f) == EOF)
            w->error = 1;
        else
            w->bytes++;
        w->buf = 0;
        w->nbits = 0;
    }
}

// Pad the final partial byte with zero bits and write it out.
static void bw_flush(BitWriter *w) {
    if (w->nbits > 0) {
        w->buf = (unsigned char)(w->buf << (8 - w->nbits));
        if (fputc(w->buf, w->f) == EOF)
            w->error = 1;
        else
            w->bytes++;
        w->buf = 0;
        w->nbits = 0;
    }
}

// Returns 0, 1, or -1 on end of file (or read error, flagged in error).
static int br_get(BitReader *r) {
    if (r->nbits == 0) {
        int c = fgetc(r->f);
        if (c == EOF) {
            if (ferror(r->f))
                r->error = 1;
            return -1;
        }
        r->buf = (unsigned char)c;
        r->nbits = 8;
    }
    r->nbits--;
    return (r->buf >> r->nbits) & 1;
}

// Build the tree from frequencies. Returns the root node index, or -1
// when there are no symbols (empty input). Ties break by smaller index:
// strict < comparisons while scanning in index order keep the earlier
// node whenever weights are equal.
static int build_tree(const uint64_t *freq, Node *nodes) {
    int count = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            nodes[count].weight = freq[i];
            nodes[count].symbol = i;
            nodes[count].left = -1;
            nodes[count].right = -1;
            nodes[count].parent = -1;
            count++;
        }
    }
    if (count == 0)
        return -1;
    while (1) {
        int first = -1;
        int second = -1;
        for (int i = 0; i < count; i++) {
            if (nodes[i].parent != -1)
                continue;
            if (first == -1 || nodes[i].weight < nodes[first].weight) {
                second = first;
                first = i;
            } else if (second == -1 || nodes[i].weight < nodes[second].weight) {
                second = i;
            }
        }
        if (second == -1)
            return first;
        nodes[count].weight = nodes[first].weight + nodes[second].weight;
        nodes[count].symbol = -1;
        nodes[count].left = first;
        nodes[count].right = second;
        nodes[count].parent = -1;
        nodes[first].parent = count;
        nodes[second].parent = count;
        count++;
    }
}

// Depth-first code assignment: left edge = 0, right edge = 1. A lone
// leaf (single-symbol input) gets the one-bit code 0 by definition.
static void build_codes(const Node *nodes, int node, uint8_t *path,
                        int depth, Code *codes) {
    if (nodes[node].symbol >= 0) {
        if (depth == 0) {
            codes[nodes[node].symbol].len = 1;
            codes[nodes[node].symbol].bits[0] = 0;
        } else {
            codes[nodes[node].symbol].len = (uint8_t)depth;
            for (int i = 0; i < depth; i++)
                codes[nodes[node].symbol].bits[i] = path[i];
        }
        return;
    }
    path[depth] = 0;
    build_codes(nodes, nodes[node].left, path, depth + 1, codes);
    path[depth] = 1;
    build_codes(nodes, nodes[node].right, path, depth + 1, codes);
}

// Error helpers: each prints exactly one line to stderr and returns 1.
static int err_open_in(const char *p) {
    fprintf(stderr, "huffman: cannot open input file '%s'\n", p);
    return 1;
}

static int err_open_out(const char *p) {
    fprintf(stderr, "huffman: cannot open output file '%s'\n", p);
    return 1;
}

static int err_read_in(const char *p) {
    fprintf(stderr, "huffman: error reading input file '%s'\n", p);
    return 1;
}

static int err_write_out(const char *p) {
    fprintf(stderr, "huffman: error writing output file '%s'\n", p);
    return 1;
}

static int err_simple(const char *m) {
    fprintf(stderr, "huffman: %s\n", m);
    return 1;
}

static int err_version(unsigned v) {
    fprintf(stderr, "huffman: unsupported version %u\n", v);
    return 1;
}

// The success report: exactly three lines, plain numbers, no separators.
static void report(uint64_t orig, uint64_t comp) {
    printf("Original: %llu bytes\n", (unsigned long long)orig);
    printf("Compressed: %llu bytes\n", (unsigned long long)comp);
    if (orig == 0)
        printf("Ratio: n/a (empty input)\n");
    else
        printf("Ratio: %.2f%%\n", 100.0 * (double)comp / (double)orig);
}

static int do_compress(const char *inpath, const char *outpath) {
    FILE *in = fopen(inpath, "rb");
    if (!in)
        return err_open_in(inpath);
    uint64_t freq[256];
    memset(freq, 0, sizeof freq);
    uint64_t total = 0;
    uint32_t crc = 0xFFFFFFFFu;
    unsigned char buf[IO_BUF];
    size_t n;
    // Pass 1: count frequencies and CRC the raw bytes, streaming.
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        for (size_t i = 0; i < n; i++)
            freq[buf[i]]++;
        crc = crc_update(crc, buf, n);
        total += n;
    }
    if (ferror(in)) {
        fclose(in);
        return err_read_in(inpath);
    }
    if (fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return err_read_in(inpath);
    }
    crc ^= 0xFFFFFFFFu;
    // The input is fully readable before the output is opened.
    Node nodes[MAX_NODES];
    Code codes[256];
    memset(codes, 0, sizeof codes);
    if (total > 0) {
        uint8_t path[256];
        int root = build_tree(freq, nodes);
        build_codes(nodes, root, path, 0, codes);
    }
    FILE *out = fopen(outpath, "wb");
    if (!out) {
        fclose(in);
        return err_open_out(outpath);
    }
    BitWriter w;
    w.f = out;
    w.buf = 0;
    w.nbits = 0;
    w.bytes = 0;
    w.error = 0;
    unsigned char hdr[HEADER_SIZE];
    memcpy(hdr, "HUFF", 4);
    hdr[4] = 1;
    put_u64(hdr + 5, total);
    for (int i = 0; i < 256; i++)
        put_u64(hdr + 13 + i * 8, freq[i]);
    put_u32(hdr + 2061, crc);
    if (fwrite(hdr, 1, HEADER_SIZE, out) != HEADER_SIZE)
        goto write_fail;
    // Pass 2: encode every byte through the bit writer.
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        for (size_t i = 0; i < n; i++) {
            Code *c = &codes[buf[i]];
            for (int j = 0; j < c->len; j++)
                bw_put(&w, c->bits[j]);
        }
        if (w.error)
            goto write_fail;
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        remove(outpath);
        return err_read_in(inpath);
    }
    bw_flush(&w);
    if (w.error)
        goto write_fail;
    fclose(in);
    if (fclose(out) != 0) {
        remove(outpath);
        return err_write_out(outpath);
    }
    report(total, HEADER_SIZE + w.bytes);
    return 0;
write_fail:
    fclose(in);
    fclose(out);
    remove(outpath);
    return err_write_out(outpath);
}

static int do_decompress(const char *inpath, const char *outpath) {
    FILE *in = fopen(inpath, "rb");
    if (!in)
        return err_open_in(inpath);
    // Input size for the report, measured before the header is read.
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return err_read_in(inpath);
    }
    long insize_l = ftell(in);
    if (insize_l < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return err_read_in(inpath);
    }
    uint64_t insize = (uint64_t)insize_l;
    unsigned char hdr[HEADER_SIZE];
    size_t got = fread(hdr, 1, HEADER_SIZE, in);
    if (got < HEADER_SIZE) {
        int rd_err = ferror(in);
        fclose(in);
        if (rd_err)
            return err_read_in(inpath);
        return err_simple("truncated file (header incomplete)");
    }
    if (memcmp(hdr, "HUFF", 4) != 0) {
        fclose(in);
        return err_simple("not a huffman file (bad magic)");
    }
    if (hdr[4] != 1) {
        unsigned v = hdr[4];
        fclose(in);
        return err_version(v);
    }
    uint64_t orig = get_u64(hdr + 5);
    uint64_t freq[256];
    uint64_t sum = 0;
    for (int i = 0; i < 256; i++) {
        freq[i] = get_u64(hdr + 13 + i * 8);
        if (freq[i] > orig || sum > UINT64_MAX - freq[i]) {
            fclose(in);
            return err_simple("inconsistent header (frequencies do not match original size)");
        }
        sum += freq[i];
    }
    if (sum != orig) {
        fclose(in);
        return err_simple("inconsistent header (frequencies do not match original size)");
    }
    uint32_t want_crc = get_u32(hdr + 2061);
    // The header is fully validated before the output file is opened.
    if (orig == 0) {
        fclose(in);
        if (want_crc != 0)
            return err_simple("checksum mismatch (file is corrupt)");
        FILE *out = fopen(outpath, "wb");
        if (!out)
            return err_open_out(outpath);
        if (fclose(out) != 0) {
            remove(outpath);
            return err_write_out(outpath);
        }
        report(0, insize);
        return 0;
    }
    Node nodes[MAX_NODES];
    int root = build_tree(freq, nodes);
    FILE *out = fopen(outpath, "wb");
    if (!out) {
        fclose(in);
        return err_open_out(outpath);
    }
    BitReader r;
    r.f = in;
    r.buf = 0;
    r.nbits = 0;
    r.error = 0;
    unsigned char obuf[IO_BUF];
    size_t olen = 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t written = 0;
    int node = root;
    int failed = 0; // 0 ok, 1 truncated bitstream, 2 write failure
    while (written < orig) {
        if (nodes[node].symbol >= 0) {
            // A leaf emits without consuming a bit; this is also what
            // makes the single-symbol case work with no special case.
            obuf[olen++] = (unsigned char)nodes[node].symbol;
            written++;
            node = root;
            if (olen == sizeof obuf) {
                if (fwrite(obuf, 1, olen, out) != olen) {
                    failed = 2;
                    break;
                }
                crc = crc_update(crc, obuf, olen);
                olen = 0;
            }
            continue;
        }
        int bit = br_get(&r);
        if (bit < 0) {
            failed = 1;
            break;
        }
        node = bit ? nodes[node].right : nodes[node].left;
    }
    if (!failed && olen > 0) {
        if (fwrite(obuf, 1, olen, out) != olen)
            failed = 2;
        else
            crc = crc_update(crc, obuf, olen);
    }
    fclose(in);
    int close_failed = (fclose(out) != 0);
    if (r.error) {
        remove(outpath);
        return err_read_in(inpath);
    }
    if (failed == 1) {
        remove(outpath);
        return err_simple("truncated file (bitstream ended early)");
    }
    if (failed == 2 || close_failed) {
        remove(outpath);
        return err_write_out(outpath);
    }
    crc ^= 0xFFFFFFFFu;
    if (crc != want_crc) {
        remove(outpath);
        return err_simple("checksum mismatch (file is corrupt)");
    }
    report(orig, insize);
    return 0;
}

static void usage(void) {
    fprintf(stderr, "Usage: huffman compress <input> <output.huff>\n");
    fprintf(stderr, "       huffman decompress <input.huff> <output>\n");
}

int main(int argc, char **argv) {
    crc_table_init();
    if (argc != 4) {
        usage();
        return 1;
    }
    int comp;
    if (strcmp(argv[1], "compress") == 0)
        comp = 1;
    else if (strcmp(argv[1], "decompress") == 0)
        comp = 0;
    else {
        usage();
        return 1;
    }
    if (strcmp(argv[2], argv[3]) == 0) {
        fprintf(stderr, "huffman: input and output must be different files\n");
        return 1;
    }
    if (comp)
        return do_compress(argv[2], argv[3]);
    return do_decompress(argv[2], argv[3]);
}
