/*
 * alpha_runtime.c
 * C runtime support for the Alpha compiler's LLVM backend.
 * Implements: print, input, table operations, typeof, strtonum, sqrt, etc.
 * Compiled to LLVM IR via clang and linked with the generated module.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>

/* ---- AlphaVal layout (must match codegen.h) ---- */
#define TAG_NIL      0
#define TAG_NUMBER   1
#define TAG_STRING   2
#define TAG_BOOL     3
#define TAG_TABLE    4
#define TAG_USERFUNC 5
#define TAG_LIBFUNC  6

typedef struct AlphaVal {
    int32_t tag;
    int64_t data; /* bitcast: double for number, ptr for string/table/func, i32 for bool */
} AlphaVal;

/* ---- Table: open-addressing hash map ---- */
#define TABLE_INIT_CAP 16

typedef struct TableEntry {
    AlphaVal* key;
    AlphaVal* val;
    int       used;
} TableEntry;

typedef struct AlphaTable {
    TableEntry* entries;
    int         cap;
    int         size;
} AlphaTable;

/* ---- AlphaVal constructors (heap allocated) ---- */
static AlphaVal* av_nil() {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_NIL; v->data = 0;
    return v;
}
static AlphaVal* av_number(double d) {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_NUMBER;
    memcpy(&v->data, &d, sizeof(double));
    return v;
}
static AlphaVal* av_string(const char* s) {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_STRING;
    char* dup = strdup(s);
    memcpy(&v->data, &dup, sizeof(char*));
    return v;
}
static AlphaVal* av_bool(int b) {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_BOOL; v->data = b ? 1 : 0;
    return v;
}
static AlphaVal* av_table(AlphaTable* t) {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_TABLE;
    memcpy(&v->data, &t, sizeof(AlphaTable*));
    return v;
}

static const char* av_tostr_raw(const AlphaVal* v) {
    const char* s = NULL;
    if (v->tag == TAG_STRING) { memcpy(&s, &v->data, sizeof(char*)); }
    return s;
}
static AlphaTable* av_totable(const AlphaVal* v) {
    AlphaTable* t = NULL;
    if (v->tag == TAG_TABLE) { memcpy(&t, &v->data, sizeof(AlphaTable*)); }
    return t;
}

/* ---- Table implementation ---- */
static uint64_t av_hash(const AlphaVal* k) {
    if (!k) return 0;
    if (k->tag == TAG_NUMBER) return (uint64_t)(uint32_t)k->data * 2654435761ULL;
    if (k->tag == TAG_STRING) {
        const char* s = av_tostr_raw(k);
        if (!s) return 0;
        uint64_t h = 5381;
        while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
        return h;
    }
    return (uint64_t)(uintptr_t)k;
}

static int av_key_eq(const AlphaVal* a, const AlphaVal* b) {
    if (!a || !b) return a == b;
    if (a->tag != b->tag) return 0;
    if (a->tag == TAG_NUMBER) return a->data == b->data;
    if (a->tag == TAG_STRING) {
        const char* sa = av_tostr_raw(a);
        const char* sb = av_tostr_raw(b);
        if (!sa || !sb) return sa == sb;
        return strcmp(sa, sb) == 0;
    }
    return a->data == b->data;
}

static AlphaTable* table_new_impl() {
    AlphaTable* t = (AlphaTable*)calloc(1, sizeof(AlphaTable));
    t->cap = TABLE_INIT_CAP;
    t->entries = (TableEntry*)calloc(t->cap, sizeof(TableEntry));
    return t;
}

static void table_resize(AlphaTable* t);

static void table_set_impl(AlphaTable* t, AlphaVal* key, AlphaVal* val) {
    if ((double)t->size / t->cap > 0.7) table_resize(t);
    uint64_t h = av_hash(key) % (uint64_t)t->cap;
    while (t->entries[h].used && !av_key_eq(t->entries[h].key, key))
        h = (h + 1) % (uint64_t)t->cap;
    if (!t->entries[h].used) t->size++;
    t->entries[h].key = key;
    t->entries[h].val = val;
    t->entries[h].used = 1;
}

static AlphaVal* table_get_impl(AlphaTable* t, const AlphaVal* key) {
    uint64_t h = av_hash(key) % (uint64_t)t->cap;
    int probed = 0;
    while (t->entries[h].used && probed < t->cap) {
        if (av_key_eq(t->entries[h].key, key))
            return t->entries[h].val;
        h = (h + 1) % (uint64_t)t->cap;
        probed++;
    }
    return av_nil();
}

static void table_resize(AlphaTable* t) {
    int oldCap = t->cap;
    TableEntry* old = t->entries;
    t->cap *= 2;
    t->entries = (TableEntry*)calloc(t->cap, sizeof(TableEntry));
    t->size = 0;
    for (int i = 0; i < oldCap; i++)
        if (old[i].used)
            table_set_impl(t, old[i].key, old[i].val);
    free(old);
}

/* ---- Value printing ---- */
static void print_val(const AlphaVal* v) {
    if (!v) { printf("nil"); return; }
    switch(v->tag) {
        case TAG_NIL:    printf("nil"); break;
        case TAG_NUMBER: {
            double d; memcpy(&d, &v->data, sizeof(double));
            if (d != d)              { printf("nan"); break; }
            if (d == 1.0/0.0)       { printf("inf"); break; }
            if (d == -1.0/0.0)      { printf("-inf"); break; }
            if (d == (long long)d && d >= -1e15 && d <= 1e15)
                printf("%lld", (long long)d);
            else
                printf("%.14g", d);
            break;
        }
        case TAG_STRING: {
            const char* s = av_tostr_raw(v);
            printf("%s", s ? s : "");
            break;
        }
        case TAG_BOOL:     printf("%s", v->data ? "true" : "false"); break;
        case TAG_TABLE:    printf("<table:%p>", (void*)(uintptr_t)v->data); break;
        case TAG_USERFUNC: printf("<function:%p>", (void*)(uintptr_t)v->data); break;
        case TAG_LIBFUNC:  printf("<libfunction>"); break;
        default:           printf("<?>");
    }
}

/* ===== Public runtime API (called from generated LLVM IR) ===== */

/* Forward declarations */
AlphaVal* alpha_rt_tostring(AlphaVal* v);

/* ---- Heap AlphaVal constructors (called from JIT'd code) ---- */
AlphaVal* alpha_rt_make_nil() {
    return av_nil();
}
AlphaVal* alpha_rt_make_number(double d) {
    return av_number(d);
}
AlphaVal* alpha_rt_make_bool(int b) {
    return av_bool(b);
}
AlphaVal* alpha_rt_make_string(const char* s) {
    return av_string(s);
}
/* func pointer stored as int64 */
AlphaVal* alpha_rt_make_func(int64_t fptr) {
    AlphaVal* v = (AlphaVal*)malloc(sizeof(AlphaVal));
    v->tag = TAG_USERFUNC;
    v->data = (int64_t)fptr;
    return v;
}
/* Extract the function pointer stored in a USERFUNC AlphaVal */
int64_t alpha_rt_get_funcptr(AlphaVal* v) {
    if (!v || v->tag != TAG_USERFUNC) return 0;
    return v->data;
}
/* Extract double from a NUMBER AlphaVal */
double alpha_rt_get_number(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return 0.0;
    double d; memcpy(&d, &v->data, sizeof(double));
    return d;
}
/* Extract bool (0 or 1) from any AlphaVal (truthiness) */
int alpha_rt_is_truthy(AlphaVal* v) {
    if (!v || v->tag == TAG_NIL) return 0;
    if (v->tag == TAG_BOOL) return v->data != 0;
    return 1; /* everything else is truthy */
}

void alpha_rt_print(AlphaVal* v) {
    print_val(v);
    printf("\n");
}

AlphaVal* alpha_rt_input() {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return av_nil();
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return av_string(buf);
}

AlphaVal* alpha_rt_table_new() {
    return av_table(table_new_impl());
}

AlphaVal* alpha_rt_table_get(AlphaVal* tbl, AlphaVal* key) {
    AlphaTable* t = av_totable(tbl);
    if (!t) { fprintf(stderr, "Runtime error: indexing non-table\n"); return av_nil(); }
    return table_get_impl(t, key);
}

void alpha_rt_table_set(AlphaVal* tbl, AlphaVal* key, AlphaVal* val) {
    AlphaTable* t = av_totable(tbl);
    if (!t) { fprintf(stderr, "Runtime error: indexing non-table\n"); return; }
    table_set_impl(t, key, val);
}

AlphaVal* alpha_rt_concat(AlphaVal* a, AlphaVal* b) {
    /* Auto-coerce both sides to string representation */
    AlphaVal* sa = alpha_rt_tostring(a);
    AlphaVal* sb = alpha_rt_tostring(b);
    const char* ca = av_tostr_raw(sa);
    const char* cb = av_tostr_raw(sb);
    if (!ca) ca = "";
    if (!cb) cb = "";
    size_t la = strlen(ca), lb = strlen(cb);
    char* buf = (char*)malloc(la + lb + 1);
    memcpy(buf, ca, la);
    memcpy(buf + la, cb, lb);
    buf[la + lb] = '\0';
    AlphaVal* res = av_string(buf);
    free(buf);
    return res;
}

AlphaVal* alpha_rt_typeof(AlphaVal* v) {
    switch(v ? v->tag : TAG_NIL) {
        case TAG_NIL:      return av_string("nil");
        case TAG_NUMBER:   return av_string("number");
        case TAG_STRING:   return av_string("string");
        case TAG_BOOL:     return av_string("boolean");
        case TAG_TABLE:    return av_string("table");
        case TAG_USERFUNC: return av_string("userfunc");
        case TAG_LIBFUNC:  return av_string("libfunc");
        default:           return av_string("undefined");
    }
}

AlphaVal* alpha_rt_strtonum(AlphaVal* s) {
    if (!s || s->tag != TAG_STRING) return av_nil();
    const char* str = av_tostr_raw(s);
    if (!str) return av_nil();
    char* end;
    double d = strtod(str, &end);
    if (end == str) return av_nil();
    return av_number(d);
}

AlphaVal* alpha_rt_sqrt(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(sqrt(d));
}

/* Deep equality: covers nil, number, string, bool */
int alpha_rt_val_eq(AlphaVal* a, AlphaVal* b) {
    if (!a || !b) return a == b;
    if (a->tag != b->tag) {
        /* nil == nil regardless of pointer identity */
        if (a->tag == TAG_NIL && b->tag == TAG_NIL) return 1;
        return 0;
    }
    switch (a->tag) {
        case TAG_NIL:    return 1;
        case TAG_NUMBER: return a->data == b->data;
        case TAG_BOOL:   return (a->data != 0) == (b->data != 0);
        case TAG_STRING: {
            const char* sa = av_tostr_raw(a);
            const char* sb = av_tostr_raw(b);
            if (!sa || !sb) return sa == sb;
            return strcmp(sa, sb) == 0;
        }
        default: return a->data == b->data; /* table/func: pointer equality */
    }
}

int alpha_rt_val_neq(AlphaVal* a, AlphaVal* b) {
    return !alpha_rt_val_eq(a, b);
}

/* ---- tostring: convert any value to a string AlphaVal ---- */
AlphaVal* alpha_rt_tostring(AlphaVal* v) {
    if (!v || v->tag == TAG_NIL)    return av_string("nil");
    if (v->tag == TAG_BOOL)         return av_string(v->data ? "true" : "false");
    if (v->tag == TAG_STRING)       return v; /* already a string */
    if (v->tag == TAG_TABLE)        return av_string("<table>");
    if (v->tag == TAG_USERFUNC)     return av_string("<function>");
    if (v->tag == TAG_NUMBER) {
        double d; memcpy(&d, &v->data, sizeof(double));
        char buf[64];
        if (d == (long long)d && d >= -1e15 && d <= 1e15)
            snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            snprintf(buf, sizeof(buf), "%.14g", d);
        return av_string(buf);
    }
    return av_string("?");
}

/* ---- objecttotalmembers(table) -> number ---- */
AlphaVal* alpha_rt_objecttotalmembers(AlphaVal* tblV) {
    AlphaTable* t = av_totable(tblV);
    if (!t) return av_number(0);
    return av_number((double)t->size);
}

/* ---- objectmemberkeys(table) -> table of keys ---- */
AlphaVal* alpha_rt_objectmemberkeys(AlphaVal* tblV) {
    AlphaTable* src = av_totable(tblV);
    AlphaTable* keys = table_new_impl();
    AlphaVal*   result = av_table(keys);
    if (!src) return result;
    int idx = 0;
    for (int i = 0; i < src->cap; i++) {
        if (src->entries[i].used) {
            table_set_impl(keys, av_number((double)idx++), src->entries[i].key);
        }
    }
    return result;
}

/* ---- objectcopy(table) -> shallow copy ---- */
AlphaVal* alpha_rt_objectcopy(AlphaVal* tblV) {
    AlphaTable* src = av_totable(tblV);
    AlphaTable* dst = table_new_impl();
    if (src) {
        for (int i = 0; i < src->cap; i++)
            if (src->entries[i].used)
                table_set_impl(dst, src->entries[i].key, src->entries[i].val);
    }
    return av_table(dst);
}

/* ---- cos / sin ---- */
AlphaVal* alpha_rt_cos(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(cos(d));
}
AlphaVal* alpha_rt_sin(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(sin(d));
}
AlphaVal* alpha_rt_pow(AlphaVal* base, AlphaVal* exp) {
    if (!base || base->tag != TAG_NUMBER) return av_nil();
    if (!exp  || exp->tag  != TAG_NUMBER) return av_nil();
    double b, e;
    memcpy(&b, &base->data, sizeof(double));
    memcpy(&e, &exp->data,  sizeof(double));
    return av_number(pow(b, e));
}

/* ---- totalarguments / argument: no variadic support yet, stubs ---- */
AlphaVal* alpha_rt_totalarguments(void) { return av_number(0); }
AlphaVal* alpha_rt_argument(AlphaVal* idx) { (void)idx; return av_nil(); }

/* ---- Math extensions ---- */
AlphaVal* alpha_rt_floor(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(floor(d));
}
AlphaVal* alpha_rt_ceil(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(ceil(d));
}
AlphaVal* alpha_rt_abs(AlphaVal* v) {
    if (!v || v->tag != TAG_NUMBER) return av_nil();
    double d; memcpy(&d, &v->data, sizeof(double));
    return av_number(fabs(d));
}
AlphaVal* alpha_rt_max(AlphaVal* a, AlphaVal* b) {
    if (!a || !b || a->tag != TAG_NUMBER || b->tag != TAG_NUMBER) return av_nil();
    double da, db;
    memcpy(&da, &a->data, sizeof(double));
    memcpy(&db, &b->data, sizeof(double));
    return av_number(da > db ? da : db);
}
AlphaVal* alpha_rt_min(AlphaVal* a, AlphaVal* b) {
    if (!a || !b || a->tag != TAG_NUMBER || b->tag != TAG_NUMBER) return av_nil();
    double da, db;
    memcpy(&da, &a->data, sizeof(double));
    memcpy(&db, &b->data, sizeof(double));
    return av_number(da < db ? da : db);
}
