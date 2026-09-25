/*
 * tag.c — xattr-based Finder tag read/write (library mode)
 * Extracted from cmd/incept-tag/tag.c — same bplist codec, no main().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tag.h"

/*
 * Platform abstraction for xattr read/write/delete.
 */
#ifdef __APPLE__
#  include <sys/xattr.h>
#  define XATTR_KEY "com.apple.metadata:_kMDItemUserTags"
#  define xattr_get(path, key, buf, sz) \
       getxattr((path), (key), (buf), (sz), 0, 0)
#  define xattr_set(path, key, buf, sz) \
       setxattr((path), (key), (buf), (sz), 0, 0)
#  define xattr_del(path, key) \
       removexattr((path), (key), 0)
#elif defined(_WIN32)
#  define XATTR_KEY ""
#  define xattr_get(path, key, buf, sz) (-1)
#  define xattr_set(path, key, buf, sz) (-1)
#  define xattr_del(path, key) (-1)
#else
#  include <sys/xattr.h>
#  ifndef XATTR_KEY
#  define XATTR_KEY "user.com.apple.metadata:_kMDItemUserTags"
#  endif
#  define xattr_get(path, key, buf, sz) \
       getxattr((path), (key), (buf), (sz))
#  define xattr_set(path, key, buf, sz) \
       setxattr((path), (key), (buf), (sz), 0)
#  define xattr_del(path, key) \
       removexattr((path), (key))
#endif

#define BPLIST_MAGIC     "bplist00"
#define BPLIST_MAGIC_LEN 8
#define MAX_XATTR  8192

/*
 * Parse a binary plist containing an array of ASCII strings.
 */
static int parse_tags(const uint8_t *buf, ssize_t len,
                      char tags[MAX_TAGS][MAX_TAGLEN], int *count) {
    *count = 0;
    if (len < 8 || memcmp(buf, BPLIST_MAGIC, BPLIST_MAGIC_LEN) != 0)
        return -1;

    if (len < 32) return -1;
    const uint8_t *trailer = buf + len - 32;
    int off_size      = trailer[6];
    int obj_size      = trailer[7];
    uint64_t count64  = 0;
    uint64_t top      = 0;
    uint64_t offset_table = 0;

    for (int i = 0; i < 8; i++) count64      = (count64      << 8) | trailer[8+i];
    for (int i = 0; i < 8; i++) top           = (top          << 8) | trailer[16+i];
    for (int i = 0; i < 8; i++) offset_table  = (offset_table << 8) | trailer[24+i];

    (void)top;
    if (count64 > MAX_TAGS) count64 = MAX_TAGS;

    uint64_t offsets[MAX_TAGS];
    for (uint64_t i = 0; i < count64; i++) {
        uint64_t pos = offset_table + i * off_size;
        if (pos + off_size > (uint64_t)len) return -1;
        offsets[i] = 0;
        for (int b = 0; b < off_size; b++)
            offsets[i] = (offsets[i] << 8) | buf[pos+b];
    }

    if (offsets[0] >= (uint64_t)len) return -1;
    uint8_t marker = buf[offsets[0]];
    if ((marker & 0xf0) != 0xa0) return -1;
    int arr_count = marker & 0x0f;
    if (arr_count == 0x0f) return -1;

    const uint8_t *refs = buf + offsets[0] + 1;
    int found = 0;
    for (int i = 0; i < arr_count && found < MAX_TAGS; i++) {
        if (refs + (i+1)*obj_size > buf + len) break;

        uint64_t ref = 0;
        for (int b = 0; b < obj_size; b++)
            ref = (ref << 8) | refs[i*obj_size+b];
        if (ref >= count64 || offsets[ref] >= (uint64_t)len) continue;

        uint8_t sm = buf[offsets[ref]];
        int str_type = sm & 0xf0;
        if (str_type != 0x50 && str_type != 0x60) continue;

        int slen = sm & 0x0f;
        uint64_t data_start = offsets[ref] + 1;

        if (slen == 0x0f) {
            if (data_start >= (uint64_t)len) continue;
            uint8_t im = buf[data_start++];
            if ((im & 0xf0) != 0x10) continue;
            int ibytes = 1 << (im & 0x0f);
            if (data_start + ibytes > (uint64_t)len) continue;
            slen = 0;
            for (int b = 0; b < ibytes; b++)
                slen = (slen << 8) | buf[data_start++];
        }

        if (str_type == 0x60) {
            int byte_len = slen * 2;
            if (data_start + byte_len > (uint64_t)len) continue;
            const uint8_t *u16 = buf + data_start;
            char utf8[MAX_TAGLEN];
            int upos = 0;
            for (int k = 0; k < byte_len && upos < MAX_TAGLEN - 4; k += 2) {
                uint16_t cp = (u16[k] << 8) | u16[k+1];
                if (cp == '\n') break;
                if (cp < 0x80) {
                    utf8[upos++] = (char)cp;
                } else if (cp < 0x800) {
                    utf8[upos++] = 0xc0 | (cp >> 6);
                    utf8[upos++] = 0x80 | (cp & 0x3f);
                } else {
                    utf8[upos++] = 0xe0 | (cp >> 12);
                    utf8[upos++] = 0x80 | ((cp >> 6) & 0x3f);
                    utf8[upos++] = 0x80 | (cp & 0x3f);
                }
            }
            utf8[upos] = '\0';
            if (upos > 0) {
                memcpy(tags[found], utf8, upos + 1);
                found++;
            }
        } else {
            if (data_start + slen > (uint64_t)len) continue;
            const char *src = (const char *)buf + data_start;
            const char *nl = memchr(src, '\n', slen);
            int copy = nl ? (int)(nl - src) : slen;
            if (copy >= MAX_TAGLEN) copy = MAX_TAGLEN-1;
            memcpy(tags[found], src, copy);
            tags[found][copy] = '\0';
            found++;
        }
    }
    *count = found;
    return 0;
}

/*
 * Encode tag strings into binary plist.
 */
static ssize_t write_tags_bplist(const char tags[MAX_TAGS][MAX_TAGLEN], int count,
                                  uint8_t *out, size_t outsz) {
    if (count > MAX_TAGS) return -1;

    uint8_t tmp[MAX_XATTR];
    size_t pos = 0;

    memcpy(tmp, BPLIST_MAGIC, 8); pos = 8;
    tmp[pos++] = 0xa0 | (uint8_t)count;

    for (int i = 0; i < count; i++)
        tmp[pos++] = (uint8_t)(i + 1);

    uint64_t offsets[MAX_TAGS+1];
    offsets[0] = 8;
    for (int i = 0; i < count; i++) {
        offsets[i+1] = pos;
        const char *tag = tags[i];
        int slen = (int)strlen(tag);
        if (slen >= MAX_TAGLEN) slen = MAX_TAGLEN-1;

        int needs_utf16 = 0;
        for (int k = 0; k < slen; k++) {
            if ((uint8_t)tag[k] > 0x7f) { needs_utf16 = 1; break; }
        }

        if (needs_utf16) {
            uint16_t u16buf[MAX_TAGLEN];
            int u16len = 0;
            for (int k = 0; k < slen; ) {
                uint32_t cp;
                uint8_t c = (uint8_t)tag[k];
                if (c < 0x80) { cp = c; k += 1; }
                else if ((c & 0xe0) == 0xc0 && k+1 < slen) {
                    cp = ((c & 0x1f) << 6) | ((uint8_t)tag[k+1] & 0x3f); k += 2;
                } else if ((c & 0xf0) == 0xe0 && k+2 < slen) {
                    cp = ((c & 0x0f) << 12) | (((uint8_t)tag[k+1] & 0x3f) << 6) | ((uint8_t)tag[k+2] & 0x3f); k += 3;
                } else { cp = '?'; k += 1; }
                if (cp < 0x10000) u16buf[u16len++] = (uint16_t)cp;
            }
            if (u16len < 15) {
                tmp[pos++] = 0x60 | (uint8_t)u16len;
            } else if (u16len < 256) {
                tmp[pos++] = 0x6f;
                tmp[pos++] = 0x10;
                tmp[pos++] = (uint8_t)u16len;
            } else {
                tmp[pos++] = 0x6f;
                tmp[pos++] = 0x11;
                tmp[pos++] = (uint8_t)(u16len >> 8);
                tmp[pos++] = (uint8_t)u16len;
            }
            for (int k = 0; k < u16len; k++) {
                tmp[pos++] = (uint8_t)(u16buf[k] >> 8);
                tmp[pos++] = (uint8_t)(u16buf[k]);
            }
        } else {
            if (slen < 15) {
                tmp[pos++] = 0x50 | (uint8_t)slen;
            } else if (slen < 256) {
                tmp[pos++] = 0x5f;
                tmp[pos++] = 0x10;
                tmp[pos++] = (uint8_t)slen;
            } else {
                tmp[pos++] = 0x5f;
                tmp[pos++] = 0x11;
                tmp[pos++] = (uint8_t)(slen >> 8);
                tmp[pos++] = (uint8_t)slen;
            }
            memcpy(tmp+pos, tag, slen); pos += slen;
        }
    }

    uint64_t ot = pos;
    int off_size = (ot + (uint64_t)(count+1)*2 + 32 < 256) ? 1 : 2;
    for (int i = 0; i <= count; i++) {
        if (off_size == 1) {
            tmp[pos++] = (uint8_t)offsets[i];
        } else {
            tmp[pos++] = (uint8_t)(offsets[i] >> 8);
            tmp[pos++] = (uint8_t)(offsets[i]);
        }
    }

    memset(tmp+pos, 0, 6); pos += 6;
    tmp[pos++] = (uint8_t)off_size;
    tmp[pos++] = 1;
    for (int i = 7; i >= 0; i--) tmp[pos++] = (uint8_t)(((uint64_t)(count+1)) >> (i*8));
    for (int i = 7; i >= 0; i--) tmp[pos++] = 0;
    for (int i = 7; i >= 0; i--) tmp[pos++] = (uint8_t)(ot >> (i*8));

    if (pos > outsz) return -1;
    memcpy(out, tmp, pos);
    return (ssize_t)pos;
}

int read_current_tags(const char *path,
                      char tags[MAX_TAGS][MAX_TAGLEN], int *count) {
    uint8_t buf[MAX_XATTR];
    ssize_t n = xattr_get(path, XATTR_KEY, buf, sizeof(buf));
    if (n <= 0) { *count = 0; return 0; }
    if (parse_tags(buf, n, tags, count) < 0) { *count = 0; }
    return 0;
}

int write_current_tags(const char *path,
                       const char tags[MAX_TAGS][MAX_TAGLEN], int count) {
    uint8_t buf[MAX_XATTR];
    ssize_t n = write_tags_bplist(tags, count, buf, sizeof(buf));
    if (n < 0) { fprintf(stderr, "tag: plist encode failed\n"); return 1; }
    if (count == 0) {
        xattr_del(path, XATTR_KEY);
        return 0;
    }
    if (xattr_set(path, XATTR_KEY, buf, n) < 0) {
        perror("tag: setxattr");
        return 1;
    }
    return 0;
}
