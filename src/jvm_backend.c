#include "cfg_builder.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JVM_MAX_LABELS = 512, JVM_MAX_FIXUPS = 512, JVM_MAX_LOCALS = 256 };
#define JVM_HEAP_SLOTS 16384
#define JVM_CLASS_NAME "SimpleLangProgram"

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
  char* type_name;
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
  for (i = 0; i < g->local_count; ++i) {
    free(g->locals[i].name);
    free(g->locals[i].type_name);
  }
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

static void local_set_type(JvmMethodGen* g, const char* name, const char* type_name) {
  int i;
  if (!g || !name || !type_name) return;
  for (i = 0; i < g->local_count; ++i) {
    if (strcmp(g->locals[i].name, name) == 0) {
      int was_wide = g->locals[i].type_name &&
                     (strcmp(g->locals[i].type_name, "long") == 0 ||
                      strcmp(g->locals[i].type_name, "ulong") == 0);
      int is_wide = strcmp(type_name, "long") == 0 ||
                    strcmp(type_name, "ulong") == 0;
      free(g->locals[i].type_name);
      g->locals[i].type_name = jvm_strdup(type_name);
      if (strcmp(type_name, "string") == 0) g->locals[i].is_ref = 1;
      if (!was_wide && is_wide && g->locals[i].slot == g->max_locals - 1)
        g->max_locals++;
      return;
    }
  }
}

static const char* local_get_type(JvmMethodGen* g, const char* name) {
  int i;
  if (!g || !name) return NULL;
  for (i = 0; i < g->local_count; ++i)
    if (strcmp(g->locals[i].name, name) == 0) return g->locals[i].type_name;
  return NULL;
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

static int jvm_type_is_ref(const char* type_name) {
  return type_name && strcmp(type_name, "string") == 0;
}

static int jvm_type_is_long(const char* type_name) {
  return type_name &&
         (strcmp(type_name, "long") == 0 || strcmp(type_name, "ulong") == 0);
}

static void emit_lload(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "lload %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x1e + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x16);
    bv_u1(&g->code, slot);
  }
}

static void emit_lstore(JvmMethodGen* g, int slot) {
  char line[64];
  snprintf(line, sizeof(line), "lstore %d", slot);
  if (slot >= 0 && slot <= 3) emit_u1(g, 0x3f + slot, line);
  else {
    if (g->listing) fprintf(g->listing, "    %s\n", line);
    bv_u1(&g->code, 0x37);
    bv_u1(&g->code, slot);
  }
}

static void emit_typed_load(JvmMethodGen* g, const char* name) {
  const char* type_name = local_get_type(g, name);
  int slot = local_slot(g, name, 1);
  if (jvm_type_is_ref(type_name) || local_is_ref(g, name))
    emit_aload(g, slot);
  else if (jvm_type_is_long(type_name))
    emit_lload(g, slot);
  else
    emit_iload(g, slot);
}

static void emit_typed_store(JvmMethodGen* g, const char* name) {
  const char* type_name = local_get_type(g, name);
  int slot = local_slot(g, name, 1);
  if (jvm_type_is_ref(type_name) || local_is_ref(g, name))
    emit_astore(g, slot);
  else if (jvm_type_is_long(type_name))
    emit_lstore(g, slot);
  else
    emit_istore(g, slot);
}

static int jvm_return_opcode(const char* desc) {
  if (!desc || strcmp(desc, "V") == 0) return 0xb1;
  if (strcmp(desc, "J") == 0) return 0xad;
  if (desc[0] == 'L' || desc[0] == '[') return 0xb0;
  return 0xac;
}

static const char* jvm_return_mnemonic(const char* desc) {
  if (!desc || strcmp(desc, "V") == 0) return "return";
  if (strcmp(desc, "J") == 0) return "lreturn";
  if (desc[0] == 'L' || desc[0] == '[') return "areturn";
  return "ireturn";
}

static void emit_default_value(JvmMethodGen* g, const char* desc) {
  if (desc && (desc[0] == 'L' || desc[0] == '[')) emit_u1(g, 0x01, "aconst_null");
  else if (desc && strcmp(desc, "J") == 0) {
    emit_u1(g, 0x09, "lconst_0");
  } else {
    emit_iconst(g, 0);
  }
}

/* marker values for constant-pool placeholders */
enum {
  JVM_MARKER_SYS_OUT    = 0xfff1,
  JVM_MARKER_SYS_IN     = 0xfff2,
  JVM_MARKER_PS_PRINT   = 0xfff3,
  JVM_MARKER_IS_READ    = 0xfff4,
  JVM_MARKER_INVOKE_USR = 0xfff5,
  JVM_MARKER_HEAP_FIELD = 0xfff6,
  JVM_MARKER_HP_FIELD   = 0xfff7,
  JVM_MARKER_ALLOC_MTH  = 0xfff8,
  /* Task 2 var.1: async-await runtime helpers */
  JVM_MARKER_AWAIT_MTH         = 0xfff9,
  JVM_MARKER_TASK_COMPLETE_MTH = 0xfffa
};

static void emit_invokestatic_user(JvmMethodGen* g, const char* name,
                                   const char* desc, const char* text) {
  if (!g || !name || !desc || g->invoke_fixup_count >= JVM_MAX_FIXUPS) return;
  if (g->listing && text) fprintf(g->listing, "    %s\n", text);
  bv_u1(&g->code, 0xb8);
  g->invoke_fixups[g->invoke_fixup_count].operand_offset = g->code.size;
  g->invoke_fixups[g->invoke_fixup_count].name = jvm_strdup(name);
  g->invoke_fixups[g->invoke_fixup_count].desc = jvm_strdup(desc);
  g->invoke_fixup_count++;
  bv_u2(&g->code, JVM_MARKER_INVOKE_USR);
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

/* Accept "ident" or "ident.ident" as a valid array-base name. */
static int is_valid_array_base(const char* s) {
  const char* dot;
  char part[128];
  if (!s) return 0;
  dot = strchr(s, '.');
  if (!dot) return is_ident_text(s);
  snprintf(part, sizeof(part), "%.*s", (int)(dot - s), s);
  return is_ident_text(part) && is_ident_text(dot + 1);
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
  return is_valid_array_base(name) && index[0] != '\0';
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

/* Sanitize a name for use as a JVM method/field identifier.
   Replaces characters illegal in JVM names (<>[]/ etc.) with '_'. */
static char* jvm_sanitize_name(const char* s) {
  char* out;
  int i;
  if (!s) s = "unknown";
  out = jvm_strdup(s);
  for (i = 0; out[i]; ++i) {
    char c = out[i];
    if (c == '<' || c == '>' || c == '[' || c == ']' || c == '/' ||
        c == ';' || c == '.' || c == ' ')
      out[i] = '_';
  }
  return out;
}

static char* jvm_func_name(FunctionCFG* f) {
  return jvm_sanitize_name((f && f->func_name) ? f->func_name : "unknown");
}

static const char* jvm_type_desc(const char* type_name);

static const char* jvm_ret_desc(FunctionCFG* f) {
  /* Async functions always return an int Task handle, regardless of the
     user-visible return type (Task 2 var.1). */
  if (f && f->is_async) return "I";
  return jvm_type_desc((f && f->return_type) ? f->return_type : "int");
}

/* Local slot name used to hold the heap handle of the Task this async
   function will complete on return. */
#define JVM_ASYNC_TASK_LOCAL "__async_task"

static void emit_invokestatic_marker(JvmMethodGen* g, int marker,
                                     const char* listing_text) {
  if (g->listing && listing_text) fprintf(g->listing, "    %s\n", listing_text);
  bv_u1(&g->code, 0xb8);
  bv_u2(&g->code, marker);
}

/* emit_async_return_complete is defined after eval_expr. */
static void emit_async_return_complete(JvmMethodGen* g, const char* expr);

static const char* jvm_type_desc(const char* type_name) {
  if (type_name && strcmp(type_name, "void") == 0) return "V";
  if (type_name && strcmp(type_name, "bool") == 0) return "Z";
  if (type_name && strcmp(type_name, "byte") == 0) return "B";
  if (type_name && strcmp(type_name, "char") == 0) return "C";
  if (type_name && (strcmp(type_name, "long") == 0 ||
                    strcmp(type_name, "ulong") == 0)) return "J";
  if (type_name && strcmp(type_name, "string") == 0) return "Ljava/lang/String;";
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

/* ---- HEAP-based object/array model helpers ---- */

static UserTypeInfo* jvm_find_utype(AnalysisResult* res, const char* type_name) {
  int i;
  if (!res || !type_name) return NULL;
  for (i = 0; i < res->type_count; ++i) {
    if (res->types[i].name && strcmp(res->types[i].name, type_name) == 0)
      return &res->types[i];
  }
  return NULL;
}

static int jvm_field_slot_of_type(AnalysisResult* res, const char* type_name,
                                   const char* field_name) {
  UserTypeInfo* type = jvm_find_utype(res, type_name);
  int i;
  if (!type) return 0;
  for (i = 0; i < type->field_count; ++i) {
    if (strcmp(type->fields[i].name, field_name) == 0)
      return type->fields[i].offset / 4;
  }
  return 0;
}

static const char* jvm_field_type_of_type(AnalysisResult* res,
                                           const char* type_name,
                                           const char* field_name) {
  UserTypeInfo* type = jvm_find_utype(res, type_name);
  int i;
  if (!type) return NULL;
  for (i = 0; i < type->field_count; ++i)
    if (strcmp(type->fields[i].name, field_name) == 0)
      return type->fields[i].type_name;
  return NULL;
}

static int jvm_parse_type_args(const char* type_name, char out[][128],
                               int max_args) {
  const char* lt;
  const char* p;
  char token[256];
  int token_len = 0;
  int depth = 0;
  int count = 0;
  if (!type_name || !out || max_args <= 0) return 0;
  lt = strchr(type_name, '<');
  if (!lt) return 0;
  for (p = lt + 1; *p; ++p) {
    if (*p == '>' && depth == 0) {
      if (token_len > 0 && count < max_args) {
        token[token_len] = '\0';
        snprintf(out[count], 128, "%s", token);
        trim(out[count++]);
      }
      break;
    }
    if (*p == ',' && depth == 0) {
      if (token_len > 0 && count < max_args) {
        token[token_len] = '\0';
        snprintf(out[count], 128, "%s", token);
        trim(out[count++]);
      }
      token_len = 0;
      continue;
    }
    if (*p == '<') depth++;
    else if (*p == '>' && depth > 0) depth--;
    if (token_len + 1 < (int)sizeof(token)) token[token_len++] = *p;
  }
  return count;
}

static const char* jvm_resolve_template_type(AnalysisResult* res,
                                             const char* owner_type_name,
                                             const char* field_type_name) {
  static char resolved[256];
  char base_name[128];
  char actual_args[16][128];
  UserTypeInfo* base_type;
  const char* lt;
  const char* p;
  int argc;
  int out = 0;
  if (!field_type_name) return NULL;
  if (!owner_type_name || !strchr(owner_type_name, '<')) return field_type_name;
  lt = strchr(owner_type_name, '<');
  snprintf(base_name, sizeof(base_name), "%.*s", (int)(lt - owner_type_name),
           owner_type_name);
  trim(base_name);
  base_type = jvm_find_utype(res, base_name);
  argc = jvm_parse_type_args(owner_type_name, actual_args, 16);
  if (!base_type || base_type->template_param_count <= 0 || argc <= 0)
    return field_type_name;

  for (p = field_type_name; *p && out + 1 < (int)sizeof(resolved);) {
    if (isalpha((unsigned char)*p) || *p == '_') {
      char ident[128];
      int ident_len = 0;
      int replaced = 0;
      int i;
      while ((isalnum((unsigned char)*p) || *p == '_') &&
             ident_len + 1 < (int)sizeof(ident)) {
        ident[ident_len++] = *p++;
      }
      ident[ident_len] = '\0';
      for (i = 0; i < base_type->template_param_count && i < argc; ++i) {
        if (strcmp(base_type->template_params[i], ident) == 0) {
          int n = (int)strlen(actual_args[i]);
          if (out + n >= (int)sizeof(resolved))
            n = (int)sizeof(resolved) - out - 1;
          memcpy(resolved + out, actual_args[i], (size_t)n);
          out += n;
          replaced = 1;
          break;
        }
      }
      if (!replaced) {
        int n = (int)strlen(ident);
        if (out + n >= (int)sizeof(resolved))
          n = (int)sizeof(resolved) - out - 1;
        memcpy(resolved + out, ident, (size_t)n);
        out += n;
      }
      continue;
    }
    resolved[out++] = *p++;
  }
  resolved[out] = '\0';
  return resolved;
}

static int jvm_instance_slots(AnalysisResult* res, const char* type_name) {
  UserTypeInfo* type = jvm_find_utype(res, type_name);
  if (!type || type->instance_size <= 0) return 0;
  return (type->instance_size + 3) / 4;
}

static int split_member_access(const char* expr, char* owner, size_t osz,
                                char* field, size_t fsz) {
  const char* dot;
  if (!expr) return 0;
  dot = strchr(expr, '.');
  if (!dot) return 0;
  if (strchr(dot + 1, '(')) return 0; /* method call, not field */
  if (strchr(dot + 1, '.')) return 0; /* nested access, too complex */
  snprintf(owner, osz, "%.*s", (int)(dot - expr), expr);
  snprintf(field, fsz, "%s", dot + 1);
  trim(owner);
  trim(field);
  return owner[0] != '\0' && field[0] != '\0' && is_ident_text(field);
}

static int split_member_call_text(const char* work, char* owner, size_t osz,
                                   char* method_out, size_t msz,
                                   char args[][128], int* argc) {
  const char* lp;
  const char* p;
  const char* dot = NULL;
  char tmp[512];
  char dummy[128];
  lp = strchr(work, '(');
  if (!lp) return 0;
  for (p = lp - 1; p >= work; p--) {
    if (*p == '.') { dot = p; break; }
    if (!isalnum((unsigned char)*p) && *p != '_') break;
  }
  if (!dot || dot == work) return 0;
  snprintf(owner, osz, "%.*s", (int)(dot - work), work);
  trim(owner);
  if (!is_ident_text(owner) && strcmp(owner, "this") != 0) return 0;
  snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(lp - dot - 1), dot + 1, lp);
  return split_call(tmp, method_out, msz, args, argc);
}

static int jvm_field_idx(JvmMethodGen* g, const char* owner_name,
                          const char* field_name) {
  const char* tname;
  if (strcmp(owner_name, "this") == 0)
    tname = (g->function) ? g->function->owner_type : NULL;
  else
    tname = local_get_type(g, owner_name);
  if (!tname) return 0;
  return jvm_field_slot_of_type(g->analysis, tname, field_name);
}

static const char* jvm_owner_field_type(JvmMethodGen* g, const char* owner_name,
                                         const char* field_name) {
  const char* tname;
  const char* field_type;
  if (strcmp(owner_name, "this") == 0)
    tname = (g->function) ? g->function->owner_type : NULL;
  else
    tname = local_get_type(g, owner_name);
  if (!tname) return NULL;
  field_type = jvm_field_type_of_type(g->analysis, tname, field_name);
  return jvm_resolve_template_type(g->analysis, tname, field_type);
}

/* Forward declarations */
static FunctionCFG* find_function_arity(AnalysisResult* res, const char* name, int argc);

/* Infer type of an expression (no bytecode emitted). */
static const char* infer_expr_type(JvmMethodGen* g, const char* expr);

static const char* infer_expr_type(JvmMethodGen* g, const char* expr) {
  char work[512];
  char owner[128], field[128];
  char base[128], idx[128];
  char cname[128];
  char args[16][128];
  int argc = 0;
  FunctionCFG* f;
  if (!expr || !g) return "int";
  snprintf(work, sizeof(work), "%s", expr);
  trim(work);
  if (!work[0]) return "int";
  /* member access: this.field or obj.field */
  if (split_member_access(work, owner, sizeof(owner), field, sizeof(field))) {
    const char* ft = jvm_owner_field_type(g, owner, field);
    return ft ? ft : "int";
  }
  /* array element: base[idx] */
  if (split_array_access(work, base, sizeof(base), idx, sizeof(idx))) {
    const char* bt = infer_expr_type(g, base);
    /* strip array suffix from bt */
    if (bt) {
      const char* lb = strchr(bt, '[');
      if (lb && lb > bt) {
        static char elem[128];
        snprintf(elem, sizeof(elem), "%.*s", (int)(lb - bt), bt);
        trim(elem);
        return elem;
      }
    }
    return "int";
  }
  /* function call */
  if (split_call(work, cname, sizeof(cname), args, &argc)) {
    f = find_function_arity(g->analysis, cname, argc);
    return (f && f->return_type) ? f->return_type : "int";
  }
  /* simple local */
  {
    const char* lt = local_get_type(g, work);
    return lt ? lt : "int";
  }
}

/* Find best matching global function using inferred arg types. */
static FunctionCFG* find_function_typed(JvmMethodGen* g, const char* name,
                                         char args[][128], int argc) {
  int i, j;
  FunctionCFG* fallback = NULL;
  const char* arg_types[16];
  for (i = 0; i < argc && i < 16; ++i)
    arg_types[i] = infer_expr_type(g, args[i]);
  for (i = 0; i < g->analysis->files_count; ++i) {
    SourceFileInfo* sf = g->analysis->files[i];
    if (!sf) continue;
    for (j = 0; j < sf->functions_count; ++j) {
      FunctionCFG* f = sf->functions[j];
      if (!f || f->owner_type) continue;
      if (f->source_name && strcmp(f->source_name, name) == 0) {
        if (f->param_count == argc) {
          int match = 1;
          int pi;
          for (pi = 0; pi < argc; ++pi) {
            if (arg_types[pi] && f->param_types && f->param_types[pi] &&
                strcmp(arg_types[pi], f->param_types[pi]) != 0) {
              match = 0;
              break;
            }
          }
          if (match) return f;
          if (!fallback) fallback = f;
        }
      }
    }
  }
  return fallback ? fallback : find_function_arity(g->analysis, name, argc);
}

/* Find method CFG for owner type + source name + user-visible argc
   (does not count the implicit 'this' param). */
static FunctionCFG* find_method_cfg(AnalysisResult* res,
                                     const char* owner_type,
                                     const char* source_name, int user_argc) {
  int i, j;
  FunctionCFG* fallback = NULL;
  if (!res || !owner_type || !source_name) return NULL;
  for (i = 0; i < res->files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (j = 0; j < sf->functions_count; ++j) {
      FunctionCFG* f = sf->functions[j];
      if (!f || !f->owner_type) continue;
      if (strcmp(f->owner_type, owner_type) != 0) continue;
      if (!f->source_name || strcmp(f->source_name, source_name) != 0) continue;
      if (f->param_count == user_argc + 1) return f;
      if (!fallback) fallback = f;
    }
  }
  return fallback;
}

/* ---- HEAP bytecode emitters ---- */

static void emit_getstatic_heap(JvmMethodGen* g) {
  emit_u2_arg(g, 0xb2, JVM_MARKER_HEAP_FIELD,
              "getstatic " JVM_CLASS_NAME "/HEAP [I");
}

static void emit_alloc_call(JvmMethodGen* g) {
  emit_u2_arg(g, 0xb8, JVM_MARKER_ALLOC_MTH,
              "invokestatic " JVM_CLASS_NAME "/alloc(I)I");
}

/* Push the base address (int) of an array onto the JVM operand stack.
   The base may be a simple local or a field of an object. */
static void emit_push_array_base(JvmMethodGen* g, const char* base_expr);

static void emit_push_array_base(JvmMethodGen* g, const char* base_expr) {
  char owner[128], field[128];
  if (split_member_access(base_expr, owner, sizeof(owner), field, sizeof(field))) {
    int ptr_slot = local_slot(g, owner, 0);
    int fi = jvm_field_idx(g, owner, field);
    emit_getstatic_heap(g);
    emit_iload(g, ptr_slot);
    emit_iconst(g, fi);
    emit_u1(g, 0x60, "iadd");
    emit_u1(g, 0x2e, "iaload");
  } else {
    emit_iload(g, local_slot(g, base_expr, 0));
  }
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
  /* __await(taskHandle) — built-in: read result slot from heap (Task 2 var.1). */
  if (strcmp(name, "__await") == 0 && argc == 1) {
    eval_expr(g, args[0]);
    emit_invokestatic_marker(g, JVM_MARKER_AWAIT_MTH,
                             "invokestatic " JVM_CLASS_NAME "/__await(I)I");
    return;
  }
  if (strcmp(name, "in") == 0 && argc == 0) {
    char* eof_ok = jvm_strdupf("L_jvm_in_ok_%s_%d", jvm_func_name(g->function),
                               g->temp_label_counter++);
    emit_u2_arg(g, 0xb2, JVM_MARKER_SYS_IN, "getstatic java/lang/System/in Ljava/io/InputStream;");
    emit_u2_arg(g, 0xb6, JVM_MARKER_IS_READ, "invokevirtual java/io/InputStream/read()I");
    emit_u1(g, 0x59, "dup");
    emit_branch(g, 0x9c, eof_ok, "ifge");
    emit_u1(g, 0x57, "pop");
    emit_iconst(g, 0);
    mark_label(g, eof_ok);
    free(eof_ok);
    return;
  }
  /* unqualified method call within current class: prepend 'this' */
  if (g->function && g->function->owner_type) {
    FunctionCFG* mf = find_method_cfg(g->analysis, g->function->owner_type,
                                      name, argc);
    if (mf) {
      emit_iload(g, local_slot(g, "this", 0));
      for (i = 0; i < argc; ++i) eval_expr(g, args[i]);
      desc = jvm_method_desc(mf);
      snprintf(line, sizeof(line), "invokestatic " JVM_CLASS_NAME "/%s%s",
               jvm_func_name(mf), desc);
      emit_invokestatic_user(g, jvm_func_name(mf), desc, line);
      free(desc);
      return;
    }
  }
  /* global function call with type-aware overload resolution */
  for (i = 0; i < argc; ++i) eval_expr(g, args[i]);
  f = find_function_typed(g, name, args, argc);
  if (!f) f = find_function(g->analysis, name);
  desc = jvm_method_desc(f);
  snprintf(line, sizeof(line), "invokestatic " JVM_CLASS_NAME "/%s%s",
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
  /* member call: obj.method(args) returns a value */
  {
    char mc_owner[128], mc_method[128];
    char mc_args[16][128];
    int mc_argc = 0;
    if (split_member_call_text(work, mc_owner, sizeof(mc_owner),
                               mc_method, sizeof(mc_method), mc_args, &mc_argc)) {
      const char* otype = (strcmp(mc_owner, "this") == 0 && g->function)
                          ? g->function->owner_type
                          : local_get_type(g, mc_owner);
      if (otype) {
        FunctionCFG* mf = find_method_cfg(g->analysis, otype, mc_method, mc_argc);
        if (mf) {
          int mi;
          emit_iload(g, local_slot(g, mc_owner, 0));
          for (mi = 0; mi < mc_argc; ++mi) eval_expr(g, mc_args[mi]);
          {
            char* mdesc = jvm_method_desc(mf);
            char mline[512];
            snprintf(mline, sizeof(mline), "invokestatic " JVM_CLASS_NAME "/%s%s",
                     jvm_func_name(mf), mdesc);
            emit_invokestatic_user(g, jvm_func_name(mf), mdesc, mline);
            free(mdesc);
          }
          return;
        }
      }
    }
  }
  if (split_call(work, name, sizeof(name), args, &argc)) {
    eval_call(g, name, args, argc);
    return;
  }
  /* member field access: this.field or obj.field */
  {
    char ma_owner[128], ma_field[128];
    if (split_member_access(work, ma_owner, sizeof(ma_owner),
                            ma_field, sizeof(ma_field))) {
      int ptr_slot = local_slot(g, ma_owner, 0);
      int fi = jvm_field_idx(g, ma_owner, ma_field);
      emit_getstatic_heap(g);
      emit_iload(g, ptr_slot);
      emit_iconst(g, fi);
      emit_u1(g, 0x60, "iadd");
      emit_u1(g, 0x2e, "iaload");
      return;
    }
  }
  /* array element access: arr[i] or this.arr[i] using HEAP */
  if (split_array_access(work, name, sizeof(name), index, sizeof(index))) {
    emit_getstatic_heap(g);
    emit_push_array_base(g, name);
    eval_expr(g, index);
    emit_u1(g, 0x60, "iadd");
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
  emit_typed_load(g, work);
}

static void emit_async_return_complete(JvmMethodGen* g, const char* expr) {
  /* Emits: load task, evaluate expr, invokestatic __task_complete(II)I, ireturn.
     Used for `return EXPR;` inside an async body (Task 2 var.1). */
  emit_iload(g, local_slot(g, JVM_ASYNC_TASK_LOCAL, 0));
  if (expr && *expr)
    eval_expr(g, expr);
  else
    emit_iconst(g, 0);
  emit_invokestatic_marker(g, JVM_MARKER_TASK_COMPLETE_MTH,
                           "invokestatic " JVM_CLASS_NAME "/__task_complete(II)I");
  emit_u1(g, 0xac, "ireturn");
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
  emit_u2_arg(g, 0xb2, JVM_MARKER_SYS_OUT,
              "getstatic java/lang/System/out Ljava/io/PrintStream;");
  eval_expr(g, expr);
  emit_u1(g, 0x92, "i2c");
  emit_u2_arg(g, 0xb6, JVM_MARKER_PS_PRINT,
              "invokevirtual java/io/PrintStream/print(C)V");
}

static void emit_out_char_value(JvmMethodGen* g, int value) {
  emit_u2_arg(g, 0xb2, JVM_MARKER_SYS_OUT,
              "getstatic java/lang/System/out Ljava/io/PrintStream;");
  emit_iconst(g, value);
  emit_u1(g, 0x92, "i2c");
  emit_u2_arg(g, 0xb6, JVM_MARKER_PS_PRINT,
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
      trim(type_name);
      trim(name);
      trim(expr);
      /* Remove trailing ';' from name and expr if any */
      { int ilen = (int)strlen(name);
        if (ilen > 0 && name[ilen-1] == ';') name[ilen-1] = '\0'; }
      { int ilen = (int)strlen(expr);
        if (ilen > 0 && expr[ilen-1] == ';') expr[ilen-1] = '\0'; }
      /* Array declaration: name has form foo[N] */
      if (split_array_access(name, array_name, sizeof(array_name), array_index,
                             sizeof(array_index))) {
        local_slot(g, array_name, 1);
        local_set_type(g, array_name, type_name);
        eval_expr(g, array_index);
        emit_alloc_call(g);
        emit_istore(g, local_slot(g, array_name, 1));
        return;
      }
      local_slot(g, name, 1);
      local_set_type(g, name, type_name);
      /* User-defined type without explicit initializer: allocate object */
      if (!expr[0] && jvm_find_utype(g->analysis, type_name)) {
        int n_slots = jvm_instance_slots(g->analysis, type_name);
        if (n_slots > 0) {
          emit_iconst(g, n_slots);
          emit_alloc_call(g);
        } else {
          emit_iconst(g, 0);
        }
        emit_istore(g, local_slot(g, name, 1));
        return;
      }
      if (expr[0]) eval_expr(g, expr);
      else emit_default_value(g, jvm_type_desc(type_name));
      emit_typed_store(g, name);
    }
    return;
  }
  if (starts_with(work, "return")) {
    char* p = work + 6;
    const char* ret_desc;
    trim(p);
    ret_desc = jvm_ret_desc(g->function);
    if (g->function && g->function->is_async) {
      /* Async return: complete the Task with the value (or 0 for void). */
      emit_async_return_complete(g, p);
    } else if (strcmp(ret_desc, "V") == 0 || p[0] == '\0') {
      emit_u1(g, 0xb1, "return");
    } else {
      eval_expr(g, p);
      emit_u1(g, jvm_return_opcode(ret_desc), jvm_return_mnemonic(ret_desc));
    }
    g->terminated = 1;
    return;
  }
  /* member call statement: obj.method(args) */
  {
    char mc_owner[128], mc_method[128];
    char mc_args[16][128];
    int mc_argc = 0;
    if (split_member_call_text(work, mc_owner, sizeof(mc_owner),
                               mc_method, sizeof(mc_method), mc_args, &mc_argc)) {
      const char* otype = (strcmp(mc_owner, "this") == 0 && g->function)
                          ? g->function->owner_type
                          : local_get_type(g, mc_owner);
      if (otype) {
        FunctionCFG* mf = find_method_cfg(g->analysis, otype, mc_method, mc_argc);
        if (mf) {
          int mi;
          char* mdesc;
          char mline[512];
          emit_iload(g, local_slot(g, mc_owner, 0));
          for (mi = 0; mi < mc_argc; ++mi) eval_expr(g, mc_args[mi]);
          mdesc = jvm_method_desc(mf);
          snprintf(mline, sizeof(mline), "invokestatic " JVM_CLASS_NAME "/%s%s",
                   jvm_func_name(mf), mdesc);
          emit_invokestatic_user(g, jvm_func_name(mf), mdesc, mline);
          free(mdesc);
          if (mf->return_type && strcmp(mf->return_type, "void") != 0)
            emit_u1(g, 0x57, "pop");
          return;
        }
      }
    }
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
    {
      FunctionCFG* cf = find_function_typed(g, call_name, args, argc);
      if (cf && strcmp(jvm_ret_desc(cf), "V") != 0)
        emit_u1(g, 0x57, "pop");
    }
    return;
  }
  eq = strchr(work, '=');
  if (eq && (eq == work || (eq[-1] != '!' && eq[-1] != '<' &&
                             eq[-1] != '>' && eq[-1] != '=')) &&
      eq[1] != '=') {
    snprintf(name, sizeof(name), "%.*s", (int)(eq - work), work);
    snprintf(expr, sizeof(expr), "%s", eq + 1);
    trim(name);
    trim(expr);
    /* member field assignment: this.field = expr or obj.field = expr */
    {
      char ma_owner[128], ma_field[128];
      if (split_member_access(name, ma_owner, sizeof(ma_owner),
                              ma_field, sizeof(ma_field))) {
        int ptr_slot = local_slot(g, ma_owner, 0);
        int fi = jvm_field_idx(g, ma_owner, ma_field);
        emit_getstatic_heap(g);
        emit_iload(g, ptr_slot);
        emit_iconst(g, fi);
        emit_u1(g, 0x60, "iadd");
        eval_expr(g, expr);
        emit_u1(g, 0x4f, "iastore");
        return;
      }
    }
    /* array element assignment: arr[i] = expr (HEAP-based) */
    if (split_array_access(name, array_name, sizeof(array_name), array_index,
                           sizeof(array_index))) {
      emit_getstatic_heap(g);
      emit_push_array_base(g, array_name);
      eval_expr(g, array_index);
      emit_u1(g, 0x60, "iadd");
      eval_expr(g, expr);
      emit_u1(g, 0x4f, "iastore");
      return;
    }
    eval_expr(g, expr);
    emit_typed_store(g, name);
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
      if (g->function && g->function->is_async) {
        /* Implicit fall-off: complete task with 0 (Task 2 var.1). */
        emit_async_return_complete(g, NULL);
      } else if (strcmp(ret_desc, "V") == 0) emit_u1(g, 0xb1, "return");
      else {
        emit_default_value(g, ret_desc);
        emit_u1(g, jvm_return_opcode(ret_desc), jvm_return_mnemonic(ret_desc));
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
    if (f->param_types && f->param_types[i])
      local_set_type(&g, f->params[i], f->param_types[i]);
  }

  /* Async prologue: allocate a 2-slot Task on the heap and stash its handle in
     a hidden local. Layout: HEAP[handle+0]=done flag, HEAP[handle+1]=result.
     The Task is filled in by __task_complete() at every return path. */
  if (f && f->is_async) {
    int task_slot = local_slot(&g, JVM_ASYNC_TASK_LOCAL, 1);
    local_set_type(&g, JVM_ASYNC_TASK_LOCAL, "Task");
    emit_iconst(&g, 2);
    emit_alloc_call(&g);
    emit_istore(&g, task_slot);
  }

  for (i = 0; i < f->node_count; ++i) emit_node(&g, f->nodes[i]);

  if (!g.terminated) {
    const char* ret_desc = jvm_ret_desc(f);
    if (f && f->is_async) {
      emit_async_return_complete(&g, NULL);
    } else if (strcmp(ret_desc, "V") == 0) emit_u1(&g, 0xb1, "return");
    else {
      emit_default_value(&g, ret_desc);
      emit_u1(&g, jvm_return_opcode(ret_desc), jvm_return_mnemonic(ret_desc));
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

/* Patch marker bytes in a ByteVec given resolved CP indices. */
static void patch_markers(ByteVec* bv, int heap_ref, int hp_ref, int alloc_ref,
                          int sout_ref, int sin_ref, int ps_ref, int ir_ref,
                          int await_ref, int task_complete_ref) {
  unsigned char* p = bv->data;
  int k = 0;
  while (k < bv->size) {
    unsigned char op = p[k++];
    if ((op == 0xb2 || op == 0xb3 || op == 0xb6 || op == 0xb8) &&
        k + 1 < bv->size) {
      int marker = ((int)p[k] << 8) | p[k + 1];
      int ref = marker;
      if (marker == JVM_MARKER_SYS_OUT)  ref = sout_ref;
      else if (marker == JVM_MARKER_SYS_IN)   ref = sin_ref;
      else if (marker == JVM_MARKER_PS_PRINT) ref = ps_ref;
      else if (marker == JVM_MARKER_IS_READ)  ref = ir_ref;
      else if (marker == JVM_MARKER_HEAP_FIELD) ref = heap_ref;
      else if (marker == JVM_MARKER_HP_FIELD)   ref = hp_ref;
      else if (marker == JVM_MARKER_ALLOC_MTH)  ref = alloc_ref;
      else if (marker == JVM_MARKER_AWAIT_MTH)         ref = await_ref;
      else if (marker == JVM_MARKER_TASK_COMPLETE_MTH) ref = task_complete_ref;
      p[k]     = (unsigned char)((ref >> 8) & 0xff);
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

int generate_jvm_classfile(AnalysisResult* res, const char* asm_outfile) {
  const char* class_name = JVM_CLASS_NAME;
  FILE* listing = NULL;
  FILE* out = NULL;
  char* class_path = NULL;
  CpPool cp;
  JvmMethod* methods = NULL;
  int method_count = 0, method_cap = 0;
  int i, j;
  int code_attr, this_class, super_class;
  int system_out_ref, system_in_ref, ps_print_char_ref, input_read_ref;
  int heap_field_ref, hp_field_ref, alloc_method_ref;
  int await_method_ref, task_complete_method_ref;
  int wrapper_name, wrapper_desc, user_main_ref;
  ByteVec wrapper_code, clinit_code, alloc_code, await_code, task_complete_code;

  memset(&cp, 0, sizeof(cp));
  if (!res || !asm_outfile) return 1;

  listing = fopen(asm_outfile, "wb");
  if (!listing) return 1;
  fprintf(listing, "; JVM assembly listing generated from SimpleLang CFG\n");
  fprintf(listing, ".class public %s\n.super java/lang/Object\n", class_name);
  fprintf(listing, ".field public static HEAP [I\n");
  fprintf(listing, ".field public static HP I\n");

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

  /* wrapper: public static main([Ljava/lang/String;)V calls main()I */
  bv_init(&wrapper_code);
  fprintf(listing, "\n.method public static main([Ljava/lang/String;)V\n");
  fprintf(listing, "    invokestatic %s/main()I\n", class_name);
  fprintf(listing, "    pop\n");
  fprintf(listing, "    return\n");
  fprintf(listing, "  .limit stack 1\n  .limit locals 1\n.end method\n");
  bv_u1(&wrapper_code, 0xb8);
  bv_u2(&wrapper_code, 0); /* patched to main()I ref */
  bv_u1(&wrapper_code, 0x57);
  bv_u1(&wrapper_code, 0xb1);

  /* static initializer: allocate HEAP int[16384], set HP=0 */
  bv_init(&clinit_code);
  fprintf(listing, "\n.method static <clinit>()V\n");
  fprintf(listing, "    sipush %d\n", JVM_HEAP_SLOTS);
  fprintf(listing, "    newarray int\n");
  fprintf(listing, "    putstatic %s/HEAP [I\n", class_name);
  fprintf(listing, "    iconst_0\n");
  fprintf(listing, "    putstatic %s/HP I\n", class_name);
  fprintf(listing, "    return\n.end method\n");
  bv_u1(&clinit_code, 0x11);
  bv_u2(&clinit_code, JVM_HEAP_SLOTS);
  bv_u1(&clinit_code, 0xbc);
  bv_u1(&clinit_code, 10); /* int */
  bv_u1(&clinit_code, 0xb3);
  bv_u2(&clinit_code, JVM_MARKER_HEAP_FIELD); /* putstatic HEAP */
  bv_u1(&clinit_code, 0x03); /* iconst_0 */
  bv_u1(&clinit_code, 0xb3);
  bv_u2(&clinit_code, JVM_MARKER_HP_FIELD);   /* putstatic HP */
  bv_u1(&clinit_code, 0xb1); /* return */

  /* alloc(int size) -> int: bump allocator on HP */
  bv_init(&alloc_code);
  fprintf(listing, "\n.method public static alloc(I)I\n");
  fprintf(listing, "    getstatic %s/HP I\n", class_name);
  fprintf(listing, "    dup\n");
  fprintf(listing, "    iload_0\n");
  fprintf(listing, "    iadd\n");
  fprintf(listing, "    putstatic %s/HP I\n", class_name);
  fprintf(listing, "    ireturn\n.end method\n");
  bv_u1(&alloc_code, 0xb2);
  bv_u2(&alloc_code, JVM_MARKER_HP_FIELD);  /* getstatic HP */
  bv_u1(&alloc_code, 0x59); /* dup */
  bv_u1(&alloc_code, 0x1a); /* iload_0 */
  bv_u1(&alloc_code, 0x60); /* iadd */
  bv_u1(&alloc_code, 0xb3);
  bv_u2(&alloc_code, JVM_MARKER_HP_FIELD);  /* putstatic HP */
  bv_u1(&alloc_code, 0xac); /* ireturn */

  /* __await(int handle) -> int: returns HEAP[handle+1] (Task 2 var.1).
     The Task layout is { done, result }. Async functions complete the Task
     synchronously before returning the handle, so __await() simply reads the
     stored result slot. */
  bv_init(&await_code);
  fprintf(listing, "\n.method public static __await(I)I\n");
  fprintf(listing, "    getstatic %s/HEAP [I\n", class_name);
  fprintf(listing, "    iload_0\n");
  fprintf(listing, "    iconst_1\n");
  fprintf(listing, "    iadd\n");
  fprintf(listing, "    iaload\n");
  fprintf(listing, "    ireturn\n.end method\n");
  bv_u1(&await_code, 0xb2);
  bv_u2(&await_code, JVM_MARKER_HEAP_FIELD); /* getstatic HEAP */
  bv_u1(&await_code, 0x1a);                  /* iload_0 */
  bv_u1(&await_code, 0x04);                  /* iconst_1 */
  bv_u1(&await_code, 0x60);                  /* iadd */
  bv_u1(&await_code, 0x2e);                  /* iaload */
  bv_u1(&await_code, 0xac);                  /* ireturn */

  /* __task_complete(int task, int result) -> int: stores result, sets done=1,
     returns task handle. Used as the tail call of every async return path. */
  bv_init(&task_complete_code);
  fprintf(listing, "\n.method public static __task_complete(II)I\n");
  fprintf(listing, "    getstatic %s/HEAP [I\n", class_name);
  fprintf(listing, "    iload_0\n");
  fprintf(listing, "    iconst_1\n");
  fprintf(listing, "    iadd\n");
  fprintf(listing, "    iload_1\n");
  fprintf(listing, "    iastore\n");
  fprintf(listing, "    getstatic %s/HEAP [I\n", class_name);
  fprintf(listing, "    iload_0\n");
  fprintf(listing, "    iconst_0\n");
  fprintf(listing, "    iadd\n");
  fprintf(listing, "    iconst_1\n");
  fprintf(listing, "    iastore\n");
  fprintf(listing, "    iload_0\n");
  fprintf(listing, "    ireturn\n.end method\n");
  bv_u1(&task_complete_code, 0xb2);
  bv_u2(&task_complete_code, JVM_MARKER_HEAP_FIELD); /* getstatic HEAP */
  bv_u1(&task_complete_code, 0x1a); /* iload_0 (task) */
  bv_u1(&task_complete_code, 0x04); /* iconst_1 */
  bv_u1(&task_complete_code, 0x60); /* iadd */
  bv_u1(&task_complete_code, 0x1b); /* iload_1 (result) */
  bv_u1(&task_complete_code, 0x4f); /* iastore */
  bv_u1(&task_complete_code, 0xb2);
  bv_u2(&task_complete_code, JVM_MARKER_HEAP_FIELD); /* getstatic HEAP */
  bv_u1(&task_complete_code, 0x1a); /* iload_0 */
  bv_u1(&task_complete_code, 0x03); /* iconst_0 */
  bv_u1(&task_complete_code, 0x60); /* iadd */
  bv_u1(&task_complete_code, 0x04); /* iconst_1 (done flag) */
  bv_u1(&task_complete_code, 0x4f); /* iastore */
  bv_u1(&task_complete_code, 0x1a); /* iload_0 */
  bv_u1(&task_complete_code, 0xac); /* ireturn */

  fclose(listing);

  code_attr    = cp_utf8(&cp, "Code");
  this_class   = cp_class(&cp, class_name);
  super_class  = cp_class(&cp, "java/lang/Object");
  system_out_ref    = cp_ref(&cp, CP_FIELDREF, "java/lang/System", "out",
                             "Ljava/io/PrintStream;");
  system_in_ref     = cp_ref(&cp, CP_FIELDREF, "java/lang/System", "in",
                             "Ljava/io/InputStream;");
  ps_print_char_ref = cp_ref(&cp, CP_METHODREF, "java/io/PrintStream", "print",
                             "(C)V");
  input_read_ref    = cp_ref(&cp, CP_METHODREF, "java/io/InputStream", "read", "()I");
  heap_field_ref    = cp_ref(&cp, CP_FIELDREF, class_name, "HEAP", "[I");
  hp_field_ref      = cp_ref(&cp, CP_FIELDREF, class_name, "HP", "I");

  for (i = 0; i < method_count; ++i) {
    cp_utf8(&cp, methods[i].name);
    cp_utf8(&cp, methods[i].desc);
    cp_ref(&cp, CP_METHODREF, class_name, methods[i].name, methods[i].desc);
  }
  /* alloc method ref */
  alloc_method_ref = cp_ref(&cp, CP_METHODREF, class_name, "alloc", "(I)I");
  /* async-await runtime helpers (Task 2 var.1) */
  await_method_ref = cp_ref(&cp, CP_METHODREF, class_name, "__await", "(I)I");
  task_complete_method_ref =
      cp_ref(&cp, CP_METHODREF, class_name, "__task_complete", "(II)I");

  wrapper_name  = cp_utf8(&cp, "main");
  wrapper_desc  = cp_utf8(&cp, "([Ljava/lang/String;)V");
  user_main_ref = cp_ref(&cp, CP_METHODREF, class_name, "main", "()I");
  /* Pre-add strings needed for <clinit> and alloc method_info,
     so all name/desc indices are valid when cp_write serializes the CP. */
  cp_utf8(&cp, "<clinit>");
  cp_utf8(&cp, "()V");
  bv_patch_u2(&wrapper_code, 1, user_main_ref);

  /* patch user methods */
  for (i = 0; i < method_count; ++i) {
    unsigned char* p = methods[i].code.data;
    int k = 0;
    while (k < methods[i].code.size) {
      unsigned char op = p[k++];
      if ((op == 0xb2 || op == 0xb3 || op == 0xb6 || op == 0xb8) &&
          k + 1 < methods[i].code.size) {
        int marker = ((int)p[k] << 8) | p[k + 1];
        int ref = marker;
        if (marker == JVM_MARKER_SYS_OUT)  ref = system_out_ref;
        else if (marker == JVM_MARKER_SYS_IN)   ref = system_in_ref;
        else if (marker == JVM_MARKER_PS_PRINT) ref = ps_print_char_ref;
        else if (marker == JVM_MARKER_IS_READ)  ref = input_read_ref;
        else if (marker == JVM_MARKER_HEAP_FIELD) ref = heap_field_ref;
        else if (marker == JVM_MARKER_HP_FIELD)   ref = hp_field_ref;
        else if (marker == JVM_MARKER_ALLOC_MTH)  ref = alloc_method_ref;
        else if (marker == JVM_MARKER_AWAIT_MTH)         ref = await_method_ref;
        else if (marker == JVM_MARKER_TASK_COMPLETE_MTH) ref = task_complete_method_ref;
        else if (marker == JVM_MARKER_INVOKE_USR) {
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
        p[k]     = (unsigned char)((ref >> 8) & 0xff);
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
  /* patch clinit, alloc, and the async-await helper bytecodes */
  patch_markers(&clinit_code, heap_field_ref, hp_field_ref, alloc_method_ref,
                system_out_ref, system_in_ref, ps_print_char_ref, input_read_ref,
                await_method_ref, task_complete_method_ref);
  patch_markers(&alloc_code, heap_field_ref, hp_field_ref, alloc_method_ref,
                system_out_ref, system_in_ref, ps_print_char_ref, input_read_ref,
                await_method_ref, task_complete_method_ref);
  patch_markers(&await_code, heap_field_ref, hp_field_ref, alloc_method_ref,
                system_out_ref, system_in_ref, ps_print_char_ref, input_read_ref,
                await_method_ref, task_complete_method_ref);
  patch_markers(&task_complete_code, heap_field_ref, hp_field_ref,
                alloc_method_ref, system_out_ref, system_in_ref,
                ps_print_char_ref, input_read_ref, await_method_ref,
                task_complete_method_ref);

  class_path = class_path_for_asm(asm_outfile);
  out = fopen(class_path, "wb");
  if (!out) {
    free(class_path);
    cp_free(&cp);
    free_methods(methods, method_count);
    bv_free(&wrapper_code);
    bv_free(&clinit_code);
    bv_free(&alloc_code);
    bv_free(&await_code);
    bv_free(&task_complete_code);
    return 1;
  }

  write_u4(out, 0xCAFEBABE);
  write_u2(out, 0);
  write_u2(out, 49); /* Java 5 */
  cp_write(out, &cp);
  write_u2(out, 0x0021); /* ACC_PUBLIC | ACC_SUPER */
  write_u2(out, this_class);
  write_u2(out, super_class);
  write_u2(out, 0); /* interfaces */
  /* fields: HEAP and HP */
  write_u2(out, 2);
  write_u2(out, 0x0008); /* ACC_STATIC */
  write_u2(out, cp_utf8(&cp, "HEAP"));
  write_u2(out, cp_utf8(&cp, "[I"));
  write_u2(out, 0);
  write_u2(out, 0x0008);
  write_u2(out, cp_utf8(&cp, "HP"));
  write_u2(out, cp_utf8(&cp, "I"));
  write_u2(out, 0);

  /* methods: user methods + wrapper + clinit + alloc + __await + __task_complete */
  write_u2(out, method_count + 5);

  for (i = 0; i < method_count; ++i) {
    write_u2(out, 0x0009); /* ACC_PUBLIC | ACC_STATIC */
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

  /* wrapper main([Ljava/lang/String;)V */
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

  /* static initializer <clinit>()V */
  write_u2(out, 0x0008); /* ACC_STATIC */
  write_u2(out, cp_utf8(&cp, "<clinit>"));
  write_u2(out, cp_utf8(&cp, "()V"));
  write_u2(out, 1);
  write_u2(out, code_attr);
  write_u4(out, (uint32_t)(12 + clinit_code.size));
  write_u2(out, 4); /* max_stack */
  write_u2(out, 0); /* max_locals */
  write_u4(out, (uint32_t)clinit_code.size);
  fwrite(clinit_code.data, 1, (size_t)clinit_code.size, out);
  write_u2(out, 0);
  write_u2(out, 0);

  /* alloc(I)I */
  write_u2(out, 0x0009);
  write_u2(out, cp_utf8(&cp, "alloc"));
  write_u2(out, cp_utf8(&cp, "(I)I"));
  write_u2(out, 1);
  write_u2(out, code_attr);
  write_u4(out, (uint32_t)(12 + alloc_code.size));
  write_u2(out, 4); /* max_stack: dup + iload_0 + iadd = 3 */
  write_u2(out, 1); /* max_locals (size param) */
  write_u4(out, (uint32_t)alloc_code.size);
  fwrite(alloc_code.data, 1, (size_t)alloc_code.size, out);
  write_u2(out, 0);
  write_u2(out, 0);

  /* __await(I)I — async-await runtime helper (Task 2 var.1) */
  write_u2(out, 0x0009);
  write_u2(out, cp_utf8(&cp, "__await"));
  write_u2(out, cp_utf8(&cp, "(I)I"));
  write_u2(out, 1);
  write_u2(out, code_attr);
  write_u4(out, (uint32_t)(12 + await_code.size));
  write_u2(out, 4); /* max_stack */
  write_u2(out, 1); /* max_locals */
  write_u4(out, (uint32_t)await_code.size);
  fwrite(await_code.data, 1, (size_t)await_code.size, out);
  write_u2(out, 0);
  write_u2(out, 0);

  /* __task_complete(II)I — async-await runtime helper (Task 2 var.1) */
  write_u2(out, 0x0009);
  write_u2(out, cp_utf8(&cp, "__task_complete"));
  write_u2(out, cp_utf8(&cp, "(II)I"));
  write_u2(out, 1);
  write_u2(out, code_attr);
  write_u4(out, (uint32_t)(12 + task_complete_code.size));
  write_u2(out, 5); /* max_stack: HEAP, task, +1, result = up to 4 + a slack */
  write_u2(out, 2); /* max_locals: task + result */
  write_u4(out, (uint32_t)task_complete_code.size);
  fwrite(task_complete_code.data, 1, (size_t)task_complete_code.size, out);
  write_u2(out, 0);
  write_u2(out, 0);

  write_u2(out, 0); /* no class attributes */
  fclose(out);

  fprintf(stderr, "JVM class written to %s\n", class_path);

  free(class_path);
  cp_free(&cp);
  free_methods(methods, method_count);
  bv_free(&wrapper_code);
  bv_free(&clinit_code);
  bv_free(&alloc_code);
  bv_free(&await_code);
  bv_free(&task_complete_code);
  return 0;
}
