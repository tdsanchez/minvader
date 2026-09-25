/*
 * tag.h — xattr-based Finder tag read/write
 * Extracted from cmd/incept-tag/tag.c (library mode, no main())
 */
#ifndef TAG_H
#define TAG_H

#include <stdint.h>
#include <sys/types.h>

#define MAX_TAGS   64
#define MAX_TAGLEN 256

/* Read current Finder tags from a file's xattr into the tags array.
 * Returns 0 on success. Sets *count = 0 if no tags found. */
int read_current_tags(const char *path,
                      char tags[MAX_TAGS][MAX_TAGLEN], int *count);

/* Write tags to a file's xattr (binary plist encoding).
 * If count == 0, removes the xattr. Returns 0 on success. */
int write_current_tags(const char *path,
                       const char tags[MAX_TAGS][MAX_TAGLEN], int count);

#endif /* TAG_H */
