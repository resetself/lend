#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>

#define BUF_SIZE 4096
#define BIN_DIR ".lend/bin"

enum { K_STR = 0, K_FILE = 1, K_DIR = 2, K_OUT = 3 };

static const char* get_app(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ---------- low-level IO ---------- */

static int write_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_exact(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Read a newline-terminated line (without the newline). */
static int read_line(int fd, char* buf, size_t cap) {
    size_t i = 0;
    while (i < cap - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}

/* ---------- growable buffer for directory blobs ---------- */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} blob_t;

static void blob_init(blob_t* b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void blob_free(blob_t* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int blob_reserve(blob_t* b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + extra) ncap *= 2;
    char* nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static void blob_put(blob_t* b, const void* p, size_t n) {
    if (blob_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void blob_put_u32(blob_t* b, uint32_t v) {
    unsigned char be[4];
    be[0] = (unsigned char)(v >> 24);
    be[1] = (unsigned char)(v >> 16);
    be[2] = (unsigned char)(v >> 8);
    be[3] = (unsigned char)(v);
    blob_put(b, be, 4);
}

static void blob_put_u64(blob_t* b, uint64_t v) {
    unsigned char be[8];
    for (int i = 7; i >= 0; i--) {
        be[i] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
    blob_put(b, be, 8);
}

static void blob_patch_u64(blob_t* b, size_t pos, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        b->data[pos + (size_t)i] = (char)(v & 0xff);
        v >>= 8;
    }
}

/* ---------- directory tree builder ---------- */

static int walk_dir(blob_t* b, const char* root, const char* rel, int depth) {
    (void)depth;
    DIR* d = opendir(root);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char full[PATH_MAX];
        char child_rel[PATH_MAX];
        snprintf(full, PATH_MAX, "%s/%s", root, e->d_name);
        if (rel[0]) snprintf(child_rel, PATH_MAX, "%s/%s", rel, e->d_name);
        else snprintf(child_rel, PATH_MAX, "%s", e->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlink(full, target, sizeof(target) - 1);
            if (n < 0) continue;
            target[n] = '\0';
            blob_put(b, "L", 1);
            blob_put_u32(b, (uint32_t)strlen(child_rel));
            blob_put(b, child_rel, strlen(child_rel));
            blob_put_u32(b, (uint32_t)n);
            blob_put(b, target, (size_t)n);
        } else if (S_ISDIR(st.st_mode)) {
            blob_put(b, "D", 1);
            blob_put_u32(b, (uint32_t)strlen(child_rel));
            blob_put(b, child_rel, strlen(child_rel));
            if (walk_dir(b, full, child_rel, depth + 1) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            int f = open(full, O_RDONLY);
            if (f < 0) continue;
            blob_put(b, "F", 1);
            blob_put_u32(b, (uint32_t)strlen(child_rel));
            blob_put(b, child_rel, strlen(child_rel));
            size_t lenpos = b->len;
            blob_put_u64(b, 0); /* placeholder */
            char buf[65536];
            ssize_t n;
            uint64_t total = 0;
            int failed = 0;
            while ((n = read(f, buf, sizeof(buf))) > 0) {
                blob_put(b, buf, (size_t)n);
                total += (uint64_t)n;
            }
            if (n < 0) failed = 1;
            close(f);
            if (failed) {
                /* roll back this entry: shrink to before the type byte */
                b->len = lenpos - 1 - 4 - strlen(child_rel);
                continue;
            }
            blob_patch_u64(b, lenpos, total);
        }
        /* skip sockets, fifos, devices */
    }
    closedir(d);
    return 0;
}

/* ---------- protocol senders ---------- */

static int send_str(int fd, const char* s) {
    size_t n = strlen(s);
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "S %zu\n", n);
    if (write_all(fd, hdr, (size_t)hl) < 0) return -1;
    return write_all(fd, s, n);
}

static int send_file(int fd, const char* path, const char* name) {
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    struct stat st;
    if (fstat(f, &st) != 0 || st.st_size < 0) { close(f); return -1; }
    char* content = malloc((size_t)st.st_size + 1);
    if (!content) { close(f); return -1; }
    /* Read in a loop: a single read() may return fewer bytes than requested. */
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(f, content + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(f);
    if (got != (size_t)st.st_size) { free(content); return -1; }

    size_t name_len = strlen(name);
    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr), "F %zu %zu\n", name_len, got);
    if (write_all(fd, hdr, (size_t)hl) < 0) { free(content); return -1; }
    if (write_all(fd, name, name_len) < 0) { free(content); return -1; }
    int rc = write_all(fd, content, (size_t)got);
    free(content);
    return rc;
}

static int send_dir(int fd, const char* path) {
    blob_t b;
    blob_init(&b);
    if (walk_dir(&b, path, "", 0) != 0) { blob_free(&b); return -1; }
    blob_put(&b, "E", 1);

    const char* name = get_app(path);
    size_t name_len = strlen(name);
    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr), "D %zu %zu\n", name_len, b.len);
    int rc = -1;
    if (write_all(fd, hdr, (size_t)hl) < 0) goto out;
    if (write_all(fd, name, name_len) < 0) goto out;
    if (write_all(fd, b.data, b.len) < 0) goto out;
    rc = 0;
out:
    blob_free(&b);
    return rc;
}

static int send_output(int fd, const char* p) {
    const char* name = get_app(p);
    size_t name_len = strlen(name);
    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr), "O %zu\n", name_len);
    if (write_all(fd, hdr, (size_t)hl) < 0) return -1;
    return write_all(fd, name, name_len);
}

/* ---------- classification ---------- */

static int classify(const char* p) {
    /* Existing paths take priority, so a file literally named "-x" still
     * resolves to a file rather than a flag. */
    struct stat st;
    if (stat(p, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return K_DIR;
        if (S_ISREG(st.st_mode)) return K_FILE;
        return K_STR; /* symlink/other resolved to non-regular: treat as string */
    }
    if (p[0] == '-') return K_STR;
    /* non-existent: a path-looking argument is an output file */
    if (strchr(p, '/') || strchr(p, '.')) return K_OUT;
    return K_STR;
}

/* GUI editors that fork and return: handled as a background live-sync session
 * (keep this list in sync with waitFlags in lendd.go). */
static int is_editor(const char* tool) {
    static const char* editors[] = {
        "subl", "sublime", "sublime_text", "code", "code-insiders",
        "atom", "gedit", "kate", NULL
    };
    for (int i = 0; editors[i]; i++) {
        if (!strcmp(tool, editors[i])) return 1;
    }
    return 0;
}

/* ---------- response handling ---------- */

static int mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void parent_dir(const char* path, char* out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char* s = strrchr(out, '/');
    if (!s) {
        strcpy(out, ".");
    } else if (s == out) {
        s[1] = '\0';
    } else {
        *s = '\0';
    }
}

static int write_file_at(const char* path, const char* data, size_t len) {
    char parent[PATH_MAX];
    parent_dir(path, parent, sizeof(parent));
    if (mkdir_p(parent) != 0) return -1;
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return -1;
    int rc = write_all(f, data, len);
    close(f);
    return rc;
}

static uint32_t read_u32be(const char* p, size_t* off) {
    uint32_t v = ((uint32_t)(unsigned char)p[*off] << 24)
               | ((uint32_t)(unsigned char)p[*off + 1] << 16)
               | ((uint32_t)(unsigned char)p[*off + 2] << 8)
               | ((uint32_t)(unsigned char)p[*off + 3]);
    *off += 4;
    return v;
}

static uint64_t read_u64be(const char* p, size_t* off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)(unsigned char)p[*off + i];
    }
    *off += 8;
    return v;
}

static int name_ok(const char* name) {
    if (name[0] == '\0' || name[0] == '/') return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    const char* p = name;
    while ((p = strstr(p, "/..")) != NULL) {
        if (p[3] == '\0' || p[3] == '/') return 0;
        p += 3;
    }
    return 1;
}

/* Read a length-prefixed payload into a freshly allocated buffer. */
static char* read_payload(int fd, long len, size_t* out_len) {
    if (len < 0) { *out_len = 0; return NULL; }
    char* buf = malloc((size_t)len);
    if (!buf) { *out_len = 0; return NULL; }
    if (read_exact(fd, buf, (size_t)len) < 0) { free(buf); *out_len = 0; return NULL; }
    *out_len = (size_t)len;
    return buf;
}

/* Apply a directory blob to dest (overwrite-or-create, never delete). */
static int apply_tree(const char* blob, size_t len, const char* dest) {
    if (mkdir_p(dest) != 0) return -1;
    size_t off = 0;
    while (off < len) {
        char typ = blob[off++];
        if (typ == 'E') return 0;

        uint32_t name_len = read_u32be(blob, &off);
        if (name_len >= PATH_MAX || off + name_len > len) return -1;
        char name[PATH_MAX];
        memcpy(name, blob + off, name_len);
        off += name_len;
        name[name_len] = '\0';
        if (!name_ok(name)) return -1;

        char full[PATH_MAX];
        snprintf(full, PATH_MAX, "%s/%s", dest, name);

        if (typ == 'D') {
            if (mkdir_p(full) != 0) return -1;
        } else if (typ == 'F') {
            uint64_t clen = read_u64be(blob, &off);
            if (clen > (uint64_t)(len - off)) return -1;
            if (write_file_at(full, blob + off, (size_t)clen) != 0) return -1;
            off += (size_t)clen;
        } else if (typ == 'L') {
            uint32_t tlen = read_u32be(blob, &off);
            if (tlen >= PATH_MAX || off + tlen > len) return -1;
            char target[PATH_MAX];
            memcpy(target, blob + off, tlen);
            off += tlen;
            target[tlen] = '\0';
            char parent[PATH_MAX];
            parent_dir(full, parent, sizeof(parent));
            if (mkdir_p(parent) != 0) return -1;
            unlink(full);
            if (symlink(target, full) != 0) return -1;
        } else {
            return -1;
        }
    }
    return -1; /* missing E terminator */
}

/* Parse a "<TAG> <n>" or "<TAG> -1" header line; return n (or -1). */
static long parse_header_value(const char* line) {
    const char* sp = strchr(line, ' ');
    if (!sp) return -1;
    return atol(sp + 1);
}

/* Consume stdout/stderr frames and stream them to the terminal. */
static int drain_frame(int fd, FILE* dest) {
    char hdr[64];
    if (read_line(fd, hdr, sizeof(hdr)) < 0) return -1;
    long len = parse_header_value(hdr);
    if (len <= 0) return 0;
    char buf[65536];
    long remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > (long)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        if (read_exact(fd, buf, chunk) < 0) return -1;
        fwrite(buf, 1, chunk, dest);
        remaining -= (long)chunk;
    }
    fflush(dest);
    return 0;
}

static int handle_response(int fd, int nargs, char** args, const int* kinds) {
    /* RESULT <code> */
    char line[256];
    if (read_line(fd, line, sizeof(line)) < 0) return 1;
    int exit_code = (int)parse_header_value(line);

    /* SO, SE */
    if (drain_frame(fd, stdout) < 0) return 1;
    if (drain_frame(fd, stderr) < 0) return 1;

    /* Per-arg frames for F/D/O args, in request order. */
    for (int i = 0; i < nargs; i++) {
        if (kinds[i] == K_STR) continue;

        char hdr[64];
        if (read_line(fd, hdr, sizeof(hdr)) < 0) return 1;
        long len = parse_header_value(hdr);

        if (len < 0) {
            continue; /* skip: unchanged or not created */
        }

        size_t plen = 0;
        char* payload = read_payload(fd, len, &plen);
        if (!payload && len > 0) return 1;

        switch (kinds[i]) {
        case K_FILE: /* RF: overwrite input file */
            if (write_file_at(args[i], payload ? payload : "", plen) != 0) {
                fprintf(stderr, "lendctl: failed to write back %s\n", args[i]);
            }
            break;
        case K_OUT: /* RO: create output file */
            if (write_file_at(args[i], payload ? payload : "", plen) != 0) {
                fprintf(stderr, "lendctl: failed to write back %s\n", args[i]);
            }
            break;
        case K_DIR: /* RD: overwrite-or-create tree */
            if (apply_tree(payload ? payload : "", plen, args[i]) != 0) {
                fprintf(stderr, "lendctl: failed to apply directory %s\n", args[i]);
            }
            break;
        }
        free(payload);
    }

    return exit_code;
}

/* Long-lived edit session: receive UPDATE/TREE frames pushed by lendd on each
 * save, write them back to the remote paths, and exit on SESSION_END. */
static int edit_loop(int fd, int nargs, char** args, const int* kinds) {
    char line[256];
    if (read_line(fd, line, sizeof(line)) < 0) return 1;
    if (strncmp(line, "EDIT", 4) != 0) return 1; /* not an edit session */

    for (;;) {
        if (read_line(fd, line, sizeof(line)) < 0) return 1;

        if (strncmp(line, "SESSION_END", 11) == 0) return 0;

        if (strncmp(line, "UPDATE ", 7) == 0) {
            char* sp = strchr(line + 7, ' ');
            if (!sp) return 1;
            *sp = '\0';
            int idx = atoi(line + 7);
            long len = atol(sp + 1);
            if (idx < 0 || idx >= nargs || kinds[idx] != K_FILE) return 1;
            size_t plen = 0;
            char* payload = read_payload(fd, len, &plen);
            if (!payload && len > 0) return 1;
            (void)write_file_at(args[idx], payload ? payload : "", plen);
            free(payload);
        } else if (strncmp(line, "TREE ", 5) == 0) {
            char* sp = strchr(line + 5, ' ');
            if (!sp) return 1;
            *sp = '\0';
            int idx = atoi(line + 5);
            long len = atol(sp + 1);
            if (idx < 0 || idx >= nargs || kinds[idx] != K_DIR) return 1;
            size_t plen = 0;
            char* payload = read_payload(fd, len, &plen);
            if (!payload && len > 0) return 1;
            (void)apply_tree(payload ? payload : "", plen, args[idx]);
            free(payload);
        } else {
            return 1;
        }
    }
}

/* ---------- misc ---------- */

static int connect_bridge(void) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    char sock[PATH_MAX];
    snprintf(sock, sizeof(sock), "%s/.lend/bridge.sock", home);

    /* The reverse forward is a separate ssh process that may still be
     * establishing the socket when the first command runs, so retry briefly. */
    for (int attempt = 0; attempt < 15; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
        if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0) {
            return fd;
        }
        close(fd);
        if (attempt < 14) usleep(200000);
    }
    return -1;
}

static void init_dir(const char* d) {
    struct stat st;
    if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (mkdir(d, 0755) != 0 && errno != EEXIST) {
            perror("failed to create directory");
            exit(1);
        }
    }
}

static int do_link(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: lendctl link <name>\n");
        return 1;
    }
    const char* home = getenv("HOME");
    if (!home) { fprintf(stderr, "HOME not set\n"); return 1; }
    char dir[PATH_MAX], link[PATH_MAX], self[PATH_MAX];
    snprintf(dir, PATH_MAX, "%s/%s", home, BIN_DIR);
    init_dir(dir);

    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len == -1) { perror("failed to get executable path"); return 1; }
    self[len] = '\0';

    snprintf(link, PATH_MAX, "%s/%s", dir, argv[2]);
    unlink(link);
    if (symlink(self, link) < 0) { perror("failed to create link"); return 1; }
    return 0;
}

static void show_help(void) {
    printf("Usage: lendctl <command> [args]\n");
    printf("Commands:\n");
    printf("  link <name>    - Create link in ~/.lend/bin\n");
    printf("  <name> [args]  - Run a local tool on remote files\n");
    printf("  -h             - Show help\n");
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        show_help();
        return 0;
    }

    const char* tool = get_app(argv[0]);
    int start = 1;

    if (!strcmp(tool, "lendctl") && argc >= 2) {
        if (!strcmp(argv[1], "link")) {
            return do_link(argc, argv);
        }
        tool = argv[1];
        start = 2;
    }

    int nargs = argc - start;
    if (nargs < 0) { show_help(); return 1; }

    int fd = connect_bridge();
    if (fd < 0) {
        fprintf(stderr, "lendctl: cannot reach local lendd. Is the SSH session active?\n");
        return 1;
    }

    /* RUN <tool> <nargs> */
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr), "RUN %s %d\n", tool, nargs);
    if (write_all(fd, hdr, (size_t)hl) < 0) { close(fd); return 1; }

    /* classify and send each arg, remembering kinds for write-back mapping */
    int* kinds = malloc((size_t)(nargs > 0 ? nargs : 1) * sizeof(int));
    if (!kinds) { close(fd); return 1; }

    int send_rc = 0;
    for (int i = 0; i < nargs; i++) {
        int k = classify(argv[start + i]);
        kinds[i] = k;
        int rc = 0;
        switch (k) {
        case K_STR:  rc = send_str(fd, argv[start + i]); break;
        case K_FILE: rc = send_file(fd, argv[start + i], get_app(argv[start + i])); break;
        case K_DIR:  rc = send_dir(fd, argv[start + i]); break;
        case K_OUT:  rc = send_output(fd, argv[start + i]); break;
        }
        if (rc < 0) { send_rc = -1; break; }
    }

    int exit_code = 1;
    if (send_rc == 0) {
        if (is_editor(tool)) {
            /* Editor: fork a detached background process to stream saves back
             * to the remote paths, while the foreground returns immediately. */
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int nullfd = open("/dev/null", O_RDWR);
                if (nullfd >= 0) {
                    dup2(nullfd, STDIN_FILENO);
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    if (nullfd > 2) close(nullfd);
                }
                int rc = edit_loop(fd, nargs, &argv[start], kinds);
                close(fd);
                free(kinds);
                _exit(rc);
            }
            exit_code = (pid < 0) ? 1 : 0;
        } else {
            exit_code = handle_response(fd, nargs, &argv[start], kinds);
        }
    }

    free(kinds);
    close(fd);
    return exit_code;
}
