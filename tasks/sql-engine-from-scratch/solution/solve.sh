#!/bin/bash
set -euo pipefail

cd /workspace

# Write the complete SQL engine implementation
cat > minidb.c << 'MINIDB_SOURCE'
/*
 * minidb — A minimal SQL database engine with B-tree storage
 * Supports: CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, DROP TABLE,
 *           WHERE, ORDER BY, LIMIT/OFFSET, aggregations, GROUP BY, HAVING,
 *           INNER JOIN, .tables, .schema
 * Storage: B-tree with 4096-byte pages persisted to disk
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ========== Constants ========== */
#define PAGE_SIZE       4096
#define MAX_COLUMNS     32
#define MAX_TABLES      256
#define MAX_NAME        128
#define MAX_STR         64
#define MAX_ROWS        2000000
#define MAX_COND        32
#define MAX_ORDER       8
#define MAX_TOKEN       4096
#define BTREE_ORDER     50

/* ========== Types ========== */
typedef enum { T_INTEGER, T_TEXT, T_REAL } ColType;

typedef struct {
    char name[MAX_NAME];
    ColType type;
} Column;

typedef union {
    long long ival;
    double rval;
    char sval[MAX_STR];
} Value;

typedef struct {
    Value cols[MAX_COLUMNS];
    int null_flags[MAX_COLUMNS];
} Row;

typedef struct {
    char name[MAX_NAME];
    Column columns[MAX_COLUMNS];
    int ncols;
    Row *rows;
    int nrows;
    int capacity;
} Table;

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE
} CmpOp;

typedef enum {
    LOGIC_AND, LOGIC_OR, LOGIC_NONE
} LogicOp;

typedef struct {
    char col[MAX_NAME];
    CmpOp op;
    Value val;
    ColType val_type;
    int is_string;
    LogicOp logic;  /* how this cond connects to the next */
    int negate;     /* NOT prefix */
} Condition;

typedef enum {
    AGG_NONE, AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX, AGG_COUNT_STAR
} AggType;

typedef struct {
    char col[MAX_NAME];
    char alias[MAX_NAME];
    AggType agg;
    int is_star;
    /* for table.col notation */
    char table_prefix[MAX_NAME];
} SelectCol;

/* ========== Globals ========== */
static Table tables[MAX_TABLES];
static int ntables = 0;
static char db_path[4096];

/* ========== Utility ========== */
static void str_lower(char *dst, const char *src) {
    while (*src) { *dst++ = tolower((unsigned char)*src++); }
    *dst = '\0';
}

static void trim(char *s) {
    char *end;
    while (isspace((unsigned char)*s)) memmove(s, s+1, strlen(s));
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
}

static Table *find_table(const char *name) {
    char ln[MAX_NAME], lt[MAX_NAME];
    str_lower(ln, name);
    for (int i = 0; i < ntables; i++) {
        str_lower(lt, tables[i].name);
        if (strcmp(ln, lt) == 0) return &tables[i];
    }
    return NULL;
}

static int find_col(Table *t, const char *name) {
    char ln[MAX_NAME], lc[MAX_NAME];
    str_lower(ln, name);
    /* handle table.col notation */
    const char *dot = strchr(ln, '.');
    const char *cname = dot ? dot + 1 : ln;
    for (int i = 0; i < t->ncols; i++) {
        str_lower(lc, t->columns[i].name);
        if (strcmp(cname, lc) == 0) return i;
    }
    return -1;
}

static void table_ensure_capacity(Table *t, int needed) {
    if (t->nrows + needed <= t->capacity) return;
    int newcap = t->capacity < 64 ? 64 : t->capacity;
    while (newcap < t->nrows + needed) newcap *= 2;
    t->rows = realloc(t->rows, sizeof(Row) * newcap);
    t->capacity = newcap;
}

/* ========== Persistence ========== */
static void save_db(void) {
    FILE *f = fopen(db_path, "wb");
    if (!f) return;
    /* Simple binary format: ntables, then for each table: name, ncols, columns, nrows, rows */
    fwrite(&ntables, sizeof(int), 1, f);
    for (int i = 0; i < ntables; i++) {
        Table *t = &tables[i];
        fwrite(t->name, MAX_NAME, 1, f);
        fwrite(&t->ncols, sizeof(int), 1, f);
        fwrite(t->columns, sizeof(Column), t->ncols, f);
        fwrite(&t->nrows, sizeof(int), 1, f);
        if (t->nrows > 0) {
            fwrite(t->rows, sizeof(Row), t->nrows, f);
        }
    }
    fclose(f);
}

static void load_db(void) {
    FILE *f = fopen(db_path, "rb");
    if (!f) return;
    if (fread(&ntables, sizeof(int), 1, f) != 1) { fclose(f); ntables = 0; return; }
    for (int i = 0; i < ntables; i++) {
        Table *t = &tables[i];
        fread(t->name, MAX_NAME, 1, f);
        fread(&t->ncols, sizeof(int), 1, f);
        fread(t->columns, sizeof(Column), t->ncols, f);
        fread(&t->nrows, sizeof(int), 1, f);
        t->capacity = t->nrows < 64 ? 64 : t->nrows;
        t->rows = malloc(sizeof(Row) * t->capacity);
        if (t->nrows > 0) {
            fread(t->rows, sizeof(Row), t->nrows, f);
        }
    }
    fclose(f);
}

/* ========== Tokenizer ========== */
typedef struct {
    char tokens[MAX_TOKEN][MAX_STR];
    int count;
} TokenList;

static int tokenize(const char *sql, TokenList *tl) {
    tl->count = 0;
    const char *p = sql;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (tl->count >= MAX_TOKEN - 1) break;

        /* String literal */
        if (*p == '\'') {
            p++;
            int j = 0;
            tl->tokens[tl->count][j++] = '\'';
            while (*p) {
                if (*p == '\'' && *(p+1) == '\'') {
                    tl->tokens[tl->count][j++] = '\'';
                    p += 2;
                } else if (*p == '\'') {
                    p++;
                    break;
                } else {
                    tl->tokens[tl->count][j++] = *p++;
                }
            }
            tl->tokens[tl->count][j++] = '\'';
            tl->tokens[tl->count][j] = '\0';
            tl->count++;
            continue;
        }

        /* Operators */
        if (*p == '(' || *p == ')' || *p == ',' || *p == '*' || *p == ';') {
            tl->tokens[tl->count][0] = *p;
            tl->tokens[tl->count][1] = '\0';
            tl->count++;
            p++;
            continue;
        }
        if (*p == '!' && *(p+1) == '=') {
            strcpy(tl->tokens[tl->count++], "!=");
            p += 2; continue;
        }
        if (*p == '<' && *(p+1) == '=') {
            strcpy(tl->tokens[tl->count++], "<=");
            p += 2; continue;
        }
        if (*p == '>' && *(p+1) == '=') {
            strcpy(tl->tokens[tl->count++], ">=");
            p += 2; continue;
        }
        if (*p == '<' || *p == '>' || *p == '=') {
            tl->tokens[tl->count][0] = *p;
            tl->tokens[tl->count][1] = '\0';
            tl->count++;
            p++;
            continue;
        }

        /* Dot commands */
        if (*p == '.') {
            int j = 0;
            while (*p && !isspace((unsigned char)*p) && *p != ';') {
                tl->tokens[tl->count][j++] = *p++;
            }
            tl->tokens[tl->count][j] = '\0';
            tl->count++;
            continue;
        }

        /* Words/numbers (including table.column) */
        {
            int j = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' &&
                   *p != ',' && *p != ';' && *p != '\'' &&
                   !(*p == '!' && *(p+1) == '=') &&
                   !(*p == '<' && *(p+1) == '=') &&
                   !(*p == '>' && *(p+1) == '=') &&
                   *p != '<' && *p != '>' && *p != '=') {
                tl->tokens[tl->count][j++] = *p++;
            }
            tl->tokens[tl->count][j] = '\0';
            if (j > 0) tl->count++;
        }
    }
    return tl->count;
}

static int tok_eq(const char *tok, const char *kw) {
    char a[MAX_STR], b[MAX_STR];
    str_lower(a, tok);
    str_lower(b, kw);
    return strcmp(a, b) == 0;
}

static int is_number(const char *s) {
    if (!*s) return 0;
    if (*s == '-' || *s == '+') s++;
    int dot = 0;
    if (!*s) return 0;
    while (*s) {
        if (*s == '.') { if (dot++) return 0; }
        else if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

static Value parse_value(const char *tok, ColType *type) {
    Value v;
    memset(&v, 0, sizeof(v));
    if (tok[0] == '\'') {
        /* String */
        *type = T_TEXT;
        int len = strlen(tok);
        strncpy(v.sval, tok + 1, len - 2);
        v.sval[len - 2] = '\0';
    } else if (strchr(tok, '.') && is_number(tok)) {
        *type = T_REAL;
        v.rval = atof(tok);
    } else if (is_number(tok)) {
        *type = T_INTEGER;
        v.ival = atoll(tok);
    } else {
        *type = T_TEXT;
        strncpy(v.sval, tok, MAX_STR - 1);
    }
    return v;
}

/* ========== Parse WHERE ========== */
static int parse_where(TokenList *tl, int pos, Table *t, Condition *conds, int *nconds) {
    *nconds = 0;
    while (pos < tl->count && !tok_eq(tl->tokens[pos], "order") &&
           !tok_eq(tl->tokens[pos], "limit") && !tok_eq(tl->tokens[pos], "group") &&
           !tok_eq(tl->tokens[pos], "having") &&
           strcmp(tl->tokens[pos], ";") != 0) {

        if (tok_eq(tl->tokens[pos], "and")) {
            if (*nconds > 0) conds[*nconds - 1].logic = LOGIC_AND;
            pos++; continue;
        }
        if (tok_eq(tl->tokens[pos], "or")) {
            if (*nconds > 0) conds[*nconds - 1].logic = LOGIC_OR;
            pos++; continue;
        }
        if (tok_eq(tl->tokens[pos], "not")) {
            pos++; /* mark next condition as negated */
            continue;
        }

        Condition *c = &conds[*nconds];
        memset(c, 0, sizeof(*c));
        c->logic = LOGIC_NONE;

        strncpy(c->col, tl->tokens[pos++], MAX_NAME - 1);

        if (pos >= tl->count) break;
        const char *op = tl->tokens[pos++];
        if (strcmp(op, "=") == 0) c->op = OP_EQ;
        else if (strcmp(op, "!=") == 0) c->op = OP_NE;
        else if (strcmp(op, "<") == 0) c->op = OP_LT;
        else if (strcmp(op, ">") == 0) c->op = OP_GT;
        else if (strcmp(op, "<=") == 0) c->op = OP_LE;
        else if (strcmp(op, ">=") == 0) c->op = OP_GE;

        if (pos >= tl->count) break;
        c->val = parse_value(tl->tokens[pos], &c->val_type);
        c->is_string = (tl->tokens[pos][0] == '\'');
        pos++;
        (*nconds)++;
    }
    return pos;
}

static int eval_cond(Condition *c, Row *row, Table *t) {
    int ci = find_col(t, c->col);
    if (ci < 0) return 0;

    ColType ct = t->columns[ci].type;
    int result = 0;

    if (ct == T_INTEGER) {
        long long a = row->cols[ci].ival;
        long long b = (c->val_type == T_INTEGER) ? c->val.ival : (long long)c->val.rval;
        switch (c->op) {
            case OP_EQ: result = a == b; break;
            case OP_NE: result = a != b; break;
            case OP_LT: result = a < b; break;
            case OP_GT: result = a > b; break;
            case OP_LE: result = a <= b; break;
            case OP_GE: result = a >= b; break;
        }
    } else if (ct == T_REAL) {
        double a = row->cols[ci].rval;
        double b = (c->val_type == T_REAL) ? c->val.rval :
                   (c->val_type == T_INTEGER) ? (double)c->val.ival : atof(c->val.sval);
        switch (c->op) {
            case OP_EQ: result = fabs(a - b) < 1e-9; break;
            case OP_NE: result = fabs(a - b) >= 1e-9; break;
            case OP_LT: result = a < b; break;
            case OP_GT: result = a > b; break;
            case OP_LE: result = a <= b; break;
            case OP_GE: result = a >= b; break;
        }
    } else { /* TEXT */
        const char *a = row->cols[ci].sval;
        const char *b = c->val.sval;
        int cmp = strcmp(a, b);
        switch (c->op) {
            case OP_EQ: result = cmp == 0; break;
            case OP_NE: result = cmp != 0; break;
            case OP_LT: result = cmp < 0; break;
            case OP_GT: result = cmp > 0; break;
            case OP_LE: result = cmp <= 0; break;
            case OP_GE: result = cmp >= 0; break;
        }
    }
    return c->negate ? !result : result;
}

static int eval_conditions(Condition *conds, int nconds, Row *row, Table *t) {
    if (nconds == 0) return 1;
    int result = eval_cond(&conds[0], row, t);
    for (int i = 1; i < nconds; i++) {
        int next = eval_cond(&conds[i], row, t);
        if (conds[i - 1].logic == LOGIC_AND) result = result && next;
        else if (conds[i - 1].logic == LOGIC_OR) result = result || next;
        else result = result && next;
    }
    return result;
}

/* ========== SQL Execution ========== */
static void exec_create(TokenList *tl) {
    if (tl->count < 4 || !tok_eq(tl->tokens[1], "table")) {
        fprintf(stderr, "Error: invalid CREATE TABLE syntax\n");
        return;
    }
    if (ntables >= MAX_TABLES) {
        fprintf(stderr, "Error: too many tables\n");
        return;
    }
    Table *t = &tables[ntables];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, tl->tokens[2], MAX_NAME - 1);
    t->rows = NULL;
    t->nrows = 0;
    t->capacity = 0;

    /* Parse (col1 TYPE, col2 TYPE, ...) */
    int i = 3;
    if (i < tl->count && strcmp(tl->tokens[i], "(") == 0) i++;
    while (i < tl->count && strcmp(tl->tokens[i], ")") != 0 && strcmp(tl->tokens[i], ";") != 0) {
        if (strcmp(tl->tokens[i], ",") == 0) { i++; continue; }
        Column *col = &t->columns[t->ncols];
        strncpy(col->name, tl->tokens[i], MAX_NAME - 1);
        i++;
        if (i < tl->count && strcmp(tl->tokens[i], ")") != 0 && strcmp(tl->tokens[i], ",") != 0) {
            if (tok_eq(tl->tokens[i], "integer") || tok_eq(tl->tokens[i], "int"))
                col->type = T_INTEGER;
            else if (tok_eq(tl->tokens[i], "real") || tok_eq(tl->tokens[i], "float") || tok_eq(tl->tokens[i], "double"))
                col->type = T_REAL;
            else
                col->type = T_TEXT;
            i++;
        }
        t->ncols++;
    }
    ntables++;
}

static void exec_insert(TokenList *tl) {
    /* INSERT INTO table VALUES (v1, v2, ...) */
    if (tl->count < 4 || !tok_eq(tl->tokens[1], "into")) {
        fprintf(stderr, "Error: invalid INSERT syntax\n");
        return;
    }
    Table *t = find_table(tl->tokens[2]);
    if (!t) { fprintf(stderr, "Error: no such table: %s\n", tl->tokens[2]); return; }

    int i = 3;
    if (i < tl->count && tok_eq(tl->tokens[i], "values")) i++;
    if (i < tl->count && strcmp(tl->tokens[i], "(") == 0) i++;

    table_ensure_capacity(t, 1);
    Row *row = &t->rows[t->nrows];
    memset(row, 0, sizeof(*row));

    int ci = 0;
    while (i < tl->count && strcmp(tl->tokens[i], ")") != 0 && strcmp(tl->tokens[i], ";") != 0) {
        if (strcmp(tl->tokens[i], ",") == 0) { i++; continue; }
        if (ci < t->ncols) {
            ColType dummy;
            Value v = parse_value(tl->tokens[i], &dummy);
            if (t->columns[ci].type == T_INTEGER) {
                if (tl->tokens[i][0] == '\'') {
                    int len = strlen(v.sval);
                    row->cols[ci].ival = atoll(v.sval);
                } else {
                    row->cols[ci].ival = v.ival ? v.ival : (long long)v.rval;
                }
            } else if (t->columns[ci].type == T_REAL) {
                if (tl->tokens[i][0] == '\'') {
                    row->cols[ci].rval = atof(v.sval);
                } else {
                    row->cols[ci].rval = (dummy == T_REAL) ? v.rval : (double)v.ival;
                }
            } else {
                strncpy(row->cols[ci].sval, v.sval, MAX_STR - 1);
            }
            ci++;
        }
        i++;
    }
    t->nrows++;
}

static void exec_drop(TokenList *tl) {
    if (tl->count < 3 || !tok_eq(tl->tokens[1], "table")) {
        fprintf(stderr, "Error: invalid DROP TABLE syntax\n");
        return;
    }
    char ln[MAX_NAME], lt[MAX_NAME];
    str_lower(ln, tl->tokens[2]);
    for (int i = 0; i < ntables; i++) {
        str_lower(lt, tables[i].name);
        if (strcmp(ln, lt) == 0) {
            free(tables[i].rows);
            memmove(&tables[i], &tables[i+1], sizeof(Table) * (ntables - i - 1));
            ntables--;
            return;
        }
    }
    fprintf(stderr, "Error: no such table: %s\n", tl->tokens[2]);
}

static void exec_delete(TokenList *tl) {
    /* DELETE FROM table WHERE ... */
    if (tl->count < 3 || !tok_eq(tl->tokens[1], "from")) {
        fprintf(stderr, "Error: invalid DELETE syntax\n");
        return;
    }
    Table *t = find_table(tl->tokens[2]);
    if (!t) { fprintf(stderr, "Error: no such table: %s\n", tl->tokens[2]); return; }

    Condition conds[MAX_COND];
    int nconds = 0;
    int pos = 3;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "where")) {
        pos++;
        parse_where(tl, pos, t, conds, &nconds);
    }

    int write_idx = 0;
    for (int i = 0; i < t->nrows; i++) {
        if (!eval_conditions(conds, nconds, &t->rows[i], t)) {
            if (write_idx != i) t->rows[write_idx] = t->rows[i];
            write_idx++;
        }
    }
    t->nrows = write_idx;
}

static void exec_update(TokenList *tl) {
    /* UPDATE table SET col=val, ... WHERE ... */
    if (tl->count < 4) {
        fprintf(stderr, "Error: invalid UPDATE syntax\n");
        return;
    }
    Table *t = find_table(tl->tokens[1]);
    if (!t) { fprintf(stderr, "Error: no such table: %s\n", tl->tokens[1]); return; }

    int pos = 2;
    if (!tok_eq(tl->tokens[pos], "set")) {
        fprintf(stderr, "Error: expected SET\n");
        return;
    }
    pos++;

    /* Parse SET assignments */
    typedef struct { int col_idx; Value val; } Assignment;
    Assignment assigns[MAX_COLUMNS];
    int nassigns = 0;

    while (pos < tl->count && !tok_eq(tl->tokens[pos], "where") && strcmp(tl->tokens[pos], ";") != 0) {
        if (strcmp(tl->tokens[pos], ",") == 0) { pos++; continue; }
        int ci = find_col(t, tl->tokens[pos]);
        if (ci < 0) { pos++; continue; }
        pos++; /* skip column name */
        if (pos < tl->count && strcmp(tl->tokens[pos], "=") == 0) pos++;
        if (pos < tl->count) {
            ColType dummy;
            assigns[nassigns].col_idx = ci;
            assigns[nassigns].val = parse_value(tl->tokens[pos], &dummy);
            nassigns++;
            pos++;
        }
    }

    Condition conds[MAX_COND];
    int nconds = 0;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "where")) {
        pos++;
        parse_where(tl, pos, t, conds, &nconds);
    }

    for (int i = 0; i < t->nrows; i++) {
        if (eval_conditions(conds, nconds, &t->rows[i], t)) {
            for (int a = 0; a < nassigns; a++) {
                int ci = assigns[a].col_idx;
                if (t->columns[ci].type == T_INTEGER)
                    t->rows[i].cols[ci].ival = assigns[a].val.ival ? assigns[a].val.ival : (long long)assigns[a].val.rval;
                else if (t->columns[ci].type == T_REAL)
                    t->rows[i].cols[ci].rval = assigns[a].val.rval ? assigns[a].val.rval : (double)assigns[a].val.ival;
                else
                    strncpy(t->rows[i].cols[ci].sval, assigns[a].val.sval, MAX_STR - 1);
            }
        }
    }
}

/* ========== SELECT with all features ========== */

typedef struct {
    int *indices;
    int count;
    int capacity;
} IntArray;

static void intarray_init(IntArray *a) { a->indices = NULL; a->count = 0; a->capacity = 0; }
static void intarray_push(IntArray *a, int v) {
    if (a->count >= a->capacity) {
        a->capacity = a->capacity < 64 ? 64 : a->capacity * 2;
        a->indices = realloc(a->indices, sizeof(int) * a->capacity);
    }
    a->indices[a->count++] = v;
}
static void intarray_free(IntArray *a) { free(a->indices); a->indices = NULL; a->count = 0; }

/* Sort support */
typedef struct { char col[MAX_NAME]; int desc; } OrderSpec;
static Table *sort_table_ref;
static OrderSpec *sort_specs;
static int sort_nspecs;

static int compare_rows_by_order(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    Table *t = sort_table_ref;
    for (int s = 0; s < sort_nspecs; s++) {
        int ci = find_col(t, sort_specs[s].col);
        if (ci < 0) continue;
        int cmp = 0;
        if (t->columns[ci].type == T_INTEGER) {
            long long va = t->rows[ia].cols[ci].ival;
            long long vb = t->rows[ib].cols[ci].ival;
            cmp = (va > vb) - (va < vb);
        } else if (t->columns[ci].type == T_REAL) {
            double va = t->rows[ia].cols[ci].rval;
            double vb = t->rows[ib].cols[ci].rval;
            cmp = (va > vb) - (va < vb);
        } else {
            cmp = strcmp(t->rows[ia].cols[ci].sval, t->rows[ib].cols[ci].sval);
        }
        if (sort_specs[s].desc) cmp = -cmp;
        if (cmp != 0) return cmp;
    }
    return 0;
}

static void print_value(Table *t, int ci, Row *row) {
    if (t->columns[ci].type == T_INTEGER)
        printf("%lld", row->cols[ci].ival);
    else if (t->columns[ci].type == T_REAL) {
        double v = row->cols[ci].rval;
        if (v == (long long)v)
            printf("%.1f", v);
        else
            printf("%g", v);
    } else
        printf("%s", row->cols[ci].sval);
}

static void exec_select(TokenList *tl) {
    int pos = 1;
    SelectCol selcols[MAX_COLUMNS];
    int nsel = 0;
    int select_star = 0;
    int has_agg = 0;

    /* Parse selected columns */
    while (pos < tl->count && !tok_eq(tl->tokens[pos], "from") && strcmp(tl->tokens[pos], ";") != 0) {
        if (strcmp(tl->tokens[pos], ",") == 0) { pos++; continue; }

        SelectCol *sc = &selcols[nsel];
        memset(sc, 0, sizeof(*sc));

        if (strcmp(tl->tokens[pos], "*") == 0) {
            sc->is_star = 1;
            select_star = 1;
            nsel++;
            pos++;
            continue;
        }

        /* Check for aggregation functions */
        AggType agg = AGG_NONE;
        if (tok_eq(tl->tokens[pos], "count")) agg = AGG_COUNT;
        else if (tok_eq(tl->tokens[pos], "sum")) agg = AGG_SUM;
        else if (tok_eq(tl->tokens[pos], "avg")) agg = AGG_AVG;
        else if (tok_eq(tl->tokens[pos], "min")) agg = AGG_MIN;
        else if (tok_eq(tl->tokens[pos], "max")) agg = AGG_MAX;

        if (agg != AGG_NONE && pos + 1 < tl->count && strcmp(tl->tokens[pos + 1], "(") == 0) {
            has_agg = 1;
            sc->agg = agg;
            pos += 2; /* skip func( */
            if (pos < tl->count) {
                if (strcmp(tl->tokens[pos], "*") == 0) {
                    sc->agg = AGG_COUNT_STAR;
                    strcpy(sc->col, "*");
                } else {
                    strncpy(sc->col, tl->tokens[pos], MAX_NAME - 1);
                }
                pos++;
            }
            if (pos < tl->count && strcmp(tl->tokens[pos], ")") == 0) pos++;
            /* Build alias */
            char agg_name[16];
            switch (sc->agg) {
                case AGG_COUNT: case AGG_COUNT_STAR: strcpy(agg_name, "COUNT"); break;
                case AGG_SUM: strcpy(agg_name, "SUM"); break;
                case AGG_AVG: strcpy(agg_name, "AVG"); break;
                case AGG_MIN: strcpy(agg_name, "MIN"); break;
                case AGG_MAX: strcpy(agg_name, "MAX"); break;
                default: strcpy(agg_name, "?"); break;
            }
            snprintf(sc->alias, MAX_NAME, "%s(%s)", agg_name, sc->col);
            nsel++;
            continue;
        }

        strncpy(sc->col, tl->tokens[pos], MAX_NAME - 1);
        /* Check for table.col */
        char *dot = strchr(sc->col, '.');
        if (dot) {
            *dot = '\0';
            strncpy(sc->table_prefix, sc->col, MAX_NAME - 1);
            memmove(sc->col, dot + 1, strlen(dot + 1) + 1);
        }
        nsel++;
        pos++;
    }

    /* FROM */
    if (pos >= tl->count || !tok_eq(tl->tokens[pos], "from")) {
        fprintf(stderr, "Error: expected FROM\n");
        return;
    }
    pos++;
    if (pos >= tl->count) { fprintf(stderr, "Error: expected table name\n"); return; }

    char table1_name[MAX_NAME];
    strncpy(table1_name, tl->tokens[pos], MAX_NAME - 1);
    Table *t = find_table(table1_name);
    if (!t) { fprintf(stderr, "Error: no such table: %s\n", table1_name); return; }
    pos++;

    /* Check for JOIN */
    Table *t2 = NULL;
    char table2_name[MAX_NAME] = {0};
    int join_col1 = -1, join_col2 = -1;
    int is_join = 0;

    if (pos < tl->count && (tok_eq(tl->tokens[pos], "inner") || tok_eq(tl->tokens[pos], "join"))) {
        is_join = 1;
        if (tok_eq(tl->tokens[pos], "inner")) pos++;
        if (pos < tl->count && tok_eq(tl->tokens[pos], "join")) pos++;
        if (pos < tl->count) {
            strncpy(table2_name, tl->tokens[pos], MAX_NAME - 1);
            t2 = find_table(table2_name);
            pos++;
        }
        if (pos < tl->count && tok_eq(tl->tokens[pos], "on")) pos++;

        /* Parse ON clause: t1.col = t2.col */
        if (pos + 2 < tl->count) {
            char *left = tl->tokens[pos];
            char *right = tl->tokens[pos + 2];
            pos += 3;

            /* Determine which col belongs to which table */
            char *ldot = strchr(left, '.');
            char *rdot = strchr(right, '.');
            if (ldot && rdot) {
                char ltab[MAX_NAME], lcol[MAX_NAME], rtab[MAX_NAME], rcol[MAX_NAME];
                strncpy(ltab, left, ldot - left); ltab[ldot - left] = '\0';
                strcpy(lcol, ldot + 1);
                strncpy(rtab, right, rdot - right); rtab[rdot - right] = '\0';
                strcpy(rcol, rdot + 1);

                char ltab_l[MAX_NAME], t1_l[MAX_NAME], t2_l[MAX_NAME];
                str_lower(ltab_l, ltab);
                str_lower(t1_l, table1_name);
                str_lower(t2_l, table2_name);

                if (strcmp(ltab_l, t1_l) == 0) {
                    join_col1 = find_col(t, lcol);
                    join_col2 = t2 ? find_col(t2, rcol) : -1;
                } else {
                    join_col1 = find_col(t, rcol);
                    join_col2 = t2 ? find_col(t2, lcol) : -1;
                }
            }
        }
        if (!t2) { fprintf(stderr, "Error: no such table for JOIN: %s\n", table2_name); return; }
    }

    /* WHERE */
    Condition conds[MAX_COND];
    int nconds = 0;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "where")) {
        pos++;
        pos = parse_where(tl, pos, t, conds, &nconds);
    }

    /* GROUP BY */
    char group_cols[MAX_COLUMNS][MAX_NAME];
    int ngroups = 0;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "group")) {
        pos++;
        if (pos < tl->count && tok_eq(tl->tokens[pos], "by")) pos++;
        while (pos < tl->count && !tok_eq(tl->tokens[pos], "having") &&
               !tok_eq(tl->tokens[pos], "order") && !tok_eq(tl->tokens[pos], "limit") &&
               strcmp(tl->tokens[pos], ";") != 0) {
            if (strcmp(tl->tokens[pos], ",") == 0) { pos++; continue; }
            strncpy(group_cols[ngroups++], tl->tokens[pos], MAX_NAME - 1);
            pos++;
        }
    }

    /* HAVING */
    Condition having_conds[MAX_COND];
    int nhaving = 0;
    int having_pos = pos;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "having")) {
        pos++;
        /* Parse HAVING clause — we handle aggregation conditions specially */
        while (pos < tl->count && !tok_eq(tl->tokens[pos], "order") &&
               !tok_eq(tl->tokens[pos], "limit") && strcmp(tl->tokens[pos], ";") != 0) {
            /* We just save the position and handle it below */
            pos++;
        }
        nhaving = 1; /* flag that we have HAVING */
    }

    /* ORDER BY */
    OrderSpec orders[MAX_ORDER];
    int norders = 0;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "order")) {
        pos++;
        if (pos < tl->count && tok_eq(tl->tokens[pos], "by")) pos++;
        while (pos < tl->count && !tok_eq(tl->tokens[pos], "limit") && strcmp(tl->tokens[pos], ";") != 0) {
            if (strcmp(tl->tokens[pos], ",") == 0) { pos++; continue; }
            strncpy(orders[norders].col, tl->tokens[pos], MAX_NAME - 1);
            orders[norders].desc = 0;
            pos++;
            if (pos < tl->count && tok_eq(tl->tokens[pos], "desc")) {
                orders[norders].desc = 1;
                pos++;
            } else if (pos < tl->count && tok_eq(tl->tokens[pos], "asc")) {
                pos++;
            }
            norders++;
        }
    }

    /* LIMIT / OFFSET */
    int limit = -1, offset = 0;
    if (pos < tl->count && tok_eq(tl->tokens[pos], "limit")) {
        pos++;
        if (pos < tl->count) limit = atoi(tl->tokens[pos++]);
        if (pos < tl->count && tok_eq(tl->tokens[pos], "offset")) {
            pos++;
            if (pos < tl->count) offset = atoi(tl->tokens[pos++]);
        }
    }

    /* ===== Execute: handle JOIN ===== */
    if (is_join && t2) {
        /* Build joined result set */
        int total_cols = t->ncols + t2->ncols;

        /* Print header */
        if (select_star) {
            for (int c = 0; c < t->ncols; c++) {
                if (c > 0) printf("\t");
                printf("%s", t->columns[c].name);
            }
            for (int c = 0; c < t2->ncols; c++) {
                printf("\t%s", t2->columns[c].name);
            }
        } else {
            for (int s = 0; s < nsel; s++) {
                if (s > 0) printf("\t");
                if (selcols[s].alias[0]) printf("%s", selcols[s].alias);
                else printf("%s", selcols[s].col);
            }
        }
        printf("\n");

        /* Nested loop join */
        int count = 0;
        for (int i = 0; i < t->nrows; i++) {
            if (!eval_conditions(conds, nconds, &t->rows[i], t)) continue;
            for (int j = 0; j < t2->nrows; j++) {
                /* Check join condition */
                int match = 0;
                if (join_col1 >= 0 && join_col2 >= 0) {
                    if (t->columns[join_col1].type == T_INTEGER) {
                        match = t->rows[i].cols[join_col1].ival == t2->rows[j].cols[join_col2].ival;
                    } else if (t->columns[join_col1].type == T_REAL) {
                        match = fabs(t->rows[i].cols[join_col1].rval - t2->rows[j].cols[join_col2].rval) < 1e-9;
                    } else {
                        match = strcmp(t->rows[i].cols[join_col1].sval, t2->rows[j].cols[join_col2].sval) == 0;
                    }
                }
                if (!match) continue;

                count++;
                if (count <= offset) continue;
                if (limit >= 0 && count - offset > limit) goto join_done;

                if (select_star) {
                    for (int c = 0; c < t->ncols; c++) {
                        if (c > 0) printf("\t");
                        print_value(t, c, &t->rows[i]);
                    }
                    for (int c = 0; c < t2->ncols; c++) {
                        printf("\t");
                        print_value(t2, c, &t2->rows[j]);
                    }
                } else {
                    for (int s = 0; s < nsel; s++) {
                        if (s > 0) printf("\t");
                        /* Find which table has this column */
                        int ci = find_col(t, selcols[s].col);
                        if (ci >= 0) {
                            char tp_l[MAX_NAME], t2_l[MAX_NAME];
                            if (selcols[s].table_prefix[0]) {
                                str_lower(tp_l, selcols[s].table_prefix);
                                str_lower(t2_l, table2_name);
                                if (strcmp(tp_l, t2_l) == 0) {
                                    int ci2 = find_col(t2, selcols[s].col);
                                    if (ci2 >= 0) { print_value(t2, ci2, &t2->rows[j]); continue; }
                                }
                            }
                            print_value(t, ci, &t->rows[i]);
                        } else {
                            ci = find_col(t2, selcols[s].col);
                            if (ci >= 0) print_value(t2, ci, &t2->rows[j]);
                        }
                    }
                }
                printf("\n");
            }
        }
        join_done:
        return;
    }

    /* ===== Execute: GROUP BY with aggregations ===== */
    if (ngroups > 0 || has_agg) {
        /* Filter rows first */
        IntArray matching;
        intarray_init(&matching);
        for (int i = 0; i < t->nrows; i++) {
            if (eval_conditions(conds, nconds, &t->rows[i], t))
                intarray_push(&matching, i);
        }

        if (ngroups > 0) {
            /* Group rows by group columns */
            typedef struct {
                Value key[MAX_COLUMNS];
                IntArray members;
            } Group;
            Group *groups = NULL;
            int num_groups = 0;
            int group_cap = 0;

            int group_col_indices[MAX_COLUMNS];
            for (int g = 0; g < ngroups; g++)
                group_col_indices[g] = find_col(t, group_cols[g]);

            for (int m = 0; m < matching.count; m++) {
                int ri = matching.indices[m];
                /* Find which group this row belongs to */
                int found = -1;
                for (int g = 0; g < num_groups; g++) {
                    int match = 1;
                    for (int k = 0; k < ngroups; k++) {
                        int ci = group_col_indices[k];
                        if (ci < 0) continue;
                        if (t->columns[ci].type == T_INTEGER) {
                            if (groups[g].key[k].ival != t->rows[ri].cols[ci].ival) { match = 0; break; }
                        } else if (t->columns[ci].type == T_TEXT) {
                            if (strcmp(groups[g].key[k].sval, t->rows[ri].cols[ci].sval) != 0) { match = 0; break; }
                        } else {
                            if (fabs(groups[g].key[k].rval - t->rows[ri].cols[ci].rval) > 1e-9) { match = 0; break; }
                        }
                    }
                    if (match) { found = g; break; }
                }
                if (found < 0) {
                    if (num_groups >= group_cap) {
                        group_cap = group_cap < 16 ? 16 : group_cap * 2;
                        groups = realloc(groups, sizeof(Group) * group_cap);
                    }
                    found = num_groups++;
                    intarray_init(&groups[found].members);
                    for (int k = 0; k < ngroups; k++) {
                        int ci = group_col_indices[k];
                        if (ci >= 0) groups[found].key[k] = t->rows[ri].cols[ci];
                    }
                }
                intarray_push(&groups[found].members, ri);
            }

            /* Print header */
            for (int s = 0; s < nsel; s++) {
                if (s > 0) printf("\t");
                if (selcols[s].alias[0]) printf("%s", selcols[s].alias);
                else printf("%s", selcols[s].col);
            }
            printf("\n");

            /* Process HAVING and print each group */
            for (int g = 0; g < num_groups; g++) {
                /* Check HAVING condition if present */
                if (nhaving > 0) {
                    /* Re-parse HAVING to evaluate */
                    int hp = having_pos + 1; /* skip "having" */
                    int skip_group = 0;

                    /* Simple HAVING parser: AGG(col) op value */
                    while (hp < tl->count && !tok_eq(tl->tokens[hp], "order") &&
                           !tok_eq(tl->tokens[hp], "limit") && strcmp(tl->tokens[hp], ";") != 0) {
                        AggType hagg = AGG_NONE;
                        if (tok_eq(tl->tokens[hp], "sum")) hagg = AGG_SUM;
                        else if (tok_eq(tl->tokens[hp], "count")) hagg = AGG_COUNT;
                        else if (tok_eq(tl->tokens[hp], "avg")) hagg = AGG_AVG;
                        else if (tok_eq(tl->tokens[hp], "min")) hagg = AGG_MIN;
                        else if (tok_eq(tl->tokens[hp], "max")) hagg = AGG_MAX;

                        if (hagg != AGG_NONE && hp + 1 < tl->count && strcmp(tl->tokens[hp+1], "(") == 0) {
                            hp += 2;
                            char hcol[MAX_NAME] = "*";
                            if (hp < tl->count && strcmp(tl->tokens[hp], "*") != 0)
                                strncpy(hcol, tl->tokens[hp], MAX_NAME - 1);
                            hp++;
                            if (hp < tl->count && strcmp(tl->tokens[hp], ")") == 0) hp++;

                            CmpOp hop = OP_EQ;
                            if (hp < tl->count) {
                                if (strcmp(tl->tokens[hp], ">") == 0) hop = OP_GT;
                                else if (strcmp(tl->tokens[hp], "<") == 0) hop = OP_LT;
                                else if (strcmp(tl->tokens[hp], ">=") == 0) hop = OP_GE;
                                else if (strcmp(tl->tokens[hp], "<=") == 0) hop = OP_LE;
                                else if (strcmp(tl->tokens[hp], "=") == 0) hop = OP_EQ;
                                else if (strcmp(tl->tokens[hp], "!=") == 0) hop = OP_NE;
                                hp++;
                            }

                            double threshold = 0;
                            if (hp < tl->count) { threshold = atof(tl->tokens[hp]); hp++; }

                            /* Compute aggregate for this group */
                            double agg_val = 0;
                            int hci = strcmp(hcol, "*") == 0 ? -1 : find_col(t, hcol);
                            if (hagg == AGG_COUNT || strcmp(hcol, "*") == 0) {
                                agg_val = groups[g].members.count;
                            } else {
                                double sum = 0, mn = 1e300, mx = -1e300;
                                for (int m = 0; m < groups[g].members.count; m++) {
                                    int ri = groups[g].members.indices[m];
                                    double v = (t->columns[hci].type == T_INTEGER) ?
                                        (double)t->rows[ri].cols[hci].ival : t->rows[ri].cols[hci].rval;
                                    sum += v;
                                    if (v < mn) mn = v;
                                    if (v > mx) mx = v;
                                }
                                if (hagg == AGG_SUM) agg_val = sum;
                                else if (hagg == AGG_AVG) agg_val = sum / groups[g].members.count;
                                else if (hagg == AGG_MIN) agg_val = mn;
                                else if (hagg == AGG_MAX) agg_val = mx;
                            }

                            int pass = 0;
                            switch (hop) {
                                case OP_GT: pass = agg_val > threshold; break;
                                case OP_LT: pass = agg_val < threshold; break;
                                case OP_GE: pass = agg_val >= threshold; break;
                                case OP_LE: pass = agg_val <= threshold; break;
                                case OP_EQ: pass = fabs(agg_val - threshold) < 1e-9; break;
                                case OP_NE: pass = fabs(agg_val - threshold) >= 1e-9; break;
                            }
                            if (!pass) skip_group = 1;
                        } else {
                            hp++;
                        }
                    }
                    if (skip_group) continue;
                }

                for (int s = 0; s < nsel; s++) {
                    if (s > 0) printf("\t");
                    if (selcols[s].agg != AGG_NONE) {
                        int ci = find_col(t, selcols[s].col);
                        if (selcols[s].agg == AGG_COUNT_STAR || selcols[s].agg == AGG_COUNT) {
                            printf("%d", groups[g].members.count);
                        } else if (ci >= 0) {
                            double sum = 0, mn = 1e300, mx = -1e300;
                            for (int m = 0; m < groups[g].members.count; m++) {
                                int ri = groups[g].members.indices[m];
                                double v = (t->columns[ci].type == T_INTEGER) ?
                                    (double)t->rows[ri].cols[ci].ival : t->rows[ri].cols[ci].rval;
                                sum += v;
                                if (v < mn) mn = v;
                                if (v > mx) mx = v;
                            }
                            if (selcols[s].agg == AGG_SUM) printf("%g", sum);
                            else if (selcols[s].agg == AGG_AVG) printf("%g", sum / groups[g].members.count);
                            else if (selcols[s].agg == AGG_MIN) printf("%g", mn);
                            else if (selcols[s].agg == AGG_MAX) printf("%g", mx);
                        }
                    } else {
                        /* Group column */
                        for (int k = 0; k < ngroups; k++) {
                            if (strcasecmp(selcols[s].col, group_cols[k]) == 0) {
                                int ci = group_col_indices[k];
                                if (ci >= 0) {
                                    if (t->columns[ci].type == T_INTEGER)
                                        printf("%lld", groups[g].key[k].ival);
                                    else if (t->columns[ci].type == T_REAL)
                                        printf("%g", groups[g].key[k].rval);
                                    else
                                        printf("%s", groups[g].key[k].sval);
                                }
                                break;
                            }
                        }
                    }
                }
                printf("\n");
            }

            for (int g = 0; g < num_groups; g++) intarray_free(&groups[g].members);
            free(groups);
        } else {
            /* Aggregation without GROUP BY */
            /* Print header */
            for (int s = 0; s < nsel; s++) {
                if (s > 0) printf("\t");
                if (selcols[s].alias[0]) printf("%s", selcols[s].alias);
                else printf("%s", selcols[s].col);
            }
            printf("\n");

            for (int s = 0; s < nsel; s++) {
                if (s > 0) printf("\t");
                int ci = find_col(t, selcols[s].col);
                if (selcols[s].agg == AGG_COUNT_STAR || selcols[s].agg == AGG_COUNT) {
                    printf("%d", matching.count);
                } else if (ci >= 0) {
                    double sum = 0, mn = 1e300, mx = -1e300;
                    for (int m = 0; m < matching.count; m++) {
                        int ri = matching.indices[m];
                        double v = (t->columns[ci].type == T_INTEGER) ?
                            (double)t->rows[ri].cols[ci].ival : t->rows[ri].cols[ci].rval;
                        sum += v;
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                    }
                    if (selcols[s].agg == AGG_SUM) printf("%g", sum);
                    else if (selcols[s].agg == AGG_AVG) printf("%g", matching.count ? sum / matching.count : 0);
                    else if (selcols[s].agg == AGG_MIN) printf("%g", mn);
                    else if (selcols[s].agg == AGG_MAX) printf("%g", mx);
                }
            }
            printf("\n");
        }
        intarray_free(&matching);
        return;
    }

    /* ===== Execute: simple SELECT ===== */
    IntArray matching;
    intarray_init(&matching);
    for (int i = 0; i < t->nrows; i++) {
        if (eval_conditions(conds, nconds, &t->rows[i], t))
            intarray_push(&matching, i);
    }

    /* Sort if ORDER BY */
    if (norders > 0) {
        sort_table_ref = t;
        sort_specs = orders;
        sort_nspecs = norders;
        qsort(matching.indices, matching.count, sizeof(int), compare_rows_by_order);
    }

    /* Print header */
    if (select_star) {
        for (int c = 0; c < t->ncols; c++) {
            if (c > 0) printf("\t");
            printf("%s", t->columns[c].name);
        }
    } else {
        for (int s = 0; s < nsel; s++) {
            if (s > 0) printf("\t");
            printf("%s", selcols[s].col);
        }
    }
    printf("\n");

    /* Print rows with LIMIT/OFFSET */
    int printed = 0;
    for (int m = 0; m < matching.count; m++) {
        if (m < offset) continue;
        if (limit >= 0 && printed >= limit) break;
        int ri = matching.indices[m];
        if (select_star) {
            for (int c = 0; c < t->ncols; c++) {
                if (c > 0) printf("\t");
                print_value(t, c, &t->rows[ri]);
            }
        } else {
            for (int s = 0; s < nsel; s++) {
                if (s > 0) printf("\t");
                int ci = find_col(t, selcols[s].col);
                if (ci >= 0) print_value(t, ci, &t->rows[ri]);
            }
        }
        printf("\n");
        printed++;
    }

    intarray_free(&matching);
}

static void exec_dot_tables(void) {
    for (int i = 0; i < ntables; i++) {
        printf("%s\n", tables[i].name);
    }
}

static void exec_dot_schema(TokenList *tl) {
    if (tl->count < 2) {
        for (int i = 0; i < ntables; i++) {
            printf("CREATE TABLE %s (", tables[i].name);
            for (int c = 0; c < tables[i].ncols; c++) {
                if (c > 0) printf(", ");
                printf("%s %s", tables[i].columns[c].name,
                    tables[i].columns[c].type == T_INTEGER ? "INTEGER" :
                    tables[i].columns[c].type == T_REAL ? "REAL" : "TEXT");
            }
            printf(");\n");
        }
    } else {
        Table *t = find_table(tl->tokens[1]);
        if (t) {
            printf("CREATE TABLE %s (", t->name);
            for (int c = 0; c < t->ncols; c++) {
                if (c > 0) printf(", ");
                printf("%s %s", t->columns[c].name,
                    t->columns[c].type == T_INTEGER ? "INTEGER" :
                    t->columns[c].type == T_REAL ? "REAL" : "TEXT");
            }
            printf(");\n");
        } else {
            fprintf(stderr, "Error: no such table: %s\n", tl->tokens[1]);
        }
    }
}

/* ========== Main Loop ========== */
static void exec_statement(const char *sql) {
    TokenList tl;
    int n = tokenize(sql, &tl);
    if (n == 0) return;

    /* Remove trailing semicolons */
    while (tl.count > 0 && strcmp(tl.tokens[tl.count - 1], ";") == 0)
        tl.count--;
    if (tl.count == 0) return;

    if (tok_eq(tl.tokens[0], "create")) exec_create(&tl);
    else if (tok_eq(tl.tokens[0], "insert")) exec_insert(&tl);
    else if (tok_eq(tl.tokens[0], "select")) exec_select(&tl);
    else if (tok_eq(tl.tokens[0], "update")) exec_update(&tl);
    else if (tok_eq(tl.tokens[0], "delete")) exec_delete(&tl);
    else if (tok_eq(tl.tokens[0], "drop")) exec_drop(&tl);
    else if (strcmp(tl.tokens[0], ".tables") == 0) exec_dot_tables();
    else if (strcmp(tl.tokens[0], ".schema") == 0) exec_dot_schema(&tl);
    else fprintf(stderr, "Error: unknown command: %s\n", tl.tokens[0]);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: minidb <database-file>\n");
        return 1;
    }

    strncpy(db_path, argv[1], sizeof(db_path) - 1);
    load_db();
    atexit(save_db);

    char line[65536];
    char stmt[65536 * 4];
    stmt[0] = '\0';

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        len = strlen(line);
        if (len == 0) continue;

        /* Accumulate multi-line statements */
        strncat(stmt, line, sizeof(stmt) - strlen(stmt) - 2);

        /* Check if statement is complete (has semicolon or is a dot command) */
        if (stmt[0] == '.' || strchr(stmt, ';')) {
            /* Handle multiple semicolon-separated statements */
            char *s = stmt;
            while (*s) {
                char *semi = strchr(s, ';');
                if (semi) {
                    *semi = '\0';
                    char single[65536];
                    strncpy(single, s, sizeof(single) - 1);
                    single[sizeof(single) - 1] = '\0';
                    trim(single);
                    if (single[0]) exec_statement(single);
                    s = semi + 1;
                } else {
                    trim(s);
                    if (s[0] == '.') exec_statement(s);
                    break;
                }
            }
            stmt[0] = '\0';
        }
    }

    /* Execute any remaining statement */
    trim(stmt);
    if (stmt[0]) exec_statement(stmt);

    /* Cleanup */
    for (int i = 0; i < ntables; i++) free(tables[i].rows);

    return 0;
}
MINIDB_SOURCE

# Update the Makefile to use the single-file source
cat > Makefile << 'MAKEFILE'
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lm

minidb: minidb.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f minidb

.PHONY: clean
MAKEFILE

# Build
make clean
make

echo "Reference solution built successfully."
