/* Brokie - MIT licensed. One C11 file; see README.md for build and protocol scope.
 * Based on FreeLLMAPI by Tashfeen Ahmed and inspired by UltimateWrap by Osama Hasoneh.
 * https://github.com/tashfeenahmed/freellmapi
 * https://github.com/Osamious/ultimatewrap
 * Upstream MIT notices and code provenance: THIRD_PARTY_NOTICES.md and README.md.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)
#define close_socket close
#define BAD_SOCKET (-1)
#define SEP "/"

#define MAX_BODY (16u * 1024u * 1024u)
#define MAX_MODELS 2048
#define MAX_KEYS 256
#define MAX_TOOLS 64
typedef struct {
    char *p;
    size_t n, cap;
} Buf;
static void die(const char *s) {
    fprintf(stderr, "brokie: %s\n", s);
    exit(1);
}
static void *mem(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}
static void reserve(Buf *b, size_t more) {
    size_t need = b->n + more + 1;
    if (need > MAX_BODY + 1) die("buffer limit exceeded");
    if (need > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < need) c *= 2;
        char *p = realloc(b->p, c);
        if (!p) die("out of memory");
        b->p = p;
        b->cap = c;
    }
}
static void putn(Buf *b, const char *s, size_t n) {
    reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void put(Buf *b, const char *s) { putn(b, s, strlen(s)); }
static void fmt(Buf *b, const char *f, ...) {
    va_list a, a2;
    va_start(a, f);
    va_copy(a2, a);
    int n = vsnprintf(NULL, 0, f, a);
    va_end(a);
    if (n < 0) die("format error");
    reserve(b, (size_t)n);
    vsnprintf(b->p + b->n, b->cap - b->n, f, a2);
    va_end(a2);
    b->n += n;
}
static void drop(Buf *b) {
    free(b->p);
    memset(b, 0, sizeof(*b));
}
static void quote(Buf *b, const char *s) {
    put(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            put(b, "\\");
            putn(b, (const char *)p, 1);
        } else if (*p < 32) fmt(b, "\\u%04x", *p);
        else putn(b, (const char *)p, 1);
    }
    put(b, "\"");
}

/* A small validating JSON tokenizer. Tokens retain source spans for lossless passthrough. */
typedef struct {
    int a, b, end, n;
    char type;
} Tok;
typedef struct {
    const char *s;
    size_t len, pos;
    Tok *t;
    int n, cap, bad;
} Json;
static void ws(Json *j) {
    while (j->pos < j->len && strchr(" \t\r\n", j->s[j->pos])) j->pos++;
}
static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)s[i];
        if (!isxdigit(c)) return -1;
        v = v * 16 + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return v;
}
static int parse_value(Json *j, int depth) {
    ws(j);
    if (j->bad || depth > 96 || j->pos >= j->len) {
        j->bad = 1;
        return -1;
    }
    if (j->n == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 256;
        Tok *p = realloc(j->t, (size_t)j->cap * sizeof(Tok));
        if (!p) die("out of memory");
        j->t = p;
    }
    int k = j->n++;
    memset(&j->t[k], 0, sizeof(Tok));
    j->t[k].a = (int)j->pos;
    char c = j->s[j->pos++];
    j->t[k].type = c;
    if (c == '{' || c == '[') {
        char end = c == '{' ? '}' : ']';
        ws(j);
        if (j->pos < j->len && j->s[j->pos] == end) j->pos++;
        else
            for (;;) {
                if (c == '{') {
                    ws(j);
                    if (j->pos >= j->len || j->s[j->pos] != '"') {
                        j->bad = 1;
                        break;
                    }
                    parse_value(j, depth + 1);
                    ws(j);
                    if (j->pos >= j->len || j->s[j->pos++] != ':') {
                        j->bad = 1;
                        break;
                    }
                }
                parse_value(j, depth + 1);
                j->t[k].n++;
                ws(j);
                if (j->bad || j->pos >= j->len) {
                    j->bad = 1;
                    break;
                }
                char x = j->s[j->pos++];
                if (x == end) break;
                if (x != ',') {
                    j->bad = 1;
                    break;
                }
            }
    } else if (c == '"') {
        int closed = 0;
        while (j->pos < j->len) {
            unsigned char x = j->s[j->pos++];
            if (x == '"') {
                closed = 1;
                break;
            }
            if (x < 32) {
                j->bad = 1;
                break;
            }
            if (x == '\\') {
                if (j->pos >= j->len) {
                    j->bad = 1;
                    break;
                }
                x = j->s[j->pos++];
                if (x == 'u') {
                    if (j->pos + 4 > j->len || hex4(j->s + j->pos) < 0) {
                        j->bad = 1;
                        break;
                    }
                    j->pos += 4;
                } else if (!strchr("\"\\/bfnrt", x)) {
                    j->bad = 1;
                    break;
                }
            }
        }
        if (!closed) j->bad = 1;
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *p = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t n = strlen(p);
        if (j->t[k].a + (int)n > (int)j->len || memcmp(j->s + j->t[k].a, p, n)) j->bad = 1;
        else j->pos = j->t[k].a + n;
    } else {
        j->t[k].type = '0';
        j->pos--;
        size_t a = j->pos;
        if (j->s[j->pos] == '-') j->pos++;
        if (j->pos >= j->len) j->bad = 1;
        else if (j->s[j->pos] == '0') j->pos++;
        else if (j->s[j->pos] >= '1' && j->s[j->pos] <= '9') {
            while (j->pos < j->len && isdigit((unsigned char)j->s[j->pos])) j->pos++;
        } else j->bad = 1;
        if (j->pos < j->len && j->s[j->pos] == '.') {
            j->pos++;
            size_t p = j->pos;
            while (j->pos < j->len && isdigit((unsigned char)j->s[j->pos])) j->pos++;
            if (p == j->pos) j->bad = 1;
        }
        if (j->pos < j->len && strchr("eE", j->s[j->pos])) {
            j->pos++;
            if (j->pos < j->len && strchr("+-", j->s[j->pos])) j->pos++;
            size_t p = j->pos;
            while (j->pos < j->len && isdigit((unsigned char)j->s[j->pos])) j->pos++;
            if (p == j->pos) j->bad = 1;
        }
        if (a == j->pos) j->bad = 1;
    }
    j->t[k].b = (int)j->pos;
    j->t[k].end = j->n;
    return k;
}
static Json json(const char *s, size_t n) {
    Json j = {0};
    j.s = s;
    j.len = n;
    parse_value(&j, 0);
    ws(&j);
    if (j.pos != n) j.bad = 1;
    return j;
}
static void jfree(Json *j) {
    free(j->t);
    j->t = NULL;
}
static char *string(Json *j, int k);
static int eq(Json *j, int k, const char *s) {
    if (k >= 0 && j->t[k].type == '"' && memchr(j->s + j->t[k].a + 1, '\\', (size_t)(j->t[k].b - j->t[k].a - 2))) {
        char *decoded = string(j, k);
        int equal = !strcmp(decoded, s);
        free(decoded);
        return equal;
    }
    return k >= 0 && j->t[k].type == '"' && (size_t)(j->t[k].b - j->t[k].a - 2) == strlen(s) &&
           !memcmp(j->s + j->t[k].a + 1, s, strlen(s));
}
static int get(Json *j, int k, const char *name) {
    if (k < 0 || j->t[k].type != '{') return -1;
    for (int i = k + 1; i < j->t[k].end;) {
        int v = i + 1;
        if (eq(j, i, name)) return v;
        i = j->t[v].end;
    }
    return -1;
}
static int at(Json *j, int k, int index) {
    if (k < 0 || j->t[k].type != '[') return -1;
    int i = k + 1;
    while (index-- && i < j->t[k].end) i = j->t[i].end;
    return i < j->t[k].end ? i : -1;
}
static void raw(Buf *b, Json *j, int k, const char *fallback) {
    if (k < 0) put(b, fallback);
    else putn(b, j->s + j->t[k].a, j->t[k].b - j->t[k].a);
}
/* Copy object members in source order, preserving their original JSON spans. */
static int copy_members(Buf *b, Json *j, int object, const char *skip, const char *fallback, int count) {
    for (int i = object + 1; object >= 0 && i < j->t[object].end;) {
        int value = i + 1;
        if (!eq(j, i, skip)) {
            if (count++) put(b, ",");
            raw(b, j, i, "\"\"");
            put(b, ":");
            raw(b, j, value, fallback);
        }
        i = j->t[value].end;
    }
    return count;
}
static double number(Json *j, int k, double fallback) {
    return k >= 0 && j->t[k].type == '0' ? strtod(j->s + j->t[k].a, NULL) : fallback;
}
static double fieldnum(Json *j, int object, const char *name, double fallback) {
    return number(j, get(j, object, name), fallback);
}
static int boolean(Json *j, int k, int fallback) {
    return k < 0 ? fallback : j->t[k].type == 't' ? 1 : j->t[k].type == 'f' ? 0 : (int)number(j, k, fallback);
}
static void utf8(Buf *b, unsigned u) {
    char x[4];
    int n;
    if (u < 128) {
        x[0] = (char)u;
        n = 1;
    } else if (u < 2048) {
        x[0] = (char)(192 | (u >> 6));
        x[1] = (char)(128 | (u & 63));
        n = 2;
    } else if (u < 65536) {
        x[0] = (char)(224 | (u >> 12));
        x[1] = (char)(128 | ((u >> 6) & 63));
        x[2] = (char)(128 | (u & 63));
        n = 3;
    } else {
        x[0] = (char)(240 | (u >> 18));
        x[1] = (char)(128 | ((u >> 12) & 63));
        x[2] = (char)(128 | ((u >> 6) & 63));
        x[3] = (char)(128 | (u & 63));
        n = 4;
    }
    putn(b, x, n);
}
static char *string(Json *j, int k) {
    Buf b = {0};
    if (k < 0 || j->t[k].type != '"') {
        put(&b, "");
        return b.p;
    }
    for (int i = j->t[k].a + 1; i < j->t[k].b - 1; i++) {
        char c = j->s[i];
        if (c != '\\') {
            putn(&b, &c, 1);
            continue;
        }
        c = j->s[++i];
        if (c == 'u') {
            unsigned u = (unsigned)hex4(j->s + i + 1);
            i += 4;
            if (u >= 0xd800 && u <= 0xdbff && i + 6 < j->t[k].b && j->s[i + 1] == '\\' && j->s[i + 2] == 'u') {
                int lo = hex4(j->s + i + 3);
                if (lo >= 0xdc00 && lo <= 0xdfff) {
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                }
            }
            if (!u || (u >= 0xd800 && u <= 0xdfff)) u = 0xfffd;
            utf8(&b, u);
        } else {
            const char *names = "bfnrt", *chars = "\b\f\n\r\t", *p = strchr(names, c);
            char x = p ? chars[p - names] : c;
            putn(&b, &x, 1);
        }
    }
    if (!b.p) put(&b, "");
    return b.p;
}
static void fieldstr(Json *j, int k, const char *name, char *out, size_t cap) {
    char *p = string(j, get(j, k, name));
    snprintf(out, cap, "%s", p);
    free(p);
}

static char directory[2048];
static void path_for(char *p, size_t n, const char *name) { snprintf(p, n, "%s%s%s", directory, SEP, name); }
static void secure_zero(void *p, size_t n) {
    volatile unsigned char *s = p;
    while (n--) *s++ = 0;
}
static int private_directory(void) {
    if (mkdir(directory, 0700) && errno != EEXIST) return 0;
    struct stat st;
    if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return 0;
    return !chmod(directory, 0700);
}
static Buf read_file(const char *name) {
    char p[4096];
    path_for(p, sizeof(p), name);
    Buf b = {0};
    FILE *f = fopen(p, "rb");
    if (!f) return b;
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f))) {
        if (b.n + n > MAX_BODY) {
            drop(&b);
            break;
        }
        putn(&b, tmp, n);
    }
    fclose(f);
    return b;
}
static int write_file(const char *name, const void *data, size_t n) {
    char p[4096], tmp[4100];
    path_for(p, sizeof(p), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    if (fclose(f)) ok = 0;
    if (!ok) return 0;
    return !rename(tmp, p);
}
static void random_bytes(void *p, int n) {
    if (RAND_bytes(p, n) != 1) die("random generator failed");
}
static int hidden_input(const char *prompt, char *out, size_t cap) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    size_t n = 0;
    struct termios old, t;
    int tty = !tcgetattr(STDIN_FILENO, &old);
    if (tty) {
        t = old;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    }
    if (!fgets(out, (int)cap, stdin)) {
        if (tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        return 0;
    }
    if (tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    n = strcspn(out, "\r\n");
    out[n] = 0;
    fputc('\n', stderr);
    return n > 0;
}
static char password[512];
static void unlock_password(int create) {
    if (password[0]) return;
    const char *env = getenv("BROKIE_PASSWORD");
    if (env && *env) {
        snprintf(password, sizeof(password), "%s", env);
        unsetenv("BROKIE_PASSWORD");
        if (strlen(password) < 12) die("BROKIE_PASSWORD must have at least 12 characters");
        return;
    }
    if (!hidden_input(create ? "New vault password (12+ characters): " : "Vault password: ", password,
                      sizeof(password)))
        die("password required");
    if (create) {
        char again[512];
        if (strlen(password) < 12 || !hidden_input("Confirm password: ", again, sizeof(again)) ||
            strcmp(password, again))
            die("passwords must match and have at least 12 characters");
        secure_zero(again, sizeof(again));
    }
}
static Buf vault_read(void) {
    Buf file = read_file("vault.bin"), plain = {0};
    if (file.n < 4) return file;
    if (file.n < 48 || memcmp(file.p, "MRP1", 4)) die("invalid vault format; add keys locally on this platform");
    unlock_password(0);
    unsigned char key[32];
    if (!PKCS5_PBKDF2_HMAC(password, (int)strlen(password), (unsigned char *)file.p + 4, 16, 600000, EVP_sha256(), 32,
                           key))
        die("key derivation failed");
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int a = 0, b = 0;
    reserve(&plain, file.n);
    int ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) &&
             EVP_DecryptInit_ex(c, NULL, NULL, key, (unsigned char *)file.p + 20) &&
             EVP_DecryptUpdate(c, (unsigned char *)plain.p, &a, (unsigned char *)file.p + 48, (int)file.n - 48) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, file.p + 32) &&
             EVP_DecryptFinal_ex(c, (unsigned char *)plain.p + a, &b);
    EVP_CIPHER_CTX_free(c);
    secure_zero(key, sizeof(key));
    if (!ok) die("incorrect password or damaged vault");
    plain.n = a + b;
    plain.p[plain.n] = 0;
    drop(&file);
    return plain;
}
static int vault_write(Buf *plain, int create) {
    Buf file = {0};
    unlock_password(create);
    unsigned char salt[16], iv[12], key[32], tag[16];
    random_bytes(salt, 16);
    random_bytes(iv, 12);
    if (!PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, 16, 600000, EVP_sha256(), 32, key)) return 0;
    put(&file, "MRP1");
    putn(&file, (char *)salt, 16);
    putn(&file, (char *)iv, 12);
    memset(tag, 0, 16);
    putn(&file, (char *)tag, 16);
    reserve(&file, plain->n + 16);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int a = 0, b = 0;
    int ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) && EVP_EncryptInit_ex(c, NULL, NULL, key, iv) &&
             EVP_EncryptUpdate(c, (unsigned char *)file.p + 48, &a, (unsigned char *)plain->p, (int)plain->n) &&
             EVP_EncryptFinal_ex(c, (unsigned char *)file.p + 48 + a, &b) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(c);
    secure_zero(key, sizeof(key));
    if (!ok) {
        drop(&file);
        return 0;
    }
    memcpy(file.p + 32, tag, 16);
    file.n = 48 + a + b;
    int okwrite = write_file("vault.bin", file.p, file.n);
    drop(&file);
    return okwrite;
}

typedef struct {
    int id, enabled, anonymous, native, health, http;
    double checked;
    char provider[96], label[160], url[2048], secret[4096], health_path[256];
} Key;
typedef struct {
    int id, enabled, tools, vision, context, rank, tier, key_id, priority;
    double budget;
    int rpm, rpd, tpm, tpd;
    char provider[96], name[320];
} Model;
typedef struct {
    double s, f, out, ms, ttfb, ttn, updated, month_tokens;
    long long minute, day, month;
    double min_tokens, day_tokens;
    int min_req, day_req, inflight;
    double cooldown;
} Stats;
static Key keys[MAX_KEYS];
static Model models[MAX_MODELS];
static Stats *stats;
static int nk, nm, port = 31817, max_attempts = 6, timeout_ms = 60000, sticky_seconds = 1800;
static int refresh_seconds = 3600, health_seconds = 900;
static char default_mode[32] = "smartest", auth_token[4096];
static char catalog_license[4096];
enum { H_UNKNOWN, H_OK, H_INVALID, H_REJECTED, H_LIMITED, H_DOWN, H_PUBLIC, H_ANONYMOUS, H_SKIP };
static const char *health_names[] = {"unknown",     "healthy",    "invalid",   "rejected",    "limited",
                                     "unreachable", "unverified", "anonymous", "request-only"};
static Buf config;
static int offline(void) { return getenv("BROKIE_OFFLINE") && !strcmp(getenv("BROKIE_OFFLINE"), "1"); }
static double cache_age(const char *name) {
    char path[4096];
    struct stat info;
    path_for(path, sizeof(path), name);
    return stat(path, &info) ? 1e30 : fmax(0, (double)time(NULL) - info.st_mtime);
}
static void save_health(void) {
    Buf b = {0};
    put(&b, "[");
    for (int k = 0; k < nk; k++)
        fmt(&b, "%s{\"id\":%d,\"health\":%d,\"http\":%d,\"checked\":%.0f}", k ? "," : "", keys[k].id, keys[k].health,
            keys[k].http, keys[k].checked);
    put(&b, "]\n");
    if (!write_file("health.json", b.p, b.n)) fprintf(stderr, "brokie: could not save key health\n");
    drop(&b);
}
static void load_health(void) {
    Buf b = read_file("health.json");
    if (!b.p) return;
    Json j = json(b.p, b.n);
    for (int r = 0; !j.bad && j.n && j.t[0].type == '[' && r < j.t[0].n; r++) {
        int row = at(&j, 0, r);
        double id = fieldnum(&j, row, "id", -1), health = fieldnum(&j, row, "health", 0),
               http = fieldnum(&j, row, "http", 0), checked = fieldnum(&j, row, "checked", 0);
        if (!isfinite(id) || !isfinite(health) || !isfinite(http) || !isfinite(checked) || health < 0 ||
            health > H_SKIP || http < 0 || http > 599 || checked < 0)
            continue;
        for (int k = 0; k < nk; k++)
            if (keys[k].id == id) {
                keys[k].health = (int)health;
                keys[k].http = (int)http;
                keys[k].checked = checked;
            }
    }
    jfree(&j);
    drop(&b);
}
static double now_s(void) { return (double)time(NULL); }
static int response_health(int ok, long http) {
    return !ok || http >= 500           ? H_DOWN
           : http == 401                ? H_INVALID
           : http == 403                ? H_REJECTED
           : http == 402 || http == 429 ? H_LIMITED
                                        : H_PUBLIC;
}
/* Caller holds the state mutex. */
static void set_health(Key *key, int health, long http) {
    key->health = health;
    key->http = (int)http;
    key->checked = now_s();
}
static int key_url(char *url, size_t cap, const Key *key, const char *path, int root_path) {
    const char *end = key->native ? NULL : strstr(key->url, "/chat/completions");
    if (root_path) {
        const char *scheme = strstr(key->url, "://");
        end = scheme ? strchr(scheme + 3, '/') : NULL;
        path++;
    }
    size_t base = end ? (size_t)(end - key->url) : strlen(key->url);
    return snprintf(url, cap, "%.*s%s", (int)base, key->url, path) < (int)cap;
}
static long long month_id(void) {
    time_t t = time(NULL);
    struct tm m;
    gmtime_r(&t, &m);
    return (m.tm_year + 1900) * 12 + m.tm_mon;
}
static int key_index(int id) {
    for (int i = 0; i < nk; i++)
        if (keys[i].id == id) return i;
    return -1;
}
static int model_index(int id) {
    for (int i = 0; i < nm; i++)
        if (models[i].id == id) return i;
    return -1;
}
static Stats *st(int m, int k) { return &stats[(size_t)m * nk + k]; }
static void decay(Stats *s, double now) {
    double d = s->updated > 0 ? pow(.5, fmax(0, now - s->updated) / 172800.) : 1;
    s->s *= d;
    s->f *= d;
    s->out *= d;
    s->ms *= d;
    s->ttfb *= d;
    s->ttn *= d;
    s->updated = now;
    long long minute = (long long)now / 60, day = (long long)now / 86400, month = month_id();
    if (s->minute != minute) {
        s->minute = minute;
        s->min_req = 0;
        s->min_tokens = 0;
    }
    if (s->day != day) {
        s->day = day;
        s->day_req = 0;
        s->day_tokens = 0;
    }
    if (s->month != month) {
        s->month = month;
        s->month_tokens = 0;
    }
}
#define PERSISTED_STATS(X)                                                                                             \
    X(s)                                                                                                               \
    X(f)                                                                                                               \
    X(out)                                                                                                             \
    X(ms)                                                                                                              \
    X(ttfb)                                                                                                            \
    X(ttn)                                                                                                             \
    X(updated) X(month_tokens) X(minute) X(day) X(month) X(min_tokens) X(day_tokens) X(min_req) X(day_req) X(cooldown)
static void load_state(void) {
    Buf b = read_file("stats.json");
    if (!b.p) return;
    Json j = json(b.p, b.n);
    if (!j.bad && j.t[0].type == '[')
        for (int a = 0; a < j.t[0].n; a++) {
            int r = at(&j, 0, a), m = model_index((int)fieldnum(&j, r, "model", -1)),
                k = key_index((int)fieldnum(&j, r, "key", -1));
            if (m < 0 || k < 0) continue;
            Stats *s = st(m, k);
#define READ_STAT(x) s->x = fieldnum(&j, r, #x, 0);
            PERSISTED_STATS(READ_STAT)
#undef READ_STAT
        }
    jfree(&j);
    drop(&b);
}
static void save_state(void) {
    Buf b = {0};
    put(&b, "[");
    int count = 0;
    for (int m = 0; m < nm; m++)
        for (int k = 0; k < nk; k++) {
            Stats *s = st(m, k);
            if (!s->updated && !s->cooldown) continue;
            if (count++) put(&b, ",");
            fmt(&b, "{\"model\":%d,\"key\":%d", models[m].id, keys[k].id);
#define WRITE_STAT(x) fmt(&b, ",\"" #x "\":%.9g", (double)s->x);
            PERSISTED_STATS(WRITE_STAT)
#undef WRITE_STAT
            put(&b, "}");
        }
    put(&b, "]\n");
    if (!write_file("stats.json", b.p, b.n)) fprintf(stderr, "brokie: could not save statistics\n");
    drop(&b);
}
static int parse_models(Json *j, Model *out, int *count) {
    int n = 0;
    if (j->bad || !j->n || j->t[0].type != '{') return 0;
    int profile = get(j, 0, "profile");
    if (profile >= 0 && !eq(j, profile, "")) return 0;
    int a = get(j, 0, "models");
    if (a >= 0 && j->t[a].type != '[') return 0;
    if (a >= 0 && j->t[a].n > MAX_MODELS) return 0;
    for (int i = 0; a >= 0 && i < j->t[a].n; i++) {
        int r = at(j, a, i);
        if (j->t[r].type != '{') return 0;
        const char *ints[] = {"id",   "enabled", "tools", "vision", "context", "rank",
                              "tier", "key_id",  "rpm",   "rpd",    "tpm",     "tpd"};
        for (size_t q = 0; q < sizeof(ints) / sizeof(*ints); q++) {
            double value = fieldnum(j, r, ints[q], 0);
            if (!isfinite(value) || value < -2147483647. || value > 2147483647.) return 0;
        }
        Model *m = &out[n++];
        m->id = (int)fieldnum(j, r, "id", n);
        m->enabled = boolean(j, get(j, r, "enabled"), 1);
        m->tools = boolean(j, get(j, r, "tools"), 0);
        m->vision = boolean(j, get(j, r, "vision"), 0);
        m->context = (int)fieldnum(j, r, "context", 0);
        m->rank = (int)fieldnum(j, r, "rank", 50);
        m->tier = (int)fieldnum(j, r, "tier", 2);
        m->key_id = (int)fieldnum(j, r, "key_id", 0);
        m->budget = fieldnum(j, r, "budget", 0);
        m->priority = i;
        m->rpm = (int)fieldnum(j, r, "rpm", 0);
        m->rpd = (int)fieldnum(j, r, "rpd", 0);
        m->tpm = (int)fieldnum(j, r, "tpm", 0);
        m->tpd = (int)fieldnum(j, r, "tpd", 0);
        fieldstr(j, r, "provider", m->provider, sizeof(m->provider));
        fieldstr(j, r, "model", m->name, sizeof(m->name));
        if (m->id < 1 || !m->provider[0] || !m->name[0] || !isfinite(m->budget)) return 0;
        for (int q = 0; q < n - 1; q++)
            if (out[q].id == m->id) return 0;
    }
    *count = n;
    return 1;
}
static void load_config(int need_vault) {
    config = read_file("config.json");
    if (!config.p) die("run 'brokie init' first");
    Json j = json(config.p, config.n);
    if (j.bad || j.t[0].type != '{') die("invalid config.json");
    int v = get(&j, 0, "mode");
    if (v >= 0) fieldstr(&j, 0, "mode", default_mode, sizeof(default_mode));
    port = (int)fieldnum(&j, 0, "port", 31817);
    timeout_ms = (int)fieldnum(&j, 0, "timeout_ms", 60000);
    max_attempts = (int)fieldnum(&j, 0, "max_attempts", 6);
    sticky_seconds = (int)fieldnum(&j, 0, "sticky_seconds", 1800);
    refresh_seconds = (int)fieldnum(&j, 0, "refresh_seconds", 3600);
    health_seconds = (int)fieldnum(&j, 0, "health_seconds", 900);
    int a = get(&j, 0, "keys");
    if (a >= 0 && j.t[a].n > MAX_KEYS) die("too many keys");
    for (int i = 0; a >= 0 && i < j.t[a].n; i++) {
        int r = at(&j, a, i);
        Key *k = &keys[nk++];
        k->id = (int)fieldnum(&j, r, "id", nk);
        k->enabled = boolean(&j, get(&j, r, "enabled"), 1);
        k->anonymous = boolean(&j, get(&j, r, "anonymous"), 0);
        k->native = boolean(&j, get(&j, r, "native"), 0);
        fieldstr(&j, r, "provider", k->provider, sizeof(k->provider));
        fieldstr(&j, r, "label", k->label, sizeof(k->label));
        fieldstr(&j, r, "url", k->url, sizeof(k->url));
        fieldstr(&j, r, "health_path", k->health_path, sizeof(k->health_path));
    }
    if (!parse_models(&j, models, &nm)) die("invalid models; named profiles were removed, use model enabled flags");
    jfree(&j);
    load_health();
    if (need_vault) {
        Buf vlt = vault_read();
        if (!vlt.p) die("vault.bin is missing");
        Json vj = json(vlt.p, vlt.n);
        if (vj.bad) die("vault is invalid");
        fieldstr(&vj, 0, "token", auth_token, sizeof(auth_token));
        fieldstr(&vj, 0, "catalog_license", catalog_license, sizeof(catalog_license));
        int ka = get(&vj, 0, "keys");
        for (int i = 0; i < nk; i++) {
            char id[32];
            snprintf(id, sizeof(id), "%d", keys[i].id);
            char *secret = string(&vj, get(&vj, ka, id));
            snprintf(keys[i].secret, sizeof(keys[i].secret), "%s", secret);
            secure_zero(secret, strlen(secret));
            free(secret);
        }
        jfree(&vj);
        secure_zero(vlt.p, vlt.n);
        drop(&vlt);
        if (!auth_token[0]) die("vault has no gateway token");
    }
    if (port < 1 || port > 65535 || timeout_ms < 1000 || timeout_ms > 600000 || max_attempts < 1 || max_attempts > 32)
        die("invalid network settings");
    if (refresh_seconds < 0 || health_seconds < 0) die("refresh intervals must be zero or positive");
    stats = mem((size_t)(nk ? nk : 1) * (nm ? nm : 1) * sizeof(Stats));
    load_state();
}

static double speed(Stats *s) {
    if (s->ms <= 0 && s->ttn <= 0) return .6;
    double t = s->ms > 0 ? s->out * 1000 / s->ms : 0, tp = 1 - exp(-t / 60), lat = s->ttn > 0 ? s->ttfb / s->ttn : 0;
    double l = fmin(1, fmax(0, 1 - (lat - 300) / 4700));
    return s->ttn > 0 ? (t > 0 ? .6 * tp + .4 * l : l) : tp;
}
static double randu(void) {
    uint32_t n;
    random_bytes(&n, sizeof(n));
    return ((double)n + 1) / 4294967297.;
}
static double gamma_sample(double a) {
    if (a < 1) return gamma_sample(a + 1) * pow(randu(), 1 / a);
    double d = a - 1. / 3, c = 1 / sqrt(9 * d);
    for (;;) {
        double x = sqrt(-2 * log(randu())) * cos(6.283185307179586 * randu()), v = 1 + c * x;
        if (v <= 0) continue;
        v = v * v * v;
        double u = randu();
        if (u < 1 - .0331 * x * x * x * x || log(u) < .5 * x * x + d * (1 - v + log(v))) return d * v;
    }
}
static double reliability(Stats *s, int sample) {
    if (!sample) return (s->s + 1) / (s->s + s->f + 2);
    double a = gamma_sample(s->s + 1), b = gamma_sample(s->f + 1);
    return a / (a + b);
}
static double weights(const char *mode, int axis) {
    static const double w[4][3] = {{.35, .10, .55}, {.35, .55, .10}, {.70, .15, .15}, {.50, .25, .25}};
    int i = !strcmp(mode, "smartest") ? 0 : !strcmp(mode, "fastest") ? 1 : !strcmp(mode, "reliable") ? 2 : 3;
    return w[i][axis];
}
static int valid_mode(const char *m) {
    return !strcmp(m, "smartest") || !strcmp(m, "fastest") || !strcmp(m, "reliable") || !strcmp(m, "balanced");
}
static double intel(Model *m) { return m->tier * 1000 - 31 * sqrt(fmax(1, m->rank)); }
static double headroom(double remaining) { return remaining >= .2 ? 1 : .1 + .9 * fmax(0, remaining) / .2; }
typedef struct {
    int m, k;
    double score;
} Route;
typedef struct {
    uint64_t hash;
    int model;
    double until;
} Sticky;
static Sticky sticky[128];
static int choose(const char *mode, const char *pin, int tools, int vision, int estimate, unsigned char *tried,
                  int sampled, uint64_t hash, Route *route) {
    double now = now_s(), lo = 1e30, hi = -1e30;
    int preferred = -1;
    for (int m = 0; m < nm; m++)
        if (models[m].enabled) {
            double v = intel(&models[m]);
            lo = fmin(lo, v);
            hi = fmax(hi, v);
        }
    if (hash && sticky_seconds > 0) {
        Sticky *p = &sticky[hash % 128];
        if (p->hash == hash && p->until > now) preferred = p->model;
    }
    double best = -1;
    int found = 0;
    for (int m = 0; m < nm; m++) {
        Model *mod = &models[m];
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", mod->provider, mod->name);
        if (!mod->enabled || (pin && strcmp(pin, mod->name) && strcmp(pin, full)) || (tools && !mod->tools) ||
            (vision && !mod->vision) || (mod->context > 0 && estimate > mod->context) ||
            (mod->tpm > 0 && estimate > mod->tpm))
            continue;
        Stats agg = {0};
        double monthly_used = 0;
        int usable_keys = 0;
        for (int k = 0; k < nk; k++)
            if (!strcmp(keys[k].provider, mod->provider) && (!mod->key_id || mod->key_id == keys[k].id) &&
                keys[k].enabled && !keys[k].native && keys[k].health != H_INVALID) {
                Stats *s = st(m, k);
                decay(s, now);
                agg.s += s->s;
                agg.f += s->f;
                agg.out += s->out;
                agg.ms += s->ms;
                agg.ttfb += s->ttfb;
                agg.ttn += s->ttn;
                monthly_used += s->month_tokens;
                usable_keys++;
            }
        double iq = hi > lo ? (intel(mod) - lo) / (hi - lo) : 1;
        double base =
            weights(mode, 0) * reliability(&agg, sampled) + weights(mode, 1) * speed(&agg) + weights(mode, 2) * iq;
        for (int k = 0; k < nk; k++) {
            Key *key = &keys[k];
            Stats *s = st(m, k);
            if (!key->enabled || key->native || key->health == H_INVALID || strcmp(key->provider, mod->provider) ||
                (mod->key_id && mod->key_id != key->id) || (!key->anonymous && !key->secret[0]) ||
                (tried && tried[(size_t)m * nk + k]) || s->cooldown > now || s->inflight >= 2)
                continue;
            if ((mod->rpm && s->min_req >= mod->rpm) || (mod->rpd && s->day_req >= mod->rpd) ||
                (mod->tpm && s->min_tokens + estimate > mod->tpm) || (mod->tpd && s->day_tokens + estimate > mod->tpd))
                continue;
            double remain = 1;
            if (mod->rpm) remain = fmin(remain, 1 - (double)s->min_req / mod->rpm);
            if (mod->rpd) remain = fmin(remain, 1 - (double)s->day_req / mod->rpd);
            if (mod->tpm) remain = fmin(remain, 1 - s->min_tokens / mod->tpm);
            if (mod->tpd) remain = fmin(remain, 1 - s->day_tokens / mod->tpd);
            if (mod->budget > 0) remain = fmin(remain, 1 - monthly_used / (mod->budget * fmax(1, usable_keys)));
            double sc = base * headroom(remain);
            if (m == preferred && !pin) sc += 2;
            double ks = .75 * reliability(s, sampled) + .25 * speed(s);
            sc += ks * .0001 - (double)mod->priority * .000000001;
            if (sc > best) {
                best = sc;
                route->m = m;
                route->k = k;
                route->score = sc;
                found = 1;
            }
        }
    }
    return found;
}

/* Anthropic Messages <-> OpenAI Chat Completions; text, images and client tools. */
static int content_parts(Buf *out, Json *j, int arr) {
    put(out, "[");
    int count = 0;
    for (int i = 0; arr >= 0 && i < j->t[arr].n; i++) {
        int b = at(j, arr, i), type = get(j, b, "type");
        if (eq(j, type, "text")) {
            if (count++) put(out, ",");
            put(out, "{\"type\":\"text\",\"text\":");
            raw(out, j, get(j, b, "text"), "\"\"");
            put(out, "}");
        } else if (eq(j, type, "image")) {
            int src = get(j, b, "source");
            if (count++) put(out, ",");
            put(out, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
            if (eq(j, get(j, src, "type"), "url")) raw(out, j, get(j, src, "url"), "\"\"");
            else if (eq(j, get(j, src, "type"), "base64")) {
                char *mime = string(j, get(j, src, "media_type")), *data = string(j, get(j, src, "data"));
                Buf u = {0};
                put(&u, "data:");
                put(&u, mime);
                put(&u, ";base64,");
                put(&u, data);
                quote(out, u.p);
                drop(&u);
                free(mime);
                free(data);
            } else return 0;
            put(out, "}}");
        } else if (!eq(j, type, "tool_use") && !eq(j, type, "tool_result") && !eq(j, type, "thinking") &&
                   !eq(j, type, "redacted_thinking"))
            return 0;
    }
    put(out, "]");
    return 1;
}
static int anthropic_request(Json *j, Buf *out) {
    int messages = get(j, 0, "messages");
    if (messages < 0 || j->t[messages].type != '[') return 0;
    put(out, "{\"messages\":[");
    int count = 0, sys = get(j, 0, "system");
    if (sys >= 0) {
        put(out, "{\"role\":\"system\",\"content\":");
        if (j->t[sys].type == '"') raw(out, j, sys, "\"\"");
        else {
            Buf txt = {0};
            for (int i = 0; i < j->t[sys].n; i++) {
                int b = at(j, sys, i);
                if (!eq(j, get(j, b, "type"), "text")) {
                    drop(&txt);
                    return 0;
                }
                char *p = string(j, get(j, b, "text"));
                if (i) put(&txt, "\n");
                put(&txt, p);
                free(p);
            }
            quote(out, txt.p ? txt.p : "");
            drop(&txt);
        }
        put(out, "}");
        count++;
    }
    for (int i = 0; i < j->t[messages].n; i++) {
        int m = at(j, messages, i), role = get(j, m, "role"), content = get(j, m, "content");
        if (content < 0 || (!eq(j, role, "user") && !eq(j, role, "assistant") && !eq(j, role, "system"))) return 0;
        if (j->t[content].type == '[') {
            for (int b = 0; b < j->t[content].n; b++) {
                int block = at(j, content, b);
                if (!eq(j, get(j, block, "type"), "tool_result")) continue;
                if (count++) put(out, ",");
                put(out, "{\"role\":\"tool\",\"tool_call_id\":");
                raw(out, j, get(j, block, "tool_use_id"), "\"\"");
                put(out, ",\"content\":");
                int c = get(j, block, "content");
                if (c >= 0 && j->t[c].type == '[') {
                    if (!content_parts(out, j, c)) return 0;
                } else raw(out, j, c, "\"\"");
                put(out, "}");
            }
        }
        Buf parts = {0};
        int calls = 0;
        if (j->t[content].type == '[') {
            if (!content_parts(&parts, j, content)) {
                drop(&parts);
                return 0;
            }
            for (int b = 0; b < j->t[content].n; b++) {
                int block = at(j, content, b);
                if (eq(j, get(j, block, "type"), "tool_use")) calls++;
            }
        }
        int normal = j->t[content].type == '"' || parts.n > 2 || calls;
        if (normal) {
            if (count++) put(out, ",");
            put(out, "{\"role\":");
            raw(out, j, role, "\"user\"");
            put(out, ",\"content\":");
            if (parts.p) putn(out, parts.p, parts.n);
            else raw(out, j, content, "\"\"");
            if (calls) {
                put(out, ",\"tool_calls\":[");
                int tc = 0;
                for (int b = 0; b < j->t[content].n; b++) {
                    int block = at(j, content, b);
                    if (!eq(j, get(j, block, "type"), "tool_use")) continue;
                    if (tc++) put(out, ",");
                    put(out, "{\"id\":");
                    raw(out, j, get(j, block, "id"), "\"\"");
                    put(out, ",\"type\":\"function\",\"function\":{\"name\":");
                    raw(out, j, get(j, block, "name"), "\"\"");
                    put(out, ",\"arguments\":");
                    Buf arg = {0};
                    raw(&arg, j, get(j, block, "input"), "{}");
                    quote(out, arg.p);
                    drop(&arg);
                    put(out, "}}");
                }
                put(out, "]");
            }
            put(out, "}");
        }
        drop(&parts);
    }
    put(out, "]");
    int tools = get(j, 0, "tools");
    if (tools >= 0) {
        put(out, ",\"tools\":[");
        for (int i = 0; i < j->t[tools].n; i++) {
            int t = at(j, tools, i);
            if (get(j, t, "input_schema") < 0) return 0;
            if (i) put(out, ",");
            put(out, "{\"type\":\"function\",\"function\":{\"name\":");
            raw(out, j, get(j, t, "name"), "\"\"");
            put(out, ",\"description\":");
            raw(out, j, get(j, t, "description"), "\"\"");
            put(out, ",\"parameters\":");
            raw(out, j, get(j, t, "input_schema"), "{}");
            put(out, "}}");
        }
        put(out, "]");
    }
    int tc = get(j, 0, "tool_choice");
    if (tc >= 0) {
        int type = get(j, tc, "type");
        put(out, ",\"tool_choice\":");
        if (eq(j, type, "tool")) {
            put(out, "{\"type\":\"function\",\"function\":{\"name\":");
            raw(out, j, get(j, tc, "name"), "\"\"");
            put(out, "}}");
        } else if (eq(j, type, "any")) quote(out, "required");
        else if (eq(j, type, "none")) quote(out, "none");
        else quote(out, "auto");
    }
    const char *names[] = {"stream", "max_tokens", "temperature", "top_p"};
    for (int i = 0; i < 4; i++) {
        int k = get(j, 0, names[i]);
        if (k >= 0) {
            put(out, ",");
            quote(out, names[i]);
            put(out, ":");
            raw(out, j, k, "null");
        }
    }
    int stop = get(j, 0, "stop_sequences");
    if (stop >= 0) {
        put(out, ",\"stop\":");
        raw(out, j, stop, "[]");
    }
    put(out, "}");
    return 1;
}
static Buf rewrite_model(Json *j, Model *m) {
    Buf out = {0};
    put(&out, "{\"model\":");
    quote(&out, m->name);
    copy_members(&out, j, 0, "model", "null", 1);
    put(&out, "}");
    return out;
}
static const char *stop_reason(Json *j, int reason) {
    return eq(j, reason, "tool_calls") ? "tool_use" : eq(j, reason, "length") ? "max_tokens" : "end_turn";
}
static int anthropic_response(Json *j, const char *requested, Buf *out) {
    int choice = at(j, get(j, 0, "choices"), 0), msg = get(j, choice, "message"), content = get(j, msg, "content"),
        calls = get(j, msg, "tool_calls"), count = 0;
    put(out, "{\"id\":");
    raw(out, j, get(j, 0, "id"), "\"msg_brokie\"");
    put(out, ",\"type\":\"message\",\"role\":\"assistant\",\"model\":");
    quote(out, requested);
    put(out, ",\"content\":[");
    if (content >= 0 && j->t[content].type == '"' && j->t[content].b - j->t[content].a > 2) {
        put(out, "{\"type\":\"text\",\"text\":");
        raw(out, j, content, "\"\"");
        put(out, "}");
        count++;
    }
    for (int i = 0; calls >= 0 && i < j->t[calls].n; i++) {
        int t = at(j, calls, i), f = get(j, t, "function");
        char *arg = string(j, get(j, f, "arguments"));
        Json a = json(arg, strlen(arg));
        if (a.bad || a.t[0].type != '{') {
            jfree(&a);
            free(arg);
            return 0;
        }
        if (count++) put(out, ",");
        put(out, "{\"type\":\"tool_use\",\"id\":");
        raw(out, j, get(j, t, "id"), "\"tool_brokie\"");
        put(out, ",\"name\":");
        raw(out, j, get(j, f, "name"), "\"\"");
        put(out, ",\"input\":");
        put(out, arg);
        put(out, "}");
        jfree(&a);
        free(arg);
    }
    if (!count) return 0;
    put(out, "],\"stop_reason\":");
    quote(out, stop_reason(j, get(j, choice, "finish_reason")));
    int usage = get(j, 0, "usage");
    fmt(out, ",\"stop_sequence\":null,\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d}}",
        (int)fieldnum(j, usage, "prompt_tokens", 0), (int)fieldnum(j, usage, "completion_tokens", 0));
    return 1;
}

static double monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000. + t.tv_nsec / 1000000.;
}
static int send_all(Sock s, const char *p, size_t n) {
    while (n) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= k;
    }
    return 1;
}
static int response_headers(Sock s, int status, const char *type, const char *route, long long length) {
    Buf h = {0};
    fmt(&h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\n", status,
        status == 200 ? "OK" : "Error", type);
    if (length >= 0) fmt(&h, "Content-Length: %lld\r\n", length);
    if (route) fmt(&h, "X-Routed-Via: %s\r\n", route);
    put(&h, "\r\n");
    int ok = send_all(s, h.p, h.n);
    drop(&h);
    return ok;
}
static int reply(Sock s, int code, const char *body) {
    return response_headers(s, code, "application/json", NULL, (long long)strlen(body)) &&
           send_all(s, body, strlen(body));
}
static int error_reply(Sock s, int code, int anthropic, const char *message) {
    Buf b = {0};
    put(&b, anthropic ? "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":"
                      : "{\"error\":{\"type\":\"routing_error\",\"message\":");
    quote(&b, message);
    put(&b, "}}");
    int ok = reply(s, code, b.p);
    drop(&b);
    return ok;
}
static int sse(Sock s, const char *name, Buf *data) {
    Buf b = {0};
    fmt(&b, "event: %s\ndata: ", name);
    putn(&b, data->p, data->n);
    put(&b, "\n\n");
    int ok = send_all(s, b.p, b.n);
    drop(&b);
    return ok;
}
static int emit(Sock s, const char *name, Buf *data) {
    int ok = sse(s, name, data);
    drop(data);
    return ok;
}
typedef int (*Chunk)(void *, const char *, size_t);
typedef struct {
    long status, retry_after;
    double first_ms, start;
    Buf body;
    Chunk chunk;
    void *ctx;
    int canceled, native, timeout;
    const char *request_type;
    char signature[256], content_type[256];
} Upstream;
static int receive_data(Upstream *u, const char *p, size_t n) {
    if (!u->first_ms && n) u->first_ms = monotonic_ms() - u->start;
    if (u->status == 200 && u->chunk) {
        if (!u->chunk(u->ctx, p, n)) {
            u->canceled = 1;
            return 0;
        }
    } else {
        if (u->body.n + n > MAX_BODY) return 0;
        putn(&u->body, p, n);
    }
    return 1;
}
static int safe_url(const char *url) {
    return !strncmp(url, "https://", 8) || !strncmp(url, "http://127.0.0.1:", 17) ||
           !strncmp(url, "http://localhost:", 17);
}
static size_t curl_body(char *p, size_t a, size_t b, void *ctx) { return receive_data(ctx, p, a * b) ? a * b : 0; }
static size_t curl_header(char *p, size_t a, size_t b, void *ctx) {
    Upstream *u = ctx;
    size_t n = a * b;
    if (n > 5 && !memcmp(p, "HTTP/", 5)) {
        char *sp = memchr(p, ' ', n);
        if (sp) u->status = strtol(sp + 1, NULL, 10);
    }
    if (n > 12 && !strncasecmp(p, "Retry-After:", 12)) u->retry_after = strtol(p + 12, NULL, 10);
    if (n > 13 && !strncasecmp(p, "Content-Type:", 13)) {
        size_t len = n - 13;
        while (len && isspace((unsigned char)p[13 + len - 1])) len--;
        snprintf(u->content_type, sizeof(u->content_type), "%.*s", (int)len, p + 13);
    }
    if (n > 20 && !strncasecmp(p, "x-catalog-signature:", 20)) {
        size_t start = 20;
        while (start < n && isspace((unsigned char)p[start])) start++;
        size_t len = n - start;
        while (len && isspace((unsigned char)p[start + len - 1])) len--;
        if (len < sizeof(u->signature)) {
            memcpy(u->signature, p + start, len);
            u->signature[len] = 0;
        }
    }
    return n;
}
static int fetch(Key *key, const char *url, const char *body, size_t n, Upstream *u) {
    if (!safe_url(url) || strpbrk(key->secret, "\r\n")) return 0;
    CURL *c = curl_easy_init();
    if (!c) return 0;
    u->start = monotonic_ms();
    struct curl_slist *h = NULL;
    Buf auth = {0}, type = {0};
    fmt(&type, "Content-Type: %s", u->request_type ? u->request_type : "application/json");
    h = curl_slist_append(h, type.p);
    if (!key->anonymous || (u->native && !strcmp(key->provider, "aihorde"))) {
        const char *prefix = "Authorization: Bearer ";
        if (u->native) {
            if (!strcmp(key->provider, "exa") || !strcmp(key->provider, "parallel") ||
                !strcmp(key->provider, "keenable"))
                prefix = "x-api-key: ";
            if (!strcmp(key->provider, "brave")) prefix = "X-Subscription-Token: ";
            if (!strcmp(key->provider, "google")) prefix = "x-goog-api-key: ";
            if (!strcmp(key->provider, "pinecone")) prefix = "Api-Key: ";
        }
        put(&auth, prefix);
        put(&auth, key->anonymous ? "0000000000" : key->secret);
        h = curl_slist_append(h, auth.p);
    }
    if (u->native && !strcmp(key->provider, "speechify")) h = curl_slist_append(h, "Speechify-Version: 2026-09-08");
    if (u->native && !strcmp(key->provider, "pinecone")) h = curl_slist_append(h, "X-Pinecone-Api-Version: 2026-07");
    if (u->native && !strcmp(key->provider, "keenable")) h = curl_slist_append(h, "X-Keenable-Title: Brokie");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(u->timeout ? u->timeout : timeout_ms));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     !strcmp(key->provider, "routeway") ? "Mozilla/5.0 Brokie/0.1" : "Brokie/0.1");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, u);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, u);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)n);
    }
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &u->status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (auth.p) secure_zero(auth.p, auth.n);
    drop(&auth);
    drop(&type);
    return rc == CURLE_OK;
}

typedef struct {
    Buf id, name, args;
} Tool;
typedef struct {
    Sock sock;
    int anthropic, committed, disconnected, text_open, finished, meaningful, bad;
    int input, output;
    double first_text_ms, start;
    char route[512], requested[512], reason[32];
    Buf line, pending;
    Tool tools[MAX_TOOLS];
} Stream;
static int commit(Stream *s) {
    if (s->committed) return !s->disconnected;
    s->committed = 1;
    if (!response_headers(s->sock, 200, "text/event-stream", s->route, -1)) {
        s->disconnected = 1;
        return 0;
    }
    if (!s->anthropic) return 1;
    Buf b = {0};
    put(&b, "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_brokie\",\"type\":\"message\",\"role\":"
            "\"assistant\",\"model\":");
    quote(&b, s->requested);
    put(&b, ",\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,\"usage\":{\"input_tokens\":0,"
            "\"output_tokens\":0}}}");
    int ok = emit(s->sock, "message_start", &b);
    if (!ok) s->disconnected = 1;
    return ok;
}
static int text_delta(Stream *s, Json *j, int token) {
    if (!s->text_open) {
        Buf b = {0};
        put(&b, "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":"
                "\"\"}}");
        if (!emit(s->sock, "content_block_start", &b)) return 0;
        s->text_open = 1;
    }
    Buf b = {0};
    put(&b, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":");
    raw(&b, j, token, "\"\"");
    put(&b, "}}");
    return emit(s->sock, "content_block_delta", &b);
}
static int process_line(Stream *s) {
    if (s->line.n < 5 || memcmp(s->line.p, "data:", 5)) return 1;
    char *p = s->line.p + 5;
    while (*p == ' ') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\r' || p[n - 1] == '\n')) n--;
    if (n == 6 && !memcmp(p, "[DONE]", 6)) {
        s->finished = 1;
        return 1;
    }
    Json j = json(p, n);
    if (j.bad) {
        jfree(&j);
        s->bad = 1;
        return 0;
    }
    if (get(&j, 0, "error") >= 0) {
        jfree(&j);
        s->bad = 1;
        return 0;
    }
    int usage = get(&j, 0, "usage");
    if (usage >= 0) {
        s->input = (int)fieldnum(&j, usage, "prompt_tokens", s->input);
        s->output = (int)fieldnum(&j, usage, "completion_tokens", s->output);
    }
    int choice = at(&j, get(&j, 0, "choices"), 0), delta = get(&j, choice, "delta"),
        content = get(&j, delta, "content"), calls = get(&j, delta, "tool_calls"),
        reason = get(&j, choice, "finish_reason");
    int text = content >= 0 && j.t[content].type == '"' && j.t[content].b - j.t[content].a > 2,
        tools = calls >= 0 && j.t[calls].n > 0;
    if (reason >= 0 && j.t[reason].type == '"') {
        char *r = string(&j, reason);
        snprintf(s->reason, sizeof(s->reason), "%s", r);
        free(r);
        s->finished = 1;
    }
    if (text || tools) {
        s->meaningful = 1;
        if (!s->first_text_ms) s->first_text_ms = monotonic_ms() - s->start;
        if (!commit(s)) {
            jfree(&j);
            return 0;
        }
    }
    if (s->anthropic) {
        if (text && !text_delta(s, &j, content)) {
            s->disconnected = 1;
            jfree(&j);
            return 0;
        }
        for (int i = 0; calls >= 0 && i < j.t[calls].n; i++) {
            int t = at(&j, calls, i), index = (int)fieldnum(&j, t, "index", 0), f = get(&j, t, "function");
            if (index < 0 || index >= MAX_TOOLS) {
                s->bad = 1;
                jfree(&j);
                return 0;
            }
            Tool *tool = &s->tools[index];
            int fields[] = {get(&j, t, "id"), get(&j, f, "name"), get(&j, f, "arguments")};
            Buf *dst[] = {&tool->id, &tool->name, &tool->args};
            for (int q = 0; q < 3; q++)
                if (fields[q] >= 0) {
                    char *v = string(&j, fields[q]);
                    put(dst[q], v);
                    free(v);
                }
        }
    }
    jfree(&j);
    return 1;
}
static int stream_chunk(void *ctx, const char *p, size_t n) {
    Stream *s = ctx;
    if (!s->anthropic && !s->committed) {
        if (s->pending.n + n > MAX_BODY) return 0;
        putn(&s->pending, p, n);
    }
    int was = s->committed;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            if (!process_line(s)) return 0;
            s->line.n = 0;
            if (s->line.p) s->line.p[0] = 0;
        } else {
            if (s->line.n >= MAX_BODY) return 0;
            putn(&s->line, p + i, 1);
        }
    }
    if (!s->anthropic && s->committed) {
        if (!was) {
            if (!send_all(s->sock, s->pending.p, s->pending.n)) {
                s->disconnected = 1;
                return 0;
            }
            drop(&s->pending);
        } else if (!send_all(s->sock, p, n)) {
            s->disconnected = 1;
            return 0;
        }
    }
    return 1;
}
static int finish_stream(Stream *s, int ok) {
    if (s->line.n && !process_line(s)) ok = 0;
    if (!s->committed) return 0;
    if (!ok || !s->finished || s->bad) {
        Buf b = {0};
        put(&b, "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"Upstream stream ended "
                "before completion; retry the request.\"}}");
        if (s->anthropic) sse(s->sock, "error", &b);
        else {
            send_all(s->sock, "data: ", 6);
            send_all(s->sock, b.p, b.n);
            send_all(s->sock, "\n\n", 2);
        }
        drop(&b);
        return 0;
    }
    if (!s->anthropic) return 1;
    Buf b = {0};
    int index = 0;
    if (s->text_open) {
        put(&b, "{\"type\":\"content_block_stop\",\"index\":0}");
        if (!emit(s->sock, "content_block_stop", &b)) return 0;
        index = 1;
    }
    for (int i = 0; i < MAX_TOOLS; i++) {
        Tool *t = &s->tools[i];
        if (!t->name.n) continue;
        Json args = json(t->args.p ? t->args.p : "{}", t->args.n ? t->args.n : 2);
        if (args.bad || args.t[0].type != '{') {
            jfree(&args);
            s->bad = 1;
            return finish_stream(s, 0);
        }
        jfree(&args);
        fmt(&b,
            "{\"type\":\"content_block_start\",\"index\":%d,\"content_block\":{\"type\":\"tool_use\",\"id\":", index);
        quote(&b, t->id.p ? t->id.p : "tool_brokie");
        put(&b, ",\"name\":");
        quote(&b, t->name.p);
        put(&b, ",\"input\":{}}}");
        if (!emit(s->sock, "content_block_start", &b)) return 0;
        fmt(&b,
            "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":\"input_json_delta\","
            "\"partial_json\":",
            index);
        quote(&b, t->args.p ? t->args.p : "{}");
        put(&b, "}}");
        if (!emit(s->sock, "content_block_delta", &b)) return 0;
        fmt(&b, "{\"type\":\"content_block_stop\",\"index\":%d}", index++);
        if (!emit(s->sock, "content_block_stop", &b)) return 0;
    }
    put(&b, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":");
    quote(&b, !strcmp(s->reason, "tool_calls") ? "tool_use" : !strcmp(s->reason, "length") ? "max_tokens" : "end_turn");
    fmt(&b, ",\"stop_sequence\":null},\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d}}", s->input, s->output);
    int done = emit(s->sock, "message_delta", &b);
    put(&b, "{\"type\":\"message_stop\"}");
    done = done && sse(s->sock, "message_stop", &b);
    drop(&b);
    return done;
}
static void free_stream(Stream *s) {
    drop(&s->line);
    drop(&s->pending);
    for (int i = 0; i < MAX_TOOLS; i++) {
        drop(&s->tools[i].id);
        drop(&s->tools[i].name);
        drop(&s->tools[i].args);
    }
}

/* The HTTP listener is loopback-only. At most 16 worker threads hold requests. */
static int active_clients, updating;
static void *automatic_refresh(void *unused);
static int sync_catalog(int live);
static int same_secret(const char *a, const char *b) {
    size_t an = strlen(a), bn = strlen(b);
    unsigned diff = (unsigned)(an ^ bn);
    for (size_t i = 0; i < bn; i++) diff |= (unsigned char)b[i] ^ (i < an ? (unsigned char)a[i] : 0);
    return !diff;
}
static int header(const char *headers, const char *name, char *out, size_t cap) {
    size_t len = strlen(name);
    const char *p = strstr(headers, "\r\n");
    if (!p) return 0;
    p += 2;
    int found = 0;
    while (*p) {
        const char *e = strstr(p, "\r\n");
        if (!e || e == p) break;
        if ((size_t)(e - p) > len && p[len] == ':' && !strncasecmp(p, name, len)) {
            if (found) return -1;
            const char *v = p + len + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            size_t n = e - v;
            while (n && isspace((unsigned char)v[n - 1])) n--;
            if (n >= cap) return -1;
            memcpy(out, v, n);
            out[n] = 0;
            found = 1;
        }
        p = e + 2;
    }
    return found;
}
static uint64_t session_hash(Json *j, const char *mode) {
    uint64_t h = 1469598103934665603ULL;
    const char *p = mode;
    while (*p) {
        h ^= (unsigned char)*p++;
        h *= 1099511628211ULL;
    }
    int arr = get(j, 0, "messages"), first = -1;
    for (int i = 0; arr >= 0 && i < j->t[arr].n; i++) {
        int m = at(j, arr, i);
        if (eq(j, get(j, m, "role"), "user")) {
            first = get(j, m, "content");
            break;
        }
    }
    if (first < 0) return 0;
    for (int i = j->t[first].a; i < j->t[first].b; i++) {
        h ^= (unsigned char)j->s[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}
static int requires_vision(Json *j) {
    for (int i = 0; i < j->n; i++)
        if (j->t[i].type == '{' && (eq(j, get(j, i, "type"), "image_url") || eq(j, get(j, i, "type"), "image")))
            return 1;
    return 0;
}
static void models_reply(Sock client) {
    Buf b = {0};
    put(&b, "{\"object\":\"list\",\"data\":[");
    const char *aliases[] = {"auto", "auto:smartest", "auto:fastest", "auto:reliable", "auto:balanced"};
    for (int i = 0; i < 5; i++) {
        if (i) put(&b, ",");
        put(&b, "{\"object\":\"model\",\"id\":");
        quote(&b, aliases[i]);
        put(&b, ",\"owned_by\":\"brokie\"}");
    }
    for (int i = 0; i < nm; i++)
        if (models[i].enabled) {
            char id[512];
            snprintf(id, sizeof(id), "%s/%s", models[i].provider, models[i].name);
            put(&b, ",{\"object\":\"model\",\"id\":");
            quote(&b, id);
            put(&b, ",\"owned_by\":");
            quote(&b, models[i].provider);
            put(&b, "}");
        }
    put(&b, "]}");
    reply(client, 200, b.p);
    drop(&b);
}
static void complete_request(Sock client, Json *original, int anthropic) {
    Buf converted = {0};
    Json translated = {0};
    Json *j = original;
    if (anthropic) {
        if (!anthropic_request(original, &converted)) {
            error_reply(client, 400, 1,
                        "Unsupported Anthropic content or server tool; text, images and client tools are supported.");
            drop(&converted);
            return;
        }
        translated = json(converted.p, converted.n);
        j = &translated;
    }
    char requested[512];
    fieldstr(original, 0, "model", requested, sizeof(requested));
    const char *mode = default_mode, *pin = NULL;
    if (!strncmp(requested, "auto:", 5)) {
        mode = requested + 5;
        if (!valid_mode(mode)) {
            error_reply(client, 400, anthropic, "Unknown routing mode.");
            goto cleanup;
        }
    } else if (valid_mode(requested)) mode = requested;
    else if (requested[0] && strcmp(requested, "auto") && !(anthropic && !strncmp(requested, "claude-", 7)))
        pin = requested;
    int messages = get(j, 0, "messages");
    if (j->bad || messages < 0 || j->t[messages].type != '[' || !j->t[messages].n || fieldnum(j, 0, "n", 1) != 1) {
        error_reply(client, 400, anthropic, "Expected messages and n=1.");
        goto cleanup;
    }
    int streaming = boolean(j, get(j, 0, "stream"), 0), tools = get(j, 0, "tools"),
        need_tools = tools >= 0 && j->t[tools].n > 0, vision = requires_vision(j);
    int estimate = (int)(j->len / 4) + ((int)fmin(2000, fmax(1, fieldnum(j, 0, "max_tokens", 1000))));
    unsigned char *tried = mem((size_t)(nm ? nm : 1) * (nk ? nk : 1));
    uint64_t hash = pin ? 0 : session_hash(j, mode);
    int answered = 0, last_status = 503;
    double start = monotonic_ms();
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        Route r = {0};
        LOCK();
        int found = choose(mode, pin, need_tools, vision, estimate, tried, 1, hash, &r);
        if (found) {
            Stats *s = st(r.m, r.k);
            s->inflight++;
            s->min_req++;
            s->day_req++;
            s->min_tokens += estimate;
            s->day_tokens += estimate;
        }
        UNLOCK();
        if (!found) break;
        tried[(size_t)r.m * nk + r.k] = 1;
        Model *m = &models[r.m];
        Key *key = &keys[r.k];
        Buf request = rewrite_model(j, m);
        Stream stream = {0};
        stream.sock = client;
        stream.anthropic = anthropic;
        stream.start = monotonic_ms();
        snprintf(stream.requested, sizeof(stream.requested), "%s", requested[0] ? requested : "auto");
        snprintf(stream.route, sizeof(stream.route), "%s/%s", m->provider, m->name);
        Upstream up = {0};
        if (streaming) {
            up.chunk = stream_chunk;
            up.ctx = &stream;
        }
        int transport = fetch(key, key->url, request.p, request.n, &up), success = 0, input = 0, output = 0;
        double latency = monotonic_ms() - stream.start, ttfb = up.first_ms;
        Json result = {0};
        Buf adapted = {0};
        if (streaming) {
            if (transport && up.status == 200 && !stream.meaningful && stream.line.n) process_line(&stream);
            success = transport && up.status == 200 && stream.meaningful && stream.finished && !stream.bad;
            if (stream.committed) {
                success = finish_stream(&stream, success);
                answered = 1;
            }
            input = stream.input;
            output = stream.output;
            ttfb = stream.first_text_ms;
        } else if (transport && up.status == 200 && up.body.p) {
            result = json(up.body.p, up.body.n);
            if (!result.bad) {
                int choice = at(&result, get(&result, 0, "choices"), 0), msg = get(&result, choice, "message"),
                    c = get(&result, msg, "content"), tc = get(&result, msg, "tool_calls");
                success = (c >= 0 && result.t[c].type == '"' && result.t[c].b - result.t[c].a > 2) ||
                          (tc >= 0 && result.t[tc].n > 0);
                int usage = get(&result, 0, "usage");
                input = (int)fieldnum(&result, usage, "prompt_tokens", 0);
                output = (int)fieldnum(&result, usage, "completion_tokens", 0);
                if (success && anthropic) success = anthropic_response(&result, stream.requested, &adapted);
                if (success) {
                    Buf *b = anthropic ? &adapted : &up.body;
                    success = response_headers(client, 200, "application/json", stream.route, (long long)b->n) &&
                              send_all(client, b->p, b->n);
                    answered = 1;
                    if (!success) stream.disconnected = 1;
                }
            }
        }
        LOCK();
        Stats *s = st(r.m, r.k);
        decay(s, now_s());
        s->inflight--;
        double actual = input + output > 0 ? input + output : success ? estimate : 0;
        s->min_tokens = fmax(0, s->min_tokens - estimate + actual);
        s->day_tokens = fmax(0, s->day_tokens - estimate + actual);
        s->month_tokens += actual;
        if (!stream.disconnected) {
            int health = response_health(transport, up.status);
            if (success || health != H_PUBLIC) set_health(key, success ? H_OK : health, up.status);
            if (success) {
                s->s++;
                s->out += output;
                s->ms += latency;
                if (ttfb > 0) {
                    s->ttfb += ttfb;
                    s->ttn++;
                }
                s->cooldown = 0;
                if (hash && sticky_seconds > 0) sticky[hash % 128] = (Sticky){hash, r.m, now_s() + sticky_seconds};
            } else {
                s->f++;
                if (!transport) {
                    s->ms += fmin(latency, 120000);
                    s->ttfb += fmin(latency, 120000);
                    s->ttn++;
                }
                double cool = up.status == 401 || up.status == 402 || up.status == 403 ? 86400
                              : up.status == 429 ? fmax(30, fmin(86400, up.retry_after))
                              : up.status == 404 ? 3600
                                                 : 30;
                s->cooldown = now_s() + cool;
                if (up.status == 401 || up.status == 402)
                    for (int q = 0; q < nm; q++) st(q, r.k)->cooldown = s->cooldown;
                if (up.status == 400 || up.status == 404)
                    for (int k = 0; k < nk; k++) tried[(size_t)r.m * nk + k] = 1;
            }
        }
        save_state();
        save_health();
        UNLOCK();
        last_status = up.status == 429 ? 429 : 502;
        fprintf(stderr, "%s/%s: %s (HTTP %ld, %.0f ms)\n", m->provider, m->name,
                success               ? "ok"
                : stream.disconnected ? "client disconnected"
                                      : "failed",
                up.status, latency);
        jfree(&result);
        drop(&adapted);
        drop(&request);
        drop(&up.body);
        free_stream(&stream);
        if (answered || stream.disconnected) break;
        if (monotonic_ms() - start > 120000) break;
    }
    free(tried);
    if (!answered)
        error_reply(client, last_status, anthropic,
                    "No eligible route completed the request. Check enabled keys, model capabilities, quotas "
                    "and provider availability.");
cleanup:
    jfree(&translated);
    drop(&converted);
}
/* Native API pass-through: pinned provider, original body, streamed response bytes. */
typedef struct {
    Sock client;
    Upstream *up;
    int started;
} Native;
static int native_chunk(void *ctx, const char *p, size_t n) {
    Native *s = ctx;
    if (!s->started) {
        s->started = 1;
        if (!response_headers(s->client, (int)s->up->status,
                              s->up->content_type[0] ? s->up->content_type : "application/octet-stream", NULL, -1))
            return 0;
    }
    return send_all(s->client, p, n);
}
static void native_request(Sock client, const char *path, const char *body, size_t n, const char *type) {
    const char *name = path + 8, *slash = strchr(name, '/');
    const char *selector = slash ? memchr(name, '@', (size_t)(slash - name)) : NULL;
    char *selected_end = NULL;
    long selected = selector ? strtol(selector + 1, &selected_end, 10) : 0;
    size_t name_len = slash ? (size_t)((selector ? selector : slash) - name) : 0;
    Key *key = NULL;
    for (int i = 0; slash && i < nk; i++)
        if (keys[i].enabled && (keys[i].anonymous || keys[i].secret[0]) &&
            (!selector || (selected == keys[i].id && selected_end == slash)) && strlen(keys[i].provider) == name_len &&
            !memcmp(keys[i].provider, name, name_len)) {
            key = &keys[i];
            break;
        }
    if (!key) {
        error_reply(client, 404, 0, "Add an enabled key for /native/PROVIDER/PATH first.");
        return;
    }
    char url[4096];
    if (!key_url(url, sizeof(url), key, slash, 0)) {
        error_reply(client, 414, 0, "Native API path is too long.");
        return;
    }
    Upstream up = {.native = 1, .request_type = type};
    Native stream = {client, &up, 0};
    up.chunk = native_chunk;
    up.ctx = &stream;
    int ok = fetch(key, url, body, n, &up);
    int health = response_health(ok, up.status);
    if ((ok && body && up.status >= 200 && up.status < 300) || health != H_PUBLIC) {
        LOCK();
        set_health(key, health == H_PUBLIC ? H_OK : health, up.status);
        save_health();
        UNLOCK();
    }
    if (!stream.started) {
        if (!ok) error_reply(client, 502, 0, "Native API request failed.");
        else if (response_headers(client, (int)up.status,
                                  up.content_type[0] ? up.content_type : "application/octet-stream", NULL,
                                  (long long)up.body.n) &&
                 up.body.n)
            send_all(client, up.body.p, up.body.n);
    }
    drop(&up.body);
}
static void *handle_client(void *arg) {
    Sock client = (Sock)(intptr_t)arg;
    Buf req = {0};
    char buf[8192];
    size_t header_size = 0, content_length = 0;
    int bad = 0;
    char method[16] = {0}, path[2048] = {0};
    while (!header_size) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) goto done;
        putn(&req, buf, n);
        char *end = strstr(req.p, "\r\n\r\n");
        if (end) header_size = (size_t)(end - req.p) + 4;
        else if (req.n > 65536) {
            bad = 431;
            break;
        }
    }
    if (bad) {
        error_reply(client, bad, 0, "HTTP headers exceed limit.");
        goto done;
    }
    if (sscanf(req.p, "%15s %2047s", method, path) != 2) {
        error_reply(client, 400, 0, "Invalid request line.");
        goto done;
    }
    int native = !strncmp(path, "/native/", 8);
    char *q = strchr(path, '?');
    if (q && !native) *q = 0;
    int anthropic = !strncmp(path, "/v1/messages", 12);
    char *headers = mem(header_size + 1);
    memcpy(headers, req.p, header_size);
    char auth[8192] = {0}, api[8192] = {0}, length[64] = {0}, transfer[64] = {0}, type[256] = {0};
    int la = header(headers, "Content-Length", length, sizeof(length)),
        au = header(headers, "Authorization", auth, sizeof(auth)), ak = header(headers, "x-api-key", api, sizeof(api)),
        te = header(headers, "Transfer-Encoding", transfer, sizeof(transfer)),
        ct = header(headers, "Content-Type", type, sizeof(type));
    free(headers);
    if (la < 0 || au < 0 || ak < 0 || te != 0 || (native && ct < 0)) {
        error_reply(client, 400, anthropic, "Ambiguous headers or chunked request body; use Content-Length.");
        goto done;
    }
    if (!strcmp(path, "/health") && !strcmp(method, "GET")) {
        reply(client, 200, "{\"status\":\"ok\"}");
        goto done;
    }
    const char *credential = au > 0 && !strncasecmp(auth, "Bearer ", 7) ? auth + 7 : ak > 0 ? api : "";
    if (!same_secret(credential, auth_token)) {
        error_reply(client, 401, anthropic, "Invalid gateway credential.");
        goto done;
    }
    secure_zero(auth, sizeof(auth));
    secure_zero(api, sizeof(api));
    if (native && !strcmp(method, "GET")) {
        if ((la && strcmp(length, "0")) || req.n != header_size)
            error_reply(client, 400, 0, "Native GET requests cannot contain a body.");
        else native_request(client, path, NULL, 0, NULL);
        goto done;
    }
    if (!strcmp(path, "/v1/models") && !strcmp(method, "GET")) {
        models_reply(client);
        goto done;
    }
    if (strcmp(method, "POST") || (!native && strcmp(path, "/v1/chat/completions") && strcmp(path, "/v1/messages") &&
                                   strcmp(path, "/v1/messages/count_tokens"))) {
        error_reply(client, 404, anthropic,
                    "Supported endpoints: /v1/models, /v1/chat/completions, /v1/messages, "
                    "/v1/messages/count_tokens, /native/PROVIDER/PATH.");
        goto done;
    }
    if (la != 1 || !length[0]) {
        error_reply(client, 411, anthropic, "Content-Length is required.");
        goto done;
    }
    for (const char *p = length; *p; p++)
        if (!isdigit((unsigned char)*p)) bad = 400;
    unsigned long long len = strtoull(length, NULL, 10);
    if (bad || len > MAX_BODY - 65536) {
        error_reply(client, len > MAX_BODY - 65536 ? 413 : 400, anthropic, "Invalid or oversized request body.");
        goto done;
    }
    content_length = (size_t)len;
    if (req.n - header_size > content_length) {
        error_reply(client, 400, anthropic, "Pipelined requests are not supported.");
        goto done;
    }
    while (req.n - header_size < content_length) {
        size_t remain = content_length - (req.n - header_size);
        int n = recv(client, buf, (int)fmin(remain, sizeof(buf)), 0);
        if (n <= 0) goto done;
        putn(&req, buf, n);
    }
    if (native) {
        native_request(client, path, req.p + header_size, content_length, type[0] ? type : NULL);
        goto done;
    }
    Json j = json(req.p + header_size, content_length);
    if (j.bad || j.t[0].type != '{') {
        error_reply(client, 400, anthropic, "Invalid JSON object.");
        jfree(&j);
        goto done;
    }
    if (!strcmp(path, "/v1/messages/count_tokens")) {
        char body[128];
        snprintf(body, sizeof(body), "{\"input_tokens\":%zu,\"approximate\":true}", (content_length + 3) / 4);
        reply(client, 200, body);
    } else complete_request(client, &j, anthropic);
    jfree(&j);
done:
    if (req.p) secure_zero(req.p, req.n);
    drop(&req);
    close_socket(client);
    LOCK();
    active_clients--;
    if (!active_clients) pthread_cond_broadcast(&idle);
    UNLOCK();
    return NULL;
}
static void serve(void) {
    signal(SIGPIPE, SIG_IGN);
    if (!offline() && refresh_seconds && !nm) sync_catalog(1);
    Sock listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == BAD_SOCKET) die("socket failed");
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 16))
        die("cannot bind loopback port (another instance may be running)");
    fprintf(stderr, "Brokie listening at http://127.0.0.1:%d; %d models, %d keys; default %s\n", port, nm, nk,
            default_mode);
    pthread_t updater;
    if (!offline()) {
        if (pthread_create(&updater, NULL, automatic_refresh, NULL)) die("cannot start automatic refresh");
        pthread_detach(updater);
    }
    for (;;) {
        Sock client = accept(listener, NULL, NULL);
        if (client == BAD_SOCKET) continue;
        struct timeval tv = {30, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        LOCK();
        while (updating) pthread_cond_wait(&idle, &mutex);
        int full = active_clients >= 16;
        if (!full) active_clients++;
        UNLOCK();
        if (full) {
            error_reply(client, 503, 0, "Router is busy.");
            close_socket(client);
            continue;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, handle_client, (void *)(intptr_t)client)) {
            close_socket(client);
            LOCK();
            active_clients--;
            if (!active_clients) pthread_cond_broadcast(&idle);
            UNLOCK();
        } else pthread_detach(t);
    }
}

/* Small local administration CLI. Neither configuration nor logs contain keys. */
static Buf replace_property(Buf *source, const char *name, Buf *value) {
    Json j = json(source->p, source->n);
    Buf b = {0};
    put(&b, "{");
    int count = copy_members(&b, &j, 0, name, "null", 0);
    if (count) put(&b, ",");
    quote(&b, name);
    put(&b, ":");
    putn(&b, value->p, value->n);
    put(&b, "}\n");
    jfree(&j);
    return b;
}
static void replace_config(const char *name, Buf *value) {
    Buf b = replace_property(&config, name, value);
    if (!write_file("config.json", b.p, b.n)) die("cannot write config.json");
    drop(&config);
    config = b;
}
static void append_config(const char *name, Buf *row) {
    Json j = json(config.p, config.n);
    int a = get(&j, 0, name);
    Buf b = {0};
    put(&b, "[");
    for (int i = 0; a >= 0 && i < j.t[a].n; i++) {
        if (i) put(&b, ",");
        raw(&b, &j, at(&j, a, i), "null");
    }
    if (a >= 0 && j.t[a].n) put(&b, ",");
    putn(&b, row->p, row->n);
    put(&b, "]");
    jfree(&j);
    replace_config(name, &b);
    drop(&b);
}
static void initialize(void) {
    Buf existing = read_file("vault.bin");
    if (existing.p) die("vault already exists; refusing to replace it");
    existing = read_file("config.json");
    if (existing.p) die("config already exists; refusing to replace it");
    if (!private_directory()) die("cannot create private state directory");
    unsigned char random[32];
    random_bytes(random, 32);
    Buf vault = {0};
    put(&vault, "{\"token\":\"brokie_");
    for (int i = 0; i < 32; i++) fmt(&vault, "%02x", random[i]);
    put(&vault, "\",\"keys\":{}}");
    if (!vault_write(&vault, 1)) die("cannot create vault");
    secure_zero(vault.p, vault.n);
    drop(&vault);
    const char *initial = "{\n  \"port\":31817, \"mode\":\"smartest\", \"timeout_ms\":60000, \"max_attempts\":6, "
                          "\"sticky_seconds\":1800,\n  \"keys\":[], \"models\":[]\n}\n";
    if (!write_file("config.json", initial, strlen(initial))) die("cannot create config");
    puts("Initialized private configuration and encrypted vault.");
}
static void write_vault_property(const char *name, const char *value, int key_id) {
    Buf v = vault_read();
    Json j = json(v.p, v.n);
    if (j.bad) die("invalid vault");
    Buf b = {0};
    put(&b, "{");
    int count = copy_members(&b, &j, 0, key_id ? "keys" : name, "null", 0);
    if (count) put(&b, ",");
    quote(&b, key_id ? "keys" : name);
    put(&b, ":");
    if (key_id) {
        put(&b, "{");
        int ka = get(&j, 0, "keys");
        char id[32];
        snprintf(id, sizeof(id), "%d", key_id);
        int n = copy_members(&b, &j, ka, id, "\"\"", 0);
        if (n) put(&b, ",");
        quote(&b, id);
        put(&b, ":");
        quote(&b, value);
        put(&b, "}");
    } else quote(&b, value);
    put(&b, "}");
    if (!vault_write(&b, 0)) die("cannot update vault");
    jfree(&j);
    secure_zero(v.p, v.n);
    secure_zero(b.p, b.n);
    drop(&v);
    drop(&b);
}
typedef struct {
    const char *id, *url, *signup, *probe;
    int native, anonymous;
} Provider;
/* Provider metadata from FreeLLMAPI; API keys never belong here. See PROVIDERS.md. */
// clang-format off
static const Provider providers[] = {
    {"aclide", "https://aclide.com/v1", "https://aclide.com/en/dashboard/api-keys", "-", 1, 0},
    {"agnes", "https://apihub.agnes-ai.com/v1", "https://platform.agnes-ai.com", "/models", 0, 0},
    {"aihorde", "https://oai.aihorde.net/v1", "https://aihorde.net/register", "-", 1, 1},
    {"ainative", "https://api.ainative.studio/api/v1", "https://ainative.studio", "/models", 0, 0},
    {"aion", "https://api.aionlabs.ai/v1", "https://www.aionlabs.ai", "/models", 0, 0},
    {"airforce", "https://api.airforce/v1", "https://api.airforce", "/models", 0, 0},
    {"anyapi", "https://api.anyapi.ai/v1", "https://anyapi.ai", "/models", 0, 0},
    {"apinex", "https://api.apinex.bond/v1", "https://apinex.bond/keys", "/models", 0, 0},
    {"bai", "https://api.b.ai/v1", "https://b.ai", "/models", 0, 0},
    {"bazaarlink", "https://bazaarlink.ai/api/v1", "https://bazaarlink.ai", "/models", 0, 0},
    {"beatapi", "https://api.beatapi.io/v1", "https://beatapi.io", "-", 1, 0},
    {"blaze", "https://api.blazeapi.org/paid/v1", "https://blazeapi.org/dashboard", "/models", 0, 0},
    {"blockrun", "https://api.blockrun.ai/v1", "https://user.blockrun.ai/dashboard/keys", "/account", 0, 0},
    {"brave", "https://api.search.brave.com/res/v1", "https://api-dashboard.search.brave.com", "-", 1, 0},
    {"cerebras", "https://api.cerebras.ai/v1", "https://cloud.cerebras.ai", "/models", 0, 0},
    {"clod", "https://api.clod.io/v1", "https://newapp.clod.io", "/models", 0, 0},
    {"cloudflare", "https://api.cloudflare.com/client/v4/accounts/YOUR_ACCOUNT_ID/ai/v1", "https://dash.cloudflare.com", "/models", 0, 0},
    {"cohere", "https://api.cohere.ai/compatibility/v1", "https://dashboard.cohere.com/api-keys", "/models", 0, 0},
    {"dreamprompting", "https://dreamprompting.com/api/v1", "https://dreamprompting.com", "/models", 0, 0},
    {"electronhub", "https://api.electronhub.ai/v1", "https://app.electronhub.ai", "/user/me", 0, 0},
    {"exa", "https://api.exa.ai", "https://dashboard.exa.ai/api-keys", "-", 1, 0},
    {"experiential", "https://api.experientiallabs.ai/v1", "https://platform.experientiallabs.ai", "/models", 0, 0},
    {"firecrawl", "https://api.firecrawl.dev/v2", "https://www.firecrawl.dev/app/api-keys", "/team/credit-usage", 1, 0},
    {"freeai", "https://api.free.ai", "https://free.ai/account/#api-keys", "-", 1, 0},
    {"gizmo", "https://api.gizmoplatforms.com/api/v1", "https://gizmoplatforms.com/developers", "/models", 0, 0},
    {"google", "https://generativelanguage.googleapis.com/v1beta/openai", "https://aistudio.google.com/apikey", "/models", 0, 0},
    {"groq", "https://api.groq.com/openai/v1", "https://console.groq.com/keys", "/models", 0, 0},
    {"huggingface", "https://router.huggingface.co/v1", "https://huggingface.co/settings/tokens", "/models", 0, 0},
    {"inferbase", "https://api.inferbase.ai/v1", "https://inferbase.ai", "//api/v1/inference/credits", 0, 0},
    {"iteracompute", "https://api.iteracompute.com/v1", "https://iteracompute.com/free/", "-", 0, 0},
    {"jamai", "https://api.jamaibase.com/api/v1", "https://cloud.jamaibase.com", "/models", 0, 0},
    {"keenable", "https://api.keenable.ai/v1", "https://app.keenable.ai", "-", 1, 1},
    {"kenari", "https://kenari.id/v1", "https://kenari.id", "/account/balance", 0, 0},
    {"kilo", "https://api.kilo.ai/api/gateway/v1", "https://app.kilo.ai", "/models", 0, 1},
    {"kiosapi", "https://api.kiosapi.id/v1", "https://kiosapi.id/dashboard/keys", "-", 0, 0},
    {"linkup", "https://api.linkup.so/v1", "https://app.linkup.so", "/credits/balance", 1, 0},
    {"llm7", "https://api.llm7.io/v1", "https://llm7.io", "/models", 0, 0},
    {"llmtr", "https://llmtr.com/v1", "https://llmtr.com", "-", 0, 0},
    {"logfare", "https://logfare.ai/v1", "https://logfare.ai", "/models", 0, 0},
    {"longcat", "https://api.longcat.chat/openai/v1", "https://longcat.chat/platform", "/models", 0, 0},
    {"lucidity", "https://composite.lucidity.sh/v1", "https://composite.lucidity.sh", "/models", 0, 0},
    {"mistral", "https://api.mistral.ai/v1", "https://console.mistral.ai/api-keys/", "/models", 0, 0},
    {"modelscope", "https://api-inference.modelscope.cn/v1", "https://modelscope.cn/my/myaccesstoken", "-", 0, 0},
    {"moondream", "https://api.moondream.ai/v1", "https://moondream.ai/c/cloud/api-keys", "/models", 0, 0},
    {"nara", "https://router.bynara.id/v1", "https://router.bynara.id", "/models", 0, 0},
    {"navy", "https://api.navy/v1", "https://api.navy", "/models", 0, 0},
    {"nvidia", "https://integrate.api.nvidia.com/v1", "https://build.nvidia.com/settings/api-keys", "/models", 0, 0},
    {"ollama", "https://ollama.com/v1", "https://ollama.com/settings/keys", "/models", 0, 0},
    {"onomeo", "https://onomeo.com/v1", "https://onomeo.com/dashboard", "/models", 0, 0},
    {"opencode", "https://opencode.ai/zen/v1", "https://opencode.ai/auth", "/models", 0, 0},
    {"openrouter", "https://openrouter.ai/api/v1", "https://openrouter.ai/keys", "/models", 0, 0},
    {"orcarouter", "https://api.orcarouter.ai/v1", "https://www.orcarouter.ai", "/models", 0, 0},
    {"ovh", "https://oai.endpoints.kepler.ai.cloud.ovh.net/v1", "https://endpoints.ai.cloud.ovh.net", "/models", 0, 1},
    {"parallel", "https://api.parallel.ai/v1", "https://platform.parallel.ai", "-", 1, 0},
    {"pinecone", "https://api.pinecone.io", "https://app.pinecone.io", "-", 1, 0},
    {"plugsky", "https://plugsky.com/v1", "https://plugsky.com/dashboard", "/models", 0, 0},
    {"pollinations", "https://gen.pollinations.ai/v1", "https://enter.pollinations.ai", "/models", 0, 0},
    {"qianfan", "https://qianfan.baidubce.com/v2", "https://console.bce.baidu.com/qianfan/overview", "/models", 0, 0},
    {"radeon", "https://developer.amd.com.cn/radeon/api/v1", "https://developer.amd.com.cn/radeon/tokenfactory", "/models", 0, 0},
    {"reka", "https://api.reka.ai/v1", "https://platform.reka.ai", "/models", 0, 0},
    {"requesty", "https://router.requesty.ai/v1", "https://www.requesty.ai", "/models", 0, 0},
    {"router9", "https://api.router9.com/v1", "https://www.router9.com", "/models", 0, 0},
    {"routeway", "https://api.routeway.ai/v1", "https://routeway.ai", "/models", 0, 0},
    {"sail", "https://api.sailresearch.com/v1", "https://app.sailresearch.com", "-", 1, 0},
    {"sealion", "https://api.sea-lion.ai/v1", "https://sea-lion.ai", "/models", 0, 0},
    {"septor", "https://api.septorlabs.com/v1", "https://septorlabs.com/dashboard", "/models", 0, 0},
    {"serpsearch", "https://api.serpsearch.com/api/v1", "https://serpsearch.com", "-", 1, 0},
    {"siliconflow", "https://api.siliconflow.com/v1", "https://siliconflow.com", "/models", 0, 0},
    {"simplellm", "https://api.simplellm.eu/v1", "https://app.simplellm.eu", "/rate-limit", 0, 0},
    {"speechify", "https://api.speechify.ai/v1", "https://platform.speechify.ai", "/workspaces/current/entitlements", 1, 0},
    {"speka", "https://speka.me/v1", "https://speka.me/dashboard/keys", "/models", 0, 0},
    {"tavily", "https://api.tavily.com", "https://app.tavily.com", "/usage", 1, 0},
    {"typhoon", "https://api.opentyphoon.ai/v1", "https://playground.opentyphoon.ai", "/models", 0, 0},
    {"unorouter", "https://api.unorouter.com/v1", "https://unorouter.com", "/models", 0, 0},
    {"volcengine", "https://ark.cn-beijing.volces.com/api/v3", "https://console.volcengine.com/ark", "/models", 0, 0},
    {"waterfall", "https://api.getwaterfall.org/v1", "https://getwaterfall.org", "/models", 0, 0},
    {"xfyun", "https://spark-api-open.xf-yun.com/v1", "https://console.xfyun.cn", "/models", 0, 0},
    {"xkiro", "https://api.xkiro.com/v1", "https://xkiro.com", "/usage", 0, 0},
    {"zhipu", "https://api.z.ai/api/paas/v4", "https://z.ai/manage-apikey/apikey-list", "/models", 0, 0}
};
// clang-format on
static Provider registry[256];
static unsigned char registry_owned[256];
static int nregistry;
static void provider_defaults(void) {
    if (nregistry) return;
    memcpy(registry, providers, sizeof(providers));
    nregistry = (int)(sizeof(providers) / sizeof(*providers));
}
static const Provider *provider_find(const char *id) {
    provider_defaults();
    for (int i = 0; i < nregistry; i++)
        if (!strcmp(registry[i].id, id)) return &registry[i];
    return NULL;
}
static void provider_free(Provider *rows, unsigned char *owned, int count) {
    for (int i = 0; i < count; i++)
        if (owned[i]) {
            free((void *)rows[i].id);
            free((void *)rows[i].url);
            free((void *)rows[i].signup);
            free((void *)rows[i].probe);
        }
}
static int provider_load(Buf *data) {
    Json j = json(data->p, data->n);
    if (j.bad || !j.n || j.t[0].type != '{') {
        jfree(&j);
        return 0;
    }
    int arr = get(&j, 0, "providers"), ok = 0;
    Provider next[256] = {0};
    unsigned char owned[256] = {0};
    memcpy(next, providers, sizeof(providers));
    int count = (int)(sizeof(providers) / sizeof(*providers));
    if (arr < 0 || j.t[arr].type != '[' || !j.t[arr].n || j.t[arr].n > 256) goto done;
    for (int q = 0; q < j.t[arr].n; q++) {
        int row = at(&j, arr, q);
        char *id = string(&j, get(&j, row, "id")), *url = string(&j, get(&j, row, "url")),
             *signup = string(&j, get(&j, row, "signup"));
        int slot = 0;
        while (slot < count && strcmp(next[slot].id, id)) slot++;
        int nt = get(&j, row, "native"), an = get(&j, row, "anonymous"), hp = get(&j, row, "health_path");
        char *probe = hp >= 0 ? string(&j, hp) : slot < count && next[slot].probe ? strdup(next[slot].probe) : NULL;
        int valid = (nt < 0 || j.t[nt].type == 't' || j.t[nt].type == 'f') &&
                    (an < 0 || j.t[an].type == 't' || j.t[an].type == 'f') && (hp < 0 || j.t[hp].type == '"') &&
                    id[0] && strlen(id) < 96 && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(id) &&
                    !strncmp(url, "https://", 8) && strlen(url) < 2048 && !strpbrk(url, "\r\n ") &&
                    !strncmp(signup, "https://", 8) && strlen(signup) < 2048 && !strpbrk(signup, "\r\n ") &&
                    (!probe || (strlen(probe) < 256 && (!probe[0] || probe[0] == '/' || !strcmp(probe, "-")) &&
                                !strstr(probe, "://") && !strpbrk(probe, "\r\n"))) &&
                    slot < 256 && !owned[slot];
        if (!valid) {
            free(id);
            free(url);
            free(signup);
            free(probe);
            goto done;
        }
        if (slot == count) count++;
        next[slot] = (Provider){id, url, signup, probe, boolean(&j, nt, 0), boolean(&j, an, 0)};
        owned[slot] = 1;
    }
    provider_free(registry, registry_owned, nregistry);
    memcpy(registry, next, sizeof(next));
    memcpy(registry_owned, owned, sizeof(owned));
    nregistry = count;
    ok = 1;
done:
    if (!ok) provider_free(next, owned, count);
    jfree(&j);
    return ok;
}
#ifndef BROKIE_PROVIDERS_URL
#define BROKIE_PROVIDERS_URL "https://raw.githubusercontent.com/ykahalan/brokie/main/providers.json"
#endif
static void provider_refresh(int force) {
    provider_defaults();
    Buf cached = read_file("providers.json");
    if (cached.p) provider_load(&cached);
    drop(&cached);
    if (offline() || (!force && cache_age("providers.json") < 86400)) return;
    Key anonymous = {.anonymous = 1};
    Upstream up = {.timeout = 10000};
    if (fetch(&anonymous, BROKIE_PROVIDERS_URL, NULL, 0, &up) && up.status == 200 && up.body.p &&
        provider_load(&up.body)) {
        if (!private_directory() || !write_file("providers.json", up.body.p, up.body.n))
            fprintf(stderr, "brokie: could not cache provider directory\n");
    } else fprintf(stderr, "brokie: provider refresh failed; keeping cached/bundled defaults\n");
    drop(&up.body);
}
static const char *key_probe(Key *key) {
    if (key->health_path[0]) return key->health_path;
    const Provider *p = provider_find(key->provider);
    if (p && p->probe) return p->probe;
    return key->native ? "-" : "/models";
}
static void check_keys(int force) {
    if (offline()) return;
    for (int i = 0; i < nk; i++) {
        double started = now_s();
        LOCK();
        Key key = keys[i];
        UNLOCK();
        if (!key.enabled || (!force && started >= key.checked && started - key.checked < health_seconds)) {
            secure_zero(key.secret, sizeof(key.secret));
            continue;
        }
        int health = H_SKIP, http = 0;
        const char *probe = key_probe(&key);
        char url[4096];
        if (key.anonymous) health = H_ANONYMOUS;
        else if (probe[0] == '/' && !strstr(probe, "://") && !strpbrk(probe, "\r\n") &&
                 key_url(url, sizeof(url), &key, probe, probe[1] == '/')) {
            Upstream up = {.native = 1, .timeout = 5000};
            int ok = fetch(&key, url, NULL, 0, &up);
            http = (int)up.status;
            health = response_health(ok, http);
            drop(&up.body);
            if (ok && http >= 200 && http < 300) {
                Key anonymous = key;
                anonymous.anonymous = 1;
                secure_zero(anonymous.secret, sizeof(anonymous.secret));
                Upstream no_key = {.native = 1, .timeout = 5000};
                if (fetch(&anonymous, url, NULL, 0, &no_key) && (no_key.status == 401 || no_key.status == 403))
                    health = H_OK;
                drop(&no_key.body);
            }
        }
        secure_zero(key.secret, sizeof(key.secret));
        LOCK();
        if (keys[i].checked <= started) {
            if (keys[i].health == H_INVALID && health == H_OK)
                for (int m = 0; m < nm; m++) st(m, i)->cooldown = 0;
            set_health(&keys[i], health, http);
        }
        UNLOCK();
    }
    LOCK();
    save_health();
    UNLOCK();
}
static void print_health(void) {
    puts("KEY  PROVIDER  HEALTH  HTTP  CHECKED_AT_UTC_EPOCH");
    for (int i = 0; i < nk; i++)
        printf("%d  %s  %s  %d  %.0f\n", keys[i].id, keys[i].provider,
               keys[i].enabled ? health_names[keys[i].health] : "disabled", keys[i].http, keys[i].checked);
}
static void provider_list(void) {
    provider_refresh(0);
    Buf catalog = read_file("catalog.json"), cfg = read_file("config.json");
    Json cat = catalog.p ? json(catalog.p, catalog.n) : (Json){0};
    Json local = cfg.p ? json(cfg.p, cfg.n) : (Json){0};
    int platforms = catalog.p && !cat.bad ? get(&cat, 0, "platforms") : -1;
    int keys = cfg.p && !local.bad ? get(&local, 0, "keys") : -1;
    puts("PROVIDER         KEY       CATALOG  MODE     SIGN UP / GET KEY; API BASE");
    for (int i = 0; i < nregistry; i++) {
        const Provider *p = &registry[i];
        int configured = 0, synced = 0;
        for (int q = 0; keys >= 0 && q < local.t[keys].n; q++) {
            int row = at(&local, keys, q);
            configured |=
                eq(&local, get(&local, row, "provider"), p->id) && boolean(&local, get(&local, row, "enabled"), 1);
        }
        for (int q = 0; platforms >= 0 && q < cat.t[platforms].n; q++)
            synced |= eq(&cat, get(&cat, at(&cat, platforms, q), "id"), p->id);
        printf("%-16s %-9s %-8s %-8s %s\n    %s\n", p->id,
               configured     ? "added"
               : p->anonymous ? "optional"
                              : "needed",
               synced ? "synced" : "-", p->native ? "native" : "chat", p->signup, p->url);
    }
    for (int q = 0; platforms >= 0 && q < cat.t[platforms].n; q++) {
        int row = at(&cat, platforms, q);
        char id[96], name[160];
        fieldstr(&cat, row, "id", id, sizeof(id));
        fieldstr(&cat, row, "name", name, sizeof(name));
        if (!provider_find(id))
            printf("%-16s new catalog provider: %s; add its documented BASE_URL manually\n", id, name);
    }
    puts("Add: brokie key add PROVIDER [--anonymous]. Native: /native/PROVIDER/PATH. Details: PROVIDERS.md");
    jfree(&cat);
    jfree(&local);
    drop(&catalog);
    drop(&cfg);
}
static void usage_stats(void) {
    puts("KEY  PROVIDER/MODEL  TOKENS: MINUTE / DAY / MONTH");
    for (int m = 0; m < nm; m++)
        for (int k = 0; k < nk; k++) {
            Stats s = *st(m, k);
            if (!s.updated) continue;
            decay(&s, now_s());
            printf("%d  %s/%s  %.0f / %.0f / %.0f\n", keys[k].id, models[m].provider, models[m].name, s.min_tokens,
                   s.day_tokens, s.month_tokens);
        }
}
static void add_key(const char *provider, const char *base, int anonymous, int native) {
    if (nk >= MAX_KEYS) die("key limit reached");
    Key k = {0};
    k.id = 1;
    for (int i = 0; i < nk; i++)
        if (keys[i].id >= k.id) k.id = keys[i].id + 1;
    snprintf(k.url, sizeof(k.url), "%s", base);
    size_t len = strlen(k.url);
    while (len && k.url[len - 1] == '/') k.url[--len] = 0;
    if (!native && !strstr(k.url, "/chat/completions"))
        strncat(k.url, "/chat/completions", sizeof(k.url) - strlen(k.url) - 1);
    if (!safe_url(k.url)) die("use HTTPS, or HTTP on localhost for local models");
    if (!anonymous && !hidden_input("Provider API key (hidden): ", k.secret, sizeof(k.secret))) die("API key required");
    if (strpbrk(k.secret, "\r\n")) die("invalid key");
    write_vault_property(NULL, k.secret, k.id);
    secure_zero(k.secret, sizeof(k.secret));
    Buf row = {0};
    fmt(&row, "{\"id\":%d,\"provider\":", k.id);
    quote(&row, provider);
    put(&row, ",\"url\":");
    quote(&row, k.url);
    fmt(&row, ",\"enabled\":true,\"anonymous\":%s,\"native\":%s}", anonymous ? "true" : "false",
        native ? "true" : "false");
    append_config("keys", &row);
    drop(&row);
    printf("Added key %d for %s.\n", k.id, provider);
}
static void add_model(const char *provider, const char *name) {
    if (nm >= MAX_MODELS) die("model limit reached");
    int id = 1;
    for (int i = 0; i < nm; i++) {
        if (!strcmp(models[i].provider, provider) && !strcmp(models[i].name, name)) die("model already exists");
        if (models[i].id >= id) id = models[i].id + 1;
    }
    Buf row = {0};
    fmt(&row, "{\"id\":%d,\"provider\":", id);
    quote(&row, provider);
    put(&row, ",\"model\":");
    quote(&row, name);
    put(&row, ",\"enabled\":true,\"tools\":false,\"vision\":false,\"tier\":2,\"rank\":50,\"source\":\"user\"}");
    append_config("models", &row);
    drop(&row);
    puts("Added model. Edit its tools, vision, tier, rank and quota fields in config.json.");
}
static void list_routes(const char *mode) {
    if (!valid_mode(mode)) die("unknown mode");
    unsigned char *tried = mem((size_t)(nm ? nm : 1) * (nk ? nk : 1));
    puts("SCORE  PROVIDER/MODEL");
    for (int i = 0; i < nm; i++) {
        Route r;
        if (!choose(mode, NULL, 0, 0, 1, tried, 0, 0, &r)) break;
        printf("%.3f  %s/%s%s\n", r.score, models[r.m].provider, models[r.m].name, models[r.m].tools ? " [tools]" : "");
        for (int k = 0; k < nk; k++) tried[(size_t)r.m * nk + k] = 1;
    }
    free(tried);
}
static void launch_client(const char *client, const char *mode, int extra_count, char **extra) {
    if (!valid_mode(mode)) die("unknown mode");
    char base[128], model[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);
    snprintf(model, sizeof(model), "auto:%s", mode);
    char *args[128];
    int count = 0;
    args[count++] = (char *)client;
    if (!strcmp(client, "claude")) {
        setenv("ANTHROPIC_BASE_URL", base, 1);
        setenv("ANTHROPIC_AUTH_TOKEN", auth_token, 1);
        setenv("ANTHROPIC_MODEL", model, 1);
        setenv("ANTHROPIC_DEFAULT_HAIKU_MODEL", model, 1);
        setenv("ANTHROPIC_DEFAULT_SONNET_MODEL", model, 1);
        setenv("ANTHROPIC_DEFAULT_OPUS_MODEL", model, 1);
        setenv("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1", 1);
        args[count++] = "--model";
        args[count++] = model;
    } else {
        strncat(base, "/v1", sizeof(base) - strlen(base) - 1);
        setenv("OPENAI_BASE_URL", base, 1);
        setenv("OPENAI_API_KEY", auth_token, 1);
        setenv("OPENAI_MODEL", model, 1);
    }
    if (extra_count > 120) die("too many client arguments");
    for (int i = 0; i < extra_count; i++) args[count++] = extra[i];
    args[count] = NULL;
    execvp(client, args);
    die("could not launch client; ensure it is installed and the server is running");
}
#ifndef BROKIE_CATALOG_URL
#define BROKIE_CATALOG_URL "https://api.freellmapi.co/v1/latest"
#endif
#ifndef BROKIE_CATALOG_KEY
#define BROKIE_CATALOG_KEY                                                                                             \
    "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAq9yv4+3EeyMHKsfVYBhkcz1lYgIXSUeHNnN6tNgYX3k=\n-----END PUBLIC KEY-----\n"
#endif
static const char catalog_pubkey[] = BROKIE_CATALOG_KEY;
static int verify_catalog(Upstream *up) {
    unsigned char sig[192];
    int n = (int)strlen(up->signature);
    if (!n || n % 4 || n > 200) return 0;
    int bytes = EVP_DecodeBlock(sig, (unsigned char *)up->signature, n);
    while (n && up->signature[n - 1] == '=') {
        bytes--;
        n--;
    }
    if (bytes != 64) return 0;
    BIO *bio = BIO_new_mem_buf(catalog_pubkey, -1);
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = key && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
             EVP_DigestVerify(ctx, sig, (size_t)bytes, (unsigned char *)up->body.p, up->body.n) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    BIO_free(bio);
    return ok;
}
static void catalog_row(Buf *b, Json *remote, int r, Json *local, int old, int id) {
    int limits = get(remote, r, "limits");
    fmt(b, "{\"id\":%d,\"provider\":", id);
    raw(b, remote, get(remote, r, "platform"), "\"\"");
    put(b, ",\"model\":");
    raw(b, remote, get(remote, r, "modelId"), "\"\"");
    int enabled =
        boolean(remote, get(remote, r, "enabled"), 1) && boolean(local, old >= 0 ? get(local, old, "enabled") : -1, 1);
    fmt(b, ",\"enabled\":%s,\"source\":\"catalog\"", enabled ? "true" : "false");
    const char *dst[] = {"tools", "vision", "context", "rank", "rpm", "rpd", "tpm", "tpd"},
               *src[] = {
                   "supportsTools", "supportsVision", "contextWindow", "intelligenceRank", "rpm", "rpd", "tpm", "tpd"};
    for (int i = 0; i < 8; i++) {
        put(b, ",");
        quote(b, dst[i]);
        put(b, ":");
        int val = get(remote, i >= 4 ? limits : r, src[i]);
        int overrides = old >= 0 ? get(local, old, "overrides") : -1, keep = 0;
        for (int q = 0; overrides >= 0 && q < local->t[overrides].n; q++)
            if (eq(local, at(local, overrides, q), dst[i])) keep = 1;
        if (keep) raw(b, local, get(local, old, dst[i]), i < 2 ? "false" : "0");
        else raw(b, remote, val, i < 2 ? "false" : i == 3 ? "50" : "0");
    }
    int tier = get(remote, r, "sizeLabel"), tv = eq(remote, tier, "Frontier") ? 4
                                                 : eq(remote, tier, "Large")  ? 3
                                                 : eq(remote, tier, "Small")  ? 1
                                                                              : 2;
    fmt(b, ",\"tier\":%d", tv);
    char *budget_label = string(remote, get(remote, r, "monthlyTokenBudget"));
    char *start = budget_label;
    while (*start && !isdigit((unsigned char)*start)) start++;
    char *end = start;
    double budget = strtod(start, &end);
    while (*end == ' ') end++;
    int suffix = toupper((unsigned char)*end);
    budget *= suffix == 'B' ? 1e9 : suffix == 'M' ? 1e6 : suffix == 'K' ? 1e3 : 1;
    fmt(b, ",\"budget\":%.9g", budget);
    free(budget_label);
    if (old >= 0) {
        int o = get(local, old, "overrides");
        if (o >= 0) {
            put(b, ",\"overrides\":");
            raw(b, local, o, "[]");
        }
    }
    put(b, "}");
}
/* Stage and validate a complete update before changing files or live routing state. */
static int sync_catalog(int live) {
    if (offline()) return 0;
    Key key = {.anonymous = !catalog_license[0]};
    snprintf(key.secret, sizeof(key.secret), "%s", catalog_license);
    Upstream up = {.timeout = 10000};
    int fetched = fetch(&key, BROKIE_CATALOG_URL, NULL, 0, &up);
    secure_zero(key.secret, sizeof(key.secret));
    Buf current = {0}, rows = {0}, next = {0}, version_json = {0};
    Json remote = {0}, local = {0};
    Model *next_models = NULL;
    Stats *next_stats = NULL;
    int ok = 0, count = 0, next_id = 1, gated = 0;
    if (!fetched || up.status != 200 || !up.body.p || !verify_catalog(&up)) goto done;
    remote = json(up.body.p, up.body.n);
    current = read_file("config.json");
    local = json(current.p, current.n);
    next_models = mem(MAX_MODELS * sizeof(Model));
    if (remote.bad || !remote.n || remote.t[0].type != '{' || !parse_models(&local, next_models, &count)) goto done;
    for (int i = 0; i < count; i++)
        if (next_models[i].id >= next_id) {
            if (next_models[i].id >= 2147483647) goto done;
            next_id = next_models[i].id + 1;
        }
    int arr = get(&remote, 0, "models"), oldarr = get(&local, 0, "models");
    char version[128], applied[128];
    fieldstr(&remote, 0, "version", version, sizeof(version));
    fieldstr(&local, 0, "catalog_version", applied, sizeof(applied));
    if (arr < 0 || remote.t[arr].type != '[' || !version[0] || (applied[0] && strcmp(version, applied) < 0)) goto done;
    put(&rows, "[");
    count = 0;
    for (int i = 0; i < remote.t[arr].n; i++) {
        int r = at(&remote, arr, i), modality = get(&remote, r, "modality");
        if (modality >= 0 && !eq(&remote, modality, "text")) continue;
        char provider[96], name[320];
        fieldstr(&remote, r, "platform", provider, sizeof(provider));
        fieldstr(&remote, r, "modelId", name, sizeof(name));
        if (!provider[0] || !name[0]) goto done;
        double enabled = fieldnum(&remote, r, "enabled", 0);
        if (!isfinite(enabled) || fabs(enabled) > 2147483647.) goto done;
        int old = -1, id = next_id;
        for (int q = 0; oldarr >= 0 && q < local.t[oldarr].n; q++) {
            int row = at(&local, oldarr, q);
            if (eq(&local, get(&local, row, "provider"), provider) && eq(&local, get(&local, row, "model"), name)) {
                old = row;
                id = (int)fieldnum(&local, row, "id", id);
                break;
            }
        }
        if (old >= 0 && !eq(&local, get(&local, old, "source"), "catalog")) continue;
        if (old < 0) {
            if (next_id >= 2147483647) goto done;
            next_id++;
        }
        if (count++) put(&rows, ",");
        catalog_row(&rows, &remote, r, &local, old, id);
    }
    for (int q = 0; oldarr >= 0 && q < local.t[oldarr].n; q++) {
        int row = at(&local, oldarr, q);
        if (!eq(&local, get(&local, row, "source"), "catalog")) {
            if (count++) put(&rows, ",");
            raw(&rows, &local, row, "{}");
        }
    }
    if (count > MAX_MODELS) goto done;
    put(&rows, "]");
    Buf updated = replace_property(&current, "models", &rows);
    quote(&version_json, version);
    next = replace_property(&updated, "catalog_version", &version_json);
    drop(&updated);
    Json parsed = json(next.p, next.n);
    int valid = parse_models(&parsed, next_models, &count);
    jfree(&parsed);
    if (!valid) goto done;
    if (live) {
        next_stats = mem((size_t)(nk ? nk : 1) * (count ? count : 1) * sizeof(Stats));
        LOCK();
        gated = updating = 1;
        while (active_clients) pthread_cond_wait(&idle, &mutex);
    }
    Buf latest = read_file("config.json");
    int unchanged = latest.n == current.n && latest.p && !memcmp(latest.p, current.p, current.n);
    drop(&latest);
    if (!unchanged || !write_file("config.json", next.p, next.n)) goto done;
    if (live) {
        for (int m = 0; m < count; m++) {
            int old = model_index(next_models[m].id);
            if (old >= 0) memcpy(next_stats + (size_t)m * nk, st(old, 0), (size_t)nk * sizeof(Stats));
        }
        for (int q = 0; q < 128; q++)
            if (sticky[q].hash) {
                int found = -1;
                for (int m = 0; m < count; m++)
                    if (models[sticky[q].model].id == next_models[m].id) found = m;
                if (found < 0) sticky[q].hash = 0;
                else sticky[q].model = found;
            }
        memcpy(models, next_models, (size_t)count * sizeof(Model));
        nm = count;
        free(stats);
        stats = next_stats;
        next_stats = NULL;
        drop(&config);
        config = next;
        memset(&next, 0, sizeof(next));
    }
    if (!write_file("catalog.json", up.body.p, up.body.n)) fprintf(stderr, "brokie: catalog cache write failed\n");
    fprintf(stderr, "Applied signed catalog %s (%d chat models)%s.\n", version, count, live ? " to live router" : "");
    ok = 1;
done:
    if (gated) {
        updating = 0;
        pthread_cond_broadcast(&idle);
        UNLOCK();
    }
    if (!ok) fprintf(stderr, "brokie: catalog refresh failed; keeping existing routing data\n");
    free(next_models);
    free(next_stats);
    jfree(&remote);
    jfree(&local);
    drop(&current);
    drop(&rows);
    drop(&next);
    drop(&version_json);
    drop(&up.body);
    return ok;
}
static void *automatic_refresh(void *unused) {
    (void)unused;
    double next_catalog = 0, next_providers = 0, next_health = 0;
    for (;;) {
        double now = now_s();
        if (now >= next_providers) {
            provider_refresh(0);
            next_providers = now_s() + 3600;
        }
        if (refresh_seconds && now >= next_catalog) {
            int ok = sync_catalog(1);
            next_catalog = now_s() + (ok ? refresh_seconds : fmin(300, refresh_seconds));
        }
        if (health_seconds && now_s() >= next_health) {
            check_keys(0);
            next_health = now_s() + fmin(60, health_seconds);
        }
        sleep(1);
    }
    return NULL;
}
int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    snprintf(directory, sizeof(directory), "%s/.brokie", home ? home : ".");
    int p = 1;
    if (argc > 3 && !strcmp(argv[p], "--dir")) {
        snprintf(directory, sizeof(directory), "%s", argv[p + 1]);
        p += 2;
    }
    if (p >= argc) {
        puts(
            "brokie [--dir DIR] init | providers | stats | health | check | serve | list [MODE] | mode MODE | key add PROVIDER [BASE_URL] "
            "[--anonymous] [--native] | model add PROVIDER MODEL | sync | license | claude [MODE] "
            "| run CLIENT [MODE]");
        return 0;
    }
    const char *cmd = argv[p++];
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!strcmp(cmd, "init")) {
        initialize();
        return 0;
    }
    if (!strcmp(cmd, "providers")) {
        provider_list();
        return 0;
    }
    load_config(strcmp(cmd, "mode") && strcmp(cmd, "model") && strcmp(cmd, "stats") && strcmp(cmd, "health"));
    if (!strcmp(cmd, "serve")) serve();
    else if (!strcmp(cmd, "stats")) usage_stats();
    else if (!strcmp(cmd, "health")) print_health();
    else if (!strcmp(cmd, "check")) {
        provider_refresh(0);
        check_keys(1);
        print_health();
    } else if (!strcmp(cmd, "list")) list_routes(p < argc ? argv[p] : default_mode);
    else if (!strcmp(cmd, "mode")) {
        if (p >= argc || !valid_mode(argv[p])) die("mode must be smartest, fastest, reliable or balanced");
        Buf b = {0};
        quote(&b, argv[p]);
        replace_config("mode", &b);
        drop(&b);
        puts("Default mode saved; restart the server to apply it.");
    } else if (!strcmp(cmd, "key") && p + 1 < argc && !strcmp(argv[p], "add")) {
        provider_refresh(0);
        const Provider *preset = provider_find(argv[p + 1]);
        int next = p + 2, anonymous = 0, native = preset && preset->native;
        const char *base = next < argc && strncmp(argv[next], "--", 2) ? argv[next++] : preset ? preset->url : NULL;
        if (!base) die("unknown provider; supply its API BASE_URL");
        for (int i = next; i < argc; i++) {
            if (!strcmp(argv[i], "--anonymous")) anonymous = 1;
            else if (!strcmp(argv[i], "--native")) native = 1;
            else die("unknown key option");
        }
        char expanded[2048], account[96];
        const char *marker = strstr(base, "YOUR_ACCOUNT_ID");
        if (marker) {
            if (!hidden_input("Cloudflare account ID (from dashboard): ", account, sizeof(account)) ||
                strspn(account, "0123456789abcdefABCDEF") != strlen(account))
                die("invalid Cloudflare account ID");
            snprintf(expanded, sizeof(expanded), "%.*s%s%s", (int)(marker - base), base, account, marker + 15);
            base = expanded;
        }
        add_key(argv[p + 1], base, anonymous, native);
    } else if (!strcmp(cmd, "model") && p + 2 < argc && !strcmp(argv[p], "add")) add_model(argv[p + 1], argv[p + 2]);
    else if (!strcmp(cmd, "sync")) {
        provider_refresh(1);
        if (!sync_catalog(0)) die("catalog sync failed");
    } else if (!strcmp(cmd, "license")) {
        char value[4096];
        if (!hidden_input("Catalog license (hidden): ", value, sizeof(value))) die("license required");
        write_vault_property("catalog_license", value, 0);
        secure_zero(value, sizeof(value));
        puts("Catalog license saved.");
    } else if (!strcmp(cmd, "claude"))
        launch_client("claude", p < argc ? argv[p] : default_mode, p < argc ? argc - p - 1 : 0, argv + p + 1);
    else if (!strcmp(cmd, "run") && p < argc)
        launch_client(argv[p], p + 1 < argc ? argv[p + 1] : default_mode, p + 2 < argc ? argc - p - 2 : 0,
                      argv + p + 2);
    else die("unknown or incomplete command; run brokie without arguments for usage");
    secure_zero(keys, sizeof(keys));
    secure_zero(auth_token, sizeof(auth_token));
    secure_zero(password, sizeof(password));
    secure_zero(catalog_license, sizeof(catalog_license));
    free(stats);
    drop(&config);
    curl_global_cleanup();
    return 0;
}
