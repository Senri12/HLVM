#include "cfg_builder.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JVM_MAX_LABELS = 512, JVM_MAX_FIXUPS = 512, JVM_MAX_LOCALS = 256 };

typedef struct {
  unsigned char* data;
  int size;
  int cap;
} ByteVec;

typedef struct {
  char* name;
  int offset;
} JvmLabel;

typedef struct {
  char* label;
  int opcode_offset;
  int operand_offset;
} JvmFixup;

typedef struct {
  char* name;
  int slot;
  int is_ref;
} JvmLocal;

typedef struct {
  int operand_offset;
  char* name;
  char* desc;
} JvmInvokeFixup;

typedef struct {
  ByteVec code;
  JvmLabel labels[JVM_MAX_LABELS];
  int label_count;
  JvmFixup fixups[JVM_MAX_FIXUPS];
  int fixup_count;
  JvmLocal locals[JVM_MAX_LOCALS];
  int local_count;
  JvmInvokeFixup invoke_fixups[JVM_MAX_FIXUPS];
  int invoke_fixup_count;
  int temp_label_counter;
  int max_locals;
  int max_stack;
  int terminated;
  FunctionCFG* function;
  AnalysisResult* analysis;
  FILE* listing;
} JvmMethodGen;

typedef enum {
  CP_UTF8 = 1,
  CP_INTEGER = 3,
  CP_CLASS = 7,
  CP_STRING = 8,
  CP_FIELDREF = 9,
  CP_METHODREF = 10,
  CP_NAMEANDTYPE = 12
} CpTag;

typedef struct {
  CpTag tag;
  char* a;
  char* b;
  int i;
  uint16_t x;
  uint16_t y;
} CpEntry;

typedef struct {
  CpEntry* items;
  int count;
  int cap;
} CpPool;

typedef struct {
  char* name;
  char* desc;
  ByteVec code;
  int max_stack;
  int max_locals;
  JvmInvokeFixup* invoke_fixups;
  int invoke_fixup_count;
} JvmMethod;

static char* jvm_strdup(const char* s) {
  char* d;
  if (!s) s = "";
  d = (char*)malloc(strlen(s) + 1);
  if (d) strcpy(d, s);
  return d;
}

static char* jvm_strdupf(const char* fmt, const char* a, int b) {
  char buf[512];
  snprintf(buf, sizeof(buf), fmt, a ? a : "", b);
  return jvm_strdup(buf);
}

static void trim(char* s) {
  char* p;
  size_t len;
  if (!s) return;
  p = s;
  while (*p && isspace((unsigned char)*p)) ++p;
  if (p != s) memmove(s, p, strlen(p) + 1);
  len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static int starts_with(const char* s, const char* prefix) {
  return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void bv_init(ByteVec* v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

static void bv_free(ByteVec* v) {
  if (!v) return;
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

static int bv_reserve(ByteVec* v, int extra) {
  int need;
  int cap;
  unsigned char* next;
  if (!v || extra < 0) return 0;
  need = v->size + extra;
  if (need <= v->cap) return 1;
  cap = v->cap ? v->cap * 2 : 128;
  while (cap < need) cap *= 2;
  next = (unsigned char*)realloc(v->data, (size_t)cap);
  if (!next) return 0;
  v->data = next;
  v->cap = cap;
  return 1;
}

static void bv_u1(ByteVec* v, int x) {
  if (!bv_reserve(v, 1)) return;
  v->data[v->size++] = (unsigned char)(x & 0xff);
}

static void bv_u2(ByteVec* v, int x) {
  if (!bv_reserve(v, 2)) return;
  v->data[v->size++] = (unsigned char)((x >> 8) & 0xff);
  v->data[v->size++] = (unsigned char)(x & 0xff);
}

static void bv_u4(ByteVec* v, uint32_t x) {
  if (!bv_reserve(v, 4)) return;
  v->data[v->size++] = (unsigned char)((x >> 24) & 0xff);
  v->data[v->size++] = (unsigned char)((x >> 16) & 0xff);
  v->data[v->size++] = (unsigned char)((x >> 8) & 0xff);
  v->data[v->size++] = (unsigned char)(x & 0xff);
}

static void bv_patch_u2(ByteVec* v, int off, int x) {
  if (!v || off < 0 || off + 1 >= v->size) return;
  v->data[off] = (unsigned char)((x >> 8) & 0xff);
  v->data[off + 1] = (unsigned char)(x & 0xff);
}

static void emit_u1(JvmMethodGen* g, int op, const char* text) {
  if (g->listing && text) fprintf(g->listing, "    %s\n", text);
  bv_u1(&g->code, op);
}

static void emit_u2_arg(JvmMethodGen* g, int op, int arg, const char* text) {
  if (g->listing && text) fprintf(g->listing, "    %s\n", text);
  bv_u1(&g->code, op);
  bv_u2(&g->code, arg);
}

static void mark_label(JvmMethodGen* g, const char* label) {
  if (!g || !label || g->label_count >= JVM_MAX_LABELS) return;
  if (g->listing) fprintf(g->listing, "%s:\n", label);
  g->labels[g->label_count].name = jvm_strdup(label);
  g->labels[g->label_count].offset = g->code.size;
  g->label_count++;
  g->terminated = 0;
}

static int find_label(JvmMethodGen* g, const char* label) {
  int i;
  if (!g || !label) return -1;
  for (i = 0; i < g->label_count; ++i)
    if (strcmp(g->labels[i].name, label) == 0) return g->labels[i].offset;
  return -1;
}

static void emit_branch(JvmMethodGen* g, int op, const char* label,
                        const char* text) {
  if (!g || !label || g->fixup_count >= JVM_MAX_FIXUPS) return;
  if (g->listing && text) fprintf(g->listing, "    %s %s\n", text, label);
  g->fixups[g->fixup_count].label = jvm_strdup(label);
  g->fixups[g->fixup_count].opcode_offset = g->code.size;
  bv_u1(&g->code, op);
  g->fixups[g->fixup_count].operand_offset = g->code.size;
  bv_u2(&g->code, 0);
  g->fixup_count++;
}

static int patch_branches(JvmMethodGen* g) {
  int i;
  if (!g) return 0;
  for (i = 0; i < g->fixup_count; ++i) {
    int target = find_label(g, g->fixups[i].label);
    int rel;
    if (target < 0) return 0;
    rel = target - g->fixups[i].opcode_offset;
    bv_patch_u2(&g->code, g->fixups[i].operand_offset, rel);
  }
  return 1;
}

static void free_method_gen(JvmMethodGen* g) {
  int i;
  if (!g) return;
  for (i = 0; i < g->label_count; ++i) free(g->labels[i].name);
  for (i = 0; i < g->fixup_count; ++i) free(g->fixups[i].label);
  for (i = 0; i < g->local_count; ++i) free(g->locals[i].name);
  for (i = 0; i < g->invoke_fixup_count; ++i) {
    free(g->invoke_fixups[i].name);
    free(g->invoke_fixups[i].desc);
  }
  bv_free(&g->code);
}

static int local_slot(JvmMethodGen* g, const char* name, int create) {
  int i;
  if (!g || !name || !*name) return 0;
  for (i = 0; i < g->local_count; ++i)
    if (strcmp(g->locals[i].name, name) == 0) return g->locals[i].slot;
  if (!create || g->local_count >= JVM_MAX_LOCALS) return 0;
  g->locals[g->local_count].name = jvm_strdup(name);
  g->locals[g->local_count].slot = g->max_locals++;
  g->locals[g->local_count].is_ref = 0;
  return g->locals[g->local_count++].slot;
}

static void mark_local_ref(JvmMethodGen* g, const char* name) {
  int i;
  if (!g || !name) return;
  for (i = 0; i < g->local_count; ++i) {
    if (strcmp(g->locals[i].name, name) == 0) {
      g->locals[i].is_ref = 1;
      return;
    }
  }
}

static int local_is_ref(JvmMethodGen* g, const char* name) {
  int i;
  if (!g || !name) return 0;
  for (i = 0; i < g->local_count; ++i)
    if (strcmp(g->locals[i].name, name) == 0) return g->locals[i].is_ref;
  return 0;
}

static void emit_iconst(JvmMethodGen* g, int value) {
  char line[64];
  if (value >= -1 && value <= 5) {
    static const int ops[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    snprintf(line, sizeof(line), "iconst_%d", value);
    emit_u1(g, ops[value + 1], line);
  } else if (value >= -128 && value <= 127) {
    snprintf(line, sizeof(line), "bipush %d", value);
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x10);
    bv_u1(&g->code, value);
  } else {
    snprintf(line, sizeof(line), "sipush %d", value);
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x11);
    bv_u2(&g->code, value);
  }
}

static void emit_iload(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "iload %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x1a + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x15);
    bv_u1(&g->code, slot);
  }
}

static void emit_istore(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "istore %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x3b + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x36);
    bv_u1(&g->code, slot);
  }
}

static void emit_aload(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "aload %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x2a + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x19);
    bv_u1(&g->code, slot);
  }
}

static void emit_astore(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "astore %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x4b + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x3a);
    bv_u1(&g->code, slot);
  }
}

static void emit_invokestatic_user(JvmMethodGen* g, const char* name,
                                   const char* desc, const char* text) {
  if (!g || !name || !desc || g->invoke_fixup_count >= JVM_MAX_FIXUPS) return;
  if (g->listing && text) fprintf(g->listing, "    %s\n", text);
  bv_u1(&g->code, 0xb8);
  g->invoke_fixups[g->invoke_fixup_count].operand_offset = g->code.size;
  g->invoke_fixups[g->invoke_fixup_count].name = jvm_strdup(name);
  g->invoke_fixups[g->invoke_fixup_count].desc = jvm_strdup(desc);
  g->invoke_fixup_count++;
  bv_u2(&g->code, 0xfff5);
}

static int parse_int_literal(const char* s, int* out) {
  char* end = NULL;
  long v;
  if (!s || !*s || !out) return 0;
  if (starts_with(s, "0x") || starts_with(s, "0X")) {
    v = strtol(s + 2, &end, 16);
    if (end && *end == '\0') {
      *out = (int)v;
      return 1;
    }
  }
  if (starts_with(s, "0b") || starts_with(s, "0B")) {
    int value = 0;
    const char* p = s + 2;
    if (!*p) return 0;
    while (*p == '0' || *p == '1') value = value * 2 + (*p++ - '0');
    if (*p == '\0') {
      *out = value;
      return 1;
    }
  }
  v = strtol(s, &end, 10);
  if (end && *end == '\0') {
    *out = (int)v;
    return 1;
  }
  return 0;
}

static int parse_char_literal(const char* s, int* out) {
  if (!s || !out || s[0] != '\'') return 0;
  if (s[1] == '\\') {
    switch (s[2]) {
      case 'n': *out = 10; return 1;
      case 'r': *out = 13; return 1;
      case 't': *out = 9; return 1;
      case '0': *out = 0; return 1;
      case '\\': *out = '\\'; return 1;
      case '\'': *out = '\''; return 1;
      default: *out = (unsigned char)s[2]; return 1;
    }
  }
  if (s[1] && s[2] == '\'') {
    *out = (unsigned char)s[1];
    return 1;
  }
  return 0;
}

static int parse_string_literal(const char* s, char* out, size_t out_sz) {
  size_t o = 0;
  const char* p;
  if (!s || !out || out_sz == 0 || s[0] != '"') return 0;
  p = s + 1;
  while (*p && *p != '"') {
    int ch;
    if (*p == '\\') {
      ++p;
      if (*p == 'n') ch = 10;
      else if (*p == 'r') ch = 13;
      else if (*p == 't') ch = 9;
      else if (*p == '0') ch = 0;
      else ch = (unsigned char)*p;
    } else {
      ch = (unsigned char)*p;
    }
    if (o + 1 < out_sz) out[o++] = (char)ch;
    if (*p) ++p;
  }
  if (*p != '"') return 0;
  out[o] = '\0';
  return p[1] == '\0';
}

static int is_ident_text(const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  if (!p || !(isalpha(*p) || *p == '_')) return 0;
  for (++p; *p; ++p)
    if (!(isalnum(*p) || *p == '_')) return 0;
  return 1;
}

static int split_array_access(const char* text, char* name, size_t name_sz,
                              char* index, size_t index_sz) {
  const char* lb;
  const char* rb;
  if (!text || !name || !index) return 0;
  lb = strchr(text, '[');
  rb = strrchr(text, ']');
  if (!lb || !rb || rb < lb || rb[1] != '\0') return 0;
  snprintf(name, name_sz, "%.*s", (int)(lb - text), text);
  snprintf(index, index_sz, "%.*s", (int)(rb - lb - 1), lb + 1);
  trim(name);
  trim(index);
  return is_ident_text(name) && index[0] != '\0';
}

static int split_top_binary(const char* expr, char* lhs, size_t lhs_sz,
                            char* op, size_t op_sz, char* rhs,
                            size_t rhs_sz, const char** ops, int op_count) {
  int depth = 0, in_char = 0, in_string = 0;
  int best = -1, best_len = 0;
  const char* best_op = NULL;
  int i, k;
  if (!expr) return 0;
  for (i = (int)strlen(expr) - 1; i >= 0; --i) {
    char ch = expr[i];
    char prev = (i > 0) ? expr[i - 1] : '\0';
    if (in_char) {
      if (ch == '\'' && prev != '\\') in_char = 0;
      continue;
    }
    if (in_string) {
      if (ch == '"' && prev != '\\') in_string = 0;
      continue;
    }
    if (ch == '\'') { in_char = 1; continue; }
    if (ch == '"') { in_string = 1; continue; }
    if (ch == ')' || ch == ']') { depth++; continue; }
    if (ch == '(' || ch == '[') { depth--; continue; }
    if (depth != 0) continue;
    for (k = 0; k < op_count; ++k) {
      int len = (int)strlen(ops[k]);
      if (i - len + 1 <= 0) continue;
      if (strncmp(expr + i - len + 1, ops[k], (size_t)len) == 0) {
        char before = expr[i - len];
        char after = expr[i + 1];
        if (strcmp(ops[k], "-") == 0 &&
            (before == '\0' || before == '(' || before == '=' ||
             before == '+' || before == '-' || before == '*' || before == '/'))
          continue;
        if (strcmp(ops[k], "<") == 0 && (before == '<' || after == '<' || after == '='))
          continue;
        if (strcmp(ops[k], ">") == 0 && (before == '>' || after == '>' || after == '='))
          continue;
        if (strcmp(ops[k], "&") == 0 && (before == '&' || after == '&'))
          continue;
        if (strcmp(ops[k], "|") == 0 && (before == '|' || after == '|'))
          continue;
        best = i - len + 1;
        best_len = len;
        best_op = ops[k];
        break;
      }
    }
    if (best >= 0) break;
  }
  if (best <= 0 || !best_op) return 0;
  snprintf(lhs, lhs_sz, "%.*s", best, expr);
  snprintf(op, op_sz, "%s", best_op);
  snprintf(rhs, rhs_sz, "%s", expr + best + best_len);
  trim(lhs);
  trim(op);
  trim(rhs);
  return lhs[0] && rhs[0];
}

static int split_call(const char* expr, char* name, size_t name_sz,
                      char args[][128], int* argc) {
  const char* lp;
  const char* rp;
  char inside[512];
  char* p;
  int count = 0;
  if (argc) *argc = 0;
  if (!expr || !name) return 0;
  lp = strchr(expr, '(');
  rp = strrchr(expr, ')');
  if (!lp || !rp || rp < lp) return 0;
  if (rp[1] != '\0') return 0;
  snprintf(name, name_sz, "%.*s", (int)(lp - expr), expr);
  trim(name);
  if (!is_ident_text(name)) return 0;
  snprintf(inside, sizeof(inside), "%.*s", (int)(rp - lp - 1), lp + 1);
  trim(inside);
  p = inside;
  while (*p && count < 16) {
    char* start = p;
    int depth = 0;
    while (*p) {
      if (*p == '(') depth++;
      else if (*p == ')') depth--;
      else if (*p == ',' && depth == 0) break;
      p++;
    }
    snprintf(args[count], 128, "%.*s", (int)(p - start), start);
    trim(args[count]);
    count++;
    if (*p == ',') p++;
  }
  if (argc) *argc = count;
  return 1;
}

static const char* jvm_func_name(FunctionCFG* f) {
  return (f && f->func_name) ? f->func_name : "unknown";
}

static const char* jvm_ret_desc(FunctionCFG* f) {
  if (!f || !f->return_type || strcmp(f->return_type, "void") != 0) {
    if (f && f->return_type && strchr(f->return_type, '[')) return "[I";
    return "I";
  }
  return "V";
}

static const char* jvm_type_desc(const char* type_name) {
  if (type_name && strchr(type_name, '[')) return "[I";
  if (type_name && strcmp(type_name, "void") == 0) return "V";
  return "I";
}

static char* jvm_method_desc(FunctionCFG* f) {
  char buf[512];
  int i;
  snprintf(buf, sizeof(buf), "(");
  if (f) {
    for (i = 0; i < f->param_count; ++i) {
      strncat(buf, jvm_type_desc(f->param_types ? f->param_types[i] : "int"),
              sizeof(buf) - strlen(buf) - 1);
    }
  }
  strncat(buf, ")", sizeof(buf) - strlen(buf) - 1);
  strncat(buf, jvm_ret_desc(f), sizeof(buf) - strlen(buf) - 1);
  return jvm_strdup(buf);
}

static FunctionCFG* find_function(AnalysisResult* res, const char* name) {
  int i, j;
  if (!res || !name) return NULL;
  for (i = 0; i < res->files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (j = 0; j < sf->functions_count; ++j) {
      FunctionCFG* f = sf->functions[j];
      if (f && f->func_name && strcmp(f->func_name, name) == 0) return f;
      if (f && f->source_name && strcmp(f->source_name, name) == 0) return f;
    }
  }
  return NULL;
}

static FunctionCFG* find_function_arity(AnalysisResult* res, const char* name,
                                        int argc) {
  int i, j;
  FunctionCFG* fallback = NULL;
  if (!res || !name) return NULL;
  for (i = 0; i < res->files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (j = 0; j < sf->functions_count; ++j) {
      FunctionCFG* f = sf->functions[j];
      if (!f) continue;
      if (f->func_name && strcmp(f->func_name, name) == 0) {
        if (f->param_count == argc) return f;
        if (!fallback) fallback = f;
      }
      if (f->source_name && strcmp(f->source_name, name) == 0) {
        if (f->param_count == argc) return f;
        if (!fallback) fallback = f;
      }
    }
  }
  return fallback;
}

static int cp_add(CpPool* cp, CpTag tag, const char* a, const char* b, int iv,
                  uint16_t x, uint16_t y) {
  CpEntry* e;
  if (cp->count + 1 >= cp->cap) {
    int nc = cp->cap ? cp->cap * 2 : 64;
    CpEntry* next = (CpEntry*)realloc(cp->items, sizeof(CpEntry) * (size_t)nc);
    if (!next) return 0;
    cp->items = next;
    cp->cap = nc;
  }
  e = &cp->items[++cp->count];
  memset(e, 0, sizeof(*e));
  e->tag = tag;
  e->a = a ? jvm_strdup(a) : NULL;
  e->b = b ? jvm_strdup(b) : NULL;
  e->i = iv;
  e->x = x;
  e->y = y;
  return cp->count;
}

static int cp_utf8(CpPool* cp, const char* s) {
  int i;
  for (i = 1; i <= cp->count; ++i)
    if (cp->items[i].tag == CP_UTF8 && cp->items[i].a &&
        strcmp(cp->items[i].a, s) == 0) return i;
  return cp_add(cp, CP_UTF8, s, NULL, 0, 0, 0);
}

static int cp_integer(CpPool* cp, int value) {
  int i;
  for (i = 1; i <= cp->count; ++i)
    if (cp->items[i].tag == CP_INTEGER && cp->items[i].i == value) return i;
  return cp_add(cp, CP_INTEGER, NULL, NULL, value, 0, 0);
}

static int cp_class(CpPool* cp, const char* name) {
  int i;
  int ni;
  for (i = 1; i <= cp->count; ++i)
    if (cp->items[i].tag == CP_CLASS && cp->items[i].a &&
        strcmp(cp->items[i].a, name) == 0) return i;
  ni = cp_utf8(cp, name);
  return cp_add(cp, CP_CLASS, name, NULL, 0, (uint16_t)ni, 0);
}

static int cp_name_type(CpPool* cp, const char* name, const char* desc) {
  int i;
  int ni, di;
  for (i = 1; i <= cp->count; ++i)
    if (cp->items[i].tag == CP_NAMEANDTYPE && cp->items[i].a &&
        cp->items[i].b && strcmp(cp->items[i].a, name) == 0 &&
        strcmp(cp->items[i].b, desc) == 0) return i;
  ni = cp_utf8(cp, name);
  di = cp_utf8(cp, desc);
  return cp_add(cp, CP_NAMEANDTYPE, name, desc, 0, (uint16_t)ni, (uint16_t)di);
}

static int cp_ref(CpPool* cp, CpTag tag, const char* cls, const char* name,
                  const char* desc) {
  int i;
  int ci, nti;
  char key[512];
  snprintf(key, sizeof(key), "%s.%s", cls, name);
  for (i = 1; i <= cp->count; ++i)
    if (cp->items[i].tag == tag && cp->items[i].a && cp->items[i].b &&
        strcmp(cp->items[i].a, key) == 0 && strcmp(cp->items[i].b, desc) == 0)
      return i;
  ci = cp_class(cp, cls);
  nti = cp_name_type(cp, name, desc);
  return cp_add(cp, tag, key, desc, 0, (uint16_t)ci, (uint16_t)nti);
}

static void cp_free(CpPool* cp) {
  int i;
  if (!cp) return;
  for (i = 1; i <= cp->count; ++i) {
    free(cp->items[i].a);
    free(cp->items[i].b);
  }
  free(cp->items);
}

static int is_wrapped_parens(const char* s) {
  int depth = 0;
  int i, len;
  if (!s || s[0] != '(') return 0;
  len = (int)strlen(s);
  if (len < 2 || s[len - 1] != ')') return 0;
  for (i = 0; i < len; ++i) {
    if (s[i] == '(') depth++;
    else if (s[i] == ')') {
      depth--;
      if (depth == 0 && i != len - 1) return 0;
    }
  }
  return depth == 0;
}

static void eval_expr(JvmMethodGen* g, const char* expr);

static void eval_compare_value(JvmMethodGen* g, const char* lhs,
                               const char* op, const char* rhs) {
  char* l_true = jvm_strdupf("L_jvm_cmp_true_%s_%d",
                             jvm_func_name(g->function),
                             g->temp_label_counter++);
  char* l_end = jvm_strdupf("L_jvm_cmp_end_%s_%d",
                            jvm_func_name(g->function),
                            g->temp_label_counter++);
  int opcode = 0x9f;
  eval_expr(g, lhs);
  eval_expr(g, rhs);
  if (strcmp(op, "==") == 0) opcode = 0x9f;
  else if (strcmp(op, "!=") == 0) opcode = 0xa0;
  else if (strcmp(op, "<") == 0) opcode = 0xa1;
  else if (strcmp(op, ">=") == 0) opcode = 0xa2;
  else if (strcmp(op, ">") == 0) opcode = 0xa3;
  else if (strcmp(op, "<=") == 0) opcode = 0xa4;
  emit_branch(g, opcode, l_true, "if_icmp");
  emit_iconst(g, 0);
  emit_branch(g, 0xa7, l_end, "goto");
  mark_label(g, l_true);
  emit_iconst(g, 1);
  mark_label(g, l_end);
  free(l_true);
  free(l_end);
}

static void eval_logical_value(JvmMethodGen* g, const char* lhs,
                               const char* op, const char* rhs) {
  char* l_true = jvm_strdupf("L_jvm_logic_true_%s_%d",
                             jvm_func_name(g->function),
                             g->temp_label_counter++);
  char* l_false = jvm_strdupf("L_jvm_logic_false_%s_%d",
                              jvm_func_name(g->function),
                              g->temp_label_counter++);
  char* l_end = jvm_strdupf("L_jvm_logic_end_%s_%d",
                            jvm_func_name(g->function),
                            g->temp_label_counter++);
  if (strcmp(op, "&&") == 0) {
    eval_expr(g, lhs);
    emit_branch(g, 0x99, l_false, "ifeq");
    eval_expr(g, rhs);
    emit_branch(g, 0x9a, l_true, "ifne");
    emit_branch(g, 0xa7, l_false, "goto");
  } else {
    eval_expr(g, lhs);
    emit_branch(g, 0x9a, l_true, "ifne");
    eval_expr(g, rhs);
    emit_branch(g, 0x9a, l_true, "ifne");
    emit_branch(g, 0xa7, l_false, "goto");
  }
  mark_label(g, l_true);
  emit_iconst(g, 1);
  emit_branch(g, 0xa7, l_end, "goto");
  mark_label(g, l_false);
  emit_iconst(g, 0);
  mark_label(g, l_end);
  free(l_true);
  free(l_false);
  free(l_end);
}

static void eval_call(JvmMethodGen* g, const char* name, char args[][128],
                      int argc) {
  int i;
  FunctionCFG* f;
  char* desc;
  char line[512];
  if (strcmp(name, "in") == 0 && argc == 0) {
    char* eof_ok = jvm_strdupf("L_jvm_in_ok_%s_%d", jvm_func_name(g->function),
                               g->temp_label_counter++);
    emit_u2_arg(g, 0xb2, 0xfff2, "getstatic java/lang/System/in Ljava/io/InputStream;");
    emit_u2_arg(g, 0xb6, 0xfff4, "invokevirtual java/io/InputStream/read()I");
    emit_u1(g, 0x59, "dup");
    emit_branch(g, 0x9c, eof_ok, "ifge");
    emit_u1(g, 0x57, "pop");
    emit_iconst(g, 0);
    mark_label(g, eof_ok);
    free(eof_ok);
    return;
  }
  for (i = 0; i < argc; ++i) eval_expr(g, args[i]);
  f = find_function_arity(g->analysis, name, argc);
  if (!f) f = find_function(g->analysis, jvm_func_name(g->function));
  desc = jvm_method_desc(f);
  snprintf(line, sizeof(line), "invokestatic SimpleLangProgram/%s%s",
           jvm_func_name(f), desc);
  emit_invokestatic_user(g, jvm_func_name(f), desc, line);
  free(desc);
}

static void eval_expr(JvmMethodGen* g, const char* expr) {
  char work[512];
  char lhs[256], rhs[256], op[8];
  int value;
  static const char* logical_or_ops[] = {"||"};
  static const char* logical_and_ops[] = {"&&"};
  static const char* bit_or_ops[] = {"|"};
  static const char* bit_xor_ops[] = {"^"};
  static const char* bit_and_ops[] = {"&"};
  static const char* cmp_ops[] = {"==", "!=", "<=", ">=", "<", ">"};
  static const char* shift_ops[] = {"<<", ">>"};
  static const char* add_ops[] = {"+", "-"};
  static const char* mul_ops[] = {"*", "/", "%"};
  char name[128];
  char index[256];
  char args[16][128];
  int argc = 0;
  if (!expr) {
    emit_iconst(g, 0);
    return;
  }
  snprintf(work, sizeof(work), "%s", expr);
  trim(work);
  if (work[0] == '\0') {
    emit_iconst(g, 0);
    return;
  }
  if (is_wrapped_parens(work)) {
    work[strlen(work) - 1] = '\0';
    eval_expr(g, work + 1);
    return;
  }
  if (work[0] == '!' && work[1] != '=') {
    char* l_true = jvm_strdupf("L_jvm_not_true_%s_%d",
                               jvm_func_name(g->function),
                               g->temp_label_counter++);
    char* l_end = jvm_strdupf("L_jvm_not_end_%s_%d",
                              jvm_func_name(g->function),
                              g->temp_label_counter++);
    eval_expr(g, work + 1);
    emit_branch(g, 0x99, l_true, "ifeq");
    emit_iconst(g, 0);
    emit_branch(g, 0xa7, l_end, "goto");
    mark_label(g, l_true);
    emit_iconst(g, 1);
    mark_label(g, l_end);
    free(l_true);
    free(l_end);
    return;
  }
  if (work[0] == '~') {
    eval_expr(g, work + 1);
    emit_iconst(g, -1);
    emit_u1(g, 0x82, "ixor");
    return;
  }
  if (work[0] == '-' && work[1] && !isdigit((unsigned char)work[1])) {
    eval_expr(g, work + 1);
    emit_u1(g, 0x74, "ineg");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       logical_or_ops, 1)) {
    eval_logical_value(g, lhs, op, rhs);
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       logical_and_ops, 1)) {
    eval_logical_value(g, lhs, op, rhs);
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       bit_or_ops, 1)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    emit_u1(g, 0x80, "ior");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       bit_xor_ops, 1)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    emit_u1(g, 0x82, "ixor");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       bit_and_ops, 1)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    emit_u1(g, 0x7e, "iand");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       cmp_ops, 6)) {
    eval_compare_value(g, lhs, op, rhs);
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       shift_ops, 2)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    emit_u1(g, strcmp(op, "<<") == 0 ? 0x78 : 0x7a,
            strcmp(op, "<<") == 0 ? "ishl" : "ishr");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       add_ops, 2)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    emit_u1(g, strcmp(op, "+") == 0 ? 0x60 : 0x64,
            strcmp(op, "+") == 0 ? "iadd" : "isub");
    return;
  }
  if (split_top_binary(work, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       mul_ops, 3)) {
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    if (strcmp(op, "*") == 0) emit_u1(g, 0x68, "imul");
    else if (strcmp(op, "/") == 0) emit_u1(g, 0x6c, "idiv");
    else emit_u1(g, 0x70, "irem");
    return;
  }
  if (split_call(work, name, sizeof(name), args, &argc)) {
    eval_call(g, name, args, argc);
    return;
  }
  if (split_array_access(work, name, sizeof(name), index, sizeof(index))) {
    emit_aload(g, local_slot(g, name, 1));
    eval_expr(g, index);
    emit_u1(g, 0x2e, "iaload");
    return;
  }
  if (strcmp(work, "true") == 0) {
    emit_iconst(g, 1);
    return;
  }
  if (strcmp(work, "false") == 0) {
    emit_iconst(g, 0);
    return;
  }
  if (parse_char_literal(work, &value) || parse_int_literal(work, &value)) {
    emit_iconst(g, value);
    return;
  }
  if (local_is_ref(g, work)) emit_aload(g, local_slot(g, work, 1));
  else emit_iload(g, local_slot(g, work, 1));
}

static void emit_condition_branch(JvmMethodGen* g, const char* cond,
                                  const char* true_label,
                                  const char* false_label) {
  char lhs[256], rhs[256], op[8];
  static const char* cmp_ops[] = {"==", "!=", "<=", ">=", "<", ">"};
  if (split_top_binary(cond, lhs, sizeof(lhs), op, sizeof(op), rhs, sizeof(rhs),
                       cmp_ops, 6)) {
    int opcode = 0x9f;
    eval_expr(g, lhs);
    eval_expr(g, rhs);
    if (strcmp(op, "==") == 0) opcode = 0x9f;
    else if (strcmp(op, "!=") == 0) opcode = 0xa0;
    else if (strcmp(op, "<") == 0) opcode = 0xa1;
    else if (strcmp(op, ">=") == 0) opcode = 0xa2;
    else if (strcmp(op, ">") == 0) opcode = 0xa3;
    else if (strcmp(op, "<=") == 0) opcode = 0xa4;
    emit_branch(g, opcode, true_label, "if_icmp");
    emit_branch(g, 0xa7, false_label, "goto");
    return;
  }
  eval_expr(g, cond);
  emit_branch(g, 0x9a, true_label, "ifne");
  emit_branch(g, 0xa7, false_label, "goto");
}

static char* node_label(FunctionCFG* f, CFGNode* n) {
  return jvm_strdupf("L_%s_%d", jvm_func_name(f), n ? n->id : 0);
}

static void emit_out_char_expr(JvmMethodGen* g, const char* expr) {
  emit_u2_arg(g, 0xb2, 0xfff1,
              "getstatic java/lang/System/out Ljava/io/PrintStream;");
  eval_expr(g, expr);
  emit_u1(g, 0x92, "i2c");
  emit_u2_arg(g, 0xb6, 0xfff3,
              "invokevirtual java/io/PrintStream/print(C)V");
}

static void emit_out_char_value(JvmMethodGen* g, int value) {
  emit_u2_arg(g, 0xb2, 0xfff1,
              "getstatic java/lang/System/out Ljava/io/PrintStream;");
  emit_iconst(g, value);
  emit_u1(g, 0x92, "i2c");
  emit_u2_arg(g, 0xb6, 0xfff3,
              "invokevirtual java/io/PrintStream/print(C)V");
}

static void emit_statement(JvmMethodGen* g, const char* stmt) {
  char work[512];
  char name[128], type_name[128], expr[384];
  char array_name[128], array_index[256];
  char call_name[128], args[16][128];
  int argc = 0;
  char* eq;
  if (!stmt) return;
  snprintf(work, sizeof(work), "%s", stmt);
  trim(work);
  if (!work[0]) return;
  if (starts_with(work, "decl ")) {
    name[0] = expr[0] = '\0';
    if (sscanf(work, "decl %127s %127s = %383[^\n]", type_name, name, expr) >= 2) {
      trim(name);
      trim(expr);
      if (split_array_access(name, array_name, sizeof(array_name), array_index,
                             sizeof(array_index))) {
        local_slot(g, array_name, 1);
        mark_local_ref(g, array_name);
        eval_expr(g, array_index);
        if (g->listing) fprintf(g->listing, "    newarray int\n");
        bv_u1(&g->code, 0xbc);
        bv_u1(&g->code, 10);
        emit_astore(g, local_slot(g, array_name, 1));
        return;
      }
      local_slot(g, name, 1);
      if (expr[0]) eval_expr(g, expr);
      else emit_iconst(g, 0);
      emit_istore(g, local_slot(g, name, 1));
    }
    return;
  }
  if (starts_with(work, "return")) {
    char* p = work + 6;
    const char* ret_desc;
    trim(p);
    ret_desc = jvm_ret_desc(g->function);
    if (strcmp(ret_desc, "V") == 0 || p[0] == '\0') {
      emit_u1(g, 0xb1, "return");
    } else {
      eval_expr(g, p);
      emit_u1(g, ret_desc[0] == '[' ? 0xb0 : 0xac,
              ret_desc[0] == '[' ? "areturn" : "ireturn");
    }
    g->terminated = 1;
    return;
  }
  if (split_call(work, call_name, sizeof(call_name), args, &argc)) {
    if (strcmp(call_name, "out") == 0 && argc == 1) {
      char str_value[512];
      if (parse_string_literal(args[0], str_value, sizeof(str_value))) {
        size_t si;
        for (si = 0; str_value[si]; ++si) {
          emit_out_char_value(g, (unsigned char)str_value[si]);
        }
      } else {
        emit_out_char_expr(g, args[0]);
      }
      return;
    }
    eval_call(g, call_name, args, argc);
    if (strcmp(jvm_ret_desc(find_function_arity(g->analysis, call_name, argc)), "V") != 0) {
      emit_u1(g, 0x57, "pop");
    }
    return;
  }
  eq = strchr(work, '=');
  if (eq && (eq == work || eq[-1] != '=')) {
    snprintf(name, sizeof(name), "%.*s", (int)(eq - work), work);
    snprintf(expr, sizeof(expr), "%s", eq + 1);
    trim(name);
    trim(expr);
    if (split_array_access(name, array_name, sizeof(array_name), array_index,
                           sizeof(array_index))) {
      emit_aload(g, local_slot(g, array_name, 1));
      eval_expr(g, array_index);
      eval_expr(g, expr);
      emit_u1(g, 0x4f, "iastore");
      return;
    }
    eval_expr(g, expr);
    if (local_is_ref(g, name)) emit_astore(g, local_slot(g, name, 1));
    else emit_istore(g, local_slot(g, name, 1));
  }
}

static void emit_node(JvmMethodGen* g, CFGNode* node) {
  int o;
  char* self = node_label(g->function, node);
  mark_label(g, self);
  free(self);

  if (node->label && strcmp(node->label, "START") == 0) {
    if (node->next_target) {
      char* lab = node_label(g->function, node->next_target);
      emit_branch(g, 0xa7, lab, "goto");
      free(lab);
    }
    return;
  }

  if (node->label && strcmp(node->label, "FINISH") == 0) {
    if (!g->terminated) {
      const char* ret_desc = jvm_ret_desc(g->function);
      if (strcmp(ret_desc, "V") == 0) emit_u1(g, 0xb1, "return");
      else {
        if (ret_desc[0] == '[') emit_u1(g, 0x01, "aconst_null");
        else emit_iconst(g, 0);
        emit_u1(g, ret_desc[0] == '[' ? 0xb0 : 0xac,
                ret_desc[0] == '[' ? "areturn" : "ireturn");
      }
      g->terminated = 1;
    }
    return;
  }

  for (o = 0; o < node->ops_count && !g->terminated; ++o) {
    char buf[1024];
    char* part;
    if (!node->ops[o] || !node->ops[o]->text) continue;
    snprintf(buf, sizeof(buf), "%s", node->ops[o]->text);
    part = strtok(buf, ";");
    while (part && !g->terminated) {
      emit_statement(g, part);
      part = strtok(NULL, ";");
    }
  }

  if (g->terminated) return;

  if (node->true_target && node->false_target && node->label &&
      node->label[0] != '\0') {
    char* t = node_label(g->function, node->true_target);
    char* f = node_label(g->function, node->false_target);
    emit_condition_branch(g, node->label, t, f);
    free(t);
    free(f);
    g->terminated = 1;
    return;
  }

  if (node->next_target) {
    char* lab = node_label(g->function, node->next_target);
    emit_branch(g, 0xa7, lab, "goto");
    free(lab);
    g->terminated = 1;
  }
}

static JvmMethod compile_function(FunctionCFG* f, AnalysisResult* res,
                                  FILE* listing) {
  JvmMethodGen g;
  JvmMethod m;
  int i;
  memset(&g, 0, sizeof(g));
  memset(&m, 0, sizeof(m));
  bv_init(&g.code);
  g.max_stack = 64;
  g.function = f;
  g.analysis = res;
  g.listing = listing;

  m.name = jvm_strdup(jvm_func_name(f));
  m.desc = jvm_method_desc(f);

  if (listing) {
    fprintf(listing, "\n.method public static %s%s\n", m.name, m.desc);
    fprintf(listing, "  .limit stack 64\n");
  }

  for (i = 0; i < f->param_count; ++i) {
    local_slot(&g, f->params[i], 1);
    if (f->param_types && f->param_types[i] && strchr(f->param_types[i], '[')) {
      mark_local_ref(&g, f->params[i]);
    }
  }

  for (i = 0; i < f->node_count; ++i) emit_node(&g, f->nodes[i]);

  if (!g.terminated) {
    const char* ret_desc = jvm_ret_desc(f);
    if (strcmp(ret_desc, "V") == 0) emit_u1(&g, 0xb1, "return");
    else {
      if (ret_desc[0] == '[') emit_u1(&g, 0x01, "aconst_null");
      else emit_iconst(&g, 0);
      emit_u1(&g, ret_desc[0] == '[' ? 0xb0 : 0xac,
              ret_desc[0] == '[' ? "areturn" : "ireturn");
    }
  }

  if (!patch_branches(&g)) fprintf(stderr, "Warning: unresolved JVM label in %s\n", m.name);

  if (listing) {
    fprintf(listing, "  .limit locals %d\n", g.max_locals > 0 ? g.max_locals : 1);
    fprintf(listing, ".end method\n");
  }

  m.code = g.code;
  g.code.data = NULL;
  g.code.size = g.code.cap = 0;
  m.max_stack = g.max_stack;
  m.max_locals = g.max_locals > 0 ? g.max_locals : 1;
  if (g.invoke_fixup_count > 0) {
    m.invoke_fixups = (JvmInvokeFixup*)calloc((size_t)g.invoke_fixup_count,
                                              sizeof(JvmInvokeFixup));
    if (m.invoke_fixups) {
      for (i = 0; i < g.invoke_fixup_count; ++i) {
        m.invoke_fixups[i].operand_offset = g.invoke_fixups[i].operand_offset;
        m.invoke_fixups[i].name = jvm_strdup(g.invoke_fixups[i].name);
        m.invoke_fixups[i].desc = jvm_strdup(g.invoke_fixups[i].desc);
      }
      m.invoke_fixup_count = g.invoke_fixup_count;
    }
  }
  free_method_gen(&g);
  return m;
}

static void write_u1(FILE* f, int x) { fputc(x & 0xff, f); }
static void write_u2(FILE* f, int x) {
  fputc((x >> 8) & 0xff, f);
  fputc(x & 0xff, f);
}
static void write_u4(FILE* f, uint32_t x) {
  fputc((int)((x >> 24) & 0xff), f);
  fputc((int)((x >> 16) & 0xff), f);
  fputc((int)((x >> 8) & 0xff), f);
  fputc((int)(x & 0xff), f);
}

static void cp_write(FILE* out, CpPool* cp) {
  int i;
  write_u2(out, cp->count + 1);
  for (i = 1; i <= cp->count; ++i) {
    CpEntry* e = &cp->items[i];
    write_u1(out, e->tag);
    switch (e->tag) {
      case CP_UTF8:
        write_u2(out, (int)strlen(e->a));
        fwrite(e->a, 1, strlen(e->a), out);
        break;
      case CP_INTEGER:
        write_u4(out, (uint32_t)e->i);
        break;
      case CP_CLASS:
      case CP_STRING:
        write_u2(out, e->x);
        break;
      case CP_FIELDREF:
      case CP_METHODREF:
      case CP_NAMEANDTYPE:
        write_u2(out, e->x);
        write_u2(out, e->y);
        break;
    }
  }
}

static char* class_path_for_asm(const char* asm_outfile) {
  char buf[1024];
  char* slash;
  snprintf(buf, sizeof(buf), "%s", asm_outfile ? asm_outfile : "SimpleLangProgram.jasm");
  slash = strrchr(buf, '/');
  if (!slash) slash = strrchr(buf, '\\');
  if (slash) {
    slash[1] = '\0';
    strncat(buf, "SimpleLangProgram.class", sizeof(buf) - strlen(buf) - 1);
  } else {
    snprintf(buf, sizeof(buf), "SimpleLangProgram.class");
  }
  return jvm_strdup(buf);
}

static void free_methods(JvmMethod* methods, int count) {
  int i;
  for (i = 0; i < count; ++i) {
    free(methods[i].name);
    free(methods[i].desc);
    for (int j = 0; j < methods[i].invoke_fixup_count; ++j) {
      free(methods[i].invoke_fixups[j].name);
      free(methods[i].invoke_fixups[j].desc);
    }
    free(methods[i].invoke_fixups);
    bv_free(&methods[i].code);
  }
  free(methods);
}

int generate_jvm_classfile(AnalysisResult* res, const char* asm_outfile) {
  const char* class_name = "SimpleLangProgram";
  FILE* listing = NULL;
  FILE* out = NULL;
  char* class_path = NULL;
  CpPool cp;
  JvmMethod* methods = NULL;
  int method_count = 0, method_cap = 0;
  int i, j;
  int code_attr, this_class, super_class;
  int system_out_ref, system_in_ref, ps_print_char_ref, input_read_ref;
  int wrapper_name, wrapper_desc, user_main_ref;
  ByteVec wrapper_code;

  memset(&cp, 0, sizeof(cp));
  if (!res || !asm_outfile) return 1;

  listing = fopen(asm_outfile, "wb");
  if (!listing) return 1;
  fprintf(listing, "; JVM assembly listing generated from SimpleLang CFG\n");
  fprintf(listing, ".class public %s\n.super java/lang/Object\n", class_name);

  for (i = 0; i < res->files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (j = 0; j < sf->functions_count; ++j) {
      if (method_count >= method_cap) {
        int nc = method_cap ? method_cap * 2 : 16;
        JvmMethod* next = (JvmMethod*)realloc(methods, sizeof(JvmMethod) * (size_t)nc);
        if (!next) {
          fclose(listing);
          free_methods(methods, method_count);
          return 1;
        }
        methods = next;
        method_cap = nc;
      }
      methods[method_count++] = compile_function(sf->functions[j], res, listing);
    }
  }

  bv_init(&wrapper_code);
  fprintf(listing, "\n.method public static main([Ljava/lang/String;)V\n");
  fprintf(listing, "    invokestatic %s/main()I\n", class_name);
  fprintf(listing, "    pop\n");
  fprintf(listing, "    return\n");
  fprintf(listing, "  .limit stack 1\n  .limit locals 1\n.end method\n");
  bv_u1(&wrapper_code, 0xb8);
  bv_u2(&wrapper_code, 0);
  bv_u1(&wrapper_code, 0x57);
  bv_u1(&wrapper_code, 0xb1);
  fclose(listing);

  code_attr = cp_utf8(&cp, "Code");
  this_class = cp_class(&cp, class_name);
  super_class = cp_class(&cp, "java/lang/Object");
  system_out_ref = cp_ref(&cp, CP_FIELDREF, "java/lang/System", "out",
                          "Ljava/io/PrintStream;");
  system_in_ref = cp_ref(&cp, CP_FIELDREF, "java/lang/System", "in",
                         "Ljava/io/InputStream;");
  ps_print_char_ref = cp_ref(&cp, CP_METHODREF, "java/io/PrintStream", "print",
                             "(C)V");
  input_read_ref = cp_ref(&cp, CP_METHODREF, "java/io/InputStream", "read", "()I");
  (void)system_out_ref;
  (void)system_in_ref;
  (void)ps_print_char_ref;
  (void)input_read_ref;

  for (i = 0; i < method_count; ++i) {
    cp_utf8(&cp, methods[i].name);
    cp_utf8(&cp, methods[i].desc);
    cp_ref(&cp, CP_METHODREF, class_name, methods[i].name, methods[i].desc);
  }
  wrapper_name = cp_utf8(&cp, "main");
  wrapper_desc = cp_utf8(&cp, "([Ljava/lang/String;)V");
  user_main_ref = cp_ref(&cp, CP_METHODREF, class_name, "main", "()I");
  bv_patch_u2(&wrapper_code, 1, user_main_ref);

  for (i = 0; i < method_count; ++i) {
    unsigned char* p = methods[i].code.data;
    int k = 0;
    while (k < methods[i].code.size) {
      unsigned char op = p[k++];
      if ((op == 0xb2 || op == 0xb6 || op == 0xb8) && k + 1 < methods[i].code.size) {
        int marker = ((int)p[k] << 8) | p[k + 1];
        int ref = marker;
        if (marker == 0xfff1) ref = system_out_ref;
        else if (marker == 0xfff2) ref = system_in_ref;
        else if (marker == 0xfff3) ref = ps_print_char_ref;
        else if (marker == 0xfff4) ref = input_read_ref;
        else if (marker == 0xfff5) {
          int q;
          ref = user_main_ref;
          for (q = 0; q < methods[i].invoke_fixup_count; ++q) {
            if (methods[i].invoke_fixups[q].operand_offset == k) {
              ref = cp_ref(&cp, CP_METHODREF, class_name,
                           methods[i].invoke_fixups[q].name,
                           methods[i].invoke_fixups[q].desc);
              break;
            }
          }
        }
        p[k] = (unsigned char)((ref >> 8) & 0xff);
        p[k + 1] = (unsigned char)(ref & 0xff);
        k += 2;
      } else if (op == 0x10 || op == 0x15 || op == 0x19 || op == 0x36 ||
                 op == 0x3a || op == 0xbc) {
        k += 1;
      } else if (op == 0x11 || op == 0xa7 || (op >= 0x99 && op <= 0xa4)) {
        k += 2;
      }
    }
  }

  class_path = class_path_for_asm(asm_outfile);
  out = fopen(class_path, "wb");
  if (!out) {
    free(class_path);
    cp_free(&cp);
    free_methods(methods, method_count);
    bv_free(&wrapper_code);
    return 1;
  }

  write_u4(out, 0xCAFEBABE);
  write_u2(out, 0);
  write_u2(out, 49);
  cp_write(out, &cp);
  write_u2(out, 0x0021);
  write_u2(out, this_class);
  write_u2(out, super_class);
  write_u2(out, 0);
  write_u2(out, 0);
  write_u2(out, method_count + 1);

  for (i = 0; i < method_count; ++i) {
    write_u2(out, 0x0009);
    write_u2(out, cp_utf8(&cp, methods[i].name));
    write_u2(out, cp_utf8(&cp, methods[i].desc));
    write_u2(out, 1);
    write_u2(out, code_attr);
    write_u4(out, (uint32_t)(12 + methods[i].code.size));
    write_u2(out, methods[i].max_stack);
    write_u2(out, methods[i].max_locals);
    write_u4(out, (uint32_t)methods[i].code.size);
    fwrite(methods[i].code.data, 1, (size_t)methods[i].code.size, out);
    write_u2(out, 0);
    write_u2(out, 0);
  }

  write_u2(out, 0x0009);
  write_u2(out, wrapper_name);
  write_u2(out, wrapper_desc);
  write_u2(out, 1);
  write_u2(out, code_attr);
  write_u4(out, (uint32_t)(12 + wrapper_code.size));
  write_u2(out, 1);
  write_u2(out, 1);
  write_u4(out, (uint32_t)wrapper_code.size);
  fwrite(wrapper_code.data, 1, (size_t)wrapper_code.size, out);
  write_u2(out, 0);
  write_u2(out, 0);

  write_u2(out, 0);
  fclose(out);

  fprintf(stderr, "JVM class written to %s\n", class_path);

  free(class_path);
  cp_free(&cp);
  free_methods(methods, method_count);
  bv_free(&wrapper_code);
  return 0;
}
