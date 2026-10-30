/*
 * minidb - a small SQL database engine with a paged B+tree storage layer.
 *
 * Usage:  echo "SELECT * FROM t;" | ./minidb data.db
 *
 * Architecture
 *   pager     4096-byte pages in one file, lazy page cache, free-list,
 *             dirty pages flushed (header last) when the process exits.
 *   B+tree    one tree per table keyed by an auto-increment rowid; the root
 *             page number never changes (a root split moves the old root
 *             into a fresh page).  The catalog is itself a B+tree.
 *   records   tagged, variable-length rows (NULL / INTEGER / REAL / TEXT).
 *   SQL       lexer -> recursive-descent parser -> expression trees that are
 *             bound to table columns, then a pull-style executor
 *             (scan / hash join / hash aggregation / sort / limit).
 *
 * Supported SQL
 *   CREATE TABLE [IF NOT EXISTS], DROP TABLE [IF EXISTS],
 *   INSERT INTO t [(cols)] VALUES (...)[, (...)],
 *   UPDATE t SET c = expr[, ...] [WHERE ...], DELETE FROM t [WHERE ...],
 *   SELECT exprs FROM t [alias] [INNER] JOIN u [alias] ON expr
 *          [WHERE] [GROUP BY] [HAVING] [ORDER BY ... ASC|DESC] [LIMIT n [OFFSET m]]
 *   expressions: + - * / %, comparisons, AND/OR/NOT (three-valued logic),
 *          IS [NOT] NULL, [NOT] BETWEEN, [NOT] IN (...), [NOT] LIKE,
 *          COUNT(*), COUNT/SUM/AVG/MIN/MAX(expr)
 *   dot commands: .tables  .schema [table]
 *
 * Known limits (deliberate, documented in the README)
 *   - rows are limited to MAX_REC bytes; no overflow pages
 *   - deletes never merge or rebalance pages (space is reused by later inserts)
 *   - no journal: a crash while the process flushes can corrupt the file
 *   - a statement that fails stops only itself; the exit code becomes 1
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>
#include <errno.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define HAVE_FSYNC 1
#endif

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define NORETURN
#define PRINTF_FMT(a, b)
#endif

/* ======================================================================
 * Errors and memory
 * ==================================================================== */
static jmp_buf err_jmp;
static char err_msg[512];

static void fail(const char *fmt, ...) NORETURN PRINTF_FMT(1, 2);
static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_msg, sizeof err_msg, fmt, ap);
    va_end(ap);
    longjmp(err_jmp, 1);
}

/* Per-statement arena: everything allocated here is released after each
 * statement, so the parser and executor never free individual objects. */
#define ARENA_BLOCK (256 * 1024)
typedef struct ABlock {
    struct ABlock *next;
    size_t used, cap;
    char data[];
} ABlock;
static ABlock *arena_head;

static void *amalloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!arena_head || arena_head->used + n > arena_head->cap) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        ABlock *b = malloc(sizeof(ABlock) + cap);
        if (!b) fail("out of memory");
        b->next = arena_head;
        b->used = 0;
        b->cap = cap;
        arena_head = b;
    }
    void *p = arena_head->data + arena_head->used;
    arena_head->used += n;
    return p;
}

static void *acalloc(size_t n) {
    void *p = amalloc(n);
    memset(p, 0, n);
    return p;
}

static void arena_reset(void) {
    /* keep the newest block (so small statements never hit malloc) */
    while (arena_head && arena_head->next) {
        ABlock *n = arena_head->next;
        arena_head->next = n->next;
        free(n);
    }
    if (arena_head) arena_head->used = 0;
}

static char *astrndup(const char *s, size_t n) {
    char *p = amalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int ieq_n(const char *s, size_t n, const char *kw) {
    size_t i;
    if (strlen(kw) != n) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)kw[i])) return 0;
    return 1;
}
static int ieq(const char *a, const char *b) { return ieq_n(a, strlen(a), b); }

/* ======================================================================
 * Values
 * ==================================================================== */
enum { V_NULL = 0, V_INT = 1, V_REAL = 2, V_TEXT = 3 };

typedef struct {
    uint8_t t;
    union {
        int64_t i;
        double r;
        struct { const char *p; uint32_t n; } s;
    } u;
} Value;

static Value v_null(void) { Value v; memset(&v, 0, sizeof v); return v; }
static Value v_int(int64_t i) { Value v = v_null(); v.t = V_INT; v.u.i = i; return v; }
static Value v_real(double r) { Value v = v_null(); v.t = V_REAL; v.u.r = r; return v; }
static Value v_text(const char *p, size_t n) {
    Value v = v_null();
    v.t = V_TEXT;
    v.u.s.p = p;
    v.u.s.n = (uint32_t)n;
    return v;
}

static int vrank(const Value *v) { return v->t == V_NULL ? 0 : v->t == V_TEXT ? 2 : 1; }
static double vnum(const Value *v) { return v->t == V_INT ? (double)v->u.i : v->u.r; }

/* Total order: NULL < numbers < text.  Used by ORDER BY, MIN/MAX, joins. */
static int value_cmp(const Value *a, const Value *b) {
    int ra = vrank(a), rb = vrank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (ra == 0) return 0;
    if (ra == 1) {
        if (a->t == V_INT && b->t == V_INT)
            return (a->u.i > b->u.i) - (a->u.i < b->u.i);
        double x = vnum(a), y = vnum(b);
        return (x > y) - (x < y);
    } else {
        uint32_t n = a->u.s.n < b->u.s.n ? a->u.s.n : b->u.s.n;
        int c = n ? memcmp(a->u.s.p, b->u.s.p, n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return (a->u.s.n > b->u.s.n) - (a->u.s.n < b->u.s.n);
    }
}

static int truthy(Value v) {
    if (v.t == V_INT) return v.u.i != 0;
    if (v.t == V_REAL) return v.u.r != 0.0;
    return 0;
}

static uint64_t fnv1a(const void *data, size_t n, uint64_t h) {
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t hash_value(const Value *v) {
    uint64_t h = 1469598103934665603ULL;
    if (v->t == V_INT) {
        int64_t i = v->u.i;
        return fnv1a(&i, sizeof i, h);
    }
    if (v->t == V_REAL) {
        double r = v->u.r;
        if (r == floor(r) && fabs(r) < 9.0e18) { /* hash equal numbers equally */
            int64_t i = (int64_t)r;
            return fnv1a(&i, sizeof i, h);
        }
        return fnv1a(&r, sizeof r, h);
    }
    if (v->t == V_TEXT) return fnv1a(v->u.s.p, v->u.s.n, h ^ 0x9e3779b97f4a7c15ULL);
    return h;
}

/* Shortest decimal text that round-trips; always shows a '.' for integral
 * values so REAL columns are visibly REAL ("100.0"). */
static void fmt_real(double r, char *buf, size_t cap) {
    int prec;
    if (isnan(r)) { snprintf(buf, cap, "NaN"); return; }
    if (isinf(r)) { snprintf(buf, cap, r < 0 ? "-Inf" : "Inf"); return; }
    for (prec = 15; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, r);
        if (strtod(buf, NULL) == r) break;
    }
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        size_t n = strlen(buf);
        if (n + 3 < cap) { buf[n] = '.'; buf[n + 1] = '0'; buf[n + 2] = '\0'; }
    }
}

/* ======================================================================
 * Pager: 4096-byte pages in a single file
 * ==================================================================== */
#define PAGE_SIZE 4096
#define DB_MAGIC "MINIDB01"

typedef struct {
    FILE *f;
    uint32_t npages;       /* including page 0 (the file header) */
    uint32_t free_head;    /* head of the free-page list, 0 = none */
    uint32_t catalog_root;
    uint8_t **cache;       /* lazily loaded pages, never evicted */
    uint8_t *dirty;
    uint32_t cache_cap;
    int modified;
} Pager;
static Pager pg;

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void cache_reserve(uint32_t n) {
    if (n <= pg.cache_cap) return;
    uint32_t cap = pg.cache_cap ? pg.cache_cap : 64;
    while (cap < n) cap *= 2;
    uint8_t **c = realloc(pg.cache, sizeof(uint8_t *) * cap);
    if (!c) fail("out of memory");
    pg.cache = c;
    uint8_t *d = realloc(pg.dirty, cap);
    if (!d) fail("out of memory");
    pg.dirty = d;
    memset(pg.cache + pg.cache_cap, 0, sizeof(uint8_t *) * (cap - pg.cache_cap));
    memset(pg.dirty + pg.cache_cap, 0, cap - pg.cache_cap);
    pg.cache_cap = cap;
}

static uint8_t *page_get(uint32_t no) {
    if (no == 0 || no >= pg.npages) fail("corrupt database: bad page number %u", no);
    if (!pg.cache[no]) {
        uint8_t *b = malloc(PAGE_SIZE);
        if (!b) fail("out of memory");
        if (fseek(pg.f, (long)no * PAGE_SIZE, SEEK_SET) != 0 ||
            fread(b, 1, PAGE_SIZE, pg.f) != PAGE_SIZE) {
            free(b);
            fail("read error on page %u", no);
        }
        pg.cache[no] = b;
    }
    return pg.cache[no];
}

static void page_dirty(uint32_t no) { pg.dirty[no] = 1; pg.modified = 1; }

static uint32_t page_alloc(void) {
    uint32_t no;
    if (pg.free_head) {
        no = pg.free_head;
        uint8_t *p = page_get(no);
        pg.free_head = rd32(p + 4);
        memset(p, 0, PAGE_SIZE);
    } else {
        no = pg.npages++;
        cache_reserve(pg.npages);
        pg.cache[no] = calloc(1, PAGE_SIZE);
        if (!pg.cache[no]) fail("out of memory");
    }
    page_dirty(no);
    return no;
}

static void page_free(uint32_t no) {
    uint8_t *p = page_get(no);
    memset(p, 0, PAGE_SIZE);
    p[0] = 3; /* NT_FREE */
    wr32(p + 4, pg.free_head);
    pg.free_head = no;
    page_dirty(no);
}

static void pager_flush(void) {
    uint32_t no;
    if (!pg.f || !pg.modified) return;
    for (no = 1; no < pg.npages; no++) {
        if (pg.dirty[no] && pg.cache[no]) {
            if (fseek(pg.f, (long)no * PAGE_SIZE, SEEK_SET) != 0 ||
                fwrite(pg.cache[no], 1, PAGE_SIZE, pg.f) != PAGE_SIZE)
                fail("write error on page %u", no);
            pg.dirty[no] = 0;
        }
    }
    /* header goes last so a torn flush never points at unwritten pages */
    uint8_t h[PAGE_SIZE];
    memset(h, 0, sizeof h);
    memcpy(h, DB_MAGIC, 8);
    wr32(h + 8, PAGE_SIZE);
    wr32(h + 12, pg.npages);
    wr32(h + 16, pg.free_head);
    wr32(h + 20, pg.catalog_root);
    if (fseek(pg.f, 0, SEEK_SET) != 0 || fwrite(h, 1, PAGE_SIZE, pg.f) != PAGE_SIZE)
        fail("write error on header");
    if (fflush(pg.f) != 0) fail("flush error");
#ifdef HAVE_FSYNC
    fsync(fileno(pg.f));
#endif
    pg.modified = 0;
}

/* B+tree node layout (see below); declared here for catalog bootstrap */
#define HDR 24
#define OFF_TYPE 0
#define OFF_NCELLS 2
#define OFF_NEXT 4
#define OFF_NKEY 8
#define OFF_START 16
#define OFF_USED 18
#define NT_LEAF 1
#define NT_INT 2
#define NT_FREE 3
#define MAX_REC 1900

static void node_init(uint8_t *p, uint8_t type) {
    memset(p, 0, PAGE_SIZE);
    p[OFF_TYPE] = type;
    wr16(p + OFF_START, PAGE_SIZE);
}

static void pager_open(const char *path) {
    pg.f = fopen(path, "r+b");
    if (!pg.f) pg.f = fopen(path, "w+b");
    if (!pg.f) fail("cannot open database file '%s': %s", path, strerror(errno));
    if (fseek(pg.f, 0, SEEK_END) != 0) fail("cannot seek in '%s'", path);
    long size = ftell(pg.f);
    if (size < 0) fail("cannot determine size of '%s'", path);
    if (size == 0) { /* brand-new database: header + empty catalog tree */
        pg.npages = 2;
        pg.catalog_root = 1;
        cache_reserve(pg.npages);
        pg.cache[1] = calloc(1, PAGE_SIZE);
        if (!pg.cache[1]) fail("out of memory");
        node_init(pg.cache[1], NT_LEAF);
        wr64(pg.cache[1] + OFF_NKEY, 1);
        pg.dirty[1] = 1;
        pg.modified = 1;
        return;
    }
    uint8_t h[PAGE_SIZE];
    if (size < PAGE_SIZE || fseek(pg.f, 0, SEEK_SET) != 0 ||
        fread(h, 1, PAGE_SIZE, pg.f) != PAGE_SIZE || memcmp(h, DB_MAGIC, 8) != 0)
        fail("'%s' is not a minidb database file", path);
    if (rd32(h + 8) != PAGE_SIZE) fail("unsupported page size %u", rd32(h + 8));
    pg.npages = rd32(h + 12);
    pg.free_head = rd32(h + 16);
    pg.catalog_root = rd32(h + 20);
    if (pg.npages < 2 || (long)pg.npages * PAGE_SIZE > size)
        fail("database file is truncated or corrupt");
    cache_reserve(pg.npages);
}

/* ======================================================================
 * B+tree
 *
 * Node: 24-byte header, a u16 cell-pointer array growing up, cells growing
 * down from the end of the page.  Leaf cell: key(8) len(2) payload.
 * Interior cell: key(8) child(4); the child holds keys <= key, and the
 * header's "next" field is the rightmost child (keys > the last key).
 * Leaves are chained through the same field.  The root's header also keeps
 * the next unused rowid.
 * ==================================================================== */
static inline int ncells(const uint8_t *p) { return rd16(p + OFF_NCELLS); }
static inline uint8_t *cell_at(uint8_t *p, int i) { return p + rd16(p + HDR + 2 * i); }
static inline int64_t cell_key(const uint8_t *c) { return (int64_t)rd64(c); }
static inline int cell_size_of(const uint8_t *p, const uint8_t *c) {
    return p[OFF_TYPE] == NT_LEAF ? 10 + rd16(c + 8) : 12;
}
static inline int free_total(const uint8_t *p) {
    return PAGE_SIZE - HDR - 2 * ncells(p) - rd16(p + OFF_USED);
}

static int find_slot(uint8_t *p, int64_t key) { /* first cell with key >= key */
    int lo = 0, hi = ncells(p);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cell_key(cell_at(p, mid)) < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void defrag(uint8_t *p) {
    uint8_t tmp[PAGE_SIZE];
    int i, n = ncells(p), start = PAGE_SIZE;
    memcpy(tmp, p, PAGE_SIZE);
    for (i = 0; i < n; i++) {
        uint8_t *c = cell_at(tmp, i);
        int sz = cell_size_of(tmp, c);
        start -= sz;
        memcpy(p + start, c, sz);
        wr16(p + HDR + 2 * i, (uint16_t)start);
    }
    wr16(p + OFF_START, (uint16_t)start);
}

/* precondition: free_total(p) >= size + 2 */
static void page_insert_cell(uint8_t *p, int idx, const uint8_t *cell, int size) {
    int n = ncells(p);
    int start = rd16(p + OFF_START);
    if (start - (HDR + 2 * n) < size + 2) {
        defrag(p);
        start = rd16(p + OFF_START);
    }
    start -= size;
    memcpy(p + start, cell, size);
    uint8_t *ptrs = p + HDR;
    memmove(ptrs + 2 * (idx + 1), ptrs + 2 * idx, 2 * (size_t)(n - idx));
    wr16(ptrs + 2 * idx, (uint16_t)start);
    wr16(p + OFF_NCELLS, (uint16_t)(n + 1));
    wr16(p + OFF_START, (uint16_t)start);
    wr16(p + OFF_USED, (uint16_t)(rd16(p + OFF_USED) + size));
}

static void page_remove_cell(uint8_t *p, int idx) {
    int n = ncells(p);
    int sz = cell_size_of(p, cell_at(p, idx));
    uint8_t *ptrs = p + HDR;
    memmove(ptrs + 2 * idx, ptrs + 2 * (idx + 1), 2 * (size_t)(n - idx - 1));
    wr16(p + OFF_NCELLS, (uint16_t)(n - 1));
    wr16(p + OFF_USED, (uint16_t)(rd16(p + OFF_USED) - sz));
}

typedef struct { int split; int64_t sep; uint32_t right; } Split;
#define MAXC 512

static void leaf_split(uint32_t pageno, uint8_t *p, int idx, const uint8_t *newcell,
                       int newsize, Split *out) {
    uint8_t tmp[PAGE_SIZE];
    const uint8_t *cells[MAXC];
    int sizes[MAXC];
    int i, n, m, split_at;
    (void)pageno;
    memcpy(tmp, p, PAGE_SIZE);
    n = ncells(tmp);
    m = n + 1;
    uint32_t oldnext = rd32(tmp + OFF_NEXT);
    uint64_t nkey = rd64(tmp + OFF_NKEY);
    for (i = 0; i < m; i++) {
        if (i < idx) { cells[i] = cell_at(tmp, i); }
        else if (i == idx) { cells[i] = newcell; sizes[i] = newsize; continue; }
        else { cells[i] = cell_at(tmp, i - 1); }
        sizes[i] = cell_size_of(tmp, cells[i]);
    }
    if (idx == n) {
        split_at = n; /* sequential append: new cell alone on the right (full pages) */
    } else {
        int total = 0, acc = 0;
        for (i = 0; i < m; i++) total += sizes[i] + 2;
        split_at = m - 1;
        for (i = 0; i < m; i++) {
            acc += sizes[i] + 2;
            if (acc * 2 >= total) { split_at = i + 1; break; }
        }
        if (split_at > m - 1) split_at = m - 1;
        if (split_at < 1) split_at = 1;
    }
    uint32_t rno = page_alloc();
    uint8_t *r = page_get(rno);
    node_init(p, NT_LEAF);
    wr64(p + OFF_NKEY, nkey);
    wr32(p + OFF_NEXT, rno);
    for (i = 0; i < split_at; i++) page_insert_cell(p, i, cells[i], sizes[i]);
    node_init(r, NT_LEAF);
    wr32(r + OFF_NEXT, oldnext);
    for (i = split_at; i < m; i++) page_insert_cell(r, i - split_at, cells[i], sizes[i]);
    page_dirty(rno);
    out->split = 1;
    out->sep = cell_key(cells[split_at - 1]);
    out->right = rno;
}

static void interior_split(uint8_t *p, int idx, const uint8_t *newcell, Split *out) {
    uint8_t tmp[PAGE_SIZE];
    int64_t keys[MAXC];
    uint32_t childs[MAXC];
    int i, n, m, mid;
    memcpy(tmp, p, PAGE_SIZE);
    n = ncells(tmp);
    m = n + 1;
    uint32_t rightmost = rd32(tmp + OFF_NEXT);
    uint64_t nkey = rd64(tmp + OFF_NKEY);
    for (i = 0; i < m; i++) {
        const uint8_t *c;
        if (i < idx) c = cell_at(tmp, i);
        else if (i == idx) c = newcell;
        else c = cell_at(tmp, i - 1);
        keys[i] = cell_key(c);
        childs[i] = rd32(c + 8);
    }
    mid = m / 2;
    uint32_t rno = page_alloc();
    uint8_t *r = page_get(rno);
    node_init(p, NT_INT);
    wr64(p + OFF_NKEY, nkey);
    wr32(p + OFF_NEXT, childs[mid]);
    for (i = 0; i < mid; i++) {
        uint8_t c[12];
        wr64(c, (uint64_t)keys[i]);
        wr32(c + 8, childs[i]);
        page_insert_cell(p, i, c, 12);
    }
    node_init(r, NT_INT);
    wr32(r + OFF_NEXT, rightmost);
    for (i = mid + 1; i < m; i++) {
        uint8_t c[12];
        wr64(c, (uint64_t)keys[i]);
        wr32(c + 8, childs[i]);
        page_insert_cell(r, i - mid - 1, c, 12);
    }
    page_dirty(rno);
    out->split = 1;
    out->sep = keys[mid];
    out->right = rno;
}

static void node_insert(uint32_t pageno, int64_t key, const uint8_t *payload, int len,
                        Split *out) {
    uint8_t *p = page_get(pageno);
    int n = ncells(p);
    int idx = find_slot(p, key);
    out->split = 0;
    if (p[OFF_TYPE] == NT_LEAF) {
        uint8_t cell[10 + MAX_REC];
        int size = 10 + len;
        if (idx < n && cell_key(cell_at(p, idx)) == key) fail("internal error: duplicate rowid");
        wr64(cell, (uint64_t)key);
        wr16(cell + 8, (uint16_t)len);
        memcpy(cell + 10, payload, len);
        if (free_total(p) >= size + 2) {
            page_insert_cell(p, idx, cell, size);
            page_dirty(pageno);
        } else {
            page_dirty(pageno);
            leaf_split(pageno, p, idx, cell, size, out);
        }
    } else if (p[OFF_TYPE] == NT_INT) {
        uint32_t child = idx < n ? rd32(cell_at(p, idx) + 8) : rd32(p + OFF_NEXT);
        Split cs;
        node_insert(child, key, payload, len, &cs);
        if (!cs.split) return;
        /* `child` now holds keys <= cs.sep; cs.right took over the old range */
        uint8_t nc[12];
        wr64(nc, (uint64_t)cs.sep);
        wr32(nc + 8, child);
        if (idx < n) wr32(cell_at(p, idx) + 8, cs.right);
        else wr32(p + OFF_NEXT, cs.right);
        page_dirty(pageno);
        if (free_total(p) >= 12 + 2) page_insert_cell(p, idx, nc, 12);
        else interior_split(p, idx, nc, out);
    } else {
        fail("corrupt database: unexpected node type %d on page %u", p[OFF_TYPE], pageno);
    }
}

static void btree_insert(uint32_t root, int64_t key, const uint8_t *payload, size_t len) {
    Split s;
    if (len > MAX_REC) fail("row too large (%zu bytes, max %d)", len, MAX_REC);
    node_insert(root, key, payload, (int)len, &s);
    if (s.split) {
        /* keep the root page number stable: move its content to a new page */
        uint8_t *rp = page_get(root);
        uint64_t nk = rd64(rp + OFF_NKEY);
        uint32_t l = page_alloc();
        memcpy(page_get(l), rp, PAGE_SIZE);
        node_init(rp, NT_INT);
        wr64(rp + OFF_NKEY, nk);
        wr32(rp + OFF_NEXT, s.right);
        uint8_t nc[12];
        wr64(nc, (uint64_t)s.sep);
        wr32(nc + 8, l);
        page_insert_cell(rp, 0, nc, 12);
        page_dirty(root);
    }
}

static uint32_t btree_create(void) {
    uint32_t no = page_alloc();
    uint8_t *p = page_get(no);
    node_init(p, NT_LEAF);
    wr64(p + OFF_NKEY, 1);
    page_dirty(no);
    return no;
}

static int64_t btree_alloc_key(uint32_t root) {
    uint8_t *p = page_get(root);
    uint64_t k = rd64(p + OFF_NKEY);
    wr64(p + OFF_NKEY, k + 1);
    page_dirty(root);
    return (int64_t)k;
}

/* returns leaf page number and slot of `key`; slot == -1 when absent */
static uint32_t btree_find_leaf(uint32_t root, int64_t key, int *slot) {
    uint32_t no = root;
    for (;;) {
        uint8_t *p = page_get(no);
        int n = ncells(p), idx = find_slot(p, key);
        if (p[OFF_TYPE] == NT_LEAF) {
            *slot = (idx < n && cell_key(cell_at(p, idx)) == key) ? idx : -1;
            return no;
        }
        no = idx < n ? rd32(cell_at(p, idx) + 8) : rd32(p + OFF_NEXT);
    }
}

static int btree_delete(uint32_t root, int64_t key) {
    int slot;
    uint32_t no = btree_find_leaf(root, key, &slot);
    if (slot < 0) return 0;
    page_remove_cell(page_get(no), slot);
    page_dirty(no);
    return 1;
}

static void btree_update(uint32_t root, int64_t key, const uint8_t *payload, size_t len) {
    int slot;
    uint32_t no = btree_find_leaf(root, key, &slot);
    if (slot >= 0) {
        uint8_t *p = page_get(no);
        uint8_t *c = cell_at(p, slot);
        if (rd16(c + 8) == len) { /* same size: overwrite in place */
            memcpy(c + 10, payload, len);
            page_dirty(no);
            return;
        }
        page_remove_cell(p, slot);
        page_dirty(no);
    }
    btree_insert(root, key, payload, len);
}

static void btree_free(uint32_t no) {
    uint8_t *p = page_get(no);
    if (p[OFF_TYPE] == NT_INT) {
        int i, n = ncells(p);
        for (i = 0; i < n; i++) btree_free(rd32(cell_at(p, i) + 8));
        btree_free(rd32(p + OFF_NEXT));
    }
    page_free(no);
}

typedef struct { uint32_t page; int idx; } Cursor;

static void cur_first(Cursor *c, uint32_t root) {
    uint32_t no = root;
    for (;;) {
        uint8_t *p = page_get(no);
        if (p[OFF_TYPE] == NT_LEAF) { c->page = no; c->idx = 0; return; }
        no = ncells(p) ? rd32(cell_at(p, 0) + 8) : rd32(p + OFF_NEXT);
    }
}

static int cur_next(Cursor *c, int64_t *key, const uint8_t **payload, uint16_t *len) {
    while (c->page) {
        uint8_t *p = page_get(c->page);
        if (c->idx < ncells(p)) {
            uint8_t *cell = cell_at(p, c->idx++);
            *key = cell_key(cell);
            *len = rd16(cell + 8);
            *payload = cell + 10;
            return 1;
        }
        c->page = rd32(p + OFF_NEXT);
        c->idx = 0;
    }
    return 0;
}

/* ======================================================================
 * Records
 * ==================================================================== */
static size_t rec_size(const Value *v, int n) {
    size_t s = 1;
    int i;
    for (i = 0; i < n; i++) {
        switch (v[i].t) {
        case V_NULL: s += 1; break;
        case V_INT: case V_REAL: s += 9; break;
        default: s += 3 + v[i].u.s.n; break;
        }
    }
    return s;
}

static size_t rec_encode(const Value *v, int n, uint8_t *out) {
    size_t o = 0;
    int i;
    out[o++] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        out[o++] = v[i].t;
        if (v[i].t == V_INT) { wr64(out + o, (uint64_t)v[i].u.i); o += 8; }
        else if (v[i].t == V_REAL) { memcpy(out + o, &v[i].u.r, 8); o += 8; }
        else if (v[i].t == V_TEXT) {
            wr16(out + o, (uint16_t)v[i].u.s.n);
            o += 2;
            memcpy(out + o, v[i].u.s.p, v[i].u.s.n);
            o += v[i].u.s.n;
        }
    }
    return o;
}

/* Decodes without copying: text values point into the (never evicted) page. */
static void rec_decode(const uint8_t *p, size_t len, Value *out, int ncols) {
    size_t o = 1;
    int i, n;
    if (len < 1) fail("corrupt database: empty record");
    n = p[0];
    for (i = 0; i < n; i++) {
        Value v = v_null();
        if (o >= len) fail("corrupt database: truncated record");
        uint8_t tag = p[o++];
        if (tag == V_INT) {
            if (o + 8 > len) fail("corrupt database: truncated record");
            v = v_int((int64_t)rd64(p + o)); o += 8;
        } else if (tag == V_REAL) {
            double r;
            if (o + 8 > len) fail("corrupt database: truncated record");
            memcpy(&r, p + o, 8); o += 8;
            v = v_real(r);
        } else if (tag == V_TEXT) {
            if (o + 2 > len) fail("corrupt database: truncated record");
            uint16_t l = rd16(p + o); o += 2;
            if (o + l > len) fail("corrupt database: truncated record");
            v = v_text((const char *)p + o, l); o += l;
        } else if (tag != V_NULL) {
            fail("corrupt database: bad value tag %d", tag);
        }
        if (i < ncols) out[i] = v;
    }
    for (i = n; i < ncols; i++) out[i] = v_null();
}

/* ======================================================================
 * Catalog (a B+tree of (name TEXT, root INTEGER, schema TEXT))
 * ==================================================================== */
#define MAX_COLS 200
typedef enum { C_INT, C_REAL, C_TEXT } ColType;
typedef struct { char *name; ColType type; } ColDef;
typedef struct {
    char *name;
    int ncols;
    ColDef *cols;
    uint32_t root;
    int64_t ckey;
} Table;

static const char *type_name(ColType t) {
    return t == C_INT ? "INTEGER" : t == C_REAL ? "REAL" : "TEXT";
}

static Table *table_from_record(const Value *v, int64_t ckey) {
    Table *t = acalloc(sizeof *t);
    const char *s = v[2].u.s.p;
    uint32_t n = v[2].u.s.n, i = 0;
    int nc = 0;
    t->name = astrndup(v[0].u.s.p, v[0].u.s.n);
    t->root = (uint32_t)v[1].u.i;
    t->ckey = ckey;
    t->cols = acalloc(sizeof(ColDef) * MAX_COLS);
    /* schema text: name \x01 type-letter \x01 name \x01 type-letter ... */
    while (i < n && nc < MAX_COLS) {
        uint32_t j = i;
        while (j < n && s[j] != '\x01') j++;
        t->cols[nc].name = astrndup(s + i, j - i);
        i = j + 1;
        if (i >= n) fail("corrupt catalog entry for table %s", t->name);
        t->cols[nc].type = s[i] == 'I' ? C_INT : s[i] == 'R' ? C_REAL : C_TEXT;
        i += 2;
        nc++;
    }
    t->ncols = nc;
    return t;
}

static Table *table_find(const char *name) {
    Cursor c;
    int64_t key;
    const uint8_t *pl;
    uint16_t len;
    Value v[3];
    cur_first(&c, pg.catalog_root);
    while (cur_next(&c, &key, &pl, &len)) {
        rec_decode(pl, len, v, 3);
        if (v[0].t == V_TEXT && ieq_n(v[0].u.s.p, v[0].u.s.n, name))
            return table_from_record(v, key);
    }
    return NULL;
}

static Table *table_require(const char *name) {
    Table *t = table_find(name);
    if (!t) fail("no such table: %s", name);
    return t;
}

/* ======================================================================
 * Lexer
 * ==================================================================== */
typedef enum { TK_END, TK_IDENT, TK_INT, TK_REAL, TK_STR, TK_SYM } TokKind;
typedef struct {
    TokKind k;
    int quoted;
    const char *s;
    int n;
    int64_t i;
    double r;
    int start, end; /* byte offsets within the statement text */
} Token;

typedef struct {
    int dot;
    const char *dotline;
    Token *t;
    int n;
    const char *src;
} Stmt;

static const char *g_in, *g_end; /* input cursor: always advances */

static const char *skip_ws(const char *p) {
    for (;;) {
        while (p < g_end && isspace((unsigned char)*p)) p++;
        if (p + 1 < g_end && p[0] == '-' && p[1] == '-') {
            while (p < g_end && *p != '\n') p++;
            continue;
        }
        if (p + 1 < g_end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < g_end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 >= g_end) { g_in = g_end; fail("unterminated comment"); }
            p += 2;
            continue;
        }
        return p;
    }
}

static int is_idstart(int c) { return isalpha(c) || c == '_'; }
static int is_idchar(int c) { return isalnum(c) || c == '_'; }

static void tok_push(Token **toks, int *n, int *cap, const Token *t) {
    if (*n >= *cap) {
        Token *nt = amalloc(sizeof(Token) * (size_t)(*cap) * 2);
        memcpy(nt, *toks, sizeof(Token) * (size_t)(*n));
        *toks = nt;
        *cap *= 2;
    }
    (*toks)[(*n)++] = *t;
}

/* Reads the next statement (up to ';') or dot-command line.  Returns 0 at EOF. */
static int lex_statement(Stmt *st) {
    for (;;) {
        const char *p = skip_ws(g_in);
        if (p >= g_end) { g_in = p; return 0; }
        memset(st, 0, sizeof *st);
        if (*p == '.') {
            const char *s = p;
            while (p < g_end && *p != '\n') p++;
            while (p > s && isspace((unsigned char)p[-1])) p--;
            st->dot = 1;
            st->dotline = astrndup(s, (size_t)(p - s));
            while (p < g_end && *p != '\n') p++;
            g_in = p;
            return 1;
        }
        const char *src = p;
        int cap = 64, n = 0;
        Token *toks = amalloc(sizeof(Token) * (size_t)cap);
        for (;;) {
            p = skip_ws(p);
            if (p >= g_end) break;
            if (*p == ';') { p++; break; }
            Token tk;
            memset(&tk, 0, sizeof tk);
            tk.start = (int)(p - src);
            int c = (unsigned char)*p;
            if (is_idstart(c)) {
                const char *q = p;
                while (q < g_end && is_idchar((unsigned char)*q)) q++;
                tk.k = TK_IDENT; tk.s = p; tk.n = (int)(q - p);
                p = q;
            } else if (isdigit(c) || (c == '.' && p + 1 < g_end && isdigit((unsigned char)p[1]))) {
                const char *q = p;
                int isreal = 0;
                char buf[64];
                while (q < g_end && isdigit((unsigned char)*q)) q++;
                if (q < g_end && *q == '.') {
                    isreal = 1; q++;
                    while (q < g_end && isdigit((unsigned char)*q)) q++;
                }
                if (q < g_end && (*q == 'e' || *q == 'E')) {
                    const char *r = q + 1;
                    if (r < g_end && (*r == '+' || *r == '-')) r++;
                    if (r < g_end && isdigit((unsigned char)*r)) {
                        isreal = 1; q = r;
                        while (q < g_end && isdigit((unsigned char)*q)) q++;
                    }
                }
                if (q - p >= (long)sizeof buf) { g_in = g_end; fail("numeric literal too long"); }
                memcpy(buf, p, (size_t)(q - p));
                buf[q - p] = '\0';
                if (!isreal) {
                    errno = 0;
                    long long v = strtoll(buf, NULL, 10);
                    if (errno == ERANGE) isreal = 1;
                    else { tk.k = TK_INT; tk.i = v; }
                }
                if (isreal) { tk.k = TK_REAL; tk.r = strtod(buf, NULL); }
                tk.n = (int)(q - p);
                p = q;
            } else if (c == '\'') {
                const char *r = p + 1;
                size_t len = 0;
                for (;;) {
                    if (r >= g_end) { g_in = g_end; fail("unterminated string literal"); }
                    if (*r == '\'') {
                        if (r + 1 < g_end && r[1] == '\'') { len++; r += 2; continue; }
                        break;
                    }
                    len++; r++;
                }
                char *buf = amalloc(len + 1);
                size_t o = 0;
                for (r = p + 1; ; ) {
                    if (*r == '\'') {
                        if (r + 1 < g_end && r[1] == '\'') { buf[o++] = '\''; r += 2; continue; }
                        break;
                    }
                    buf[o++] = *r++;
                }
                buf[o] = '\0';
                tk.k = TK_STR; tk.s = buf; tk.n = (int)len;
                p = r + 1;
            } else if (c == '"' || c == '`') {
                const char *q = p + 1;
                while (q < g_end && *q != c) q++;
                if (q >= g_end) { g_in = g_end; fail("unterminated quoted identifier"); }
                tk.k = TK_IDENT; tk.quoted = 1;
                tk.s = astrndup(p + 1, (size_t)(q - p - 1));
                tk.n = (int)(q - p - 1);
                p = q + 1;
            } else {
                static const char *two[] = { "<=", ">=", "<>", "!=", "==", "||", NULL };
                int k, len = 0;
                for (k = 0; two[k]; k++)
                    if (p + 1 < g_end && p[0] == two[k][0] && p[1] == two[k][1]) len = 2;
                if (!len && strchr("(),*+-/%=<>.", c)) len = 1;
                if (!len) { g_in = g_end; fail("unrecognized character '%c'", c); }
                tk.k = TK_SYM; tk.s = p; tk.n = len;
                p += len;
            }
            tk.end = (int)(p - src);
            tok_push(&toks, &n, &cap, &tk);
        }
        g_in = p;
        if (n == 0) continue; /* stray ';' */
        Token endt;
        memset(&endt, 0, sizeof endt);
        endt.k = TK_END;
        endt.start = endt.end = (int)(p - src);
        tok_push(&toks, &n, &cap, &endt);
        st->t = toks;
        st->n = n - 1;
        st->src = src;
        return 1;
    }
}

/* ======================================================================
 * Expressions
 * ==================================================================== */
typedef enum { E_LIT, E_COL, E_NEG, E_NOT, E_BIN, E_ISNULL, E_BETWEEN, E_IN, E_LIKE, E_AGG } EKind;
typedef enum { B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_EQ, B_NE, B_LT, B_GT, B_LE, B_GE, B_AND, B_OR } BinOp;
typedef enum { AG_COUNT, AG_SUM, AG_AVG, AG_MIN, AG_MAX } AggFn;

typedef struct Expr {
    EKind k;
    int op;           /* BinOp or AggFn */
    Value lit;
    char *tname, *cname;
    int src, col;     /* bound column reference */
    struct Expr *a, *b, *c;
    struct Expr **list;
    int nlist;
    int neg;          /* NOT BETWEEN / NOT IN / NOT LIKE / IS NOT NULL */
    int star;         /* COUNT(*) */
    int slot;         /* aggregate slot, -1 = unassigned */
} Expr;

static Expr *new_expr(EKind k) {
    Expr *e = acalloc(sizeof *e);
    e->k = k;
    e->slot = -1;
    return e;
}

typedef struct { Token *t; int n; int pos; const char *src; } Parser;
static Parser P;

static Token *peek(void) { return &P.t[P.pos]; }
static Token *peek_at(int off) {
    int i = P.pos + off;
    return i > P.n ? &P.t[P.n] : &P.t[i];
}
static int is_kw(const Token *t, const char *kw) {
    return t->k == TK_IDENT && !t->quoted && ieq_n(t->s, (size_t)t->n, kw);
}
static int is_sym(const Token *t, const char *s) {
    return t->k == TK_SYM && (size_t)t->n == strlen(s) && memcmp(t->s, s, (size_t)t->n) == 0;
}

static void syntax_err(const char *fmt, ...) NORETURN PRINTF_FMT(1, 2);
static void syntax_err(const char *fmt, ...) {
    char what[200], where[80];
    va_list ap;
    Token *t = peek();
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (t->k == TK_END) snprintf(where, sizeof where, "end of statement");
    else snprintf(where, sizeof where, "'%.*s'", t->n > 40 ? 40 : t->n,
                  t->k == TK_INT || t->k == TK_REAL ? P.src + t->start : t->s);
    fail("syntax error: %s near %s", what, where);
}

static int accept_kw(const char *kw) {
    if (is_kw(peek(), kw)) { P.pos++; return 1; }
    return 0;
}
static void expect_kw(const char *kw) {
    if (!accept_kw(kw)) syntax_err("expected %s", kw);
}
static int accept_sym(const char *s) {
    if (is_sym(peek(), s)) { P.pos++; return 1; }
    return 0;
}
static void expect_sym(const char *s) {
    if (!accept_sym(s)) syntax_err("expected '%s'", s);
}

static int is_reserved(const Token *t) {
    static const char *kws[] = { "from", "where", "group", "order", "having", "limit", "offset",
        "inner", "join", "on", "as", "and", "or", "not", "set", "values", "select", "union",
        "using", "left", "right", "outer", "cross", NULL };
    int i;
    if (t->k != TK_IDENT || t->quoted) return 0;
    for (i = 0; kws[i]; i++)
        if (is_kw(t, kws[i])) return 1;
    return 0;
}

static char *parse_ident(void) {
    Token *t = peek();
    if (t->k != TK_IDENT || is_reserved(t)) syntax_err("expected identifier");
    P.pos++;
    return astrndup(t->s, (size_t)t->n);
}

static Expr *parse_or(void);

static Expr *parse_primary(void) {
    Token *t = peek();
    Expr *e;
    switch (t->k) {
    case TK_INT: P.pos++; e = new_expr(E_LIT); e->lit = v_int(t->i); return e;
    case TK_REAL: P.pos++; e = new_expr(E_LIT); e->lit = v_real(t->r); return e;
    case TK_STR: P.pos++; e = new_expr(E_LIT); e->lit = v_text(t->s, (size_t)t->n); return e;
    case TK_SYM:
        if (is_sym(t, "(")) {
            P.pos++;
            e = parse_or();
            expect_sym(")");
            return e;
        }
        syntax_err("unexpected symbol");
    case TK_IDENT: {
        static const char *aggs[] = { "count", "sum", "avg", "min", "max", NULL };
        int a;
        if (!t->quoted) {
            if (is_kw(t, "null")) { P.pos++; e = new_expr(E_LIT); e->lit = v_null(); return e; }
            for (a = 0; aggs[a]; a++) {
                if (is_kw(t, aggs[a]) && is_sym(peek_at(1), "(")) {
                    P.pos += 2;
                    e = new_expr(E_AGG);
                    e->op = a;
                    if (accept_sym("*")) {
                        if (a != AG_COUNT) syntax_err("only COUNT accepts *");
                        e->star = 1;
                    } else {
                        if (is_kw(peek(), "distinct")) fail("DISTINCT is not supported");
                        e->a = parse_or();
                    }
                    expect_sym(")");
                    return e;
                }
            }
        }
        e = new_expr(E_COL);
        e->cname = parse_ident();
        if (accept_sym(".")) {
            e->tname = e->cname;
            e->cname = parse_ident();
        }
        return e;
    }
    default:
        syntax_err("unexpected end of statement");
    }
}

static Expr *parse_unary(void) {
    if (accept_sym("-")) { Expr *e = new_expr(E_NEG); e->a = parse_unary(); return e; }
    if (accept_sym("+")) return parse_unary();
    return parse_primary();
}

static Expr *new_bin(int op, Expr *a, Expr *b) {
    Expr *e = new_expr(E_BIN);
    e->op = op; e->a = a; e->b = b;
    return e;
}

static Expr *parse_mul(void) {
    Expr *e = parse_unary();
    for (;;) {
        if (accept_sym("*")) e = new_bin(B_MUL, e, parse_unary());
        else if (accept_sym("/")) e = new_bin(B_DIV, e, parse_unary());
        else if (accept_sym("%")) e = new_bin(B_MOD, e, parse_unary());
        else return e;
    }
}

static Expr *parse_add(void) {
    Expr *e = parse_mul();
    for (;;) {
        if (accept_sym("+")) e = new_bin(B_ADD, e, parse_mul());
        else if (accept_sym("-")) e = new_bin(B_SUB, e, parse_mul());
        else return e;
    }
}

static Expr *parse_cmp(void) {
    Expr *e = parse_add();
    static const struct { const char *s; int op; } ops[] = {
        { "=", B_EQ }, { "==", B_EQ }, { "!=", B_NE }, { "<>", B_NE },
        { "<=", B_LE }, { ">=", B_GE }, { "<", B_LT }, { ">", B_GT } };
    size_t i;
    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (accept_sym(ops[i].s)) return new_bin(ops[i].op, e, parse_add());
    }
    if (accept_kw("is")) {
        Expr *r = new_expr(E_ISNULL);
        r->a = e;
        r->neg = accept_kw("not");
        expect_kw("null");
        return r;
    }
    int neg = 0;
    if (is_kw(peek(), "not") && (is_kw(peek_at(1), "between") || is_kw(peek_at(1), "in") ||
                                 is_kw(peek_at(1), "like"))) {
        P.pos++;
        neg = 1;
    }
    if (accept_kw("between")) {
        Expr *r = new_expr(E_BETWEEN);
        r->a = e;
        r->b = parse_add();
        expect_kw("and");
        r->c = parse_add();
        r->neg = neg;
        return r;
    }
    if (accept_kw("in")) {
        Expr *r = new_expr(E_IN);
        int cap = 8;
        r->a = e;
        r->neg = neg;
        r->list = amalloc(sizeof(Expr *) * (size_t)cap);
        expect_sym("(");
        do {
            if (r->nlist >= cap) {
                Expr **nl = amalloc(sizeof(Expr *) * (size_t)cap * 2);
                memcpy(nl, r->list, sizeof(Expr *) * (size_t)r->nlist);
                r->list = nl;
                cap *= 2;
            }
            r->list[r->nlist++] = parse_add();
        } while (accept_sym(","));
        expect_sym(")");
        return r;
    }
    if (accept_kw("like")) {
        Expr *r = new_expr(E_LIKE);
        r->a = e;
        r->b = parse_add();
        r->neg = neg;
        return r;
    }
    return e;
}

static Expr *parse_not(void) {
    if (accept_kw("not")) { Expr *e = new_expr(E_NOT); e->a = parse_not(); return e; }
    return parse_cmp();
}

static Expr *parse_and(void) {
    Expr *e = parse_not();
    while (accept_kw("and")) e = new_bin(B_AND, e, parse_not());
    return e;
}

static Expr *parse_or(void) {
    Expr *e = parse_and();
    while (accept_kw("or")) e = new_bin(B_OR, e, parse_and());
    return e;
}

/* ======================================================================
 * Binding and evaluation
 * ==================================================================== */
typedef struct { Table *t; char *alias; } Source;
static Source g_src[2];
static int g_nsrc;

static int col_exists(const char *name) {
    int s, c;
    for (s = 0; s < g_nsrc; s++)
        for (c = 0; c < g_src[s].t->ncols; c++)
            if (ieq(g_src[s].t->cols[c].name, name)) return 1;
    return 0;
}

static void resolve_col(Expr *e) {
    int found = -1, fcol = -1, s, c;
    for (s = 0; s < g_nsrc; s++) {
        if (e->tname && !(ieq(e->tname, g_src[s].alias) || ieq(e->tname, g_src[s].t->name)))
            continue;
        for (c = 0; c < g_src[s].t->ncols; c++) {
            if (!ieq(g_src[s].t->cols[c].name, e->cname)) continue;
            if (found >= 0) fail("ambiguous column name: %s", e->cname);
            found = s;
            fcol = c;
        }
    }
    if (found < 0) {
        if (e->tname) fail("no such column: %s.%s", e->tname, e->cname);
        fail("no such column: %s", e->cname);
    }
    e->src = found;
    e->col = fcol;
}

static void bind_expr(Expr *e) {
    int i;
    if (!e) return;
    if (e->k == E_COL) { resolve_col(e); return; }
    bind_expr(e->a);
    bind_expr(e->b);
    bind_expr(e->c);
    for (i = 0; i < e->nlist; i++) bind_expr(e->list[i]);
}

static int contains_agg(const Expr *e) {
    int i;
    if (!e) return 0;
    if (e->k == E_AGG) return 1;
    if (contains_agg(e->a) || contains_agg(e->b) || contains_agg(e->c)) return 1;
    for (i = 0; i < e->nlist; i++)
        if (contains_agg(e->list[i])) return 1;
    return 0;
}

#define MAX_AGGS 64
static Expr *g_aggs[MAX_AGGS];
static int g_nagg;

static void collect_aggs(Expr *e, int inagg) {
    int i;
    if (!e) return;
    if (e->k == E_AGG) {
        if (inagg) fail("aggregate functions cannot be nested");
        if (e->slot < 0) {
            if (g_nagg >= MAX_AGGS) fail("too many aggregate functions");
            e->slot = g_nagg;
            g_aggs[g_nagg++] = e;
        }
        collect_aggs(e->a, 1);
        return;
    }
    collect_aggs(e->a, inagg);
    collect_aggs(e->b, inagg);
    collect_aggs(e->c, inagg);
    for (i = 0; i < e->nlist; i++) collect_aggs(e->list[i], inagg);
}

typedef struct { Value *row[2]; Value *aggv; } EvalCtx;

static Value arith(int op, Value a, Value b) {
    if (a.t == V_NULL || b.t == V_NULL) return v_null();
    if (a.t == V_TEXT || b.t == V_TEXT) fail("cannot do arithmetic on a TEXT value");
    if (a.t == V_INT && b.t == V_INT) {
        int64_t x = a.u.i, y = b.u.i;
        switch (op) {
        case B_ADD: return v_int((int64_t)((uint64_t)x + (uint64_t)y));
        case B_SUB: return v_int((int64_t)((uint64_t)x - (uint64_t)y));
        case B_MUL: return v_int((int64_t)((uint64_t)x * (uint64_t)y));
        case B_DIV:
            if (y == 0) return v_null();
            if (y == -1) return v_int((int64_t)(0 - (uint64_t)x));
            return v_int(x / y);
        default:
            if (y == 0) return v_null();
            if (y == -1) return v_int(0);
            return v_int(x % y);
        }
    }
    {
        double x = vnum(&a), y = vnum(&b);
        switch (op) {
        case B_ADD: return v_real(x + y);
        case B_SUB: return v_real(x - y);
        case B_MUL: return v_real(x * y);
        case B_DIV: return y == 0 ? v_null() : v_real(x / y);
        default: return y == 0 ? v_null() : v_real(fmod(x, y));
        }
    }
}

static int like_match(const char *p, int pn, const char *s, int sn) {
    while (pn > 0) {
        char c = *p;
        if (c == '%') {
            int i;
            while (pn > 0 && *p == '%') { p++; pn--; }
            if (pn == 0) return 1;
            for (i = 0; i <= sn; i++)
                if (like_match(p, pn, s + i, sn - i)) return 1;
            return 0;
        }
        if (sn == 0) return 0;
        if (c != '_' && tolower((unsigned char)c) != tolower((unsigned char)*s)) return 0;
        p++; pn--; s++; sn--;
    }
    return sn == 0;
}

static Value tri(int known, int val) { return known ? v_int(val ? 1 : 0) : v_null(); }

static Value eval(const Expr *e, const EvalCtx *cx) {
    switch (e->k) {
    case E_LIT: return e->lit;
    case E_COL: return cx->row[e->src][e->col];
    case E_NEG: {
        Value v = eval(e->a, cx);
        if (v.t == V_INT) return v_int((int64_t)(0 - (uint64_t)v.u.i));
        if (v.t == V_REAL) return v_real(-v.u.r);
        if (v.t == V_TEXT) fail("cannot negate a TEXT value");
        return v;
    }
    case E_NOT: {
        Value v = eval(e->a, cx);
        return tri(v.t != V_NULL, !truthy(v));
    }
    case E_ISNULL: {
        Value v = eval(e->a, cx);
        return v_int((v.t == V_NULL) != (e->neg != 0));
    }
    case E_BIN: {
        if (e->op == B_AND) {
            Value a = eval(e->a, cx);
            if (a.t != V_NULL && !truthy(a)) return v_int(0);
            Value b = eval(e->b, cx);
            if (b.t != V_NULL && !truthy(b)) return v_int(0);
            return tri(a.t != V_NULL && b.t != V_NULL, 1);
        }
        if (e->op == B_OR) {
            Value a = eval(e->a, cx);
            if (a.t != V_NULL && truthy(a)) return v_int(1);
            Value b = eval(e->b, cx);
            if (b.t != V_NULL && truthy(b)) return v_int(1);
            return tri(a.t != V_NULL && b.t != V_NULL, 0);
        }
        Value a = eval(e->a, cx), b = eval(e->b, cx);
        if (e->op <= B_MOD) return arith(e->op, a, b);
        if (a.t == V_NULL || b.t == V_NULL) return v_null();
        int c = value_cmp(&a, &b), r = 0;
        switch (e->op) {
        case B_EQ: r = c == 0; break;
        case B_NE: r = c != 0; break;
        case B_LT: r = c < 0; break;
        case B_GT: r = c > 0; break;
        case B_LE: r = c <= 0; break;
        default: r = c >= 0; break;
        }
        return v_int(r);
    }
    case E_BETWEEN: {
        Value v = eval(e->a, cx), lo = eval(e->b, cx), hi = eval(e->c, cx);
        if (v.t == V_NULL || lo.t == V_NULL || hi.t == V_NULL) return v_null();
        int in = value_cmp(&v, &lo) >= 0 && value_cmp(&v, &hi) <= 0;
        return v_int(in != (e->neg != 0));
    }
    case E_IN: {
        Value v = eval(e->a, cx);
        int i, saw_null = 0, hit = 0;
        if (v.t == V_NULL) return v;
        for (i = 0; i < e->nlist && !hit; i++) {
            Value x = eval(e->list[i], cx);
            if (x.t == V_NULL) saw_null = 1;
            else if (value_cmp(&v, &x) == 0) hit = 1;
        }
        if (!hit && saw_null) return v_null();
        return v_int(hit != (e->neg != 0));
    }
    case E_LIKE: {
        Value v = eval(e->a, cx), p = eval(e->b, cx);
        if (v.t == V_NULL || p.t == V_NULL) return v_null();
        if (v.t != V_TEXT || p.t != V_TEXT) fail("LIKE requires TEXT operands");
        int m = like_match(p.u.s.p, (int)p.u.s.n, v.u.s.p, (int)v.u.s.n);
        return v_int(m != (e->neg != 0));
    }
    case E_AGG:
        if (!cx->aggv) fail("aggregate functions are not allowed here");
        return cx->aggv[e->slot];
    }
    return v_null();
}

/* ======================================================================
 * Aggregation
 * ==================================================================== */
typedef struct {
    int64_t cnt, isum;
    double rsum;
    int is_real, has;
    Value mn, mx;
} Acc;

static void acc_update(Acc *a, const Expr *agg, Value v) {
    if (agg->star) { a->cnt++; return; }
    if (v.t == V_NULL) return;
    switch (agg->op) {
    case AG_COUNT: a->cnt++; break;
    case AG_SUM: case AG_AVG:
        if (v.t == V_TEXT) fail("cannot aggregate a TEXT value with SUM/AVG");
        a->cnt++;
        if (v.t == V_INT) { a->isum = (int64_t)((uint64_t)a->isum + (uint64_t)v.u.i); a->rsum += (double)v.u.i; }
        else { a->is_real = 1; a->rsum += v.u.r; }
        break;
    case AG_MIN:
        if (!a->has || value_cmp(&v, &a->mn) < 0) a->mn = v;
        a->has = 1;
        break;
    default:
        if (!a->has || value_cmp(&v, &a->mx) > 0) a->mx = v;
        a->has = 1;
        break;
    }
}

static Value acc_result(const Acc *a, const Expr *agg) {
    switch (agg->op) {
    case AG_COUNT: return v_int(a->cnt);
    case AG_SUM: return a->cnt == 0 ? v_null() : a->is_real ? v_real(a->rsum) : v_int(a->isum);
    case AG_AVG: return a->cnt == 0 ? v_null() : v_real(a->rsum / (double)a->cnt);
    case AG_MIN: return a->has ? a->mn : v_null();
    default: return a->has ? a->mx : v_null();
    }
}

typedef struct Group {
    Value *key;
    Value *rows[2];
    Acc *acc;
    uint64_t hash;
    unsigned char *kb;
    size_t kn;
} Group;

typedef struct {
    Group **tab;
    size_t cap, n;
    Group **list;
    size_t lcap;
} GroupSet;

static unsigned char *kbuf;
static size_t kcap, klen;

static void kb_put(const void *d, size_t n) {
    if (klen + n > kcap) {
        kcap = (klen + n) * 2 + 64;
        kbuf = realloc(kbuf, kcap);
        if (!kbuf) fail("out of memory");
    }
    memcpy(kbuf + klen, d, n);
    klen += n;
}

static void kb_value(const Value *v) {
    unsigned char tag = v->t;
    kb_put(&tag, 1);
    if (v->t == V_INT) kb_put(&v->u.i, 8);
    else if (v->t == V_REAL) { double r = v->u.r == 0.0 ? 0.0 : v->u.r; kb_put(&r, 8); }
    else if (v->t == V_TEXT) { uint32_t n = v->u.s.n; kb_put(&n, 4); kb_put(v->u.s.p, n); }
}

static void gs_grow(GroupSet *gs) {
    size_t ncap = gs->cap ? gs->cap * 2 : 256, i;
    Group **nt = acalloc(sizeof(Group *) * ncap);
    for (i = 0; i < gs->cap; i++) {
        Group *g = gs->tab[i];
        if (!g) continue;
        size_t j = g->hash & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = g;
    }
    gs->tab = nt;
    gs->cap = ncap;
}

/* returns the group for the key currently in kbuf; *created tells if it is new */
static Group *gs_find(GroupSet *gs, int *created) {
    uint64_t h = fnv1a(kbuf, klen, 1469598103934665603ULL);
    size_t j;
    if ((gs->n + 1) * 2 > gs->cap) gs_grow(gs);
    j = h & (gs->cap - 1);
    while (gs->tab[j]) {
        Group *g = gs->tab[j];
        if (g->hash == h && g->kn == klen && memcmp(g->kb, kbuf, klen) == 0) {
            *created = 0;
            return g;
        }
        j = (j + 1) & (gs->cap - 1);
    }
    Group *g = acalloc(sizeof *g);
    g->hash = h;
    g->kb = amalloc(klen ? klen : 1);
    memcpy(g->kb, kbuf, klen);
    g->kn = klen;
    gs->tab[j] = g;
    if (gs->n >= gs->lcap) {
        size_t ncap = gs->lcap ? gs->lcap * 2 : 256;
        Group **nl = amalloc(sizeof(Group *) * ncap);
        if (gs->n) memcpy(nl, gs->list, sizeof(Group *) * gs->n);
        gs->list = nl;
        gs->lcap = ncap;
    }
    gs->list[gs->n++] = g;
    *created = 1;
    return g;
}

/* ======================================================================
 * SELECT
 * ==================================================================== */
typedef struct { Expr *e; char *alias; int explicit_alias; int star; char *star_tbl; } SelItem;
typedef struct { Expr *e; int desc; } OrderItem;

typedef struct {
    SelItem *items; int nitems;
    Table *t[2]; int nsrc;
    Expr *on, *where, *having;
    Expr **groupby; int ngroup;
    OrderItem *order; int norder;
    int64_t limit, offset; /* limit < 0: none */
} Select;

typedef struct { Value *vals; Value *keys; size_t seq; } OutRow;

static Expr **g_pexpr;
static char **g_pname;
static int g_np;

static void alias_subst(Expr **ep) {
    Expr *e = *ep;
    int i;
    if (!e) return;
    if (e->k == E_COL && !e->tname) {
        if (!col_exists(e->cname))
            for (i = 0; i < g_np; i++)
                if (ieq(g_pname[i], e->cname)) { *ep = g_pexpr[i]; return; }
        return;
    }
    alias_subst(&e->a);
    alias_subst(&e->b);
    alias_subst(&e->c);
    for (i = 0; i < e->nlist; i++) alias_subst(&e->list[i]);
}

static void out_text(const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\t') fputs("\\t", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else fputc(c, stdout);
    }
}

static void print_value(const Value *v) {
    char buf[64];
    switch (v->t) {
    case V_INT: printf("%lld", (long long)v->u.i); break;
    case V_REAL: fmt_real(v->u.r, buf, sizeof buf); fputs(buf, stdout); break;
    case V_TEXT: out_text(v->u.s.p, v->u.s.n); break;
    default: fputs("NULL", stdout); break;
    }
}

static void print_row(const Value *vals, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (i) fputc('\t', stdout);
        print_value(&vals[i]);
    }
    fputc('\n', stdout);
}

typedef int (*RowCb)(EvalCtx *cx, void *ud);
static int64_t g_rowkey;

/* Finds `colA = colB` (one column from each side) inside an AND-chain. */
static int find_equi(Expr *on, int *lcol, int *rcol) {
    if (!on) return 0;
    if (on->k == E_BIN && on->op == B_AND)
        return find_equi(on->a, lcol, rcol) || find_equi(on->b, lcol, rcol);
    if (on->k == E_BIN && on->op == B_EQ && on->a->k == E_COL && on->b->k == E_COL &&
        on->a->src != on->b->src) {
        const Expr *l = on->a->src == 0 ? on->a : on->b;
        const Expr *r = on->a->src == 0 ? on->b : on->a;
        *lcol = l->col;
        *rcol = r->col;
        return 1;
    }
    return 0;
}

/* Drives the FROM clause: table scan, or hash/nested-loop inner join. */
static void run_scan(Select *s, RowCb cb, void *ud) {
    Table *t0 = s->t[0];
    Value *row0 = amalloc(sizeof(Value) * (size_t)(t0->ncols ? t0->ncols : 1));
    EvalCtx cx;
    Cursor c;
    int64_t key;
    const uint8_t *pl;
    uint16_t len;
    memset(&cx, 0, sizeof cx);
    cx.row[0] = row0;
    cur_first(&c, t0->root);
    if (s->nsrc == 1) {
        while (cur_next(&c, &key, &pl, &len)) {
            rec_decode(pl, len, row0, t0->ncols);
            g_rowkey = key;
            if (cb(&cx, ud)) break;
        }
        return;
    }

    Table *t1 = s->t[1];
    Value **rrows;
    size_t nr = 0, rcap = 1024, j;
    Cursor rc;
    rrows = amalloc(sizeof(Value *) * rcap);
    cur_first(&rc, t1->root);
    while (cur_next(&rc, &key, &pl, &len)) {
        Value *r = amalloc(sizeof(Value) * (size_t)(t1->ncols ? t1->ncols : 1));
        rec_decode(pl, len, r, t1->ncols);
        if (nr >= rcap) {
            Value **nrr = amalloc(sizeof(Value *) * rcap * 2);
            memcpy(nrr, rrows, sizeof(Value *) * nr);
            rrows = nrr;
            rcap *= 2;
        }
        rrows[nr++] = r;
    }

    int lcol = 0, rcol = 0, use_hash = find_equi(s->on, &lcol, &rcol);
    int32_t *head = NULL, *nxt = NULL;
    size_t nb = 16;
    if (use_hash) {
        long i;
        while (nb < nr * 2) nb *= 2;
        head = amalloc(sizeof(int32_t) * nb);
        nxt = amalloc(sizeof(int32_t) * (nr ? nr : 1));
        for (j = 0; j < nb; j++) head[j] = -1;
        for (i = (long)nr - 1; i >= 0; i--) { /* reverse push keeps insertion order */
            const Value *v = &rrows[i][rcol];
            nxt[i] = -1;
            if (v->t == V_NULL) continue;
            size_t h = (size_t)(hash_value(v) & (nb - 1));
            nxt[i] = head[h];
            head[h] = (int32_t)i;
        }
    }
    while (cur_next(&c, &key, &pl, &len)) {
        rec_decode(pl, len, row0, t0->ncols);
        if (use_hash) {
            const Value *lv = &row0[lcol];
            if (lv->t == V_NULL) continue;
            size_t h = (size_t)(hash_value(lv) & (nb - 1));
            int32_t k;
            for (k = head[h]; k >= 0; k = nxt[k]) {
                if (value_cmp(lv, &rrows[k][rcol]) != 0) continue;
                cx.row[1] = rrows[k];
                if (s->on && !truthy(eval(s->on, &cx))) continue;
                if (cb(&cx, ud)) return;
            }
        } else {
            for (j = 0; j < nr; j++) {
                cx.row[1] = rrows[j];
                if (s->on && !truthy(eval(s->on, &cx))) continue;
                if (cb(&cx, ud)) return;
            }
        }
    }
}

typedef struct {
    Select *s;
    int grouped;
    OutRow *out;
    size_t nout, ocap;
    int64_t skipped, printed;
    GroupSet gs;
} SelState;

static Value *project(const EvalCtx *cx) {
    Value *vals = amalloc(sizeof(Value) * (size_t)(g_np ? g_np : 1));
    int i;
    for (i = 0; i < g_np; i++) vals[i] = eval(g_pexpr[i], cx);
    return vals;
}

static Value *order_keys(Select *s, const EvalCtx *cx) {
    Value *k = amalloc(sizeof(Value) * (size_t)(s->norder ? s->norder : 1));
    int i;
    for (i = 0; i < s->norder; i++) k[i] = eval(s->order[i].e, cx);
    return k;
}

static void out_push(SelState *st, Value *vals, Value *keys) {
    if (st->nout >= st->ocap) {
        size_t ncap = st->ocap ? st->ocap * 2 : 1024;
        OutRow *no = amalloc(sizeof(OutRow) * ncap);
        if (st->nout) memcpy(no, st->out, sizeof(OutRow) * st->nout);
        st->out = no;
        st->ocap = ncap;
    }
    st->out[st->nout].vals = vals;
    st->out[st->nout].keys = keys;
    st->out[st->nout].seq = st->nout;
    st->nout++;
}

static int cb_plain(EvalCtx *cx, void *ud) {
    SelState *st = ud;
    Select *s = st->s;
    if (s->where && !truthy(eval(s->where, cx))) return 0;
    if (s->norder == 0) { /* stream straight to stdout, stop early at LIMIT */
        if (st->skipped < s->offset) { st->skipped++; return 0; }
        if (s->limit >= 0 && st->printed >= s->limit) return 1;
        Value *vals = project(cx);
        print_row(vals, g_np);
        st->printed++;
        return s->limit >= 0 && st->printed >= s->limit;
    }
    out_push(st, project(cx), order_keys(s, cx));
    return 0;
}

static int cb_group(EvalCtx *cx, void *ud) {
    SelState *st = ud;
    Select *s = st->s;
    int i, created = 0;
    Group *g;
    if (s->where && !truthy(eval(s->where, cx))) return 0;
    klen = 0;
    for (i = 0; i < s->ngroup; i++) {
        Value v = eval(s->groupby[i], cx);
        kb_value(&v);
    }
    g = gs_find(&st->gs, &created);
    if (created) {
        int src;
        g->key = amalloc(sizeof(Value) * (size_t)(s->ngroup ? s->ngroup : 1));
        for (i = 0; i < s->ngroup; i++) g->key[i] = eval(s->groupby[i], cx);
        for (src = 0; src < s->nsrc; src++) {
            size_t nbytes = sizeof(Value) * (size_t)(s->t[src]->ncols ? s->t[src]->ncols : 1);
            g->rows[src] = amalloc(nbytes);
            memcpy(g->rows[src], cx->row[src], sizeof(Value) * (size_t)s->t[src]->ncols);
        }
        g->acc = acalloc(sizeof(Acc) * (size_t)(g_nagg ? g_nagg : 1));
    }
    for (i = 0; i < g_nagg; i++) {
        Value v = g_aggs[i]->star ? v_null() : eval(g_aggs[i]->a, cx);
        acc_update(&g->acc[i], g_aggs[i], v);
    }
    return 0;
}

static const int *g_sort_desc;
static int g_sort_n;

static int cmp_out(const void *pa, const void *pb) {
    const OutRow *a = pa, *b = pb;
    int i;
    for (i = 0; i < g_sort_n; i++) {
        int c = value_cmp(&a->keys[i], &b->keys[i]);
        if (c) return g_sort_desc[i] ? -c : c;
    }
    return (a->seq > b->seq) - (a->seq < b->seq);
}

static void do_select(void) {
    Select *s = acalloc(sizeof *s);
    int cap = 8, i, j;
    char *alias0 = NULL, *alias1 = NULL;

    if (is_kw(peek(), "distinct")) fail("DISTINCT is not supported");
    s->items = amalloc(sizeof(SelItem) * (size_t)cap);
    do {
        SelItem it;
        memset(&it, 0, sizeof it);
        if (accept_sym("*")) {
            it.star = 1;
        } else if (peek()->k == TK_IDENT && is_sym(peek_at(1), ".") && is_sym(peek_at(2), "*")) {
            it.star = 1;
            it.star_tbl = parse_ident();
            P.pos += 2;
        } else {
            int first = P.pos;
            it.e = parse_or();
            if (accept_kw("as")) it.alias = parse_ident();
            else if (peek()->k == TK_IDENT && !is_reserved(peek())) it.alias = parse_ident();
            if (!it.alias) {
                if (it.e->k == E_COL) it.alias = it.e->cname;
                else {
                    int a = P.t[first].start, b = P.t[P.pos - 1].end;
                    it.alias = astrndup(P.src + a, (size_t)(b - a));
                }
            } else {
                it.explicit_alias = 1;
            }
        }
        if (s->nitems >= cap) {
            SelItem *ni = amalloc(sizeof(SelItem) * (size_t)cap * 2);
            memcpy(ni, s->items, sizeof(SelItem) * (size_t)s->nitems);
            s->items = ni;
            cap *= 2;
        }
        s->items[s->nitems++] = it;
    } while (accept_sym(","));

    expect_kw("from");
    s->nsrc = 1;
    {
        char *tn = parse_ident();
        s->t[0] = table_require(tn);
        alias0 = tn;
        if (accept_kw("as")) alias0 = parse_ident();
        else if (peek()->k == TK_IDENT && !is_reserved(peek())) alias0 = parse_ident();
    }
    if (is_kw(peek(), "inner") || is_kw(peek(), "join")) {
        accept_kw("inner");
        expect_kw("join");
        char *tn = parse_ident();
        s->t[1] = table_require(tn);
        s->nsrc = 2;
        alias1 = tn;
        if (accept_kw("as")) alias1 = parse_ident();
        else if (peek()->k == TK_IDENT && !is_reserved(peek())) alias1 = parse_ident();
        expect_kw("on");
        s->on = parse_or();
    } else if (is_kw(peek(), "left") || is_kw(peek(), "right") || is_kw(peek(), "cross") ||
               is_kw(peek(), "outer")) {
        fail("only INNER JOIN is supported");
    }
    if (accept_kw("where")) s->where = parse_or();
    if (accept_kw("group")) {
        int gcap = 8;
        expect_kw("by");
        s->groupby = amalloc(sizeof(Expr *) * (size_t)gcap);
        do {
            if (s->ngroup >= gcap) {
                Expr **ng = amalloc(sizeof(Expr *) * (size_t)gcap * 2);
                memcpy(ng, s->groupby, sizeof(Expr *) * (size_t)s->ngroup);
                s->groupby = ng;
                gcap *= 2;
            }
            s->groupby[s->ngroup++] = parse_or();
        } while (accept_sym(","));
    }
    if (accept_kw("having")) s->having = parse_or();
    if (accept_kw("order")) {
        int ocap = 8;
        expect_kw("by");
        s->order = amalloc(sizeof(OrderItem) * (size_t)ocap);
        do {
            if (s->norder >= ocap) {
                OrderItem *no = amalloc(sizeof(OrderItem) * (size_t)ocap * 2);
                memcpy(no, s->order, sizeof(OrderItem) * (size_t)s->norder);
                s->order = no;
                ocap *= 2;
            }
            OrderItem oi;
            oi.e = parse_or();
            oi.desc = 0;
            if (accept_kw("desc")) oi.desc = 1;
            else accept_kw("asc");
            s->order[s->norder++] = oi;
        } while (accept_sym(","));
    }
    s->limit = -1;
    if (accept_kw("limit")) {
        Token *t = peek();
        if (t->k != TK_INT) syntax_err("expected an integer after LIMIT");
        P.pos++;
        s->limit = t->i;
        if (accept_kw("offset")) {
            t = peek();
            if (t->k != TK_INT) syntax_err("expected an integer after OFFSET");
            P.pos++;
            s->offset = t->i;
        }
    }
    if (peek()->k != TK_END) syntax_err("unexpected token");
    if (s->offset < 0) s->offset = 0;

    /* ---- bind ---- */
    g_nsrc = s->nsrc;
    g_src[0].t = s->t[0]; g_src[0].alias = alias0;
    g_src[1].t = s->t[1]; g_src[1].alias = alias1;

    /* expand * / t.* and fix output names */
    {
        int pcap = 0;
        for (i = 0; i < s->nitems; i++) {
            if (!s->items[i].star) pcap++;
            else for (j = 0; j < s->nsrc; j++) pcap += s->t[j]->ncols;
        }
        g_pexpr = amalloc(sizeof(Expr *) * (size_t)(pcap ? pcap : 1));
        g_pname = amalloc(sizeof(char *) * (size_t)(pcap ? pcap : 1));
        g_np = 0;
        for (i = 0; i < s->nitems; i++) {
            SelItem *it = &s->items[i];
            if (!it->star) {
                bind_expr(it->e);
                g_pexpr[g_np] = it->e;
                /* a bare column is labelled with its declared name, not as typed */
                if (!it->explicit_alias && it->e->k == E_COL)
                    it->alias = g_src[it->e->src].t->cols[it->e->col].name;
                g_pname[g_np++] = it->alias;
                continue;
            }
            int matched = 0;
            for (j = 0; j < s->nsrc; j++) {
                int c;
                if (it->star_tbl && !(ieq(it->star_tbl, g_src[j].alias) || ieq(it->star_tbl, g_src[j].t->name)))
                    continue;
                matched = 1;
                for (c = 0; c < s->t[j]->ncols; c++) {
                    Expr *e = new_expr(E_COL);
                    e->src = j;
                    e->col = c;
                    e->cname = s->t[j]->cols[c].name;
                    g_pexpr[g_np] = e;
                    g_pname[g_np++] = s->t[j]->cols[c].name;
                }
            }
            if (!matched) fail("no such table: %s", it->star_tbl);
        }
    }
    if (s->where && contains_agg(s->where)) fail("aggregate functions are not allowed in WHERE");
    bind_expr(s->on);
    bind_expr(s->where);
    for (i = 0; i < s->ngroup; i++) bind_expr(s->groupby[i]);
    alias_subst(&s->having);
    bind_expr(s->having);
    for (i = 0; i < s->norder; i++) {
        Expr *e = s->order[i].e;
        if (e->k == E_LIT && e->lit.t == V_INT) { /* ORDER BY <position> */
            if (e->lit.u.i < 1 || e->lit.u.i > g_np) fail("ORDER BY position %lld is out of range", (long long)e->lit.u.i);
            s->order[i].e = g_pexpr[e->lit.u.i - 1];
            continue;
        }
        alias_subst(&s->order[i].e);
        bind_expr(s->order[i].e);
    }

    g_nagg = 0;
    for (i = 0; i < g_np; i++) collect_aggs(g_pexpr[i], 0);
    collect_aggs(s->having, 0);
    for (i = 0; i < s->norder; i++) collect_aggs(s->order[i].e, 0);
    for (i = 0; i < s->ngroup; i++)
        if (contains_agg(s->groupby[i])) fail("aggregate functions are not allowed in GROUP BY");
    if (s->having && !(g_nagg > 0 || s->ngroup > 0))
        fail("HAVING requires GROUP BY or an aggregate");

    int grouped = g_nagg > 0 || s->ngroup > 0;
    SelState st;
    memset(&st, 0, sizeof st);
    st.s = s;
    st.grouped = grouped;

    for (i = 0; i < g_np; i++) {
        if (i) fputc('\t', stdout);
        out_text(g_pname[i], strlen(g_pname[i]));
    }
    fputc('\n', stdout);

    if (!grouped) {
        run_scan(s, cb_plain, &st);
        if (s->norder == 0) return;
    } else {
        run_scan(s, cb_group, &st);
        if (st.gs.n == 0 && s->ngroup == 0) { /* aggregates over zero rows still yield one row */
            Group *g = acalloc(sizeof *g);
            int src;
            for (src = 0; src < s->nsrc; src++) {
                g->rows[src] = acalloc(sizeof(Value) * (size_t)(s->t[src]->ncols ? s->t[src]->ncols : 1));
            }
            g->acc = acalloc(sizeof(Acc) * (size_t)(g_nagg ? g_nagg : 1));
            g->key = amalloc(sizeof(Value));
            st.gs.list = amalloc(sizeof(Group *));
            st.gs.list[0] = g;
            st.gs.n = 1;
        }
        size_t gi;
        for (gi = 0; gi < st.gs.n; gi++) {
            Group *g = st.gs.list[gi];
            EvalCtx cx;
            Value *aggv = amalloc(sizeof(Value) * (size_t)(g_nagg ? g_nagg : 1));
            for (i = 0; i < g_nagg; i++) aggv[i] = acc_result(&g->acc[i], g_aggs[i]);
            cx.row[0] = g->rows[0];
            cx.row[1] = g->rows[1];
            cx.aggv = aggv;
            if (s->having && !truthy(eval(s->having, &cx))) continue;
            out_push(&st, project(&cx), s->norder ? order_keys(s, &cx) : g->key);
        }
    }

    /* sort: explicit ORDER BY, else groups come out ordered by group key */
    {
        int n = s->norder ? s->norder : s->ngroup;
        if (n > 0 && st.nout > 1) {
            int *desc = acalloc(sizeof(int) * (size_t)n);
            for (i = 0; i < s->norder; i++) desc[i] = s->order[i].desc;
            g_sort_desc = desc;
            g_sort_n = n;
            qsort(st.out, st.nout, sizeof(OutRow), cmp_out);
        }
    }
    {
        size_t k;
        int64_t printed = 0;
        for (k = 0; k < st.nout; k++) {
            if ((int64_t)k < s->offset) continue;
            if (s->limit >= 0 && printed >= s->limit) break;
            print_row(st.out[k].vals, g_np);
            printed++;
        }
    }
}

/* ======================================================================
 * DML / DDL
 * ==================================================================== */
static Value coerce(Value v, const ColDef *c) {
    if (v.t == V_NULL) return v;
    switch (c->type) {
    case C_INT:
        if (v.t == V_INT) return v;
        if (v.t == V_REAL) {
            if (v.u.r == floor(v.u.r) && fabs(v.u.r) < 9.0e18) return v_int((int64_t)v.u.r);
            fail("cannot store REAL value in INTEGER column '%s'", c->name);
        } else {
            char *tmp = astrndup(v.u.s.p, v.u.s.n), *end;
            errno = 0;
            long long x = strtoll(tmp, &end, 10);
            if (*tmp && !*end && errno == 0) return v_int(x);
            fail("cannot store TEXT value '%s' in INTEGER column '%s'", tmp, c->name);
        }
        break;
    case C_REAL:
        if (v.t == V_REAL) return v;
        if (v.t == V_INT) return v_real((double)v.u.i);
        {
            char *tmp = astrndup(v.u.s.p, v.u.s.n), *end;
            double x = strtod(tmp, &end);
            if (*tmp && !*end) return v_real(x);
            fail("cannot store TEXT value '%s' in REAL column '%s'", tmp, c->name);
        }
        break;
    default:
        if (v.t == V_TEXT) return v;
        {
            char buf[64];
            if (v.t == V_INT) snprintf(buf, sizeof buf, "%lld", (long long)v.u.i);
            else fmt_real(v.u.r, buf, sizeof buf);
            return v_text(astrndup(buf, strlen(buf)), strlen(buf));
        }
    }
    return v;
}

typedef struct { int64_t key; uint8_t *rec; size_t len; } Pending;
typedef struct { Pending *v; size_t n, cap; } PendingVec;

static void pend_push(PendingVec *pv, int64_t key, const Value *row, int ncols) {
    size_t len = rec_size(row, ncols);
    if (len > MAX_REC) fail("row too large (%zu bytes, max %d)", len, MAX_REC);
    if (pv->n >= pv->cap) {
        size_t ncap = pv->cap ? pv->cap * 2 : 256;
        Pending *nv = amalloc(sizeof(Pending) * ncap);
        if (pv->n) memcpy(nv, pv->v, sizeof(Pending) * pv->n);
        pv->v = nv;
        pv->cap = ncap;
    }
    pv->v[pv->n].key = key;
    pv->v[pv->n].rec = amalloc(len);
    pv->v[pv->n].len = rec_encode(row, ncols, pv->v[pv->n].rec);
    pv->n++;
}

static void do_insert(void) {
    Table *t;
    int *map, nmap = 0, i;
    PendingVec pv;
    EvalCtx cx;
    expect_kw("into");
    t = table_require(parse_ident());
    memset(&pv, 0, sizeof pv);
    memset(&cx, 0, sizeof cx);
    map = amalloc(sizeof(int) * (size_t)t->ncols);
    if (accept_sym("(")) {
        do {
            char *cn = parse_ident();
            int c, found = -1;
            for (c = 0; c < t->ncols; c++)
                if (ieq(t->cols[c].name, cn)) found = c;
            if (found < 0) fail("table %s has no column named %s", t->name, cn);
            if (nmap >= t->ncols) fail("too many columns in INSERT column list");
            map[nmap++] = found;
        } while (accept_sym(","));
        expect_sym(")");
    } else {
        for (i = 0; i < t->ncols; i++) map[i] = i;
        nmap = t->ncols;
    }
    expect_kw("values");
    g_nsrc = 0;
    do {
        Value *row = acalloc(sizeof(Value) * (size_t)t->ncols);
        int k = 0;
        expect_sym("(");
        do {
            Expr *e = parse_or();
            bind_expr(e);
            if (contains_agg(e)) fail("aggregate functions are not allowed in VALUES");
            if (k >= nmap) fail("table %s: more values than columns", t->name);
            row[map[k]] = coerce(eval(e, &cx), &t->cols[map[k]]);
            k++;
        } while (accept_sym(","));
        expect_sym(")");
        if (k != nmap) fail("table %s: %d values for %d columns", t->name, k, nmap);
        pend_push(&pv, 0, row, t->ncols);
    } while (accept_sym(","));
    if (peek()->k != TK_END) syntax_err("unexpected token");
    for (i = 0; i < (int)pv.n; i++)
        btree_insert(t->root, btree_alloc_key(t->root), pv.v[i].rec, pv.v[i].len);
}

typedef struct {
    Select sel;
    Expr *where;
    int nassign;
    int *acol;
    Expr **aexpr;
    Table *t;
    PendingVec pv;
    int is_update;
} DmlState;

static int cb_dml(EvalCtx *cx, void *ud) {
    DmlState *d = ud;
    if (d->where && !truthy(eval(d->where, cx))) return 0;
    if (d->is_update) {
        Value *nr = amalloc(sizeof(Value) * (size_t)d->t->ncols);
        int i;
        memcpy(nr, cx->row[0], sizeof(Value) * (size_t)d->t->ncols);
        for (i = 0; i < d->nassign; i++)
            nr[d->acol[i]] = coerce(eval(d->aexpr[i], cx), &d->t->cols[d->acol[i]]);
        pend_push(&d->pv, g_rowkey, nr, d->t->ncols);
    } else {
        pend_push(&d->pv, g_rowkey, NULL, 0);
    }
    return 0;
}

static void do_update_delete(int is_update) {
    DmlState d;
    char *name;
    size_t i;
    memset(&d, 0, sizeof d);
    d.is_update = is_update;
    if (!is_update) expect_kw("from");
    name = parse_ident();
    d.t = table_require(name);
    d.sel.nsrc = 1;
    d.sel.t[0] = d.t;
    if (is_update) {
        int cap = 8;
        expect_kw("set");
        d.acol = amalloc(sizeof(int) * (size_t)cap);
        d.aexpr = amalloc(sizeof(Expr *) * (size_t)cap);
        do {
            char *cn = parse_ident();
            int c, found = -1;
            for (c = 0; c < d.t->ncols; c++)
                if (ieq(d.t->cols[c].name, cn)) found = c;
            if (found < 0) fail("table %s has no column named %s", d.t->name, cn);
            expect_sym("=");
            if (d.nassign >= cap) {
                int *na = amalloc(sizeof(int) * (size_t)cap * 2);
                Expr **ne = amalloc(sizeof(Expr *) * (size_t)cap * 2);
                memcpy(na, d.acol, sizeof(int) * (size_t)d.nassign);
                memcpy(ne, d.aexpr, sizeof(Expr *) * (size_t)d.nassign);
                d.acol = na;
                d.aexpr = ne;
                cap *= 2;
            }
            d.acol[d.nassign] = found;
            d.aexpr[d.nassign++] = parse_or();
        } while (accept_sym(","));
    }
    if (accept_kw("where")) d.where = parse_or();
    if (peek()->k != TK_END) syntax_err("unexpected token");

    g_nsrc = 1;
    g_src[0].t = d.t;
    g_src[0].alias = name;
    bind_expr(d.where);
    if (d.where && contains_agg(d.where)) fail("aggregate functions are not allowed in WHERE");
    for (int a = 0; a < d.nassign; a++) {
        bind_expr(d.aexpr[a]);
        if (contains_agg(d.aexpr[a])) fail("aggregate functions are not allowed in SET");
    }
    /* phase 1: scan and collect (so the tree is not modified while iterating) */
    run_scan(&d.sel, cb_dml, &d);
    /* phase 2: apply */
    for (i = 0; i < d.pv.n; i++) {
        if (is_update) btree_update(d.t->root, d.pv.v[i].key, d.pv.v[i].rec, d.pv.v[i].len);
        else btree_delete(d.t->root, d.pv.v[i].key);
    }
}

static void skip_to_comma_or_close(void) {
    int depth = 0;
    for (;;) {
        Token *t = peek();
        if (t->k == TK_END) syntax_err("unterminated column list");
        if (is_sym(t, "(")) depth++;
        else if (is_sym(t, ")")) { if (depth == 0) return; depth--; }
        else if (depth == 0 && is_sym(t, ",")) return;
        P.pos++;
    }
}

static void do_create(void) {
    ColDef *cols = amalloc(sizeof(ColDef) * MAX_COLS);
    int nc = 0, i, ine = 0;
    char *name;
    expect_kw("table");
    if (accept_kw("if")) { expect_kw("not"); expect_kw("exists"); ine = 1; }
    name = parse_ident();
    expect_sym("(");
    do {
        Token *t = peek();
        if (is_kw(t, "primary") || is_kw(t, "unique") || is_kw(t, "foreign") ||
            is_kw(t, "check") || is_kw(t, "constraint")) {
            skip_to_comma_or_close();
            continue;
        }
        if (nc >= MAX_COLS) fail("too many columns (max %d)", MAX_COLS);
        cols[nc].name = parse_ident();
        for (i = 0; i < nc; i++)
            if (ieq(cols[i].name, cols[nc].name)) fail("duplicate column name: %s", cols[nc].name);
        cols[nc].type = C_TEXT;
        t = peek();
        if (t->k == TK_IDENT && !is_kw(t, "primary") && !is_kw(t, "not") && !is_kw(t, "null") &&
            !is_kw(t, "unique") && !is_kw(t, "default") && !is_kw(t, "check") &&
            !is_kw(t, "references")) {
            if (is_kw(t, "integer") || is_kw(t, "int") || is_kw(t, "bigint") ||
                is_kw(t, "smallint") || is_kw(t, "tinyint"))
                cols[nc].type = C_INT;
            else if (is_kw(t, "real") || is_kw(t, "float") || is_kw(t, "double"))
                cols[nc].type = C_REAL;
            P.pos++;
        }
        skip_to_comma_or_close(); /* (n,m) sizes and column constraints are accepted, not enforced */
        nc++;
    } while (accept_sym(","));
    expect_sym(")");
    if (nc == 0) fail("a table needs at least one column");
    if (peek()->k != TK_END) syntax_err("unexpected token");
    if (table_find(name)) {
        if (ine) return;
        fail("table %s already exists", name);
    }
    {
        size_t cap = 16, o = 0;
        for (i = 0; i < nc; i++) cap += strlen(cols[i].name) + 3;
        char *schema = amalloc(cap);
        Value v[3];
        uint8_t rec[MAX_REC];
        for (i = 0; i < nc; i++) {
            size_t l = strlen(cols[i].name);
            memcpy(schema + o, cols[i].name, l); o += l;
            schema[o++] = '\x01';
            schema[o++] = cols[i].type == C_INT ? 'I' : cols[i].type == C_REAL ? 'R' : 'T';
            schema[o++] = '\x01';
        }
        v[0] = v_text(name, strlen(name));
        v[1] = v_int((int64_t)btree_create());
        v[2] = v_text(schema, o);
        size_t len = rec_size(v, 3);
        if (len > MAX_REC) fail("table definition too large");
        rec_encode(v, 3, rec);
        btree_insert(pg.catalog_root, btree_alloc_key(pg.catalog_root), rec, len);
    }
}

static void do_drop(void) {
    int ie = 0;
    char *name;
    Table *t;
    expect_kw("table");
    if (accept_kw("if")) { expect_kw("exists"); ie = 1; }
    name = parse_ident();
    if (peek()->k != TK_END) syntax_err("unexpected token");
    t = table_find(name);
    if (!t) {
        if (ie) return;
        fail("no such table: %s", name);
    }
    btree_free(t->root);
    btree_delete(pg.catalog_root, t->ckey);
}

static int cmp_names(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    while (*x && *y) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) return cx - cy;
        x++; y++;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

static void print_schema(const Table *t) {
    int c;
    printf("CREATE TABLE %s (", t->name);
    for (c = 0; c < t->ncols; c++)
        printf("%s%s %s", c ? ", " : "", t->cols[c].name, type_name(t->cols[c].type));
    printf(");\n");
}

static void exec_dot(const char *line) {
    char cmd[64], arg[256];
    int n = sscanf(line, "%63s %255s", cmd, arg);
    if (n >= 1 && strcmp(cmd, ".tables") == 0) {
        Cursor c;
        int64_t key;
        const uint8_t *pl;
        uint16_t len;
        Value v[3];
        char **names = amalloc(sizeof(char *) * 1024);
        size_t nn = 0, cap = 1024, i;
        cur_first(&c, pg.catalog_root);
        while (cur_next(&c, &key, &pl, &len)) {
            rec_decode(pl, len, v, 3);
            if (nn >= cap) {
                char **nnames = amalloc(sizeof(char *) * cap * 2);
                memcpy(nnames, names, sizeof(char *) * nn);
                names = nnames;
                cap *= 2;
            }
            names[nn++] = astrndup(v[0].u.s.p, v[0].u.s.n);
        }
        qsort(names, nn, sizeof(char *), cmp_names);
        for (i = 0; i < nn; i++) printf("%s\n", names[i]);
    } else if (n >= 1 && strcmp(cmd, ".schema") == 0) {
        if (n >= 2) {
            print_schema(table_require(arg));
        } else {
            Cursor c;
            int64_t key;
            const uint8_t *pl;
            uint16_t len;
            Value v[3];
            cur_first(&c, pg.catalog_root);
            while (cur_next(&c, &key, &pl, &len)) {
                rec_decode(pl, len, v, 3);
                print_schema(table_from_record(v, key));
            }
        }
    } else {
        fail("unknown command: %s", line);
    }
}

static void exec_statement(Stmt *st) {
    Token *t;
    if (st->dot) { exec_dot(st->dotline); return; }
    P.t = st->t;
    P.n = st->n;
    P.pos = 0;
    P.src = st->src;
    t = peek();
    if (is_kw(t, "select")) { P.pos++; do_select(); }
    else if (is_kw(t, "insert")) { P.pos++; do_insert(); }
    else if (is_kw(t, "update")) { P.pos++; do_update_delete(1); }
    else if (is_kw(t, "delete")) { P.pos++; do_update_delete(0); }
    else if (is_kw(t, "create")) { P.pos++; do_create(); }
    else if (is_kw(t, "drop")) { P.pos++; do_drop(); }
    else if (is_kw(t, "begin") || is_kw(t, "commit")) { /* every run is one implicit transaction */ }
    else syntax_err("unknown statement");
}

/* ======================================================================
 * main
 * ==================================================================== */
static char *read_all_stdin(size_t *out_len) {
    size_t cap = 1 << 16, len = 0, n;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap *= 2;
        }
    }
    *out_len = len;
    return buf;
}

int main(int argc, char **argv) {
    volatile int rc = 0;
    char *input;
    size_t len = 0;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <database-file>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (setjmp(err_jmp)) {
        fprintf(stderr, "Error: %s\n", err_msg);
        return 1;
    }
    pager_open(argv[1]);
    input = read_all_stdin(&len);
    if (!input) { fprintf(stderr, "Error: out of memory reading input\n"); return 1; }
    g_in = input;
    g_end = input + len;

    for (;;) {
        Stmt st;
        if (setjmp(err_jmp) != 0) { /* a failed statement only aborts itself */
            fflush(stdout);
            fprintf(stderr, "Error: %s\n", err_msg);
            rc = 1;
        }
        arena_reset();
        if (!lex_statement(&st)) break;
        exec_statement(&st);
    }

    if (setjmp(err_jmp) != 0) {
        fflush(stdout);
        fprintf(stderr, "Error: %s\n", err_msg);
        return 1;
    }
    fflush(stdout);
    pager_flush();
    return rc;
}
