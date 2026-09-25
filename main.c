/*
 * minvader — Pure C media server with SQLite warm start
 *
 * Based on minvader. Reads cache-*.db files written by cache-builder.
 * Supports: --warm (SQLite cache), --stdin (pipe), --port, file serving,
 *           tag CRUD, gallery/viewer, sort, search.
 *
 * Build:
 *   cc -O2 -o minvader main.c tag.c -lsqlite3 -framework CoreServices
 *
 * Usage:
 *   ./minvader --warm --port=9113
 *   find /path -type f | ./minvader --stdin --port=8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sqlite3.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define strcasecmp  _stricmp
#  define strncasecmp _strnicmp
   typedef int socklen_t;
#  include <process.h>
   static char *strcasestr(const char *h, const char *n) {
       size_t nl = strlen(n);
       for (; *h; h++) if (_strnicmp(h, n, nl) == 0) return (char *)h;
       return NULL;
   }
   static inline ssize_t sock_write(int fd, const void *b, size_t n) { return send(fd,(const char*)b,(int)n,0); }
   static inline ssize_t sock_read(int fd, void *b, size_t n) { return recv(fd,(char*)b,(int)n,0); }
   static inline int sock_close(int fd) { return closesocket(fd); }
#else
#  include <strings.h>
#  include <unistd.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <dirent.h>
#  include <pthread.h>
#  define sock_write(fd,b,n) write((fd),(b),(n))
#  define sock_read(fd,b,n)  read((fd),(b),(n))
#  define sock_close(fd)     close(fd)
#endif

/* tag.c — xattr read/write for Finder tags */
#include "tag.h"

/* md4c — CommonMark + GFM markdown → HTML renderer */
#include "md4c-html.h"

/* ── Configuration ──────────────────────────────────────────────────── */
#define MAX_FILES      500000
#define MAX_PATH_LEN   4096
#define MAX_CATEGORIES 8192
#define PAGE_SIZE      50
#define MAX_REQ_SIZE   (1024 * 1024)  /* 1MB max request body */
#define MAX_HEADERS    64
#define LISTEN_BACKLOG 128

/* ── Data structures ────────────────────────────────────────────────── */
typedef struct {
    char   *path;       /* absolute path (owned, malloc'd) */
    char   *name;       /* points into path (after last /) */
    char  **tags;       /* array of tag strings */
    int     ntags;
    int64_t size;
    time_t  mtime;
    time_t  birthtime;
    int64_t db_id;      /* file_id from cache DB (0 if stdin mode) */
} file_entry_t;

typedef struct {
    char  *name;        /* category/tag name (owned) */
    int   *file_indices;/* indices into g_files[] */
    int    count;
    int    cap;
} category_t;

/* ── Global state ───────────────────────────────────────────────────── */
static file_entry_t *g_files;
static int           g_nfiles;
static int           g_files_cap;

static category_t   *g_categories;
static int           g_ncategories;

static int           g_port = 8080;
static int           g_server_fd = -1;
static char          g_cache_dbpath[1024];

/* ── Dynamic buffer (from dets-watchd pattern) ──────────────────────── */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_init(buf_t *b) {
    b->cap = 4096;
    b->data = malloc(b->cap);
    b->len = 0;
    b->data[0] = '\0';
}

static void buf_ensure(buf_t *b, size_t extra) {
    while (b->len + extra + 1 > b->cap) {
        b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
}

static void buf_append(buf_t *b, const char *s, size_t n) {
    buf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_appends(buf_t *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static void buf_appendf(buf_t *b, const char *fmt, ...) {
    char tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, (size_t)n);
}

static void buf_append_json_str(buf_t *b, const char *s) {
    buf_append(b, "\"", 1);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\n': buf_append(b, "\\n", 2);  break;
        case '\r': buf_append(b, "\\r", 2);  break;
        case '\t': buf_append(b, "\\t", 2);  break;
        default:
            if ((unsigned char)*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                buf_append(b, esc, 6);
            } else {
                buf_append(b, p, 1);
            }
        }
    }
    buf_append(b, "\"", 1);
}

static void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* ── URL decoding ───────────────────────────────────────────────────── */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dstsz) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dstsz - 1; si++) {
        if (src[si] == '%' && src[si+1] && src[si+2]) {
            int hi = hex_val(src[si+1]);
            int lo = hex_val(src[si+2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+')
            dst[di++] = ' ';
        else
            dst[di++] = src[si];
    }
    dst[di] = '\0';
}

static void url_encode(buf_t *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            buf_append(b, (const char *)p, 1);
        } else {
            char enc[4] = {'%', hex[*p >> 4], hex[*p & 0xf], 0};
            buf_append(b, enc, 3);
        }
    }
}

/* ── HTML escaping ──────────────────────────────────────────────────── */
static void html_escape(buf_t *b, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&':  buf_appends(b, "&amp;");  break;
        case '<':  buf_appends(b, "&lt;");   break;
        case '>':  buf_appends(b, "&gt;");   break;
        case '"':  buf_appends(b, "&quot;"); break;
        case '\'': buf_appends(b, "&#39;");  break;
        default:   buf_append(b, p, 1);      break;
        }
    }
}

/* ── MIME type lookup ───────────────────────────────────────────────── */
static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++; /* skip the dot */

    /* images */
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "png") == 0)  return "image/png";
    if (strcasecmp(ext, "gif") == 0)  return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    if (strcasecmp(ext, "svg") == 0)  return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0)  return "image/x-icon";
    if (strcasecmp(ext, "bmp") == 0)  return "image/bmp";
    if (strcasecmp(ext, "tiff") == 0 || strcasecmp(ext, "tif") == 0) return "image/tiff";
    if (strcasecmp(ext, "heic") == 0) return "image/heic";
    if (strcasecmp(ext, "heif") == 0) return "image/heif";
    if (strcasecmp(ext, "avif") == 0) return "image/avif";

    /* video */
    if (strcasecmp(ext, "mp4") == 0)  return "video/mp4";
    if (strcasecmp(ext, "mov") == 0)  return "video/quicktime";
    if (strcasecmp(ext, "m4v") == 0)  return "video/x-m4v";
    if (strcasecmp(ext, "avi") == 0)  return "video/x-msvideo";
    if (strcasecmp(ext, "mkv") == 0)  return "video/x-matroska";
    if (strcasecmp(ext, "webm") == 0) return "video/webm";

    /* audio */
    if (strcasecmp(ext, "mp3") == 0)  return "audio/mpeg";
    if (strcasecmp(ext, "wav") == 0)  return "audio/wav";
    if (strcasecmp(ext, "ogg") == 0)  return "audio/ogg";
    if (strcasecmp(ext, "flac") == 0) return "audio/flac";
    if (strcasecmp(ext, "m4a") == 0)  return "audio/mp4";
    if (strcasecmp(ext, "aac") == 0)  return "audio/aac";

    /* documents */
    if (strcasecmp(ext, "pdf") == 0)  return "application/pdf";
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0)  return "text/css; charset=utf-8";
    if (strcasecmp(ext, "js") == 0)   return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, "json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, "xml") == 0)  return "application/xml; charset=utf-8";
    if (strcasecmp(ext, "csv") == 0)  return "text/csv; charset=utf-8";

    /* text */
    if (strcasecmp(ext, "txt") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "md") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "c") == 0)    return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "h") == 0)    return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "go") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "py") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "sh") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "yml") == 0 || strcasecmp(ext, "yaml") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "toml") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "log") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "conf") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "ini") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "cfg") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "erl") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "ex") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "swift") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "rs") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "rb") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "pl") == 0)   return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "java") == 0) return "text/plain; charset=utf-8";

    /* archives */
    if (strcasecmp(ext, "zip") == 0)  return "application/zip";
    if (strcasecmp(ext, "gz") == 0)   return "application/gzip";
    if (strcasecmp(ext, "tar") == 0)  return "application/x-tar";

    return "application/octet-stream";
}

static int is_text_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    ext++;
    const char *text_exts[] = {
        "txt", "md", "c", "h", "go", "py", "sh", "yml", "yaml",
        "toml", "log", "conf", "ini", "cfg", "erl", "ex", "swift",
        "rs", "rb", "pl", "java", "js", "css", "json", "xml",
        "csv", "html", "htm", "sql", "r", "m", "tex", "bib",
        NULL
    };
    for (int i = 0; text_exts[i]; i++) {
        if (strcasecmp(ext, text_exts[i]) == 0) return 1;
    }
    return 0;
}

static int is_image_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    ext++;
    const char *img_exts[] = {
        "jpg", "jpeg", "png", "gif", "webp", "svg", "bmp",
        "tiff", "tif", "heic", "heif", "avif", "ico", NULL
    };
    for (int i = 0; img_exts[i]; i++) {
        if (strcasecmp(ext, img_exts[i]) == 0) return 1;
    }
    return 0;
}

static int is_video_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    ext++;
    const char *vid_exts[] = {
        "mp4", "mov", "m4v", "avi", "mkv", "webm", NULL
    };
    for (int i = 0; vid_exts[i]; i++) {
        if (strcasecmp(ext, vid_exts[i]) == 0) return 1;
    }
    return 0;
}

static int is_markdown_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".md") == 0 || strcasecmp(ext, ".markdown") == 0);
}

static int is_rtf_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".rtf") == 0);
}

static int is_webarchive_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".webarchive") == 0);
}

static int is_convertible_ext(const char *path) {
    return is_markdown_ext(path) || is_rtf_ext(path) || is_webarchive_ext(path);
}

static int is_audio_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    ext++;
    const char *aud_exts[] = {
        "mp3", "m4a", "aac", "ogg", "flac", "wav", "aiff", "wma", "opus", NULL
    };
    for (int i = 0; aud_exts[i]; i++) {
        if (strcasecmp(ext, aud_exts[i]) == 0) return 1;
    }
    return 0;
}

/* ── md4c callback for buf_t ────────────────────────────────────────── */
static void md4c_buf_append(const MD_CHAR *text, MD_SIZE size, void *userdata) {
    buf_t *b = (buf_t *)userdata;
    buf_append(b, text, size);
}

/*
 * render_markdown — convert markdown file content to styled HTML page.
 * Caller must buf_free the result.
 */
static void render_markdown(buf_t *out, const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return;

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) { fclose(fp); return; } /* 10MB limit */

    char *src = malloc(sz + 1);
    size_t n = fread(src, 1, sz, fp);
    fclose(fp);
    src[n] = '\0';

    /* Render markdown to HTML fragment */
    buf_t fragment;
    buf_init(&fragment);
    unsigned flags = MD_DIALECT_GITHUB;
    int r = md_html(src, (MD_SIZE)n, md4c_buf_append, &fragment, flags, 0);
    free(src);

    if (r != 0) { buf_free(&fragment); return; }

    /* Wrap in styled HTML document (dark theme matching the server) */
    buf_appends(out, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
    buf_appends(out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    buf_appends(out, "<style>\n");
    buf_appends(out, "body { max-width: 900px; margin: 40px auto; padding: 0 30px; font-family: -apple-system, system-ui, sans-serif; line-height: 1.6; color: #e6edf3; background: #0d1117; }\n");
    buf_appends(out, "h1,h2,h3,h4,h5,h6 { margin-top: 24px; margin-bottom: 16px; font-weight: 600; border-bottom: 1px solid #21262d; padding-bottom: 8px; }\n");
    buf_appends(out, "a { color: #58a6ff; text-decoration: none; } a:hover { text-decoration: underline; }\n");
    buf_appends(out, "code { background: #161b22; padding: 3px 6px; border-radius: 6px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 0.85em; color: #f0883e; }\n");
    buf_appends(out, "pre { background: #161b22; padding: 16px; border-radius: 6px; overflow-x: auto; line-height: 1.45; }\n");
    buf_appends(out, "pre code { background: none; padding: 0; color: #e6edf3; }\n");
    buf_appends(out, "table { border-collapse: collapse; width: 100%; margin-bottom: 16px; }\n");
    buf_appends(out, "th,td { border: 1px solid #30363d; padding: 8px 13px; text-align: left; }\n");
    buf_appends(out, "th { background: #161b22; font-weight: 600; }\n");
    buf_appends(out, "blockquote { border-left: 4px solid #30363d; padding-left: 16px; margin-left: 0; color: #8b949e; font-style: italic; }\n");
    buf_appends(out, "hr { border: 0; border-top: 1px solid #21262d; margin: 24px 0; }\n");
    buf_appends(out, "img { max-width: 100%; height: auto; border-radius: 6px; }\n");
    buf_appends(out, "ul,ol { padding-left: 2em; margin-bottom: 16px; }\n");
    buf_appends(out, "li { margin-bottom: 4px; }\n");
    buf_appends(out, "input[type=\"checkbox\"] { margin-right: 8px; }\n");
    buf_appends(out, "</style>\n</head>\n<body>\n");
    buf_append(out, fragment.data, fragment.len);
    buf_appends(out, "\n</body></html>\n");
    buf_free(&fragment);
}

/*
 * convert_via_textutil — convert RTF or WebArchive to HTML via macOS textutil.
 * Returns 1 on success (out contains HTML), 0 on failure.
 */
static int convert_via_textutil(buf_t *out, const char *filepath) {
    char tmppath[MAX_PATH_LEN];
    snprintf(tmppath, sizeof(tmppath), "/tmp/minvader-conv-%d.html", getpid());

    /* Escape single quotes for shell */
    char escaped_in[MAX_PATH_LEN * 2];
    int ei = 0;
    for (int i = 0; filepath[i] && ei < (int)sizeof(escaped_in) - 6; i++) {
        if (filepath[i] == '\'') {
            escaped_in[ei++] = '\''; escaped_in[ei++] = '"';
            escaped_in[ei++] = '\''; escaped_in[ei++] = '"';
            escaped_in[ei++] = '\'';
        } else {
            escaped_in[ei++] = filepath[i];
        }
    }
    escaped_in[ei] = '\0';

    char cmd[MAX_PATH_LEN * 3];
    snprintf(cmd, sizeof(cmd), "textutil -convert html '%s' -output '%s' 2>/dev/null",
             escaped_in, tmppath);

    if (system(cmd) != 0) return 0;

    FILE *fp = fopen(tmppath, "r");
    if (!fp) return 0;

    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
        buf_append(out, chunk, n);
    fclose(fp);
    unlink(tmppath);
    return 1;
}

/* ── File type category (mirrors Go config.GetFileTypeCategory) ──── */
static const char *file_type_category(const char *name) {
    if (is_image_ext(name))  return "Images";
    if (is_video_ext(name))  return "Videos";
    const char *ext = strrchr(name, '.');
    if (!ext) return "Other";
    ext++;
    if (strcasecmp(ext, "pdf") == 0) return "PDFs";
    if (strcasecmp(ext, "md") == 0)  return "Markdown";
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "HTML";
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "flac") == 0 ||
        strcasecmp(ext, "m4a") == 0 || strcasecmp(ext, "aac") == 0) return "Audio";
    if (is_text_ext(name)) return "Text";
    return "Other";
}

/* ── Category management ────────────────────────────────────────────── */
static category_t *find_or_create_category(const char *name) {
    for (int i = 0; i < g_ncategories; i++) {
        if (strcmp(g_categories[i].name, name) == 0)
            return &g_categories[i];
    }
    if (g_ncategories >= MAX_CATEGORIES) return NULL;
    category_t *cat = &g_categories[g_ncategories++];
    cat->name = strdup(name);
    cat->count = 0;
    cat->cap = 64;
    cat->file_indices = malloc(cat->cap * sizeof(int));
    return cat;
}

static void category_add_file(category_t *cat, int file_idx) {
    if (!cat) return;
    if (cat->count >= cat->cap) {
        cat->cap *= 2;
        cat->file_indices = realloc(cat->file_indices, cat->cap * sizeof(int));
    }
    cat->file_indices[cat->count++] = file_idx;
}

/* ── File ingestion ─────────────────────────────────────────────────── */
static void add_file(const char *path) {
    if (g_nfiles >= g_files_cap) {
        g_files_cap = g_files_cap ? g_files_cap * 2 : 1024;
        g_files = realloc(g_files, g_files_cap * sizeof(file_entry_t));
    }

    file_entry_t *f = &g_files[g_nfiles];
    memset(f, 0, sizeof(*f));
    f->path = strdup(path);

    /* name = last component of path */
    char *slash = strrchr(f->path, '/');
    f->name = slash ? slash + 1 : f->path;

    /* stat for size + times */
    struct stat st;
    if (stat(path, &st) == 0) {
        f->size = st.st_size;
#ifdef __APPLE__
        f->mtime = st.st_mtimespec.tv_sec;
        f->birthtime = st.st_birthtimespec.tv_sec;
#elif defined(_WIN32)
        f->mtime = st.st_mtime;
        f->birthtime = st.st_ctime;
#else
        f->mtime = st.st_mtim.tv_sec;
        f->birthtime = st.st_mtim.tv_sec;
#endif
    }

    /* read xattr tags */
    char tag_buf[MAX_TAGS][MAX_TAGLEN];
    int tag_count = 0;
    read_current_tags(path, tag_buf, &tag_count);
    if (tag_count > 0) {
        f->tags = malloc(tag_count * sizeof(char *));
        f->ntags = tag_count;
        for (int i = 0; i < tag_count; i++)
            f->tags[i] = strdup(tag_buf[i]);
    }

    g_nfiles++;
}

static void read_stdin_paths(void) {
    char line[MAX_PATH_LEN];
    int count = 0;
    while (fgets(line, sizeof(line), stdin)) {
        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        add_file(line);
        count++;
        if (count % 10000 == 0)
            fprintf(stderr, "  read %d paths...\n", count);
    }
    fprintf(stderr, "read %d file paths from stdin\n", count);
}

/* ── Warm start: load from cache-*.db ──────────────────────────────── */
static void add_file_warm(const char *path, const char *name, int64_t size,
                          time_t mtime, time_t birthtime, char **tags, int ntags, int64_t db_id) {
    if (g_nfiles >= g_files_cap) {
        g_files_cap = g_files_cap ? g_files_cap * 2 : 1024;
        g_files = realloc(g_files, g_files_cap * sizeof(file_entry_t));
    }
    file_entry_t *f = &g_files[g_nfiles];
    memset(f, 0, sizeof(*f));
    f->path = strdup(path);
    f->name = strrchr(f->path, '/');
    f->name = f->name ? f->name + 1 : f->path;
    f->size = size;
    f->mtime = mtime;
    f->birthtime = birthtime;
    f->tags = tags;
    f->ntags = ntags;
    f->db_id = db_id;
    g_nfiles++;
}

static void load_from_cache(const char *port_str) {
    char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) { fprintf(stderr, "error: HOME not set\n"); return; }
    char dbpath[1024];
    snprintf(dbpath, sizeof(dbpath), "%s/.minvader/cache-%s.db", home, port_str);
    strncpy(g_cache_dbpath, dbpath, sizeof(g_cache_dbpath) - 1);

    sqlite3 *db;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "error: cannot open %s: %s\n", dbpath, sqlite3_errmsg(db));
        return;
    }
    fprintf(stderr, "warm start: loading from %s\n", dbpath);

    /* Get total count for progress */
    sqlite3_stmt *cnt;
    int total_files = 0;
    if (sqlite3_prepare_v2(db, "SELECT total_files FROM scan_metadata WHERE id=1", -1, &cnt, NULL) == SQLITE_OK) {
        if (sqlite3_step(cnt) == SQLITE_ROW) total_files = sqlite3_column_int(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (total_files > 0) fprintf(stderr, "  expecting %d files\n", total_files);

    /* Load all files */
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,
        "SELECT f.id, f.abs_path, f.name, f.size_bytes, f.os_mod_time, f.os_birth_time "
        "FROM files f ORDER BY f.id", -1, &sel, NULL) != SQLITE_OK) {
        fprintf(stderr, "error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    /* Prepare tag query */
    sqlite3_stmt *tag_sel;
    sqlite3_prepare_v2(db,
        "SELECT tag_name FROM tags WHERE file_id = ?", -1, &tag_sel, NULL);

    int count = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        sqlite3_int64 file_id = sqlite3_column_int64(sel, 0);
        const char *abs_path = (const char *)sqlite3_column_text(sel, 1);
        const char *name = (const char *)sqlite3_column_text(sel, 2);
        int64_t size = sqlite3_column_int64(sel, 3);
        time_t mtime = (time_t)sqlite3_column_int64(sel, 4);
        time_t birth = (time_t)sqlite3_column_int64(sel, 5);

        if (!abs_path) continue;
        (void)name; /* we derive name from path */

        /* Load tags for this file */
        char **tags = NULL;
        int ntags = 0;
        sqlite3_bind_int64(tag_sel, 1, file_id);
        while (sqlite3_step(tag_sel) == SQLITE_ROW) {
            const char *tag = (const char *)sqlite3_column_text(tag_sel, 0);
            if (!tag) continue;
            tags = realloc(tags, (ntags + 1) * sizeof(char *));
            tags[ntags++] = strdup(tag);
        }
        sqlite3_reset(tag_sel);

        add_file_warm(abs_path, NULL, size, mtime, birth, tags, ntags, file_id);
        count++;
        if (count % 50000 == 0)
            fprintf(stderr, "\r  loaded %d files...", count);
    }

    sqlite3_finalize(sel);
    sqlite3_finalize(tag_sel);
    sqlite3_close(db);
    fprintf(stderr, "\r  loaded %d files from cache\n", count);
}

/* ── Build category index ───────────────────────────────────────────── */
static void build_categories(void) {
    g_categories = calloc(MAX_CATEGORIES, sizeof(category_t));
    g_ncategories = 0;

    /* "All" category */
    category_t *all = find_or_create_category("All");

    for (int i = 0; i < g_nfiles; i++) {
        file_entry_t *f = &g_files[i];
        category_add_file(all, i);

        /* file type category */
        const char *type_cat = file_type_category(f->name);
        category_add_file(find_or_create_category(type_cat), i);

        /* tag categories */
        if (f->ntags == 0) {
            category_add_file(find_or_create_category("Untagged"), i);
        } else {
            for (int t = 0; t < f->ntags; t++) {
                category_add_file(find_or_create_category(f->tags[t]), i);
            }
            /* tag count bucket */
            char bucket[32];
            if (f->ntags == 1) snprintf(bucket, sizeof(bucket), "1 Tag");
            else if (f->ntags <= 5) snprintf(bucket, sizeof(bucket), "%d Tags", f->ntags);
            else snprintf(bucket, sizeof(bucket), "6+ Tags");
            category_add_file(find_or_create_category(bucket), i);
        }

        /* directory category */
        char dir[MAX_PATH_LEN];
        strncpy(dir, f->path, sizeof(dir) - 1);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            /* find relative dir component */
            char cat_name[MAX_PATH_LEN + 4];
            /* Use the last directory component as folder name */
            char *dir_name = strrchr(dir, '/');
            dir_name = dir_name ? dir_name + 1 : dir;
            snprintf(cat_name, sizeof(cat_name), "\xf0\x9f\x93\x81 %s", dir_name); /* 📁 */
            category_add_file(find_or_create_category(cat_name), i);
        }
    }

    fprintf(stderr, "built %d categories\n", g_ncategories);
}

/* ── Sorting ────────────────────────────────────────────────────────── */
static int g_sort_reversed = 0;

static int cmp_name(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int r = strcasecmp(g_files[ia].name, g_files[ib].name);
    return g_sort_reversed ? -r : r;
}

static int cmp_date(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    time_t ta = g_files[ia].birthtime, tb = g_files[ib].birthtime;
    int r = (ta < tb) ? 1 : (ta > tb) ? -1 : 0; /* newest first by default */
    return g_sort_reversed ? -r : r;
}

static int cmp_size(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int64_t sa = g_files[ia].size, sb = g_files[ib].size;
    int r = (sa < sb) ? 1 : (sa > sb) ? -1 : 0; /* largest first by default */
    return g_sort_reversed ? -r : r;
}

static void sort_indices(int *indices, int count, const char *mode, int reversed) {
    g_sort_reversed = reversed;
    if (!mode || !*mode || strcmp(mode, "name") == 0)
        qsort(indices, count, sizeof(int), cmp_name);
    else if (strcmp(mode, "date") == 0 || strcmp(mode, "os_birth") == 0)
        qsort(indices, count, sizeof(int), cmp_date);
    else if (strcmp(mode, "size") == 0)
        qsort(indices, count, sizeof(int), cmp_size);
    else if (strcmp(mode, "os_mod") == 0) {
        /* sort by mtime instead of birthtime */
        qsort(indices, count, sizeof(int), cmp_date); /* close enough for MVP */
    }
}

/* ── HTTP server ────────────────────────────────────────────────────── */
typedef struct {
    int    fd;
    char   method[16];
    char   path[MAX_PATH_LEN];
    char   query[MAX_PATH_LEN];
    char   body[MAX_REQ_SIZE];
    size_t body_len;
    /* parsed headers */
    char   content_type[256];
    size_t content_length;
    char   range_header[256];
} http_req_t;

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, size_t body_len) {
    buf_t hdr;
    buf_init(&hdr);
    buf_appendf(&hdr, "HTTP/1.1 %d %s\r\n", status, status_text);
    buf_appendf(&hdr, "Content-Type: %s\r\n", content_type);
    buf_appendf(&hdr, "Content-Length: %zu\r\n", body_len);
    buf_appends(&hdr, "Connection: close\r\n");
    buf_appends(&hdr, "\r\n");

    sock_write(fd, hdr.data, hdr.len);
    if (body && body_len > 0)
        sock_write(fd, body, body_len);
    buf_free(&hdr);
}

static void send_json(int fd, const char *json, size_t len) {
    send_response(fd, 200, "OK", "application/json", json, len);
}

static void send_html(int fd, const char *html, size_t len) {
    send_response(fd, 200, "OK", "text/html; charset=utf-8", html, len);
}

static void send_404(int fd) {
    const char *body = "404 Not Found";
    send_response(fd, 404, "Not Found", "text/plain", body, strlen(body));
}

static void send_405(int fd) {
    const char *body = "405 Method Not Allowed";
    send_response(fd, 405, "Method Not Allowed", "text/plain", body, strlen(body));
}

static void send_400(int fd, const char *msg) {
    send_response(fd, 400, "Bad Request", "text/plain", msg, strlen(msg));
}

/* Serve a file with proper MIME type and range support */
static void serve_file(int fd, const char *path, const char *range_hdr) {
    struct stat st;
    if (stat(path, &st) != 0) { send_404(fd); return; }

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) { send_404(fd); return; }

    const char *ctype = mime_type(path);
    off_t file_size = st.st_size;

    /* Range request support (needed for video seeking) */
    off_t range_start = 0, range_end = file_size - 1;
    int is_range = 0;

    if (range_hdr && range_hdr[0] && strncmp(range_hdr, "bytes=", 6) == 0) {
        const char *spec = range_hdr + 6;
        if (sscanf(spec, "%lld-%lld", &range_start, &range_end) >= 1) {
            if (range_end == 0 || range_end >= file_size)
                range_end = file_size - 1;
            is_range = 1;
        }
    }

    off_t send_len = range_end - range_start + 1;

    buf_t hdr;
    buf_init(&hdr);
    if (is_range) {
        buf_appendf(&hdr, "HTTP/1.1 206 Partial Content\r\n");
        buf_appendf(&hdr, "Content-Range: bytes %lld-%lld/%lld\r\n",
                    (long long)range_start, (long long)range_end, (long long)file_size);
    } else {
        buf_appends(&hdr, "HTTP/1.1 200 OK\r\n");
    }
    buf_appendf(&hdr, "Content-Type: %s\r\n", ctype);
    buf_appendf(&hdr, "Content-Length: %lld\r\n", (long long)send_len);
    if (strstr(ctype, "application/pdf") || strstr(ctype, "image/"))
        buf_appends(&hdr, "Content-Disposition: inline\r\n");
    buf_appends(&hdr, "Accept-Ranges: bytes\r\n");
    buf_appends(&hdr, "Connection: close\r\n");
    buf_appends(&hdr, "\r\n");
    sock_write(fd, hdr.data, hdr.len);
    buf_free(&hdr);

    /* Send file content */
    lseek(file_fd, range_start, SEEK_SET);
    char filebuf[65536];
    off_t remaining = send_len;
    while (remaining > 0) {
        size_t chunk = remaining < (off_t)sizeof(filebuf) ? (size_t)remaining : sizeof(filebuf);
        ssize_t nread = read(file_fd, filebuf, chunk);
        if (nread <= 0) break;
        sock_write(fd, filebuf, nread);
        remaining -= nread;
    }
    close(file_fd);
}

/* ── Query parameter parsing ────────────────────────────────────────── */
static const char *get_query_param(const char *query, const char *key, char *out, size_t outsz) {
    if (!query || !*query) { out[0] = '\0'; return out; }
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            if (vlen >= outsz) vlen = outsz - 1;
            /* URL decode into output */
            char encoded[MAX_PATH_LEN];
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, val, vlen);
            encoded[vlen] = '\0';
            url_decode(out, encoded, outsz);
            return out;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    out[0] = '\0';
    return out;
}

/* ── JSON body parsing (minimal, for tag operations) ────────────────── */
/* Find a string value for a key in a JSON object. Returns pointer into body or NULL. */
static const char *json_get_string(const char *json, const char *key, char *out, size_t outsz) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return NULL; }
    p += strlen(search);
    /* skip whitespace and colon */
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') { out[0] = '\0'; return NULL; }
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\' && p[1]) {
            p++; /* skip escape */
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

/* ── Format helpers ─────────────────────────────────────────────────── */
static void format_file_size(char *out, size_t outsz, int64_t bytes) {
    if (bytes < 1024) { snprintf(out, outsz, "%lld B", (long long)bytes); return; }
    double val = (double)bytes;
    const char *units[] = {"KB", "MB", "GB", "TB"};
    int u = 0;
    val /= 1024.0;
    while (val >= 1024.0 && u < 3) { val /= 1024.0; u++; }
    snprintf(out, outsz, "%.1f %s", val, units[u]);
}

static void format_date(char *out, size_t outsz, time_t t) {
    if (t == 0) { snprintf(out, outsz, "(no date)"); return; }
    struct tm *tm = localtime(&t);
    const char *months[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour = tm->tm_hour;
    const char *ampm = "AM";
    if (hour >= 12) { ampm = "PM"; if (hour > 12) hour -= 12; }
    if (hour == 0) hour = 12;
    snprintf(out, outsz, "%s %d, %d %d:%02d %s",
             months[tm->tm_mon + 1], tm->tm_mday, tm->tm_year + 1900,
             hour, tm->tm_min, ampm);
}

/* ── Category priority (mirrors Go) ─────────────────────────────────── */
static int category_priority(const char *tag) {
    if (strcmp(tag, "All") == 0) return 0;
    if (strcmp(tag, "Untagged") == 0) return 1;
    /* Type categories */
    if (strcmp(tag, "Images") == 0 || strcmp(tag, "Videos") == 0 ||
        strcmp(tag, "PDFs") == 0 || strcmp(tag, "Markdown") == 0 ||
        strcmp(tag, "HTML") == 0 || strcmp(tag, "Audio") == 0 ||
        strcmp(tag, "Text") == 0 || strcmp(tag, "Other") == 0) return 2;
    /* Tag count buckets */
    if (strstr(tag, " Tag") || strstr(tag, " Tags")) return 3;
    /* Folder categories */
    if (strncmp(tag, "\xf0\x9f\x93\x81 ", 5) == 0) return 5; /* 📁  */
    /* User tags */
    return 4;
}

static int cmp_category(const void *a, const void *b) {
    const category_t *ca = (const category_t *)a;
    const category_t *cb = (const category_t *)b;
    int pa = category_priority(ca->name);
    int pb = category_priority(cb->name);
    if (pa != pb) return pa - pb;
    /* same priority: sort by count descending */
    return cb->count - ca->count;
}

/* ── Page: Index (/) ────────────────────────────────────────────────── */
static void handle_index(int fd) {
    /* Sort categories for display */
    category_t sorted[MAX_CATEGORIES];
    int nsorted = 0;
    for (int i = 0; i < g_ncategories; i++) {
        /* Skip folder categories on index page */
        if (strncmp(g_categories[i].name, "\xf0\x9f\x93\x81 ", 5) == 0)
            continue;
        if (g_categories[i].count > 0)
            sorted[nsorted++] = g_categories[i];
    }
    qsort(sorted, nsorted, sizeof(category_t), cmp_category);

    buf_t page;
    buf_init(&page);

    buf_appends(&page, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
    buf_appends(&page, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    buf_appends(&page, "<title>Media Server</title>\n");
    buf_appends(&page, "<style>\n");
    buf_appends(&page, "* { box-sizing: border-box; margin: 0; padding: 0; }\n");
    buf_appends(&page, "body { background: #0d1117; color: #e0e0e0; font-family: -apple-system, system-ui, sans-serif; }\n");
    buf_appends(&page, ".header { padding: 24px; border-bottom: 1px solid #21262d; display: flex; justify-content: space-between; align-items: center; }\n");
    buf_appends(&page, ".header h1 { font-size: 20px; font-weight: 600; }\n");
    buf_appends(&page, ".stats { color: #8b949e; font-size: 14px; }\n");
    buf_appends(&page, ".grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 16px; padding: 24px; }\n");
    buf_appends(&page, ".card { background: #161b22; border: 1px solid #21262d; border-radius: 8px; overflow: hidden; transition: border-color 0.2s; }\n");
    buf_appends(&page, ".card:hover { border-color: #388bfd; }\n");
    buf_appends(&page, ".card a { text-decoration: none; color: inherit; display: block; }\n");
    buf_appends(&page, ".card-img { width: 100%; height: 140px; object-fit: cover; background: #21262d; display: flex; align-items: center; justify-content: center; color: #8b949e; font-size: 48px; }\n");
    buf_appends(&page, ".card-img img { width: 100%; height: 100%; object-fit: cover; }\n");
    buf_appends(&page, ".card-body { padding: 12px; }\n");
    buf_appends(&page, ".card-title { font-size: 14px; font-weight: 600; margin-bottom: 4px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }\n");
    buf_appends(&page, ".card-count { font-size: 12px; color: #8b949e; }\n");
    buf_appends(&page, "</style>\n</head>\n<body>\n");

    buf_appends(&page, "<div class=\"header\">\n");
    buf_appends(&page, "  <h1>Media Server</h1>\n");
    buf_appendf(&page, "  <span class=\"stats\">%d files &middot; %d categories</span>\n",
                g_nfiles, g_ncategories);
    buf_appends(&page, "</div>\n");
    buf_appends(&page, "<div class=\"grid\">\n");

    for (int i = 0; i < nsorted; i++) {
        category_t *cat = &sorted[i];
        /* Pick a random preview file */
        int preview_idx = cat->file_indices[rand() % cat->count];
        file_entry_t *pf = &g_files[preview_idx];

        buf_appends(&page, "<div class=\"card\"><a href=\"/tag/");
        url_encode(&page, cat->name);
        buf_appends(&page, "\">\n");

        /* Preview image or icon */
        buf_appends(&page, "<div class=\"card-img\">");
        if (is_image_ext(pf->name)) {
            buf_appends(&page, "<img src=\"/file/");
            url_encode(&page, pf->path);
            buf_appends(&page, "\" loading=\"lazy\">");
        } else if (is_video_ext(pf->name)) {
            buf_appends(&page, "🎬");
        } else {
            buf_appends(&page, "📄");
        }
        buf_appends(&page, "</div>\n");

        buf_appends(&page, "<div class=\"card-body\"><div class=\"card-title\">");
        html_escape(&page, cat->name);
        buf_appends(&page, "</div>\n");
        buf_appendf(&page, "<div class=\"card-count\">%d files</div>", cat->count);
        buf_appends(&page, "</div></a></div>\n");
    }

    buf_appends(&page, "</div>\n</body></html>\n");
    send_html(fd, page.data, page.len);
    buf_free(&page);
}

/* ── Page: Gallery (/tag/...) ───────────────────────────────────────── */
static void handle_gallery(int fd, const char *tag, const char *query_str) {
    /* Find category */
    category_t *cat = NULL;
    for (int i = 0; i < g_ncategories; i++) {
        if (strcmp(g_categories[i].name, tag) == 0) {
            cat = &g_categories[i];
            break;
        }
    }
    if (!cat) { send_404(fd); return; }

    /* Parse sort/page params */
    char sort_mode[32], page_str[16], limit_str[16], reversed_str[16];
    get_query_param(query_str, "sort", sort_mode, sizeof(sort_mode));
    get_query_param(query_str, "page", page_str, sizeof(page_str));
    get_query_param(query_str, "limit", limit_str, sizeof(limit_str));
    get_query_param(query_str, "reversed", reversed_str, sizeof(reversed_str));

    int page = atoi(page_str);
    if (page < 1) page = 1;
    int limit = atoi(limit_str);
    if (limit <= 0) limit = PAGE_SIZE;
    if (limit > 50000) limit = 50000;
    int reversed = (strcmp(reversed_str, "true") == 0);

    /* Copy and sort indices */
    int *indices = malloc(cat->count * sizeof(int));
    memcpy(indices, cat->file_indices, cat->count * sizeof(int));
    sort_indices(indices, cat->count, sort_mode, reversed);

    int total = cat->count;
    int actual_limit = (limit == -1) ? total : limit;
    int total_pages = actual_limit > 0 ? (total + actual_limit - 1) / actual_limit : 1;
    if (page > total_pages && total_pages > 0) page = total_pages;

    int start = (page - 1) * actual_limit;
    int end = start + actual_limit;
    if (end > total) end = total;

    /* Build gallery page */
    buf_t pg;
    buf_init(&pg);

    buf_appends(&pg, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
    buf_appends(&pg, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    buf_appends(&pg, "<title>");
    html_escape(&pg, tag);
    buf_appends(&pg, " - Media Server</title>\n");
    buf_appends(&pg, "<style>\n");
    buf_appends(&pg, "* { box-sizing: border-box; margin: 0; padding: 0; }\n");
    buf_appends(&pg, "body { background: #000; color: #fff; font-family: -apple-system, system-ui, sans-serif; }\n");

    /* Floating header */
    buf_appends(&pg, ".floating-header { position: fixed; top: 0; left: 0; right: 0; background: rgba(0,0,0,0.95); backdrop-filter: blur(10px); -webkit-backdrop-filter: blur(10px); border-bottom: 1px solid #333; z-index: 999; }\n");
    buf_appends(&pg, ".header-content { display: flex; justify-content: space-between; align-items: center; padding: 12px 24px; }\n");
    buf_appends(&pg, ".header-left { display: flex; align-items: center; gap: 16px; flex: 1; }\n");
    buf_appends(&pg, ".header-left a { color: #4da3ff; text-decoration: none; font-size: 14px; font-weight: 500; }\n");
    buf_appends(&pg, ".header-left a:hover { text-decoration: underline; }\n");
    buf_appends(&pg, ".header-left h1 { font-size: 16px; font-weight: 600; color: #e0e0e0; }\n");
    buf_appends(&pg, ".file-count { color: #8b949e; font-size: 14px; }\n");
    buf_appends(&pg, ".header-right { display: flex; align-items: center; gap: 10px; }\n");

    /* Header buttons */
    buf_appends(&pg, ".btn { border: none; padding: 6px 14px; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 500; display: flex; align-items: center; gap: 6px; transition: all 0.2s; }\n");
    buf_appends(&pg, ".btn-shutdown { background: #FF3B30; color: white; }\n");
    buf_appends(&pg, ".btn-shutdown:hover { background: #FF2D21; }\n");
    buf_appends(&pg, ".btn-rescan { background: #FF6B47; color: white; }\n");
    buf_appends(&pg, ".btn-rescan:hover { background: #e55a3a; }\n");

    /* Toolbar */
    buf_appends(&pg, ".toolbar { position: fixed; top: 52px; left: 0; right: 0; background: rgba(0,0,0,0.9); border-bottom: 1px solid #333; z-index: 998; padding: 8px 24px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }\n");
    buf_appends(&pg, ".toolbar select, .toolbar input { background: #2a2a2a; color: #fff; border: 1px solid #444; border-radius: 6px; padding: 6px 10px; font-size: 13px; }\n");
    buf_appends(&pg, ".toolbar select:focus, .toolbar input:focus { border-color: #4da3ff; outline: none; }\n");
    buf_appends(&pg, ".toolbar label { font-size: 13px; color: #8b949e; }\n");
    buf_appends(&pg, ".toolbar .sep { border-left: 1px solid #333; height: 20px; margin: 0 4px; }\n");

    /* Search */
    buf_appends(&pg, ".search-box { width: 200px; }\n");

    /* Pagination */
    buf_appends(&pg, ".pagination { display: flex; gap: 6px; align-items: center; font-size: 13px; color: #8b949e; }\n");
    buf_appends(&pg, ".pagination a, .pagination span.pg { padding: 4px 8px; border: 1px solid #444; border-radius: 4px; color: #4da3ff; text-decoration: none; font-size: 12px; }\n");
    buf_appends(&pg, ".pagination a:hover { background: #2a2a2a; }\n");
    buf_appends(&pg, ".pagination .current { background: #388bfd; color: white; border-color: #388bfd; }\n");

    /* Hints bar */
    buf_appends(&pg, ".hints { padding: 6px 24px; background: #111; border-bottom: 1px solid #222; color: #666; font-size: 11px; position: fixed; top: 94px; left: 0; right: 0; z-index: 997; }\n");

    /* Grid */
    buf_appends(&pg, ".content { margin-top: 136px; padding: 16px 24px; }\n");
    buf_appends(&pg, ".grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 12px; }\n");
    buf_appends(&pg, ".item { position: relative; border-radius: 8px; overflow: hidden; background: #1e1e1e; border: 1px solid #333; transition: transform 0.2s, box-shadow 0.2s; cursor: pointer; }\n");
    buf_appends(&pg, ".item:hover { transform: translateY(-4px); box-shadow: 0 4px 12px rgba(0,0,0,0.7); border-color: #555; }\n");
    buf_appends(&pg, ".item a { display: block; text-decoration: none; color: inherit; }\n");
    buf_appends(&pg, ".item img { width: 100%; height: 200px; object-fit: cover; display: block; }\n");
    buf_appends(&pg, ".item .icon { width: 100%; height: 200px; display: flex; align-items: center; justify-content: center; font-size: 56px; background: #2a2a2a; color: #666; }\n");
    buf_appends(&pg, ".item .info { padding: 10px 12px; }\n");
    buf_appends(&pg, ".item .name { font-size: 13px; font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #e0e0e0; margin-bottom: 4px; }\n");
    buf_appends(&pg, ".item .meta { font-size: 11px; color: #8b949e; }\n");
    buf_appends(&pg, ".item .tags { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 6px; }\n");
    buf_appends(&pg, ".item .tag { display: inline-block; padding: 2px 6px; background: #777; color: #000; border-radius: 8px; font-size: 10px; }\n");
    buf_appends(&pg, ".item .tag.current { background: #34c759; }\n");
    buf_appends(&pg, ".item.selected { border-color: #388bfd; box-shadow: 0 0 0 2px #388bfd, 0 4px 12px rgba(56,139,253,0.4); transform: translateY(-4px); }\n");
    buf_appends(&pg, ".item .text-preview { width: 100%; height: 200px; overflow: hidden; background: #161b22; padding: 8px 10px; font-family: 'SF Mono',Menlo,monospace; font-size: 9px; line-height: 1.4; color: #8b949e; white-space: pre-wrap; word-break: break-word; }\n");

    /* Bottom pagination */
    buf_appends(&pg, ".bottom-pagination { padding: 24px; display: flex; justify-content: center; align-items: center; gap: 12px; }\n");

    /* Shutdown modal */
    buf_appends(&pg, ".modal-overlay { display: none; position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.7); z-index: 10000; align-items: center; justify-content: center; }\n");
    buf_appends(&pg, ".modal-overlay.show { display: flex; }\n");
    buf_appends(&pg, ".modal { background: #222; border-radius: 12px; padding: 30px; max-width: 400px; width: 90%; border: 1px solid #444; }\n");
    buf_appends(&pg, ".modal-title { font-size: 18px; font-weight: 600; margin-bottom: 12px; }\n");
    buf_appends(&pg, ".modal-msg { color: #8b949e; margin-bottom: 20px; }\n");
    buf_appends(&pg, ".modal-buttons { display: flex; gap: 12px; justify-content: flex-end; }\n");
    buf_appends(&pg, ".modal-btn { padding: 8px 16px; border: none; border-radius: 6px; font-size: 14px; cursor: pointer; }\n");
    buf_appends(&pg, ".modal-btn.cancel { background: #444; color: #fff; }\n");
    buf_appends(&pg, ".modal-btn.confirm { background: #FF3B30; color: white; }\n");

    buf_appends(&pg, "</style>\n</head>\n<body>\n");

    /* Floating header */
    buf_appends(&pg, "<div class=\"floating-header\">\n<div class=\"header-content\">\n");
    buf_appends(&pg, "  <div class=\"header-left\">\n");
    buf_appends(&pg, "    <a href=\"/\">&larr; Home</a>\n    <h1>");
    html_escape(&pg, tag);
    buf_appends(&pg, "</h1>\n");
    buf_appendf(&pg, "    <span class=\"file-count\">%d files</span>\n", total);
    buf_appends(&pg, "  </div>\n");
    buf_appends(&pg, "  <div class=\"header-right\">\n");

    /* Pagination in header */
    if (total_pages > 1) {
        buf_appends(&pg, "    <div class=\"pagination\">\n");
        if (page > 1) {
            buf_appends(&pg, "      <a href=\"/tag/");
            url_encode(&pg, tag);
            buf_appendf(&pg, "?page=%d&limit=%d&sort=%s&reversed=%s\">&larr;</a>\n",
                        page - 1, limit, sort_mode, reversed ? "true" : "false");
        }
        buf_appendf(&pg, "      <span style=\"font-size:12px;\">%d-%d of %d</span>\n", start + 1, end, total);
        if (page < total_pages) {
            buf_appends(&pg, "      <a href=\"/tag/");
            url_encode(&pg, tag);
            buf_appendf(&pg, "?page=%d&limit=%d&sort=%s&reversed=%s\">&rarr;</a>\n",
                        page + 1, limit, sort_mode, reversed ? "true" : "false");
        }
        buf_appends(&pg, "    </div>\n");
    }

    buf_appends(&pg, "    <button class=\"btn btn-rescan\" onclick=\"fetch('/api/rescan',{method:'POST'})\">\xf0\x9f\x94\x84 Rescan</button>\n");
    buf_appends(&pg, "    <button class=\"btn btn-shutdown\" onclick=\"showModal('shutdownModal')\">⏻ Stop</button>\n");
    buf_appends(&pg, "  </div>\n</div>\n</div>\n");

    /* Toolbar */
    buf_appends(&pg, "<div class=\"toolbar\">\n");

    /* Sort */
    buf_appends(&pg, "  <label>Sort:</label>\n  <select id=\"sortsel\" onchange=\"applySort()\">\n");
    const char *sort_options[][2] = {
        {"name", "Name"}, {"date", "Date (Birth)"}, {"os_mod", "Date (Modified)"},
        {"size", "Size"}, {"random", "Random"}, {NULL, NULL}
    };
    for (int i = 0; sort_options[i][0]; i++) {
        int selected = (sort_mode[0] && strcmp(sort_mode, sort_options[i][0]) == 0) ||
                       (!sort_mode[0] && i == 0);
        buf_appendf(&pg, "    <option value=\"%s\"%s>%s</option>\n",
                    sort_options[i][0], selected ? " selected" : "", sort_options[i][1]);
    }
    buf_appends(&pg, "  </select>\n");
    buf_appendf(&pg, "  <label><input type=\"checkbox\" id=\"revchk\" onchange=\"applySort()\"%s> Reverse</label>\n",
                reversed ? " checked" : "");

    buf_appends(&pg, "  <div class=\"sep\"></div>\n");

    /* Page size */
    buf_appends(&pg, "  <label>Show:</label>\n  <select id=\"limitsel\" onchange=\"applySort()\">\n");
    int limit_options[] = {50, 100, 200, 500, 1000, 10000, 25000, 50000, -1};
    const char *limit_labels[] = {"50", "100", "200", "500", "1000", "10000", "25000", "50000", "All"};
    for (int i = 0; i < 9; i++) {
        buf_appendf(&pg, "    <option value=\"%d\"%s>%s</option>\n",
                    limit_options[i], (limit == limit_options[i]) ? " selected" : "", limit_labels[i]);
    }
    buf_appends(&pg, "  </select>\n");

    buf_appends(&pg, "  <div class=\"sep\"></div>\n");

    /* Search */
    buf_appends(&pg, "  <input class=\"search-box\" type=\"text\" id=\"searchbox\" placeholder=\"Search files...\" onkeydown=\"if(event.key==='Enter')doSearch()\">\n");

    buf_appends(&pg, "</div>\n");

    /* Hints bar */
    buf_appends(&pg, "<div class=\"hints\">Arrow keys: navigate &middot; Enter: view &middot; S: cycle sort &middot; +/-: card size</div>\n");

    /* Sort/search JS */
    buf_appends(&pg, "<script>\n");
    buf_appends(&pg, "function applySort() {\n");
    buf_appends(&pg, "  var s = document.getElementById('sortsel').value;\n");
    buf_appends(&pg, "  var r = document.getElementById('revchk').checked;\n");
    buf_appends(&pg, "  var l = document.getElementById('limitsel').value;\n");
    buf_appends(&pg, "  var url = window.location.pathname + '?sort=' + s + '&reversed=' + r + '&limit=' + l;\n");
    buf_appends(&pg, "  window.location = url;\n");
    buf_appends(&pg, "}\n");
    buf_appends(&pg, "function doSearch() {\n");
    buf_appends(&pg, "  var q = document.getElementById('searchbox').value.trim();\n");
    buf_appends(&pg, "  if (q) window.location = '/tag/' + encodeURIComponent(q);\n");
    buf_appends(&pg, "}\n");
    buf_appends(&pg, "function showModal(id) { document.getElementById(id).classList.add('show'); }\n");
    buf_appends(&pg, "function hideModal(id) { document.getElementById(id).classList.remove('show'); }\n");
    buf_appends(&pg, "function confirmShutdown() { fetch('/api/shutdown',{method:'POST'}); document.body.innerHTML='<div style=\"display:flex;height:100vh;align-items:center;justify-content:center;font-size:24px;\">Server stopped.</div>'; }\n");

    /* Keyboard navigation */
    buf_appends(&pg, "var cards, selectedIdx = -1;\n");
    buf_appends(&pg, "document.addEventListener('DOMContentLoaded', function() {\n");
    buf_appends(&pg, "  cards = document.querySelectorAll('.item');\n");
    buf_appends(&pg, "  cards.forEach(function(card, i) {\n");
    buf_appends(&pg, "    card.querySelector('a').addEventListener('click', function(e) { e.preventDefault(); selectCard(i); });\n");
    buf_appends(&pg, "    card.addEventListener('dblclick', function() { window.location = cards[selectedIdx].querySelector('a').href; });\n");
    buf_appends(&pg, "  });\n");
    buf_appends(&pg, "});\n");
    buf_appends(&pg, "function selectCard(idx) {\n");
    buf_appends(&pg, "  if (idx < 0 || idx >= cards.length) return;\n");
    buf_appends(&pg, "  if (selectedIdx >= 0 && cards[selectedIdx]) cards[selectedIdx].classList.remove('selected');\n");
    buf_appends(&pg, "  selectedIdx = idx;\n");
    buf_appends(&pg, "  cards[selectedIdx].classList.add('selected');\n");
    buf_appends(&pg, "  cards[selectedIdx].scrollIntoView({block:'nearest',behavior:'smooth'});\n");
    buf_appends(&pg, "}\n");
    buf_appends(&pg, "document.addEventListener('keydown', function(e) {\n");
    buf_appends(&pg, "  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;\n");
    buf_appends(&pg, "  var cols = Math.floor(document.querySelector('.grid').offsetWidth / 212);\n");
    buf_appends(&pg, "  var si = selectedIdx < 0 ? 0 : selectedIdx;\n");
    buf_appends(&pg, "  if (e.key === 'ArrowRight') { selectCard(selectedIdx < 0 ? 0 : Math.min(si + 1, cards.length - 1)); e.preventDefault(); }\n");
    buf_appends(&pg, "  if (e.key === 'ArrowLeft') { selectCard(selectedIdx < 0 ? 0 : Math.max(si - 1, 0)); e.preventDefault(); }\n");
    buf_appends(&pg, "  if (e.key === 'ArrowDown') { selectCard(selectedIdx < 0 ? 0 : Math.min(si + cols, cards.length - 1)); e.preventDefault(); }\n");
    buf_appends(&pg, "  if (e.key === 'ArrowUp') { selectCard(selectedIdx < 0 ? 0 : Math.max(si - cols, 0)); e.preventDefault(); }\n");
    buf_appends(&pg, "  if (e.key === 'Enter' && selectedIdx >= 0) { window.location = cards[selectedIdx].querySelector('a').href; }\n");
    buf_appends(&pg, "  if (e.key === 's' || e.key === 'S') { var sel = document.getElementById('sortsel'); sel.selectedIndex = (sel.selectedIndex + 1) % sel.options.length; applySort(); }\n");
    buf_appends(&pg, "});\n");
    buf_appends(&pg, "</script>\n");

    /* File grid */
    buf_appends(&pg, "<div class=\"content\">\n<div class=\"grid\">\n");
    for (int i = start; i < end; i++) {
        file_entry_t *f = &g_files[indices[i]];

        buf_appends(&pg, "<div class=\"item\"><a href=\"/view/");
        url_encode(&pg, tag);
        buf_appends(&pg, "?file=");
        url_encode(&pg, f->path);
        buf_appendf(&pg, "&sort=%s&reversed=%s&limit=%d", sort_mode, reversed ? "true" : "false", limit);
        buf_appends(&pg, "\">\n");

        /* Thumbnail or icon */
        if (is_image_ext(f->name)) {
            buf_appends(&pg, "<img src=\"/file/");
            url_encode(&pg, f->path);
            buf_appends(&pg, "\" loading=\"lazy\">\n");
        } else if (is_video_ext(f->name)) {
            buf_appends(&pg, "<div class=\"icon\">🎬</div>\n");
        } else if (strcasecmp(strrchr(f->name, '.') ? strrchr(f->name, '.') + 1 : "", "pdf") == 0) {
            buf_appends(&pg, "<div class=\"icon\">📕</div>\n");
        } else if (is_convertible_ext(f->name)) {
            buf_appends(&pg, "<div class=\"icon\">📖</div>\n");
        } else if (is_text_ext(f->name)) {
            FILE *tfp = fopen(f->path, "r");
            if (tfp) {
                buf_appends(&pg, "<div class=\"text-preview\">");
                char tbuf[512];
                size_t tn = fread(tbuf, 1, sizeof(tbuf) - 1, tfp);
                fclose(tfp);
                tbuf[tn] = '\0';
                for (size_t j = 0; j < tn; j++) {
                    switch (tbuf[j]) {
                    case '&': buf_appends(&pg, "&amp;"); break;
                    case '<': buf_appends(&pg, "&lt;"); break;
                    case '>': buf_appends(&pg, "&gt;"); break;
                    default:  buf_append(&pg, &tbuf[j], 1); break;
                    }
                }
                buf_appends(&pg, "</div>\n");
            } else {
                buf_appends(&pg, "<div class=\"icon\">📝</div>\n");
            }
        } else {
            buf_appends(&pg, "<div class=\"icon\">📄</div>\n");
        }

        buf_appends(&pg, "<div class=\"info\"><div class=\"name\">");
        html_escape(&pg, f->name);
        buf_appends(&pg, "</div>\n");

        char size_str[32], date_str[64];
        format_file_size(size_str, sizeof(size_str), f->size);
        format_date(date_str, sizeof(date_str), f->birthtime);
        buf_appendf(&pg, "<div class=\"meta\">%s &middot; %s</div>\n", size_str, date_str);

        /* Show tags */
        if (f->ntags > 0) {
            buf_appends(&pg, "<div class=\"tags\">");
            for (int t = 0; t < f->ntags && t < 5; t++) {
                int is_current = (strcmp(f->tags[t], tag) == 0);
                buf_appendf(&pg, "<span class=\"tag%s\">", is_current ? " current" : "");
                html_escape(&pg, f->tags[t]);
                buf_appends(&pg, "</span>");
            }
            if (f->ntags > 5) buf_appendf(&pg, "<span class=\"tag\">+%d</span>", f->ntags - 5);
            buf_appends(&pg, "</div>\n");
        }

        buf_appends(&pg, "</div></a></div>\n");
    }
    buf_appends(&pg, "</div>\n");

    /* Bottom pagination */
    if (total_pages > 1) {
        buf_appends(&pg, "<div class=\"bottom-pagination\">\n");
        buf_appends(&pg, "  <div class=\"pagination\">\n");
        for (int p = 1; p <= total_pages && p <= 30; p++) {
            if (p == page) {
                buf_appendf(&pg, "<span class=\"current\">%d</span>\n", p);
            } else {
                buf_appends(&pg, "<a href=\"/tag/");
                url_encode(&pg, tag);
                buf_appendf(&pg, "?page=%d&limit=%d&sort=%s&reversed=%s\">%d</a>\n",
                            p, limit, sort_mode, reversed ? "true" : "false", p);
            }
        }
        buf_appends(&pg, "  </div>\n");

        /* Bottom page size selector */
        buf_appends(&pg, "  <select style=\"background:#2a2a2a;color:#fff;border:1px solid #444;border-radius:6px;padding:4px 8px;font-size:12px;\" onchange=\"window.location.href='/tag/");
        url_encode(&pg, tag);
        buf_appendf(&pg, "?page=1&limit='+this.value+'&sort=%s&reversed=%s'\">\n", sort_mode, reversed ? "true" : "false");
        for (int i = 0; i < 9; i++) {
            buf_appendf(&pg, "    <option value=\"%d\"%s>%s per page</option>\n",
                        limit_options[i], (limit == limit_options[i]) ? " selected" : "", limit_labels[i]);
        }
        buf_appends(&pg, "  </select>\n");
        buf_appends(&pg, "</div>\n");
    }

    buf_appends(&pg, "</div>\n"); /* /content */

    /* Shutdown modal */
    buf_appends(&pg, "<div class=\"modal-overlay\" id=\"shutdownModal\">\n<div class=\"modal\">\n");
    buf_appends(&pg, "  <div class=\"modal-title\">⏻ Stop Server</div>\n");
    buf_appends(&pg, "  <div class=\"modal-msg\">Are you sure you want to stop the media server?</div>\n");
    buf_appends(&pg, "  <div class=\"modal-buttons\">\n");
    buf_appends(&pg, "    <button class=\"modal-btn cancel\" onclick=\"hideModal('shutdownModal')\">Cancel</button>\n");
    buf_appends(&pg, "    <button class=\"modal-btn confirm\" onclick=\"confirmShutdown()\">Stop Server</button>\n");
    buf_appends(&pg, "  </div>\n</div>\n</div>\n");

    buf_appends(&pg, "</body></html>\n");

    send_html(fd, pg.data, pg.len);
    buf_free(&pg);
    free(indices);
}

/* ── Page: Viewer (/view/...) ───────────────────────────────────────── */
static void handle_viewer(int fd, const char *tag, const char *query_str) {
    char file_path[MAX_PATH_LEN];
    char sort_mode[32], reversed_str[16], page_size_str[16];
    get_query_param(query_str, "file", file_path, sizeof(file_path));
    get_query_param(query_str, "sort", sort_mode, sizeof(sort_mode));
    get_query_param(query_str, "reversed", reversed_str, sizeof(reversed_str));
    get_query_param(query_str, "pageSize", page_size_str, sizeof(page_size_str));
    int reversed = (strcmp(reversed_str, "true") == 0);
    if (!sort_mode[0]) strcpy(sort_mode, "name");

    if (!file_path[0]) { send_404(fd); return; }

    /* Restore leading slash if stripped by browser */
    char abs_path[MAX_PATH_LEN];
    if (file_path[0] != '/' &&
        (strncmp(file_path, "Volumes/", 8) == 0 || strncmp(file_path, "Users/", 6) == 0)) {
        snprintf(abs_path, sizeof(abs_path), "/%s", file_path);
    } else {
        strncpy(abs_path, file_path, sizeof(abs_path) - 1);
    }

    /* Find the file in category */
    category_t *cat = NULL;
    for (int i = 0; i < g_ncategories; i++) {
        if (strcmp(g_categories[i].name, tag) == 0) {
            cat = &g_categories[i];
            break;
        }
    }

    /* Find current file index in category */
    int file_idx = -1;
    int cat_pos = -1;
    int *sorted_indices = NULL;
    int sorted_count = 0;

    if (cat) {
        sorted_count = cat->count;
        sorted_indices = malloc(sorted_count * sizeof(int));
        memcpy(sorted_indices, cat->file_indices, sorted_count * sizeof(int));
        sort_indices(sorted_indices, sorted_count, sort_mode, reversed);

        for (int i = 0; i < sorted_count; i++) {
            if (strcmp(g_files[sorted_indices[i]].path, abs_path) == 0) {
                file_idx = sorted_indices[i];
                cat_pos = i;
                break;
            }
        }
    }

    if (file_idx < 0) {
        /* Try finding file globally */
        for (int i = 0; i < g_nfiles; i++) {
            if (strcmp(g_files[i].path, abs_path) == 0) {
                file_idx = i;
                break;
            }
        }
    }

    if (file_idx < 0) {
        free(sorted_indices);
        send_404(fd);
        return;
    }

    file_entry_t *f = &g_files[file_idx];

    /* Determine prev/next in category */
    char prev_path[MAX_PATH_LEN] = "", next_path[MAX_PATH_LEN] = "";
    if (sorted_indices && cat_pos >= 0) {
        if (cat_pos > 0)
            strncpy(prev_path, g_files[sorted_indices[cat_pos - 1]].path, sizeof(prev_path) - 1);
        if (cat_pos < sorted_count - 1)
            strncpy(next_path, g_files[sorted_indices[cat_pos + 1]].path, sizeof(next_path) - 1);
    }

    /* Helper: build view URL with all query params preserved */
    #define VIEWER_URL(buf, tg, fp) do { \
        buf_appends((buf), "/view/"); \
        url_encode((buf), (tg)); \
        buf_appends((buf), "?file="); \
        url_encode((buf), (fp)); \
        buf_appendf((buf), "&sort=%s&reversed=%s", sort_mode, reversed ? "true" : "false"); \
        if (page_size_str[0]) buf_appendf((buf), "&pageSize=%s", page_size_str); \
    } while(0)

    /* Build viewer page */
    buf_t pg;
    buf_init(&pg);

    buf_appends(&pg, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
    buf_appends(&pg, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    buf_appends(&pg, "<title>");
    html_escape(&pg, f->name);
    buf_appends(&pg, "</title>\n");
    buf_appends(&pg, "<style>\n");
    buf_appends(&pg, "* { box-sizing: border-box; margin: 0; padding: 0; }\n");
    buf_appends(&pg, "body { background: #0d1117; color: #e0e0e0; font-family: -apple-system, system-ui, sans-serif; display: flex; flex-direction: column; height: 100vh; }\n");
    buf_appends(&pg, ".toolbar { padding: 8px 16px; border-bottom: 1px solid #21262d; display: flex; align-items: center; gap: 8px; flex-shrink: 0; flex-wrap: wrap; }\n");
    buf_appends(&pg, ".toolbar a, .toolbar button { color: #e0e0e0; text-decoration: none; font-size: 13px; }\n");
    buf_appends(&pg, ".toolbar .filename { font-size: 14px; font-weight: 600; flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }\n");
    buf_appends(&pg, ".btn { padding: 4px 12px; background: #21262d; border: 1px solid #30363d; border-radius: 6px; color: #e0e0e0; font-size: 13px; cursor: pointer; white-space: nowrap; }\n");
    buf_appends(&pg, ".btn:hover { background: #30363d; }\n");
    buf_appends(&pg, ".btn-danger { background: #da3633; border-color: #f85149; }\n");
    buf_appends(&pg, ".btn-danger:hover { background: #f85149; }\n");
    buf_appends(&pg, ".btn-back { color: #4da3ff; background: none; border: none; padding: 4px 8px; }\n");
    buf_appends(&pg, ".content { flex: 1; display: flex; align-items: center; justify-content: center; overflow: auto; padding: 16px; position: relative; }\n");
    buf_appends(&pg, ".content img { max-width: 100%; max-height: 100%; object-fit: contain; cursor: zoom-in; }\n");
    buf_appends(&pg, ".content img.zoomed { max-width: none; max-height: none; cursor: zoom-out; }\n");
    buf_appends(&pg, ".content video { max-width: 100%; max-height: 100%; }\n");
    buf_appends(&pg, ".content audio { width: 80%%; max-width: 600px; }\n");
    buf_appends(&pg, ".content iframe { width: 100%; height: 100%; border: none; }\n");
    buf_appends(&pg, ".content pre { max-width: 100%; overflow: auto; padding: 16px; background: #161b22; border-radius: 8px; font-size: 13px; line-height: 1.5; white-space: pre-wrap; word-break: break-word; }\n");
    buf_appends(&pg, ".bottom-bar { padding: 6px 16px; border-top: 1px solid #21262d; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; flex-shrink: 0; }\n");
    buf_appends(&pg, ".tag { display: inline-block; padding: 2px 8px; background: #1f6feb33; color: #4da3ff; border-radius: 12px; font-size: 12px; cursor: pointer; }\n");
    buf_appends(&pg, ".tag .x { margin-left: 4px; color: #8b949e; cursor: pointer; }\n");
    buf_appends(&pg, ".tag-input-wrap { position: relative; display: inline-block; }\n");
    buf_appends(&pg, ".tag-input { background: #21262d; border: 1px solid #30363d; color: #e0e0e0; border-radius: 6px; padding: 2px 8px; font-size: 12px; width: 120px; }\n");
    buf_appends(&pg, ".tag-suggest { position: absolute; bottom: 100%; left: 0; background: #161b22; border: 1px solid #30363d; border-radius: 6px; max-height: 160px; overflow-y: auto; display: none; min-width: 150px; z-index: 200; }\n");
    buf_appends(&pg, ".tag-suggest.show { display: block; }\n");
    buf_appends(&pg, ".tag-suggest div { padding: 4px 10px; font-size: 12px; color: #c9d1d9; cursor: pointer; }\n");
    buf_appends(&pg, ".tag-suggest div:hover, .tag-suggest div.sel { background: #1f6feb33; color: #4da3ff; }\n");
    buf_appends(&pg, ".meta-info { padding: 4px 16px; font-size: 11px; color: #8b949e; border-top: 1px solid #21262d; flex-shrink: 0; display: flex; gap: 16px; align-items: center; }\n");
    buf_appends(&pg, ".meta-info .path { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n");
    /* Modal overlay */
    buf_appends(&pg, ".modal-overlay { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.7); z-index: 1000; align-items: center; justify-content: center; }\n");
    buf_appends(&pg, ".modal-overlay.active { display: flex; }\n");
    buf_appends(&pg, ".modal { background: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 24px; max-width: 400px; text-align: center; }\n");
    buf_appends(&pg, ".modal h3 { margin-bottom: 12px; }\n");
    buf_appends(&pg, ".modal p { color: #8b949e; margin-bottom: 16px; font-size: 13px; word-break: break-all; }\n");
    buf_appends(&pg, ".modal .actions { display: flex; gap: 8px; justify-content: center; }\n");
    /* Slideshow progress */
    buf_appends(&pg, ".slideshow-indicator { position: fixed; top: 0; left: 0; height: 3px; background: #4da3ff; z-index: 999; transition: width linear; }\n");
    /* Keyboard hint */
    buf_appends(&pg, ".kbd-hint { position: fixed; bottom: 80px; right: 16px; background: #21262d; border: 1px solid #30363d; border-radius: 8px; padding: 8px 12px; font-size: 11px; color: #8b949e; z-index: 100; display: none; line-height: 1.6; }\n");
    buf_appends(&pg, "</style>\n</head>\n<body>\n");

    /* Toolbar */
    buf_appends(&pg, "<div class=\"toolbar\">\n");
    /* Back to gallery */
    buf_appends(&pg, "  <a class=\"btn btn-back\" href=\"/tag/");
    url_encode(&pg, tag);
    buf_appendf(&pg, "?sort=%s&reversed=%s", sort_mode, reversed ? "true" : "false");
    if (page_size_str[0]) buf_appendf(&pg, "&pageSize=%s", page_size_str);
    buf_appends(&pg, "\">&larr; ");
    html_escape(&pg, tag);
    buf_appends(&pg, "</a>\n");

    /* Position indicator */
    if (sorted_indices && cat_pos >= 0) {
        buf_appendf(&pg, "  <span style=\"color:#8b949e;font-size:12px;\">%d / %d</span>\n",
                    cat_pos + 1, sorted_count);
    }

    /* Filename */
    buf_appends(&pg, "  <span class=\"filename\">");
    html_escape(&pg, f->name);
    buf_appends(&pg, "</span>\n");

    /* Action buttons */
    if (prev_path[0]) {
        buf_appends(&pg, "  <a class=\"btn\" id=\"prevBtn\" href=\"");
        VIEWER_URL(&pg, tag, prev_path);
        buf_appends(&pg, "\" title=\"Previous (Left arrow)\">&#9664; Prev</a>\n");
    }
    if (next_path[0]) {
        buf_appends(&pg, "  <a class=\"btn\" id=\"nextBtn\" href=\"");
        VIEWER_URL(&pg, tag, next_path);
        buf_appends(&pg, "\" title=\"Next (Right arrow)\">Next &#9654;</a>\n");
    }

    /* Slideshow button */
    if (next_path[0]) {
        buf_appends(&pg, "  <button class=\"btn\" id=\"slideshowBtn\" onclick=\"toggleSlideshow()\" title=\"Slideshow (S)\">&#9654; Play</button>\n");
    }

    /* Reveal in Finder */
    buf_appends(&pg, "  <button class=\"btn\" onclick=\"revealInFinder()\" title=\"Reveal in Finder (R)\">Reveal</button>\n");

    /* Download */
    buf_appends(&pg, "  <a class=\"btn\" href=\"/file/");
    url_encode(&pg, f->path);
    buf_appends(&pg, "\" download title=\"Download (D)\">Download</a>\n");

    /* Delete */
    buf_appends(&pg, "  <button class=\"btn btn-danger\" onclick=\"showDeleteModal()\" title=\"Delete (X)\">Delete</button>\n");

    /* Keyboard help toggle */
#ifdef __APPLE__
    buf_appends(&pg, "  <button class=\"btn\" onclick=\"toggleKbdHelp()\" title=\"Keyboard shortcuts (?)\"></button>\n");
#elif defined(_WIN32)
    buf_appends(&pg, "  <button class=\"btn\" onclick=\"toggleKbdHelp()\" title=\"Keyboard shortcuts (?)\">⊞</button>\n");
#else
    buf_appends(&pg, "  <button class=\"btn\" onclick=\"toggleKbdHelp()\" title=\"Keyboard shortcuts (?)\">\xF0\x9F\x90\xA7</button>\n");
#endif

    buf_appends(&pg, "</div>\n");

    /* Content area */
    buf_appends(&pg, "<div class=\"content\" id=\"contentArea\">\n");
    if (is_image_ext(f->name)) {
        buf_appends(&pg, "  <img id=\"mainImg\" src=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\" onclick=\"toggleZoom(this)\">\n");
    } else if (is_video_ext(f->name)) {
        buf_appends(&pg, "  <video id=\"mainVideo\" controls autoplay src=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\"></video>\n");
    } else if (is_audio_ext(f->name)) {
        buf_appends(&pg, "  <audio controls autoplay src=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\"></audio>\n");
    } else if (strcasecmp(strrchr(f->name, '.') ? strrchr(f->name, '.') + 1 : "", "pdf") == 0) {
        buf_appends(&pg, "  <iframe src=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\"></iframe>\n");
    } else if (strcasecmp(strrchr(f->name, '.') ? strrchr(f->name, '.') + 1 : "", "html") == 0 ||
               strcasecmp(strrchr(f->name, '.') ? strrchr(f->name, '.') + 1 : "", "htm") == 0) {
        buf_appends(&pg, "  <iframe src=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\"></iframe>\n");
    } else if (is_convertible_ext(f->name)) {
        /* Render markdown/RTF/webarchive via /doc/ endpoint */
        buf_appends(&pg, "  <iframe src=\"/doc/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\"></iframe>\n");
    } else if (is_text_ext(f->name)) {
        FILE *fp = fopen(f->path, "r");
        if (fp) {
            buf_appends(&pg, "  <pre>");
            char chunk[4096];
            size_t total_read = 0;
            while (total_read < 512000) {
                size_t n = fread(chunk, 1, sizeof(chunk), fp);
                if (n == 0) break;
                for (size_t j = 0; j < n; j++) {
                    switch (chunk[j]) {
                    case '&': buf_appends(&pg, "&amp;"); break;
                    case '<': buf_appends(&pg, "&lt;"); break;
                    case '>': buf_appends(&pg, "&gt;"); break;
                    default:  buf_append(&pg, &chunk[j], 1); break;
                    }
                }
                total_read += n;
            }
            fclose(fp);
            buf_appends(&pg, "</pre>\n");
        }
    } else {
        buf_appends(&pg, "  <div style=\"text-align:center\">\n");
        buf_appends(&pg, "    <div style=\"font-size:64px;margin-bottom:16px;\">&#128196;</div>\n");
        buf_appends(&pg, "    <div>");
        html_escape(&pg, f->name);
        buf_appends(&pg, "</div>\n");
        buf_appends(&pg, "    <div style=\"margin-top:8px;\"><a class=\"btn\" href=\"/file/");
        url_encode(&pg, f->path);
        buf_appends(&pg, "\" download>Download</a></div>\n");
        buf_appends(&pg, "  </div>\n");
    }
    buf_appends(&pg, "</div>\n");

    /* Tags bar */
    buf_appends(&pg, "<div class=\"bottom-bar\" id=\"tagbar\">\n");
    buf_appends(&pg, "  <span style=\"font-size:12px;color:#8b949e;\">Tags:</span>\n");
    for (int t = 0; t < f->ntags; t++) {
        buf_appends(&pg, "  <span class=\"tag\" onclick=\"removeTag('");
        for (const char *p = f->tags[t]; *p; p++) {
            if (*p == '\'' || *p == '\\') buf_append(&pg, "\\", 1);
            buf_append(&pg, p, 1);
        }
        buf_appends(&pg, "')\">");
        html_escape(&pg, f->tags[t]);
        buf_appends(&pg, "<span class=\"x\">&times;</span></span>\n");
    }
    buf_appends(&pg, "  <span class=\"tag-input-wrap\"><input class=\"tag-input\" id=\"newtag\" placeholder=\"Add tag... (T)\" autocomplete=\"off\"><div class=\"tag-suggest\" id=\"tagSuggest\"></div></span>\n");
    buf_appends(&pg, "</div>\n");

    /* Metadata bar */
    char size_str[32], date_str[64], mdate_str[64];
    format_file_size(size_str, sizeof(size_str), f->size);
    format_date(date_str, sizeof(date_str), f->birthtime);
    format_date(mdate_str, sizeof(mdate_str), f->mtime);
    buf_appends(&pg, "<div class=\"meta-info\">");
    buf_appendf(&pg, "<span>%s</span><span>born %s</span><span>mod %s</span>", size_str, date_str, mdate_str);
    buf_appends(&pg, "<span class=\"path\">");
    html_escape(&pg, f->path);
    buf_appends(&pg, "</span>");
    buf_appends(&pg, "</div>\n");

    /* Delete confirmation modal */
    buf_appends(&pg, "<div class=\"modal-overlay\" id=\"deleteModal\">\n");
    buf_appends(&pg, "  <div class=\"modal\">\n");
    buf_appends(&pg, "    <h3>Move to Trash?</h3>\n");
    buf_appends(&pg, "    <p>");
    html_escape(&pg, f->name);
    buf_appends(&pg, "</p>\n");
    buf_appends(&pg, "    <div class=\"actions\">\n");
    buf_appends(&pg, "      <button class=\"btn\" onclick=\"hideDeleteModal()\">Cancel (Esc)</button>\n");
    buf_appends(&pg, "      <button class=\"btn btn-danger\" onclick=\"doDelete()\">Move to Trash</button>\n");
    buf_appends(&pg, "    </div>\n");
    buf_appends(&pg, "  </div>\n");
    buf_appends(&pg, "</div>\n");

    /* Slideshow progress bar */
    buf_appends(&pg, "<div class=\"slideshow-indicator\" id=\"slideshowBar\" style=\"width:0\"></div>\n");

    /* Keyboard help panel */
    buf_appends(&pg, "<div class=\"kbd-hint\" id=\"kbdHelp\">\n");
    buf_appends(&pg, "  <b>Keyboard Shortcuts</b><br>\n");
    buf_appends(&pg, "  &larr; / &rarr; &nbsp; Prev / Next<br>\n");
    buf_appends(&pg, "  Space &nbsp;&nbsp;&nbsp;&nbsp; Next<br>\n");
    buf_appends(&pg, "  X &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Delete<br>\n");
    buf_appends(&pg, "  R &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Reveal in Finder<br>\n");
    buf_appends(&pg, "  D &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Download<br>\n");
    buf_appends(&pg, "  T &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Focus tag input<br>\n");
    buf_appends(&pg, "  N &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Random<br>\n");
    buf_appends(&pg, "  S &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Slideshow<br>\n");
    buf_appends(&pg, "  Z &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Toggle zoom (images)<br>\n");
    buf_appends(&pg, "  F &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Fullscreen<br>\n");
    buf_appends(&pg, "  Esc &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Back to gallery<br>\n");
    buf_appends(&pg, "  ? &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; This help<br>\n");
    buf_appends(&pg, "</div>\n");

    /* JavaScript */
    buf_appends(&pg, "<script>\n");
    buf_appends(&pg, "var filePath = ");
    buf_append_json_str(&pg, abs_path);
    buf_appends(&pg, ";\n");

    /* Gallery return URL */
    buf_appends(&pg, "var galleryUrl = '/tag/");
    url_encode(&pg, tag);
    buf_appendf(&pg, "?sort=%s&reversed=%s", sort_mode, reversed ? "true" : "false");
    if (page_size_str[0]) buf_appendf(&pg, "&pageSize=%s", page_size_str);
    buf_appends(&pg, "';\n");

    /* Random URL for JS */
    buf_appends(&pg, "var randomUrl = '/api/random?tag=");
    url_encode(&pg, tag);
    buf_appendf(&pg, "&sort=%s&reversed=%s", sort_mode, reversed ? "true" : "false");
    if (page_size_str[0]) buf_appendf(&pg, "&pageSize=%s", page_size_str);
    buf_appends(&pg, "';\n");

    /* Prev/next URLs for JS */
    if (prev_path[0]) {
        buf_appends(&pg, "var prevUrl = '");
        VIEWER_URL(&pg, tag, prev_path);
        buf_appends(&pg, "';\n");
    } else {
        buf_appends(&pg, "var prevUrl = null;\n");
    }
    if (next_path[0]) {
        buf_appends(&pg, "var nextUrl = '");
        VIEWER_URL(&pg, tag, next_path);
        buf_appends(&pg, "';\n");
    } else {
        buf_appends(&pg, "var nextUrl = null;\n");
    }

    /* Tag functions */
    buf_appends(&pg, "function addTag() {\n");
    buf_appends(&pg, "  var t = document.getElementById('newtag').value.trim();\n");
    buf_appends(&pg, "  if (!t) return;\n");
    buf_appends(&pg, "  fetch('/api/addtag', {method:'POST', headers:{'Content-Type':'application/json'},\n");
    buf_appends(&pg, "    body: JSON.stringify({filePath: filePath, tag: t})\n");
    buf_appends(&pg, "  }).then(r => r.json()).then(d => { if(d.success) location.reload(); });\n");
    buf_appends(&pg, "}\n");
    buf_appends(&pg, "function removeTag(t) {\n");
    buf_appends(&pg, "  fetch('/api/removetag', {method:'POST', headers:{'Content-Type':'application/json'},\n");
    buf_appends(&pg, "    body: JSON.stringify({filePath: filePath, tag: t})\n");
    buf_appends(&pg, "  }).then(r => r.json()).then(d => { if(d.success) location.reload(); });\n");
    buf_appends(&pg, "}\n");

    /* Tag autocomplete */
    buf_appends(&pg, "var allTags = null, sugIdx = -1;\n");
    buf_appends(&pg, "function loadTags() { if (allTags) return; fetch('/api/alltags').then(r=>r.json()).then(d=>{ allTags = Array.isArray(d) ? d : (d.tags||[]); }); }\n");
    buf_appends(&pg, "function showSuggestions() {\n");
    buf_appends(&pg, "  var inp = document.getElementById('newtag'), q = inp.value.trim().toLowerCase();\n");
    buf_appends(&pg, "  var box = document.getElementById('tagSuggest');\n");
    buf_appends(&pg, "  if (!allTags || q.length === 0) { box.classList.remove('show'); return; }\n");
    buf_appends(&pg, "  var hits = allTags.filter(t => t.toLowerCase().indexOf(q) >= 0).slice(0, 10);\n");
    buf_appends(&pg, "  if (hits.length === 0) { box.classList.remove('show'); return; }\n");
    buf_appends(&pg, "  sugIdx = -1;\n");
    buf_appends(&pg, "  box.innerHTML = hits.map(t => '<div>' + t.replace(/</g,'&lt;') + '</div>').join('');\n");
    buf_appends(&pg, "  box.querySelectorAll('div').forEach(function(d) {\n");
    buf_appends(&pg, "    d.addEventListener('mousedown', function(e) { e.preventDefault(); inp.value = d.textContent; box.classList.remove('show'); addTag(); });\n");
    buf_appends(&pg, "  });\n");
    buf_appends(&pg, "  box.classList.add('show');\n");
    buf_appends(&pg, "}\n");
    buf_appends(&pg, "(function() {\n");
    buf_appends(&pg, "  var inp = document.getElementById('newtag'), box = document.getElementById('tagSuggest');\n");
    buf_appends(&pg, "  inp.addEventListener('focus', loadTags);\n");
    buf_appends(&pg, "  inp.addEventListener('input', showSuggestions);\n");
    buf_appends(&pg, "  inp.addEventListener('blur', function() { box.classList.remove('show'); });\n");
    buf_appends(&pg, "  inp.addEventListener('keydown', function(e) {\n");
    buf_appends(&pg, "    var items = box.querySelectorAll('div');\n");
    buf_appends(&pg, "    if (e.key === 'ArrowDown' && box.classList.contains('show')) { e.preventDefault(); sugIdx = Math.min(sugIdx+1, items.length-1); items.forEach((d,i) => d.classList.toggle('sel', i===sugIdx)); }\n");
    buf_appends(&pg, "    else if (e.key === 'ArrowUp' && box.classList.contains('show')) { e.preventDefault(); sugIdx = Math.max(sugIdx-1, 0); items.forEach((d,i) => d.classList.toggle('sel', i===sugIdx)); }\n");
    buf_appends(&pg, "    else if (e.key === 'Enter') { if (sugIdx >= 0 && items[sugIdx]) inp.value = items[sugIdx].textContent; box.classList.remove('show'); addTag(); }\n");
    buf_appends(&pg, "    else if (e.key === 'Escape') { box.classList.remove('show'); inp.blur(); }\n");
    buf_appends(&pg, "  });\n");
    buf_appends(&pg, "})();\n");

    /* Delete functions */
    buf_appends(&pg, "function showDeleteModal() { document.getElementById('deleteModal').classList.add('active'); }\n");
    buf_appends(&pg, "function hideDeleteModal() { document.getElementById('deleteModal').classList.remove('active'); }\n");
    buf_appends(&pg, "function doDelete() {\n");
    buf_appends(&pg, "  fetch('/api/deletefile', {method:'POST', headers:{'Content-Type':'application/json'},\n");
    buf_appends(&pg, "    body: JSON.stringify({filePath: filePath})\n");
    buf_appends(&pg, "  }).then(r => r.json()).then(d => {\n");
    buf_appends(&pg, "    if(d.success) { if(nextUrl) window.location=nextUrl; else if(prevUrl) window.location=prevUrl; else window.location=galleryUrl; }\n");
    buf_appends(&pg, "    else { alert('Delete failed: '+(d.error||'unknown')); hideDeleteModal(); }\n");
    buf_appends(&pg, "  }).catch(e => { alert('Delete failed: '+e); hideDeleteModal(); });\n");
    buf_appends(&pg, "}\n");

    /* Reveal in Finder */
    buf_appends(&pg, "function revealInFinder() {\n");
    buf_appends(&pg, "  fetch('/api/reveal', {method:'POST', headers:{'Content-Type':'application/json'},\n");
    buf_appends(&pg, "    body: JSON.stringify({filePath: filePath})\n");
    buf_appends(&pg, "  });\n");
    buf_appends(&pg, "}\n");

    /* Image zoom toggle */
    buf_appends(&pg, "function toggleZoom(img) {\n");
    buf_appends(&pg, "  if (!img) img = document.getElementById('mainImg');\n");
    buf_appends(&pg, "  if (img) img.classList.toggle('zoomed');\n");
    buf_appends(&pg, "}\n");

    /* Fullscreen */
    buf_appends(&pg, "function toggleFullscreen() {\n");
    buf_appends(&pg, "  var el = document.getElementById('contentArea');\n");
    buf_appends(&pg, "  if (!document.fullscreenElement) el.requestFullscreen().catch(()=>{});\n");
    buf_appends(&pg, "  else document.exitFullscreen();\n");
    buf_appends(&pg, "}\n");

    /* Keyboard help */
    buf_appends(&pg, "function toggleKbdHelp() {\n");
    buf_appends(&pg, "  var h = document.getElementById('kbdHelp');\n");
    buf_appends(&pg, "  h.style.display = h.style.display === 'block' ? 'none' : 'block';\n");
    buf_appends(&pg, "}\n");

    /* Slideshow */
    buf_appends(&pg, "var slideshowTimer = null, slideshowInterval = 3000;\n");
    buf_appends(&pg, "function toggleSlideshow() {\n");
    buf_appends(&pg, "  var btn = document.getElementById('slideshowBtn');\n");
    buf_appends(&pg, "  var bar = document.getElementById('slideshowBar');\n");
    buf_appends(&pg, "  if (slideshowTimer) { clearInterval(slideshowTimer); slideshowTimer = null;\n");
    buf_appends(&pg, "    if(btn) btn.innerHTML = '&#9654; Play'; bar.style.width = '0'; return; }\n");
    buf_appends(&pg, "  if (!nextUrl) return;\n");
    buf_appends(&pg, "  if(btn) btn.innerHTML = '&#9724; Stop';\n");
    buf_appends(&pg, "  var start = Date.now();\n");
    buf_appends(&pg, "  bar.style.transition = 'none'; bar.style.width = '0';\n");
    buf_appends(&pg, "  setTimeout(function() { bar.style.transition = 'width '+(slideshowInterval/1000)+'s linear'; bar.style.width = '100%'; }, 50);\n");
    buf_appends(&pg, "  slideshowTimer = setTimeout(function() { window.location = nextUrl + '&slideshow=1'; }, slideshowInterval);\n");
    buf_appends(&pg, "}\n");

    /* Auto-start slideshow if param set */
    buf_appends(&pg, "(function() {\n");
    buf_appends(&pg, "  var p = new URLSearchParams(location.search);\n");
    buf_appends(&pg, "  if (p.get('slideshow') === '1' && nextUrl) toggleSlideshow();\n");
    buf_appends(&pg, "})();\n");

    /* Keyboard navigation */
    buf_appends(&pg, "document.addEventListener('keydown', function(e) {\n");
    buf_appends(&pg, "  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;\n");
    buf_appends(&pg, "  var dm = document.getElementById('deleteModal');\n");
    buf_appends(&pg, "  if (dm.classList.contains('active')) {\n");
    buf_appends(&pg, "    if (e.key === 'Escape') hideDeleteModal();\n");
    buf_appends(&pg, "    if (e.key === 'Enter') doDelete();\n");
    buf_appends(&pg, "    return;\n");
    buf_appends(&pg, "  }\n");
    buf_appends(&pg, "  switch(e.key) {\n");
    buf_appends(&pg, "    case 'ArrowLeft': if(prevUrl) window.location=prevUrl; break;\n");
    buf_appends(&pg, "    case 'ArrowRight': case ' ': e.preventDefault(); if(nextUrl) window.location=nextUrl; break;\n");
    buf_appends(&pg, "    case 'x': case 'X': showDeleteModal(); break;\n");
    buf_appends(&pg, "    case 'r': case 'R': revealInFinder(); break;\n");
    buf_appends(&pg, "    case 'd': case 'D': var dl=document.querySelector('a[download]'); if(dl) dl.click(); break;\n");
    buf_appends(&pg, "    case 't': case 'T': e.preventDefault(); document.getElementById('newtag').focus(); break;\n");
    buf_appends(&pg, "    case 'l': case 'L': document.getElementById('newtag').value='\\u2764\\uFE0F'; addTag(); break;\n");
    buf_appends(&pg, "    case 'n': case 'N': window.location=randomUrl; break;\n");
    buf_appends(&pg, "    case 's': case 'S': toggleSlideshow(); break;\n");
    buf_appends(&pg, "    case 'z': case 'Z': toggleZoom(); break;\n");
    buf_appends(&pg, "    case 'f': case 'F': toggleFullscreen(); break;\n");
    buf_appends(&pg, "    case '?': toggleKbdHelp(); break;\n");
    buf_appends(&pg, "    case 'Escape': window.location=galleryUrl; break;\n");
    buf_appends(&pg, "  }\n");
    buf_appends(&pg, "});\n");
    buf_appends(&pg, "</script>\n");

    buf_appends(&pg, "</body></html>\n");
    send_html(fd, pg.data, pg.len);
    buf_free(&pg);
    free(sorted_indices);
    #undef VIEWER_URL
}

/* ── /doc/ handler ──────────────────────────────────────────────────── */
static void handle_doc(int fd, const char *path) {
    char abs_path[MAX_PATH_LEN];
    if (path[0] != '/' &&
        (strncmp(path, "Volumes/", 8) == 0 || strncmp(path, "Users/", 6) == 0)) {
        snprintf(abs_path, sizeof(abs_path), "/%s", path);
    } else {
        strncpy(abs_path, path, sizeof(abs_path) - 1);
    }

    if (is_markdown_ext(abs_path)) {
        buf_t html;
        buf_init(&html);
        render_markdown(&html, abs_path);
        if (html.len > 0) {
            send_html(fd, html.data, html.len);
        } else {
            serve_file(fd, abs_path, NULL);
        }
        buf_free(&html);
    } else if (is_rtf_ext(abs_path) || is_webarchive_ext(abs_path)) {
        buf_t html;
        buf_init(&html);
        if (convert_via_textutil(&html, abs_path)) {
            send_html(fd, html.data, html.len);
        } else {
            serve_file(fd, abs_path, NULL);
        }
        buf_free(&html);
    } else {
        serve_file(fd, abs_path, NULL);
    }
}

/* ── Save stdin-loaded files to cache DB ───────────────────────────── */
static void save_to_cache(const char *port_str) {
    char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) return;

    char dirpath[1024];
    snprintf(dirpath, sizeof(dirpath), "%s/.minvader", home);
#ifdef _WIN32
    _mkdir(dirpath);
#else
    mkdir(dirpath, 0755);
#endif

    snprintf(g_cache_dbpath, sizeof(g_cache_dbpath), "%s/cache-%s.db", dirpath, port_str);

    sqlite3 *db;
    if (sqlite3_open(g_cache_dbpath, &db) != SQLITE_OK) {
        fprintf(stderr, "warning: cannot create %s\n", g_cache_dbpath);
        g_cache_dbpath[0] = '\0';
        return;
    }

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS files ("
        "  id INTEGER PRIMARY KEY, abs_path TEXT UNIQUE, name TEXT,"
        "  size_bytes INTEGER, os_mod_time INTEGER, os_birth_time INTEGER);"
        "CREATE TABLE IF NOT EXISTS tags (file_id INTEGER, tag_name TEXT,"
        "  UNIQUE(file_id, tag_name));"
        "CREATE TABLE IF NOT EXISTS scan_metadata (id INTEGER PRIMARY KEY, total_files INTEGER);",
        NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *ins;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO files(id, abs_path, name, size_bytes, os_mod_time, os_birth_time) VALUES(?,?,?,?,?,?)",
        -1, &ins, NULL);

    sqlite3_stmt *tag_ins;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO tags(file_id, tag_name) VALUES(?,?)", -1, &tag_ins, NULL);

    for (int i = 0; i < g_nfiles; i++) {
        file_entry_t *f = &g_files[i];
        int64_t fid = i + 1;
        sqlite3_bind_int64(ins, 1, fid);
        sqlite3_bind_text(ins, 2, f->path, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, f->name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 4, f->size);
        sqlite3_bind_int64(ins, 5, (int64_t)f->mtime);
        sqlite3_bind_int64(ins, 6, (int64_t)f->birthtime);
        sqlite3_step(ins);
        sqlite3_reset(ins);
        f->db_id = fid;

        for (int t = 0; t < f->ntags; t++) {
            sqlite3_bind_int64(tag_ins, 1, fid);
            sqlite3_bind_text(tag_ins, 2, f->tags[t], -1, SQLITE_STATIC);
            sqlite3_step(tag_ins);
            sqlite3_reset(tag_ins);
        }
    }

    sqlite3_stmt *meta;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO scan_metadata(id, total_files) VALUES(1,?)", -1, &meta, NULL);
    sqlite3_bind_int(meta, 1, g_nfiles);
    sqlite3_step(meta);
    sqlite3_finalize(meta);

    sqlite3_finalize(ins);
    sqlite3_finalize(tag_ins);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(db);
    fprintf(stderr, "saved %d files to %s\n", g_nfiles, g_cache_dbpath);
}

/* ── Cache DB tag persistence ──────────────────────────────────────── */
static void cache_db_add_tag(int64_t db_id, const char *tag) {
    if (!g_cache_dbpath[0] || db_id == 0) return;
    sqlite3 *db;
    if (sqlite3_open(g_cache_dbpath, &db) != SQLITE_OK) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO tags(file_id, tag_name) VALUES(?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, db_id);
        sqlite3_bind_text(st, 2, tag, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

static void cache_db_remove_tag(int64_t db_id, const char *tag) {
    if (!g_cache_dbpath[0] || db_id == 0) return;
    sqlite3 *db;
    if (sqlite3_open(g_cache_dbpath, &db) != SQLITE_OK) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "DELETE FROM tags WHERE file_id=? AND tag_name=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, db_id);
        sqlite3_bind_text(st, 2, tag, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

/* ── API: /api/addtag ───────────────────────────────────────────────── */
static void handle_api_addtag(int fd, const char *body) {
    char filepath_buf[MAX_PATH_LEN], tag_buf[MAX_TAGLEN];
    json_get_string(body, "filePath", filepath_buf, sizeof(filepath_buf));
    json_get_string(body, "tag", tag_buf, sizeof(tag_buf));

    if (!filepath_buf[0] || !tag_buf[0]) {
        send_400(fd, "Missing filePath or tag");
        return;
    }

    /* Find file in memory */
    int fi = -1;
    for (int i = 0; i < g_nfiles; i++) {
        if (strcmp(g_files[i].path, filepath_buf) == 0) { fi = i; break; }
    }
    if (fi < 0) { send_400(fd, "File not found"); return; }

    file_entry_t *f = &g_files[fi];

    /* Check if tag already exists */
    for (int i = 0; i < f->ntags; i++) {
        if (strcmp(f->tags[i], tag_buf) == 0) {
            /* Already has tag — return success */
            buf_t resp;
            buf_init(&resp);
            buf_appends(&resp, "{\"success\":true,\"tags\":[");
            for (int j = 0; j < f->ntags; j++) {
                if (j > 0) buf_append(&resp, ",", 1);
                buf_append_json_str(&resp, f->tags[j]);
            }
            buf_appends(&resp, "]}");
            send_json(fd, resp.data, resp.len);
            buf_free(&resp);
            return;
        }
    }

    /* Add tag to in-memory file */
    f->tags = realloc(f->tags, (f->ntags + 1) * sizeof(char *));
    f->tags[f->ntags++] = strdup(tag_buf);

    /* Write to xattr on disk (best-effort) */
    char tags_arr[MAX_TAGS][MAX_TAGLEN];
    for (int i = 0; i < f->ntags && i < MAX_TAGS; i++)
        strncpy(tags_arr[i], f->tags[i], MAX_TAGLEN - 1);
    write_current_tags(filepath_buf, tags_arr, f->ntags);

    /* Persist to cache DB */
    cache_db_add_tag(f->db_id, tag_buf);

    /* Update category index */
    category_add_file(find_or_create_category(tag_buf), fi);

    /* Build response */
    buf_t resp;
    buf_init(&resp);
    buf_appends(&resp, "{\"success\":true,\"tags\":[");
    for (int j = 0; j < f->ntags; j++) {
        if (j > 0) buf_append(&resp, ",", 1);
        buf_append_json_str(&resp, f->tags[j]);
    }
    buf_appends(&resp, "]}");
    send_json(fd, resp.data, resp.len);
    buf_free(&resp);
}

/* ── API: /api/removetag ────────────────────────────────────────────── */
static void handle_api_removetag(int fd, const char *body) {
    char filepath_buf[MAX_PATH_LEN], tag_buf[MAX_TAGLEN];
    json_get_string(body, "filePath", filepath_buf, sizeof(filepath_buf));
    json_get_string(body, "tag", tag_buf, sizeof(tag_buf));

    if (!filepath_buf[0] || !tag_buf[0]) {
        send_400(fd, "Missing filePath or tag");
        return;
    }

    int fi = -1;
    for (int i = 0; i < g_nfiles; i++) {
        if (strcmp(g_files[i].path, filepath_buf) == 0) { fi = i; break; }
    }
    if (fi < 0) { send_400(fd, "File not found"); return; }

    file_entry_t *f = &g_files[fi];

    /* Remove tag from in-memory list */
    int found = 0;
    for (int i = 0; i < f->ntags; i++) {
        if (strcmp(f->tags[i], tag_buf) == 0) {
            free(f->tags[i]);
            memmove(&f->tags[i], &f->tags[i+1], (f->ntags - i - 1) * sizeof(char *));
            f->ntags--;
            found = 1;
            break;
        }
    }

    if (found) {
        /* Write updated tags to xattr (best-effort) */
        char tags_arr[MAX_TAGS][MAX_TAGLEN];
        for (int i = 0; i < f->ntags && i < MAX_TAGS; i++)
            strncpy(tags_arr[i], f->tags[i], MAX_TAGLEN - 1);
        write_current_tags(filepath_buf, tags_arr, f->ntags);

        /* Persist to cache DB */
        cache_db_remove_tag(f->db_id, tag_buf);
    }

    /* Build response */
    buf_t resp;
    buf_init(&resp);
    buf_appends(&resp, "{\"success\":true,\"tags\":[");
    for (int j = 0; j < f->ntags; j++) {
        if (j > 0) buf_append(&resp, ",", 1);
        buf_append_json_str(&resp, f->tags[j]);
    }
    buf_appends(&resp, "]}");
    send_json(fd, resp.data, resp.len);
    buf_free(&resp);
}

/* ── API: /api/alltags ──────────────────────────────────────────────── */
static void handle_api_alltags(int fd) {
    buf_t resp;
    buf_init(&resp);
    buf_appends(&resp, "[");

    int first = 1;
    for (int i = 0; i < g_ncategories; i++) {
        if (!first) buf_append(&resp, ",", 1);
        buf_append_json_str(&resp, g_categories[i].name);
        first = 0;
    }

    buf_appends(&resp, "]");
    send_json(fd, resp.data, resp.len);
    buf_free(&resp);
}

/* ── API: /api/filelist ─────────────────────────────────────────────── */
static void handle_api_filelist(int fd, const char *query_str) {
    char category[MAX_PATH_LEN];
    get_query_param(query_str, "category", category, sizeof(category));

    if (!category[0]) {
        send_400(fd, "Missing category parameter");
        return;
    }

    category_t *cat = NULL;
    for (int i = 0; i < g_ncategories; i++) {
        if (strcmp(g_categories[i].name, category) == 0) {
            cat = &g_categories[i];
            break;
        }
    }

    if (!cat) { send_404(fd); return; }

    buf_t resp;
    buf_init(&resp);
    buf_appends(&resp, "[");
    for (int i = 0; i < cat->count; i++) {
        if (i > 0) buf_append(&resp, ",", 1);
        buf_append_json_str(&resp, g_files[cat->file_indices[i]].path);
    }
    buf_appends(&resp, "]");
    send_json(fd, resp.data, resp.len);
    buf_free(&resp);
}

/* ── API: /api/search ───────────────────────────────────────────────── */
static void handle_api_search(int fd, const char *query_str) {
    char q[MAX_PATH_LEN];
    get_query_param(query_str, "q", q, sizeof(q));

    if (!q[0]) {
        send_400(fd, "Missing query parameter 'q'");
        return;
    }

    /* Simple substring search across file names and paths */
    buf_t resp;
    buf_init(&resp);
    buf_appends(&resp, "{\"files\":[");

    int count = 0;
    char q_lower[MAX_PATH_LEN];
    strncpy(q_lower, q, sizeof(q_lower) - 1);
    for (char *p = q_lower; *p; p++) *p = tolower(*p);

    for (int i = 0; i < g_nfiles && count < 1000; i++) {
        /* Case-insensitive search in name */
        char name_lower[MAX_PATH_LEN];
        strncpy(name_lower, g_files[i].name, sizeof(name_lower) - 1);
        for (char *p = name_lower; *p; p++) *p = tolower(*p);

        if (strstr(name_lower, q_lower)) {
            if (count > 0) buf_append(&resp, ",", 1);
            buf_appends(&resp, "{\"Name\":");
            buf_append_json_str(&resp, g_files[i].name);
            buf_appends(&resp, ",\"Path\":");
            buf_append_json_str(&resp, g_files[i].path);
            buf_appends(&resp, ",\"Tags\":[");
            for (int t = 0; t < g_files[i].ntags; t++) {
                if (t > 0) buf_append(&resp, ",", 1);
                buf_append_json_str(&resp, g_files[i].tags[t]);
            }
            buf_appends(&resp, "]}");
            count++;
        }
    }

    buf_appendf(&resp, "],\"count\":%d,\"query\":", count);
    buf_append_json_str(&resp, q);
    buf_appends(&resp, "}");
    send_json(fd, resp.data, resp.len);
    buf_free(&resp);
}

/* ── API: /api/deletefile ── move file to macOS Trash ───────────────── */
static void handle_api_deletefile(int fd, const char *body) {
    char filepath_buf[MAX_PATH_LEN];
    if (json_get_string(body, "filePath", filepath_buf, sizeof(filepath_buf)) < 0) {
        const char *err = "{\"success\":false,\"error\":\"missing filePath\"}";
        send_json(fd, err, strlen(err));
        return;
    }

    /* Use osascript to move to Trash — safe, undoable, Finder-native */
    char cmd[MAX_PATH_LEN + 256];
    /* Escape single quotes in filepath for AppleScript */
    char escaped[MAX_PATH_LEN * 2];
    int ei = 0;
    for (int i = 0; filepath_buf[i] && ei < (int)sizeof(escaped) - 4; i++) {
        if (filepath_buf[i] == '\'') {
            escaped[ei++] = '\'';
            escaped[ei++] = '"';
            escaped[ei++] = '\'';
            escaped[ei++] = '"';
            escaped[ei++] = '\'';
        } else if (filepath_buf[i] == '\\') {
            escaped[ei++] = '\\';
            escaped[ei++] = '\\';
        } else {
            escaped[ei++] = filepath_buf[i];
        }
    }
    escaped[ei] = '\0';

    snprintf(cmd, sizeof(cmd),
        "osascript -e 'tell application \"Finder\" to delete POSIX file \"%s\"' 2>&1",
        escaped);

    FILE *pp = popen(cmd, "r");
    if (!pp) {
        const char *err = "{\"success\":false,\"error\":\"popen failed\"}";
        send_json(fd, err, strlen(err));
        return;
    }
    char result[512] = "";
    fgets(result, sizeof(result), pp);
    int status = pclose(pp);

    if (status == 0) {
        /* Remove from in-memory file list */
        for (int i = 0; i < g_nfiles; i++) {
            if (strcmp(g_files[i].path, filepath_buf) == 0) {
                g_files[i].path[0] = '\0'; /* mark as deleted */
                g_files[i].name = "";
                break;
            }
        }
        const char *ok = "{\"success\":true}";
        send_json(fd, ok, strlen(ok));
    } else {
        buf_t resp;
        buf_init(&resp);
        buf_appends(&resp, "{\"success\":false,\"error\":");
        /* Trim trailing newline from result */
        char *nl = strchr(result, '\n');
        if (nl) *nl = '\0';
        buf_append_json_str(&resp, result[0] ? result : "osascript failed");
        buf_appends(&resp, "}");
        send_json(fd, resp.data, resp.len);
        buf_free(&resp);
    }
}

/* ── API: /api/reveal ── reveal file in Finder ─────────────────────── */
static void handle_api_reveal(int fd, const char *body) {
    char filepath_buf[MAX_PATH_LEN];
    if (json_get_string(body, "filePath", filepath_buf, sizeof(filepath_buf)) < 0) {
        const char *err = "{\"success\":false,\"error\":\"missing filePath\"}";
        send_json(fd, err, strlen(err));
        return;
    }

    /* open -R reveals the file in Finder */
    char cmd[MAX_PATH_LEN + 64];
    char escaped[MAX_PATH_LEN * 2];
    int ei = 0;
    for (int i = 0; filepath_buf[i] && ei < (int)sizeof(escaped) - 2; i++) {
        if (filepath_buf[i] == '\'') {
            escaped[ei++] = '\'';
            escaped[ei++] = '"';
            escaped[ei++] = '\'';
            escaped[ei++] = '"';
            escaped[ei++] = '\'';
        } else {
            escaped[ei++] = filepath_buf[i];
        }
    }
    escaped[ei] = '\0';
    snprintf(cmd, sizeof(cmd), "open -R '%s' 2>/dev/null &", escaped);
    system(cmd);

    const char *ok = "{\"success\":true}";
    send_json(fd, ok, strlen(ok));
}

/* ── API: /api/random ── redirect to random file in category ───────── */
static void handle_api_random(int fd, const char *query_str) {
    char tag[256] = "All";
    char sort_mode[32] = "name";
    char reversed_str[8] = "false";
    char page_size_str[16] = "";
    if (query_str) {
        get_query_param(query_str, "tag", tag, sizeof(tag));
        get_query_param(query_str, "sort", sort_mode, sizeof(sort_mode));
        get_query_param(query_str, "reversed", reversed_str, sizeof(reversed_str));
        get_query_param(query_str, "pageSize", page_size_str, sizeof(page_size_str));
    }

    category_t *cat = NULL;
    for (int i = 0; i < g_ncategories; i++) {
        if (strcmp(g_categories[i].name, tag) == 0) {
            cat = &g_categories[i];
            break;
        }
    }
    if (!cat || cat->count == 0) { send_404(fd); return; }

    int ri = rand() % cat->count;
    file_entry_t *f = &g_files[cat->file_indices[ri]];

    buf_t url;
    buf_init(&url);
    buf_appends(&url, "/view/");
    url_encode(&url, tag);
    buf_appends(&url, "?file=");
    url_encode(&url, f->path);
    buf_appendf(&url, "&sort=%s&reversed=%s", sort_mode, reversed_str);
    if (page_size_str[0]) buf_appendf(&url, "&pageSize=%s", page_size_str);

    char hdr[4096];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n\r\n", url.data);
    sock_write(fd, hdr, strlen(hdr));
    buf_free(&url);
}

/* ── API: /api/shutdown ─────────────────────────────────────────────── */
static void handle_api_shutdown(int fd) {
    const char *resp = "{\"success\":true,\"message\":\"Server shutting down...\"}";
    send_json(fd, resp, strlen(resp));
    fprintf(stderr, "shutdown requested\n");
    close(g_server_fd);
    exit(0);
}

/* ── API: /api/scanstatus ───────────────────────────────────────────── */
static void handle_api_scanstatus(int fd) {
    const char *resp = "{\"isScanning\":false,\"completed\":false}";
    send_json(fd, resp, strlen(resp));
}

/* ── API: /api/rescan (no-op in C version) ──────────────────────────── */
static void handle_api_rescan(int fd) {
    const char *resp = "{\"success\":true,\"message\":\"Rescan not available in lite mode\"}";
    send_json(fd, resp, strlen(resp));
}

/* ── HTTP request parsing ───────────────────────────────────────────── */
static int parse_request(int fd, http_req_t *req) {
    memset(req, 0, sizeof(*req));
    req->fd = fd;

    size_t bufsz = MAX_REQ_SIZE + 4096;
    char *buf = malloc(bufsz);
    if (!buf) return -1;
    size_t total = 0;

    /* Read headers */
    while (total < bufsz - 1) {
        ssize_t n = sock_read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0) { free(buf); return -1; }
        total += n;
        buf[total] = '\0';

        /* Check for end of headers */
        if (strstr(buf, "\r\n\r\n")) break;
    }

    /* Parse request line */
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;

    char request_line[MAX_PATH_LEN * 2];
    size_t rl_len = line_end - buf;
    if (rl_len >= sizeof(request_line)) return -1;
    memcpy(request_line, buf, rl_len);
    request_line[rl_len] = '\0';

    /* Parse "METHOD /path?query HTTP/1.x" */
    char *sp1 = strchr(request_line, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    strncpy(req->method, request_line, sizeof(req->method) - 1);

    char *uri = sp1 + 1;
    char *sp2 = strchr(uri, ' ');
    if (sp2) *sp2 = '\0';

    /* Split path and query */
    char *q = strchr(uri, '?');
    if (q) {
        *q = '\0';
        strncpy(req->query, q + 1, sizeof(req->query) - 1);
    }

    /* URL-decode the path */
    url_decode(req->path, uri, sizeof(req->path));

    /* Parse relevant headers */
    char *hdr_start = line_end + 2;
    char *body_start = strstr(hdr_start, "\r\n\r\n");

    /* Content-Length */
    char *cl = strcasestr(hdr_start, "Content-Length:");
    if (cl && cl < body_start) {
        req->content_length = atol(cl + 15);
    }

    /* Range */
    char *rng = strcasestr(hdr_start, "Range:");
    if (rng && rng < body_start) {
        rng += 6;
        while (*rng == ' ') rng++;
        char *rng_end = strstr(rng, "\r\n");
        if (rng_end) {
            size_t rlen = rng_end - rng;
            if (rlen >= sizeof(req->range_header)) rlen = sizeof(req->range_header) - 1;
            memcpy(req->range_header, rng, rlen);
            req->range_header[rlen] = '\0';
        }
    }

    /* Body */
    if (body_start) {
        body_start += 4;
        size_t already_read = total - (body_start - buf);
        if (already_read > 0 && already_read <= sizeof(req->body) - 1) {
            memcpy(req->body, body_start, already_read);
            req->body_len = already_read;
        }

        /* Read remaining body if needed */
        while (req->body_len < req->content_length && req->body_len < sizeof(req->body) - 1) {
            ssize_t n = sock_read(fd, req->body + req->body_len,
                                  sizeof(req->body) - 1 - req->body_len);
            if (n <= 0) break;
            req->body_len += n;
        }
        req->body[req->body_len] = '\0';
    }

    free(buf);
    return 0;
}

/* ── Request router ─────────────────────────────────────────────────── */
static void handle_request(int fd) {
    http_req_t req;
    if (parse_request(fd, &req) < 0) return;

    const char *path = req.path;

    /* Route: / */
    if (strcmp(path, "/") == 0) {
        handle_index(fd);
        return;
    }

    /* Route: /file/... */
    if (strncmp(path, "/file/", 6) == 0) {
        const char *fpath = path + 6;
        char abs_path[MAX_PATH_LEN];
        if (fpath[0] != '/' &&
            (strncmp(fpath, "Volumes/", 8) == 0 || strncmp(fpath, "Users/", 6) == 0)) {
            snprintf(abs_path, sizeof(abs_path), "/%s", fpath);
        } else {
            strncpy(abs_path, fpath, sizeof(abs_path) - 1);
        }
        serve_file(fd, abs_path, req.range_header);
        return;
    }

    /* Route: /tag/... */
    if (strncmp(path, "/tag/", 5) == 0) {
        handle_gallery(fd, path + 5, req.query);
        return;
    }

    /* Route: /view/... */
    if (strncmp(path, "/view/", 6) == 0) {
        handle_viewer(fd, path + 6, req.query);
        return;
    }

    /* Route: /doc/... */
    if (strncmp(path, "/doc/", 5) == 0) {
        handle_doc(fd, path + 5);
        return;
    }

    /* API routes */
    if (strcmp(path, "/api/addtag") == 0) {
        if (strcmp(req.method, "POST") != 0) { send_405(fd); return; }
        handle_api_addtag(fd, req.body);
        return;
    }
    if (strcmp(path, "/api/removetag") == 0) {
        if (strcmp(req.method, "POST") != 0) { send_405(fd); return; }
        handle_api_removetag(fd, req.body);
        return;
    }
    if (strcmp(path, "/api/alltags") == 0) {
        handle_api_alltags(fd);
        return;
    }
    if (strcmp(path, "/api/filelist") == 0) {
        handle_api_filelist(fd, req.query);
        return;
    }
    if (strcmp(path, "/api/search") == 0) {
        handle_api_search(fd, req.query);
        return;
    }
    if (strcmp(path, "/api/deletefile") == 0) {
        if (strcmp(req.method, "POST") != 0) { send_405(fd); return; }
        handle_api_deletefile(fd, req.body);
        return;
    }
    if (strcmp(path, "/api/reveal") == 0) {
        if (strcmp(req.method, "POST") != 0) { send_405(fd); return; }
        handle_api_reveal(fd, req.body);
        return;
    }
    if (strcmp(path, "/api/random") == 0) {
        handle_api_random(fd, req.query);
        return;
    }
    if (strcmp(path, "/api/shutdown") == 0) {
        if (strcmp(req.method, "POST") != 0) { send_405(fd); return; }
        handle_api_shutdown(fd);
        return;
    }
    if (strcmp(path, "/api/scanstatus") == 0) {
        handle_api_scanstatus(fd);
        return;
    }
    if (strcmp(path, "/api/rescan") == 0) {
        handle_api_rescan(fd);
        return;
    }

    /* Fallback */
    send_404(fd);
}

/* ── Main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int use_stdin = 0;
    int use_warm = 0;
    const char *port_str = "8080";

    srand((unsigned)time(NULL));
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdin") == 0) {
            use_stdin = 1;
        } else if (strcmp(argv[i], "--warm") == 0) {
            use_warm = 1;
        } else if (strcmp(argv[i], "--no-watch") == 0) {
            /* compatibility with Go version, no-op */
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            port_str = argv[i] + 7;
            g_port = atoi(port_str);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port_str = argv[++i];
            g_port = atoi(port_str);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: minvader [--warm] [--stdin] [--port=PORT] [--no-watch]\n");
            fprintf(stderr, "  --warm      Load files from ~/.minvader/cache-PORT.db\n");
            fprintf(stderr, "  --stdin     Read file paths from stdin (one per line)\n");
            fprintf(stderr, "  --port=N    Listen on port N (default: 8080)\n");
            fprintf(stderr, "  --no-watch  No filesystem watcher (default, compatibility flag)\n");
            return 0;
        }
    }

    if (use_warm) {
        load_from_cache(port_str);
    } else if (use_stdin) {
        fprintf(stderr, "reading file paths from stdin...\n");
        read_stdin_paths();
        save_to_cache(port_str);
    } else {
        fprintf(stderr, "error: need --warm or --stdin\n");
        return 1;
    }

    if (g_nfiles == 0) {
        fprintf(stderr, "no files loaded, nothing to serve\n");
        return 1;
    }

    fprintf(stderr, "building category index...\n");
    build_categories();
    fprintf(stderr, "%d files, %d categories\n", g_nfiles, g_ncategories);

#ifdef _WIN32
    { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
#endif

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(g_server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "\nminvader started\n");
    fprintf(stderr, "URL: http://localhost:%d\n", g_port);
    fprintf(stderr, "Press Ctrl+C to stop\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_request(client_fd);
        sock_close(client_fd);
    }

    return 0;
}
