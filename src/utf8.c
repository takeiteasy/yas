/*
 * utf8.c - UTF-8 encoding/decoding utilities
 *
 * Provides functions for working with UTF-8 encoded strings:
 * - Decode UTF-8 bytes to Unicode codepoints
 * - Encode Unicode codepoints to UTF-8 bytes
 * - Count codepoints in UTF-8 string
 * - Navigate UTF-8 strings by codepoint index
 */

#include "yas.h"

/*
 * Decode a single UTF-8 character from the string.
 * Returns the number of bytes consumed (1-4), or 0 on error.
 * The decoded codepoint is stored in *cp.
 */
int yas_utf8_decode(const char *s, int32_t *cp)
{
    unsigned char c = (unsigned char)s[0];

    if (c == 0) {
        *cp = 0;
        return 0;
    }

    /* ASCII (0xxxxxxx) */
    if (c < 0x80) {
        *cp = c;
        return 1;
    }

    /* Invalid start byte */
    if (c < 0xC0) {
        *cp = 0xFFFD; /* Replacement character */
        return 1;
    }

    /* 2-byte sequence (110xxxxx 10xxxxxx) */
    if (c < 0xE0) {
        if ((s[1] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }

    /* 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx) */
    if (c < 0xF0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }

    /* 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
    if (c < 0xF8) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }

    /* Invalid byte */
    *cp = 0xFFFD;
    return 1;
}

/*
 * Encode a Unicode codepoint to UTF-8.
 * Returns the number of bytes written (1-4).
 * The output buffer must have at least 4 bytes available.
 */
int yas_utf8_encode(int32_t cp, char *out)
{
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        /* Invalid codepoint, encode replacement character */
        cp = 0xFFFD;
    }

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }

    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }

    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }

    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * Count the number of Unicode codepoints in a UTF-8 string.
 */
size_t yas_utf8_strlen(const char *s)
{
    size_t len = 0;
    int32_t cp;

    while (*s) {
        int n = yas_utf8_decode(s, &cp);
        if (n == 0)
            break;
        s += n;
        len++;
    }

    return len;
}

/*
 * Get the codepoint at a given index (0-indexed).
 * Returns -1 if index is out of bounds.
 */
int32_t yas_utf8_codepoint_at(const char *s, size_t index)
{
    int32_t cp;
    size_t i = 0;

    while (*s) {
        int n = yas_utf8_decode(s, &cp);
        if (n == 0)
            break;
        if (i == index)
            return cp;
        s += n;
        i++;
    }

    return -1;
}

/*
 * Get pointer to the start of the codepoint at a given index.
 * Returns NULL if index is out of bounds.
 */
const char *yas_utf8_index_ptr(const char *s, size_t index)
{
    int32_t cp;
    size_t i = 0;

    while (*s) {
        if (i == index)
            return s;
        int n = yas_utf8_decode(s, &cp);
        if (n == 0)
            break;
        s += n;
        i++;
    }

    return (i == index) ? s : NULL;
}
