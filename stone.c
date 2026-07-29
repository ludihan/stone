#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#define PROGRAM_NAME "stone"

enum {
    FLAG_DELETE   = 1 << 0,
    FLAG_ADOPT    = 1 << 1,
    FLAG_OVERWRITE = 1 << 2,
};

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", PROGRAM_NAME, msg);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t sz)
{
    void *p = malloc(sz);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    bool need_slash = la > 0 && a[la - 1] != '/';
    char *p = xmalloc(la + need_slash + lb + 1);
    memcpy(p, a, la);
    if (need_slash) p[la] = '/';
    memcpy(p + la + need_slash, b, lb + 1);
    return p;
}

static char *parent_dir(const char *path)
{
    char *p = xstrdup(path);
    char *s = strrchr(p, '/');
    if (!s) {
        free(p);
        return xstrdup(".");
    }
    if (s == p)
        *(s + 1) = '\0';
    else
        *s = '\0';
    return p;
}

// -- ignore rules --

typedef struct {
    char *pattern;
    bool negated;
    bool dir_only;
    bool anchored;
} IgnoreEntry;

typedef struct {
    IgnoreEntry *entries;
    int count;
    int capacity;
} IgnoreRules;

static bool
match_glob(const char *pat, const char *str)
{
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            pat += 2;
            if (*pat == '/') pat++;
            while (*str) {
                if (match_glob(pat, str)) return true;
                str++;
            }
            return match_glob(pat, str);
        } else if (*pat == '*') {
            pat++;
            while (*str && *str != '/') {
                if (match_glob(pat, str)) return true;
                str++;
            }
            return match_glob(pat, str);
        } else if (*pat == '?') {
            pat++;
            if (!*str || *str == '/') return false;
            str++;
        } else {
            if (*pat != *str) return false;
            pat++;
            str++;
        }
    }
    return *str == '\0';
}

static void
ignore_free(IgnoreRules *rules)
{
    if (!rules) return;
    for (int i = 0; i < rules->count; i++)
        free(rules->entries[i].pattern);
    free(rules->entries);
    free(rules);
}

static void
ignore_add_entry(IgnoreRules *rules, const char *pat,
                 bool negated, bool dir_only, bool anchored)
{
    if (rules->count >= rules->capacity) {
        rules->capacity = rules->capacity ? rules->capacity * 2 : 8;
        rules->entries = realloc(rules->entries,
            (size_t)rules->capacity * sizeof(IgnoreEntry));
        if (!rules->entries) die("out of memory");
    }
    IgnoreEntry *e = &rules->entries[rules->count++];
    e->pattern = xstrdup(pat);
    e->negated = negated;
    e->dir_only = dir_only;
    e->anchored = anchored;
}

static IgnoreRules *
ignore_load(const char *src_base)
{
    IgnoreRules *rules = xmalloc(sizeof(IgnoreRules));
    rules->entries = NULL;
    rules->count = 0;
    rules->capacity = 0;

    ignore_add_entry(rules, ".git",     false, true,  true);
    ignore_add_entry(rules, ".github",  false, true,  true);
    ignore_add_entry(rules, ".gitignore",   false, false, true);
    ignore_add_entry(rules, ".gitattributes", false, false, true);
    ignore_add_entry(rules, ".gitmodules",   false, false, true);

    char *path = path_join(src_base, ".stone-ignore");
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return rules;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        bool negated = false;
        if (*p == '!') {
            negated = true;
            p++;
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        size_t plen = strlen(p);
        while (plen > 0 && (p[plen - 1] == ' ' || p[plen - 1] == '\t'))
            plen--;

        bool dir_only = plen > 0 && p[plen - 1] == '/';
        if (dir_only) plen--;

        bool anchored = false;

        const char *pat_start = p;
        size_t pat_len = plen;

        if (pat_len > 0 && pat_start[0] == '/') {
            anchored = true;
            pat_start++;
            pat_len--;
        }

        if (!anchored) {
            for (size_t j = 0; j < pat_len; j++) {
                if (pat_start[j] == '/') { anchored = true; break; }
            }
        }

        if (pat_len == 0) continue;

        if (rules->count >= rules->capacity) {
            rules->capacity = rules->capacity ? rules->capacity * 2 : 8;
            rules->entries = realloc(rules->entries,
                (size_t)rules->capacity * sizeof(IgnoreEntry));
            if (!rules->entries) die("out of memory");
        }

        IgnoreEntry *e = &rules->entries[rules->count++];
        e->pattern = strndup(pat_start, pat_len);
        if (!e->pattern) die("out of memory");
        e->negated = negated;
        e->dir_only = dir_only;
        e->anchored = anchored;
    }

    fclose(f);
    return rules;
}

static bool
ignore_match(IgnoreRules *rules, const char *rel_path, bool is_dir)
{
    if (!rules) return false;

    bool ignored = false;
    for (int i = 0; i < rules->count; i++) {
        IgnoreEntry *e = &rules->entries[i];
        if (e->dir_only && !is_dir) continue;

        const char *target = e->anchored ? rel_path : strrchr(rel_path, '/');
        if (target && !e->anchored) target++;
        if (!target) target = rel_path;

        if (match_glob(e->pattern, target))
            ignored = !e->negated;
    }
    return ignored;
}

// -- file ops --

static int copy_file(const char *src, const char *dst)
{
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) return -1;
    int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0) { close(fd_src); return -1; }

    char buf[8192];
    ssize_t n;
    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd_dst, buf + off, (size_t)(n - off));
            if (w < 0) { close(fd_src); close(fd_dst); return -1; }
            off += w;
        }
    }
    close(fd_src);
    if (close(fd_dst) < 0) return -1;
    return n == 0 ? 0 : -1;
}

static bool files_equal(const char *a, const char *b)
{
    struct stat sa, sb;
    if (lstat(a, &sa) < 0 || lstat(b, &sb) < 0) return false;
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode)) return false;
    if (sa.st_size != sb.st_size) return false;

    int fa = open(a, O_RDONLY);
    int fb = open(b, O_RDONLY);
    if (fa < 0 || fb < 0) {
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        return false;
    }

    char bufa[4096], bufb[4096];
    bool equal = true;
    for (;;) {
        ssize_t na = read(fa, bufa, sizeof(bufa));
        ssize_t nb = read(fb, bufb, sizeof(bufb));
        if (na != nb) { equal = false; break; }
        if (na == 0) break;
        if (memcmp(bufa, bufb, (size_t)na) != 0) { equal = false; break; }
    }
    close(fa);
    close(fb);
    return equal;
}

static int mkdir_p(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0) return 0;

    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(p, mode);
            *s = '/';
        }
    }
    int r = mkdir(p, mode);
    free(p);
    return (r == 0 || errno == EEXIST) ? 0 : -1;
}

static char *make_relpath(const char *from_dir, const char *to_path)
{
    char *rf = realpath(from_dir, NULL);
    char *rt = realpath(to_path, NULL);
    if (!rf || !rt) { free(rf); free(rt); return NULL; }

    int nf = 0;
    for (const char *p = rf + 1; *p; p++) if (*p == '/') nf++;
    nf++;
    int nt = 0;
    for (const char *p = rt + 1; *p; p++) if (*p == '/') nt++;
    nt++;

    char **fc = xmalloc((size_t)nf * sizeof(char *));
    char **tc = xmalloc((size_t)nt * sizeof(char *));

    const char *p = rf;
    if (*p == '/') p++;
    for (int i = 0; i < nf; i++) {
        const char *next = strchr(p, '/');
        if (next) {
            fc[i] = strndup(p, (size_t)(next - p));
            p = next + 1;
        } else {
            fc[i] = xstrdup(p);
            if (*p) p += strlen(p);
        }
    }

    p = rt;
    if (*p == '/') p++;
    for (int i = 0; i < nt; i++) {
        const char *next = strchr(p, '/');
        if (next) {
            tc[i] = strndup(p, (size_t)(next - p));
            p = next + 1;
        } else {
            tc[i] = xstrdup(p);
            if (*p) p += strlen(p);
        }
    }

    int common = 0;
    while (common < nf && common < nt && strcmp(fc[common], tc[common]) == 0)
        common++;

    size_t sz = 1;
    for (int i = common; i < nf; i++) sz += 3;
    for (int i = common; i < nt; i++) sz += strlen(tc[i]) + 1;

    char *result = xmalloc(sz);
    result[0] = '\0';
    for (int i = common; i < nf; i++) strcat(result, "../");
    for (int i = common; i < nt; i++) {
        strcat(result, tc[i]);
        if (i < nt - 1) strcat(result, "/");
    }
    if (result[0] == '\0') strcpy(result, ".");

    for (int i = 0; i < nf; i++) free(fc[i]);
    for (int i = 0; i < nt; i++) free(tc[i]);
    free(fc);
    free(tc);
    free(rf);
    free(rt);

    return result;
}

static char *get_symlink_target(const char *src_abs, const char *tgt_base, const char *rel)
{
    char *tgt_path = path_join(tgt_base, rel);
    char *tgt_dir = parent_dir(tgt_path);
    char *target = make_relpath(tgt_dir, src_abs);
    free(tgt_path);
    free(tgt_dir);
    if (target) return target;
    return xstrdup(src_abs);
}

typedef struct {
    char *src_base;
    char *tgt_base;
    int flags;
    IgnoreRules *ignore;
} Context;

static int walk_delete(Context *, const char *, const char *, const char *);
static int walk_stow(Context *, const char *, const char *, const char *);
static int walk_check(Context *, const char *, const char *, const char *);

static int check_entry_stow(Context *ctx, const char *src_path,
                            const char *tgt_path, const char *rel)
{
    struct stat st;
    if (lstat(src_path, &st) < 0) return 0;

    if (S_ISDIR(st.st_mode))
        return walk_check(ctx, src_path, tgt_path, rel);

    if (S_ISLNK(st.st_mode)) {
        char linkbuf[PATH_MAX];
        ssize_t n = readlink(src_path, linkbuf, sizeof(linkbuf) - 1);
        if (n < 0) return 0;
        linkbuf[n] = '\0';

        char *src_dir = parent_dir(src_path);
        char *candidate = linkbuf[0] == '/'
                            ? xstrdup(linkbuf)
                            : path_join(src_dir, linkbuf);
        free(src_dir);

        char *resolved = realpath(candidate, NULL);
        free(candidate);
        if (!resolved) return 0;

        struct stat tst;
        int ret = 0;
        if (lstat(tgt_path, &tst) == 0) {
            bool ok = false;
            if (S_ISLNK(tst.st_mode)) {
                char *tgt_resolved = realpath(tgt_path, NULL);
                if (tgt_resolved && strcmp(resolved, tgt_resolved) == 0)
                    ok = true;
                free(tgt_resolved);
            }
            if (!ok && !(ctx->flags & FLAG_ADOPT) && !(ctx->flags & FLAG_OVERWRITE)) {
                fprintf(stderr, "%s: warning: %s points to a different target than %s\n",
                        PROGRAM_NAME, tgt_path, src_path);
                ret = -1;
            }
        }
        free(resolved);
        return ret;
    }

    if (S_ISREG(st.st_mode)) {
        struct stat tst;
        if (lstat(tgt_path, &tst) < 0) return 0;

        if (S_ISLNK(tst.st_mode)) {
            char *expected = get_symlink_target(src_path, ctx->tgt_base, rel);
            char buf[PATH_MAX];
            ssize_t n = readlink(tgt_path, buf, sizeof(buf) - 1);
            int ok = false;
            if (n > 0) {
                buf[n] = '\0';
                ok = (strcmp(buf, expected) == 0);
            }
            free(expected);
            if (ok) return 0;
        }

        bool same = (S_ISREG(tst.st_mode) && files_equal(src_path, tgt_path));

        if (same || (ctx->flags & FLAG_ADOPT) || (ctx->flags & FLAG_OVERWRITE))
            return 0;

        fprintf(stderr, "%s: warning: %s already exists and differs from %s\n",
                PROGRAM_NAME, tgt_path, src_path);
        return -1;
    }

    return 0;
}

static int walk_check(Context *ctx, const char *src_dir,
                      const char *tgt_dir, const char *rel_prefix)
{
    DIR *d = opendir(src_dir);
    if (!d) return 0;

    int ret = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".stone-ignore") == 0)
            continue;

        char *rel;
        if (rel_prefix[0] == '\0')
            rel = xstrdup(entry->d_name);
        else
            rel = path_join(rel_prefix, entry->d_name);

        struct stat st;
        bool is_dir = false;
        char *src_path = path_join(src_dir, entry->d_name);
        if (lstat(src_path, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = true;

        if (ignore_match(ctx->ignore, rel, is_dir)) {
            free(src_path);
            free(rel);
            continue;
        }

        char *tgt_path = path_join(tgt_dir, entry->d_name);

        if (check_entry_stow(ctx, src_path, tgt_path, rel) < 0)
            ret = -1;

        free(src_path);
        free(tgt_path);
        free(rel);
    }

    closedir(d);
    return ret;
}

static int process_entry_stow(Context *ctx, const char *src_path,
                              const char *tgt_path, const char *rel)
{
    struct stat st;
    if (lstat(src_path, &st) < 0) {
        fprintf(stderr, "%s: cannot stat %s: %s\n",
                PROGRAM_NAME, src_path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        mkdir_p(tgt_path, st.st_mode);
        return walk_stow(ctx, src_path, tgt_path, rel);
    }

    if (S_ISLNK(st.st_mode)) {
        char linkbuf[PATH_MAX];
        ssize_t n = readlink(src_path, linkbuf, sizeof(linkbuf) - 1);
        if (n < 0) {
            fprintf(stderr, "%s: cannot read symlink %s: %s\n",
                    PROGRAM_NAME, src_path, strerror(errno));
            return -1;
        }
        linkbuf[n] = '\0';

        char *src_dir = parent_dir(src_path);
        char *candidate;
        if (linkbuf[0] == '/')
            candidate = xstrdup(linkbuf);
        else
            candidate = path_join(src_dir, linkbuf);
        free(src_dir);

        char *resolved = realpath(candidate, NULL);
        if (!resolved) {
            fprintf(stderr, "%s: warning: cannot resolve symlink %s -> %s, skipping\n",
                    PROGRAM_NAME, src_path, linkbuf);
            free(candidate);
            return -1;
        }
        free(candidate);

        struct stat tst;
        if (lstat(tgt_path, &tst) == 0) {
            if (S_ISLNK(tst.st_mode)) {
                char *tgt_resolved = realpath(tgt_path, NULL);
                if (tgt_resolved && strcmp(resolved, tgt_resolved) == 0) {
                    free(tgt_resolved);
                    free(resolved);
                    return 0;
                }
                free(tgt_resolved);
            }
            if (ctx->flags & FLAG_OVERWRITE) {
                unlink(tgt_path);
            } else {
                fprintf(stderr, "%s: warning: %s already exists and points to a different target\n",
                        PROGRAM_NAME, tgt_path);
                free(resolved);
                return -1;
            }
        }

        char *tgt_dir = parent_dir(tgt_path);
        mkdir_p(tgt_dir, 0755);
        free(tgt_dir);

        if (symlink(resolved, tgt_path) < 0) {
            fprintf(stderr, "%s: cannot create symlink %s -> %s: %s\n",
                    PROGRAM_NAME, tgt_path, resolved, strerror(errno));
            free(resolved);
            return -1;
        }
        printf("  symlink %s -> %s\n", tgt_path, resolved);
        free(resolved);
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        struct stat tst;
        bool target_exists = lstat(tgt_path, &tst) == 0;

        if (target_exists) {
            if (S_ISLNK(tst.st_mode)) {
                char *expected = get_symlink_target(src_path, ctx->tgt_base, rel);
                char buf[PATH_MAX];
                ssize_t n = readlink(tgt_path, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    if (strcmp(buf, expected) == 0) {
                        free(expected);
                        return 0;
                    }
                }
                free(expected);
                unlink(tgt_path);
                target_exists = false;
            } else if (S_ISREG(tst.st_mode) && files_equal(src_path, tgt_path)) {
                unlink(tgt_path);
                target_exists = false;
            } else if (ctx->flags & FLAG_ADOPT) {
                if (copy_file(tgt_path, src_path) < 0) {
                    fprintf(stderr, "%s: cannot copy %s to %s\n",
                            PROGRAM_NAME, tgt_path, src_path);
                    return -1;
                }
                unlink(tgt_path);
                target_exists = false;
            } else if (ctx->flags & FLAG_OVERWRITE) {
                if (copy_file(src_path, tgt_path) < 0) {
                    fprintf(stderr, "%s: cannot copy %s to %s\n",
                            PROGRAM_NAME, src_path, tgt_path);
                    return -1;
                }
                unlink(tgt_path);
                target_exists = false;
            } else {
                fprintf(stderr, "%s: warning: %s already exists and differs from %s\n",
                        PROGRAM_NAME, tgt_path, src_path);
                return -1;
            }
        }

        char *tgt_dir = parent_dir(tgt_path);
        mkdir_p(tgt_dir, 0755);
        free(tgt_dir);

        char *target = get_symlink_target(src_path, ctx->tgt_base, rel);
        if (symlink(target, tgt_path) < 0) {
            fprintf(stderr, "%s: cannot create symlink %s -> %s: %s\n",
                    PROGRAM_NAME, tgt_path, target, strerror(errno));
            free(target);
            return -1;
        }
        printf("  symlink %s -> %s\n", tgt_path, target);
        free(target);
        return 0;
    }

    return 0;
}

static int walk_stow(Context *ctx, const char *src_dir,
                     const char *tgt_dir, const char *rel_prefix)
{
    DIR *d = opendir(src_dir);
    if (!d) {
        fprintf(stderr, "%s: cannot open %s: %s\n",
                PROGRAM_NAME, src_dir, strerror(errno));
        return -1;
    }

    int ret = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".stone-ignore") == 0)
            continue;

        char *rel;
        if (rel_prefix[0] == '\0')
            rel = xstrdup(entry->d_name);
        else
            rel = path_join(rel_prefix, entry->d_name);

        struct stat st;
        bool is_dir = false;
        char *src_path = path_join(src_dir, entry->d_name);
        if (lstat(src_path, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = true;

        if (ignore_match(ctx->ignore, rel, is_dir)) {
            free(src_path);
            free(rel);
            continue;
        }

        char *tgt_path = path_join(tgt_dir, entry->d_name);

        if (process_entry_stow(ctx, src_path, tgt_path, rel) < 0)
            ret = -1;

        free(src_path);
        free(tgt_path);
        free(rel);
    }

    closedir(d);
    return ret;
}

static int process_entry_delete(Context *ctx, const char *src_path,
                                const char *tgt_path, const char *rel)
{
    struct stat st;
    if (lstat(src_path, &st) < 0) {
        fprintf(stderr, "%s: cannot stat %s: %s\n",
                PROGRAM_NAME, src_path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        int r = walk_delete(ctx, src_path, tgt_path, rel);
        if (rmdir(tgt_path) == 0)
            printf("  rmdir %s\n", tgt_path);
        return r;
    }

    if (S_ISLNK(st.st_mode)) {
        struct stat tst;
        if (lstat(tgt_path, &tst) < 0) return 0;
        if (!S_ISLNK(tst.st_mode)) return 0;

        char linkbuf[PATH_MAX];
        ssize_t n = readlink(src_path, linkbuf, sizeof(linkbuf) - 1);
        if (n < 0) return 0;
        linkbuf[n] = '\0';

        char *src_dir = parent_dir(src_path);
        char *candidate;
        if (linkbuf[0] == '/')
            candidate = xstrdup(linkbuf);
        else
            candidate = path_join(src_dir, linkbuf);
        free(src_dir);

        char *src_resolved = realpath(candidate, NULL);
        free(candidate);
        if (!src_resolved) return 0;

        char *tgt_resolved = realpath(tgt_path, NULL);
        if (!tgt_resolved) { free(src_resolved); return 0; }

        bool match = strcmp(src_resolved, tgt_resolved) == 0;
        free(src_resolved);
        free(tgt_resolved);

        if (match) {
            if (unlink(tgt_path) < 0) {
                fprintf(stderr, "%s: cannot remove %s: %s\n",
                        PROGRAM_NAME, tgt_path, strerror(errno));
                return -1;
            }
            printf("  remove %s\n", tgt_path);
        }
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        struct stat tst;
        if (lstat(tgt_path, &tst) < 0) return 0;
        if (!S_ISLNK(tst.st_mode)) return 0;

        char *expected = get_symlink_target(src_path, ctx->tgt_base, rel);
        char buf[PATH_MAX];
        ssize_t n = readlink(tgt_path, buf, sizeof(buf) - 1);
        if (n < 0) { free(expected); return 0; }
        buf[n] = '\0';

        if (strcmp(buf, expected) != 0) { free(expected); return 0; }
        free(expected);

        if (unlink(tgt_path) < 0) {
            fprintf(stderr, "%s: cannot remove %s: %s\n",
                    PROGRAM_NAME, tgt_path, strerror(errno));
            return -1;
        }
        printf("  remove %s\n", tgt_path);
        return 0;
    }

    return 0;
}

static int walk_delete(Context *ctx, const char *src_dir,
                       const char *tgt_dir, const char *rel_prefix)
{
    DIR *d = opendir(src_dir);
    if (!d) {
        fprintf(stderr, "%s: cannot open %s: %s\n",
                PROGRAM_NAME, src_dir, strerror(errno));
        return -1;
    }

    int ret = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".stone-ignore") == 0)
            continue;

        char *rel;
        if (rel_prefix[0] == '\0')
            rel = xstrdup(entry->d_name);
        else
            rel = path_join(rel_prefix, entry->d_name);

        struct stat st;
        bool is_dir = false;
        char *src_path = path_join(src_dir, entry->d_name);
        if (lstat(src_path, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = true;

        if (ignore_match(ctx->ignore, rel, is_dir)) {
            free(src_path);
            free(rel);
            continue;
        }

        char *tgt_path = path_join(tgt_dir, entry->d_name);

        if (process_entry_delete(ctx, src_path, tgt_path, rel) < 0)
            ret = -1;

        free(src_path);
        free(tgt_path);
        free(rel);
    }

    closedir(d);
    return ret;
}

static void print_help(void)
{
    printf("Usage: %s [options] <source_dir> [target_dir]\n"
           "\n"
           "Manage symlinks from a stow directory to a target directory.\n"
           "For each file in source_dir, stone creates a symlink in\n"
           "target_dir preserving the relative directory structure.\n"
           "Directories are created as needed.\n"
           "\n"
           "If any file already exists at the target path and differs from the\n"
           "source, stone prints a warning and aborts without creating any\n"
           "symlinks. Use -a or -o to resolve such conflicts.\n"
           "\n"
           "Symlinks inside source_dir are re-created as symlinks in\n"
           "target_dir pointing to the same resolved target.\n"
           "\n"
           "If a file named .stone-ignore exists in source_dir, its patterns\n"
           "are treated like .gitignore entries: # comments, ! negation,\n"
           "* ? ** wildcards, / anchoring, and trailing / for directories.\n"
           "\n"
           "Arguments:\n"
           "  source_dir   Directory containing files to stow\n"
           "  target_dir   Target directory (default: parent of source_dir)\n"
           "\n"
           "Options:\n"
           "  -h, --help        Show this help message\n"
           "  -d, --delete      Remove symlinks installed by a previous stow\n"
           "  -a, --adopt       Adopt existing files by copying them into source_dir\n"
           "  -o, --overwrite   Overwrite existing files with the stow version\n",
           PROGRAM_NAME);
}

int main(int argc, char **argv)
{
    int flags = 0;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0) {
            flags |= FLAG_DELETE;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--adopt") == 0) {
            flags |= FLAG_ADOPT;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--overwrite") == 0) {
            flags |= FLAG_OVERWRITE;
        } else {
            fprintf(stderr, "%s: unknown option: %s\n", PROGRAM_NAME, argv[i]);
            fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
            return 1;
        }
        i++;
    }

    if (i >= argc) {
        fprintf(stderr, "%s: missing source directory\n", PROGRAM_NAME);
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
        return 1;
    }

    char *src_raw = argv[i];
    char *src_resolved = realpath(src_raw, NULL);
    if (!src_resolved) {
        fprintf(stderr, "%s: cannot resolve source directory '%s': %s\n",
                PROGRAM_NAME, src_raw, strerror(errno));
        return 1;
    }

    struct stat src_stat;
    if (stat(src_resolved, &src_stat) < 0 || !S_ISDIR(src_stat.st_mode)) {
        fprintf(stderr, "%s: '%s' is not a directory\n", PROGRAM_NAME, src_resolved);
        free(src_resolved);
        return 1;
    }

    char *tgt_base;
    if (i + 1 < argc) {
        tgt_base = realpath(argv[i + 1], NULL);
        if (!tgt_base) {
            if (mkdir_p(argv[i + 1], 0755) < 0) {
                fprintf(stderr, "%s: cannot create target directory '%s': %s\n",
                        PROGRAM_NAME, argv[i + 1], strerror(errno));
                free(src_resolved);
                return 1;
            }
            tgt_base = realpath(argv[i + 1], NULL);
            if (!tgt_base) {
                fprintf(stderr, "%s: cannot resolve target directory '%s': %s\n",
                        PROGRAM_NAME, argv[i + 1], strerror(errno));
                free(src_resolved);
                return 1;
            }
        }
    } else {
        tgt_base = parent_dir(src_resolved);
    }

    if (strcmp(src_resolved, tgt_base) == 0) {
        fprintf(stderr, "%s: source and target directories must differ\n", PROGRAM_NAME);
        free(src_resolved);
        free(tgt_base);
        return 1;
    }

    Context ctx;
    ctx.src_base = src_resolved;
    ctx.tgt_base = tgt_base;
    ctx.flags = flags;
    ctx.ignore = ignore_load(src_resolved);

    int ret;
    if (flags & FLAG_DELETE) {
        ret = walk_delete(&ctx, ctx.src_base, ctx.tgt_base, "");
    } else {
        if (walk_check(&ctx, ctx.src_base, ctx.tgt_base, "") < 0) {
            fprintf(stderr, "%s: conflicts found, aborting\n", PROGRAM_NAME);
            ret = 1;
        } else {
            ret = walk_stow(&ctx, ctx.src_base, ctx.tgt_base, "");
        }
    }

    ignore_free(ctx.ignore);
    free(src_resolved);
    free(tgt_base);
    return ret == 0 ? 0 : 1;
}
