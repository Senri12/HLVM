#include "cfg_builder.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Вспомогательные функции --- */

#define LOOP_STACK_INIT 16

static void trim_whitespace(char* s);


typedef struct {
  int offset;  // например -4, +8
  int is_arg;  // 1 если offset > 0 (обычно)
  int is_object;
  int is_array;
  int array_length;
  int storage_offset;
  int storage_size;
  char name[128];
  char type_name[128];
} SymEntry;

typedef struct {
  char func[128];
  int frame_size;  // N из sub sp, sp, #N
  int start_addr;
  int end_addr;
  SymEntry entries[512];
  int entry_count;

} FuncSyms;
static void add_named_entry_typed(FuncSyms* fs, const char* name,
                                  const char* type_name, int off,
                                  int is_object, int is_array,
                                  int array_length, int storage_offset,
                                  int storage_size) {
  int is_arg = off > 0;
  if (!fs) return;
  if (fs->entry_count >= 512) return;

  for (int i = 0; i < fs->entry_count; ++i) {
    if (fs->entries[i].offset == off && fs->entries[i].is_arg == is_arg) return;
  }

  SymEntry* e = &fs->entries[fs->entry_count++];
  e->offset = off;
  e->is_arg = is_arg;
  e->is_object = is_object;
  e->is_array = is_array;
  e->array_length = array_length;
  e->storage_offset = storage_offset;
  e->storage_size = storage_size;
  e->name[0] = '\0';
  e->type_name[0] = '\0';
  if (name && name[0]) {
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
  }
  if (type_name && type_name[0]) {
    strncpy(e->type_name, type_name, sizeof(e->type_name) - 1);
    e->type_name[sizeof(e->type_name) - 1] = '\0';
  }
}

static void add_named_entry(FuncSyms* fs, const char* name, int off) {
  add_named_entry_typed(fs, name, "int", off, 0, 0, 0, 0, 0);
}

static int is_simple_ident(const char* s) {
  if (!s || !*s) return 0;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (const char* p = s + 1; *p; ++p) {
    if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
  }
  return 1;
}


static int has_entry(FuncSyms* fs, int off, int is_arg) {
  for (int i = 0; i < fs->entry_count; ++i)
    if (fs->entries[i].offset == off && fs->entries[i].is_arg == is_arg)
      return 1;
  return 0;
}

static void add_entry(FuncSyms* fs, int off) {
  int is_arg = off > 0;
  if (fs->entry_count >= 512) return;
  if (has_entry(fs, off, is_arg)) return;
  fs->entries[fs->entry_count++] = (SymEntry){.offset = off, .is_arg = is_arg};
}
static int sym_cmp(const void* a, const void* b) {
  const SymEntry* x = (const SymEntry*)a;
  const SymEntry* y = (const SymEntry*)b;
  return (x->offset - y->offset);
}

static void compact_type_token(const char* src, char* dst, size_t dst_size) {
  size_t written = 0;
  if (!dst || dst_size == 0) return;
  dst[0] = '\0';
  if (!src) return;
  while (*src && written + 1 < dst_size) {
    if (*src != ' ' && *src != '\t' && *src != '\r' && *src != '\n') {
      dst[written++] = *src;
    }
    src++;
  }
  dst[written] = '\0';
}

static int parse_fp_mem(const char* tok, int* out_off) {
  // ожидаем "[fp-92]" или "[fp+8]" или "[fp]"
  if (!tok || tok[0] != '[') return 0;
  const char* p = tok;
  if (strncmp(p, "[fp", 3) != 0) return 0;

  p += 3;
  int sign = 1;
  if (*p == '+') {
    sign = 1;
    p++;
  } else if (*p == '-') {
    sign = -1;
    p++;
  }

  if (*p == ']') {
    *out_off = 0;
    return 1;
  }

  char* end = NULL;
  long v = strtol(p, &end, 10);
  if (!end || *end != ']') return 0;
  *out_off = (int)(sign * v);
  return 1;
}

static int parse_int_literal_strict(const char* text, int* out_val) {
  char buf[128];
  char tail[2];
  int value;

  if (!text || !out_val) return 0;
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  trim_whitespace(buf);
  if (buf[0] == '\0') return 0;

  if (sscanf(buf, "%d%1s", &value, tail) != 1) return 0;
  *out_val = value;
  return 1;
}

static int parse_char_literal_value(const char* text, int* out_val) {
  const unsigned char* p = NULL;
  int value = 0;
  int digits = 0;

  if (!text || !out_val) return 0;
  if (text[0] != '\'') return 0;

  p = (const unsigned char*)text + 1;
  if (*p == '\0') return 0;

  if (*p == '\\') {
    ++p;
    switch (*p) {
      case 'b':
        value = '\b';
        ++p;
        break;
      case 't':
        value = '\t';
        ++p;
        break;
      case 'n':
        value = '\n';
        ++p;
        break;
      case 'f':
        value = '\f';
        ++p;
        break;
      case 'r':
        value = '\r';
        ++p;
        break;
      case '\\':
        value = '\\';
        ++p;
        break;
      case '\'':
        value = '\'';
        ++p;
        break;
      case '"':
        value = '"';
        ++p;
        break;
      default:
        if (*p >= '0' && *p <= '7') {
          value = 0;
          while (digits < 3 && *p >= '0' && *p <= '7') {
            value = value * 8 + (*p - '0');
            ++p;
            ++digits;
          }
        } else {
          return 0;
        }
        break;
    }
  } else {
    value = *p++;
  }

  if (*p != '\'' || p[1] != '\0') return 0;
  *out_val = value & 0xFF;
  return 1;
}



typedef struct {
  CFGNode* cond;
  CFGNode* brk;  // куда прыгает break (merge после цикла)
} LoopCtx;

typedef struct {
  LoopCtx* loop_stack;
  size_t loop_capacity;
  int loop_top;
  NodeId next_node_id;
  int tmp_mod_counter;
  int cond_label_counter;
} CfgBuilderState;


static void init_builder_state(CfgBuilderState* st) {
  if (!st) return;
  st->loop_stack = NULL;
  st->loop_capacity = 0;
  st->loop_top = -1;
  st->next_node_id = 1;
  st->tmp_mod_counter = 0;
  st->cond_label_counter = 0;
}

static void free_builder_state(CfgBuilderState* st) {
  if (!st) return;
  free(st->loop_stack);
  st->loop_stack = NULL;
  st->loop_capacity = 0;
  st->loop_top = -1;
}

static void reset_function_state(CfgBuilderState* st) {
  if (!st) return;
  st->next_node_id = 1;
  st->loop_top = -1;
  st->tmp_mod_counter = 0;
  st->cond_label_counter = 0;
}

static int ensure_loop_stack_capacity(CfgBuilderState* st) {
  if (!st) return 0;
  if (st->loop_top + 1 < (int)st->loop_capacity) return 1;
  size_t new_cap =
      (st->loop_capacity == 0) ? LOOP_STACK_INIT : st->loop_capacity * 2;
  LoopCtx* ns = realloc(st->loop_stack, new_cap * sizeof(LoopCtx));
  if (!ns) return 0;
  st->loop_stack = ns;
  st->loop_capacity = new_cap;
  return 1;
}

// Добавляет ребро from -> to в граф CFG
static void add_edge(CFGNode* from, CFGNode* to) {
  if (!from || !to) return;
  from->outs = realloc(from->outs, sizeof(CFGNode*) * (from->outs_count + 1));
  from->outs[from->outs_count++] = to;
}

static CFGNode* create_node(CfgBuilderState* st, const char* label) {
  CFGNode* n = malloc(sizeof(CFGNode));
  n->id = st ? st->next_node_id++ : 0;
  n->label = strdup(label ? label : "");
  n->ops = NULL;
  n->ops_count = 0;
  n->true_target = NULL;
  n->false_target = NULL;
  n->next_target = NULL;
  n->outs = NULL;
  n->outs_count = 0;
  n->line = 0;
  n->col = 0;
  n->color = NULL;
  return n;
}

static FunctionCFG* create_function(const char* name, const char* srcfile) {
  FunctionCFG* f = malloc(sizeof(FunctionCFG));
  f->func_name = strdup(name);
  f->source_name = strdup(name);
  f->owner_type = NULL;
  f->return_type = NULL;
  f->signature = strdup(name);
  f->source_filename = strdup(srcfile);
  f->params = NULL;
  f->param_types = NULL;
  f->param_count = 0;
  f->is_method = 0;
  f->is_async = 0;
  f->nodes = NULL;
  f->node_count = 0;
  f->called_functions = NULL;
  f->called_count = 0;
  f->entry = NULL;
  f->exit = NULL;
  return f;
}

static void add_node(FunctionCFG* f, CFGNode* n) {
  f->nodes = realloc(f->nodes, sizeof(CFGNode*) * (f->node_count + 1));
  f->nodes[f->node_count++] = n;
}

static void add_param_typed(FunctionCFG* f, const char* name,
                            const char* type_name);

/* зменено: теперь каждый вызов добавляется как отдельная запись (не
   дедуплицируем). Это чтобы в секции CALLS появились все вызовы в порядке их
   появления. */
static void add_called_function(FunctionCFG* f, const char* name) {
  if (!name) return;
  f->called_functions =
      realloc(f->called_functions, sizeof(char*) * (f->called_count + 1));
  f->called_functions[f->called_count++] = strdup(name);
}

static void add_param(FunctionCFG* f, const char* name) {
  if (!f) return;
  add_param_typed(f, name, NULL);
}

static void add_param_typed(FunctionCFG* f, const char* name,
                            const char* type_name) {
  if (!f || !name || !*name) return;
  f->params = realloc(f->params, sizeof(char*) * (f->param_count + 1));
  f->param_types =
      realloc(f->param_types, sizeof(char*) * (f->param_count + 1));
  f->params[f->param_count++] = strdup(name);
  f->param_types[f->param_count - 1] =
      type_name ? strdup(type_name) : strdup("int");
}

static void append_operation(FunctionCFG* f, CFGNode* block, OpType type,
                             const char* text) {
  if (!block) return;
  Operation* op = malloc(sizeof(Operation));
  op->type = type;
  op->text = text ? strdup(text) : strdup("");
  op->args = NULL;
  op->argc = 0;
  op->line = 0;
  op->col = 0;
  block->ops = realloc(block->ops, sizeof(Operation*) * (block->ops_count + 1));
  block->ops[block->ops_count++] = op;
}

/* --- Структура для представления дерева разбора --- */
typedef struct TreeNode {
  char* label;
  struct TreeNode** children;
  int child_count;
  int capacity;
  int line;
  int col;
} TreeNode;

static TreeNode* create_tree_node_at(const char* label, int line, int col) {
  TreeNode* t = malloc(sizeof(TreeNode));
  t->label = strdup(label ? label : "");
  t->children = NULL;
  t->child_count = 0;
  t->capacity = 0;
  t->line = line > 0 ? line : 0;
  t->col = col > 0 ? col : 0;
  return t;
}

static TreeNode* create_tree_node(const char* label) {
  return create_tree_node_at(label, 0, 0);
}

static void add_child(TreeNode* parent, TreeNode* child) {
  if (parent->child_count >= parent->capacity) {
    parent->capacity = parent->capacity == 0 ? 4 : parent->capacity * 2;
    parent->children =
        realloc(parent->children, sizeof(TreeNode*) * parent->capacity);
  }
  parent->children[parent->child_count++] = child;
}

static void free_tree_node(TreeNode* t) {
  if (!t) return;
  for (int i = 0; i < t->child_count; i++) {
    free_tree_node(t->children[i]);
  }
  free(t->children);
  free(t->label);
  free(t);
}

static int antlr_tree_is_nil(pANTLR3_BASE_TREE tree) {
  pANTLR3_STRING text = NULL;
  if (!tree) return 0;
  if (tree->isNilNode && tree->isNilNode(tree)) return 1;
  if (!tree->toString) return 0;
  text = tree->toString(tree);
  if (!text || !text->chars) return 0;
  return strcmp((const char*)text->chars, "nil") == 0;
}

static int antlr_tree_get_line(pANTLR3_BASE_TREE tree) {
  if (!tree || !tree->getLine) return 0;
  return (int)tree->getLine(tree);
}

static int antlr_tree_get_column(pANTLR3_BASE_TREE tree) {
  int zero_based = 0;
  if (!tree || !tree->getCharPositionInLine) return 0;
  zero_based = (int)tree->getCharPositionInLine(tree);
  return zero_based >= 0 ? (zero_based + 1) : 0;
}

static void set_cfg_node_source(CFGNode* node, int line, int col) {
  if (!node) return;
  node->line = line > 0 ? line : 0;
  node->col = col > 0 ? col : 0;
}

static void set_cfg_node_source_from_tree(CFGNode* node, const TreeNode* tree) {
  if (!node || !tree) return;
  set_cfg_node_source(node, tree->line, tree->col);
}

static void set_operation_source(Operation* op, int line, int col) {
  if (!op) return;
  op->line = line > 0 ? line : 0;
  op->col = col > 0 ? col : 0;
}

static void set_last_operation_source_from_tree(CFGNode* block,
                                                const TreeNode* tree) {
  if (!block || block->ops_count <= 0 || !tree) return;
  set_operation_source(block->ops[block->ops_count - 1], tree->line, tree->col);
}

static TreeNode* convert_antlr_tree_node(pANTLR3_BASE_TREE tree) {
  TreeNode* node = NULL;
  pANTLR3_STRING text = NULL;
  ANTLR3_UINT32 child_count = 0;
  int line = 0;
  int col = 0;

  if (!tree) return NULL;
  line = antlr_tree_get_line(tree);
  col = antlr_tree_get_column(tree);
  if (antlr_tree_is_nil(tree)) {
    node = create_tree_node_at("", line, col);
  } else {
    text = tree->toString ? tree->toString(tree) : NULL;
    node = create_tree_node_at(
        (text && text->chars) ? (const char*)text->chars : "", line, col);
  }

  child_count = tree->getChildCount ? tree->getChildCount(tree) : 0;
  for (ANTLR3_UINT32 i = 0; i < child_count; ++i) {
    pANTLR3_BASE_TREE child =
        (pANTLR3_BASE_TREE)(tree->getChild ? tree->getChild(tree, i) : NULL);
    TreeNode* child_node = convert_antlr_tree_node(child);
    if (!child_node) continue;
    if (child_node->label[0] == '\0') {
      for (int j = 0; j < child_node->child_count; ++j) {
        add_child(node, child_node->children[j]);
        child_node->children[j] = NULL;
      }
      child_node->child_count = 0;
      free_tree_node(child_node);
    } else {
      add_child(node, child_node);
    }
  }

  if (node->line <= 0) {
    for (int i = 0; i < node->child_count; ++i) {
      TreeNode* child = node->children[i];
      if (!child || child->line <= 0) continue;
      node->line = child->line;
      node->col = child->col;
      break;
    }
  }

  return node;
}

/* --- Парсинг toStringTree в структуру дерева --- */
static TreeNode* parse_tree_from_string(const char* s) {
  if (!s || !*s) return NULL;

  const char* p = s;
  TreeNode* stack[1024];

  // Всегда создаём один общий корень
  TreeNode* root = create_tree_node("PROGRAM");
  int stack_top = 0;
  stack[0] = root;

  char label[1024];

  while (*p) {
    if (*p == '(') {
      p++;
      int idx = 0;
      while (*p && *p != '(' && *p != ')' && !isspace((unsigned char)*p) &&
             idx < 1023) {
        label[idx++] = *p++;
      }
      label[idx] = '\0';

      if (idx > 0) {
        TreeNode* node = create_tree_node(label);
        add_child(stack[stack_top], node);
        stack[++stack_top] = node;
      } else {
        // NIL-  toStringTree(): "((...)(...))"
        //   ,    .
        if (stack_top < 1023) {
          stack[stack_top + 1] = stack[stack_top];  // ""
          stack_top++;
        }
      }

    } else if (*p == ')') {
      if (stack_top > 0) stack_top--;  // 0 — это PROGRAM, ниже не опускаемся
      p++;
    } else if (!isspace((unsigned char)*p)) {
      int idx = 0;
      while (*p && *p != '(' && *p != ')' && !isspace((unsigned char)*p) &&
             idx < 1023) {
        label[idx++] = *p++;
      }
      label[idx] = '\0';

      if (idx > 0) {
        TreeNode* leaf = create_tree_node(label);
        add_child(stack[stack_top], leaf);
      }
    } else {
      p++;
    }
  }

  return root;
}

/* --- Преобразование поддерева в человекочитаемую строку (фильтруем AST-лейблы)
 * --- */
static int is_wrapper_label(const char* s) {
  if (!s || !s[0]) return 0;
  // Считаем "обёртками" метки, начинающиеся с заглавной буквы (EXPR_LIST,
  // VAR_DECL, CALL, и т.д.)
  return isupper((unsigned char)s[0]);
}

static void node_to_code(TreeNode* tree, char* buf, int bufsize);

static int is_postfix_label(const char* s) {
  return s && (strcmp(s, "CALL") == 0 || strcmp(s, "ARRAY_INDEX") == 0 ||
               strcmp(s, "MEMBER_ACCESS") == 0);
}

static int is_infix_operator_label(const char* s) {
  if (!s) return 0;
  return (strcmp(s, "+") == 0 || strcmp(s, "-") == 0 ||
          strcmp(s, "*") == 0 || strcmp(s, "/") == 0 ||
          strcmp(s, "%") == 0 || strcmp(s, "==") == 0 ||
          strcmp(s, "!=") == 0 || strcmp(s, "<") == 0 ||
          strcmp(s, "<=") == 0 || strcmp(s, ">") == 0 ||
          strcmp(s, ">=") == 0 || strcmp(s, "&&") == 0 ||
          strcmp(s, "||") == 0 || strcmp(s, "&") == 0 ||
          strcmp(s, "|") == 0 || strcmp(s, "^") == 0 ||
          strcmp(s, "<<") == 0 || strcmp(s, ">>") == 0);
}

static void append_postfix_node(TreeNode* ch, char* buf, int bufsize) {
  TreeNode* args_node = NULL;
  char args[768] = "";

  if (!ch || !buf || bufsize <= 0) return;

  if (strcmp(ch->label, "MEMBER_ACCESS") == 0) {
    node_to_code(ch, buf, bufsize);
    return;
  }

  for (int j = 0; j < ch->child_count; ++j) {
    if (strcmp(ch->children[j]->label, "EXPR_LIST") == 0) {
      args_node = ch->children[j];
      break;
    }
  }

  if (args_node) {
    node_to_code(args_node, args, sizeof(args));
  }

  if (strcmp(ch->label, "CALL") == 0) {
    strncat(buf, "(", bufsize - strlen(buf) - 1);
    strncat(buf, args, bufsize - strlen(buf) - 1);
    strncat(buf, ")", bufsize - strlen(buf) - 1);
  } else if (strcmp(ch->label, "ARRAY_INDEX") == 0) {
    strncat(buf, "[", bufsize - strlen(buf) - 1);
    strncat(buf, args, bufsize - strlen(buf) - 1);
    strncat(buf, "]", bufsize - strlen(buf) - 1);
  }
}

static void node_to_code(TreeNode* tree, char* buf, int bufsize) {
  if (!tree || bufsize <= 0) return;

  if (strcmp(tree->label, "TYPE_ARGS") == 0) {
    strncat(buf, "<", bufsize - strlen(buf) - 1);
    for (int i = 0; i < tree->child_count; ++i) {
      if (i > 0) strncat(buf, ", ", bufsize - strlen(buf) - 1);
      node_to_code(tree->children[i], buf, bufsize);
    }
    strncat(buf, ">", bufsize - strlen(buf) - 1);
    return;
  }

  if (strcmp(tree->label, "TYPE_REF") == 0) {
    for (int i = 0; i < tree->child_count; ++i) {
      TreeNode* ch = tree->children[i];
      if (strcmp(ch->label, "TYPE_ARGS") == 0 ||
          strcmp(ch->label, "ARRAY_TYPE") == 0) {
        node_to_code(ch, buf, bufsize);
      } else if (ch->child_count == 0) {
        strncat(buf, ch->label, bufsize - strlen(buf) - 1);
      } else {
        node_to_code(ch, buf, bufsize);
      }
    }
    return;
  }

  if (strcmp(tree->label, "MEMBER_ACCESS") == 0) {
    strncat(buf, ".", bufsize - strlen(buf) - 1);
    if (tree->child_count > 0) node_to_code(tree->children[0], buf, bufsize);
    return;
  }

  if (strcmp(tree->label, "ARRAY_DECL") == 0) {
    strncat(buf, "[", bufsize - strlen(buf) - 1);
    if (tree->child_count > 0) {
      node_to_code(tree->children[0], buf, bufsize);
      for (int i = 1; i < tree->child_count; ++i) {
        if (is_postfix_label(tree->children[i]->label))
          append_postfix_node(tree->children[i], buf, bufsize);
        else
          node_to_code(tree->children[i], buf, bufsize);
      }
    }
    strncat(buf, "]", bufsize - strlen(buf) - 1);
    return;
  }

  // ASSIGN как оператор присваивания (ANTLR может дать больше 2 детей)
  if (strcmp(tree->label, "=") == 0 || strcmp(tree->label, "ASSIGN") == 0) {
    char lhs[256] = "";
    char rhs[768] = "";
    int rhs_start = 1;
    if (tree->child_count > 0)
      node_to_code(tree->children[0], lhs, sizeof(lhs));

    while (rhs_start < tree->child_count) {
      const char* lab = tree->children[rhs_start]->label;
      if (strcmp(lab, "ARRAY_INDEX") == 0 || strcmp(lab, "MEMBER_ACCESS") == 0) {
        append_postfix_node(tree->children[rhs_start], lhs, sizeof(lhs));
        rhs_start++;
      } else {
        break;
      }
    }

    if (tree->child_count > 1) {
      // Шаблон: lhs, callee, CALL(args)
      if (tree->child_count >= rhs_start + 2 &&
          tree->children[rhs_start]->child_count == 0 &&
          strcmp(tree->children[rhs_start + 1]->label, "CALL") == 0 &&
          rhs_start + 2 == tree->child_count) {
        char callee[256] = "";
        node_to_code(tree->children[rhs_start], callee, sizeof(callee));
        char args[512] = "";
        TreeNode* args_node = NULL;
        for (int i = 0; i < tree->children[rhs_start + 1]->child_count; i++) {
          if (strcmp(tree->children[rhs_start + 1]->children[i]->label,
                     "EXPR_LIST") == 0) {
            args_node = tree->children[rhs_start + 1]->children[i];
            break;
          }
        }
        if (args_node) {
          node_to_code(args_node, args, sizeof(args));
        }
        snprintf(rhs, sizeof(rhs), "%s(%s)", callee, args);
      } else {
        TreeNode temp = {.label = "",
                         .children = tree->children + rhs_start,
                         .child_count = tree->child_count - rhs_start,
                         .capacity = 0};
        node_to_code(&temp, rhs, sizeof(rhs));
      }
    }
    snprintf(buf + strlen(buf), bufsize - strlen(buf), "%s = %s", lhs, rhs);
    return;
  }

  /* AWAIT must be handled before the generic postfix-chain logic below,
     otherwise an `await foo(args)` (children: foo + CALL) is mistakenly
     rewritten as plain `foo(args)`, losing the await semantics. */
  if (strcmp(tree->label, "AWAIT") == 0) {
    strncat(buf, "__await(", bufsize - strlen(buf) - 1);
    if (tree->child_count > 0) {
      node_to_code(tree->children[0], buf, bufsize);
      for (int i = 1; i < tree->child_count; ++i) {
        if (is_postfix_label(tree->children[i]->label))
          append_postfix_node(tree->children[i], buf, bufsize);
        else
          node_to_code(tree->children[i], buf, bufsize);
      }
    }
    strncat(buf, ")", bufsize - strlen(buf) - 1);
    return;
  }

  if (tree->child_count > 1) {
    int is_postfix_chain = 1;
    for (int i = 1; i < tree->child_count; ++i) {
      const char* label = tree->children[i]->label;
      if (is_postfix_label(label)) {
        continue;
      }
      is_postfix_chain = 0;
      break;
    }
    if (is_postfix_chain) {
      node_to_code(tree->children[0], buf, bufsize);
      for (int i = 1; i < tree->child_count; ++i) {
        append_postfix_node(tree->children[i], buf, bufsize);
      }
      return;
    }
  }

  // Специальная обработка вызовов, чтобы печатать имя и аргументы в скобках
  if (strcmp(tree->label, "CALL") == 0) {
    char callee[256] = "";
    char args[768] = "";
    TreeNode* args_node = NULL;

    for (int i = 0; i < tree->child_count; i++) {
      TreeNode* ch = tree->children[i];
      if (strcmp(ch->label, "EXPR_LIST") == 0) {
        args_node = ch;
      } else if (callee[0] == '\0') {
        node_to_code(ch, callee, sizeof(callee));
      }
    }
    if (!args_node && tree->child_count > 0 && callee[0] == '\0') {
      node_to_code(tree->children[0], callee, sizeof(callee));
    }
    if (args_node) {
      node_to_code(args_node, args, sizeof(args));
    }
    if (callee[0] == '\0') strncpy(callee, "call", sizeof(callee) - 1);
    strncat(buf, callee, bufsize - strlen(buf) - 1);
    strncat(buf, "(", bufsize - strlen(buf) - 1);
    strncat(buf, args, bufsize - strlen(buf) - 1);
    strncat(buf, ")", bufsize - strlen(buf) - 1);
    return;
  }

  // EXPR_LIST: просто список через запятую
  if (strcmp(tree->label, "EXPR_LIST") == 0) {
    int item_idx = 0;
    for (int i = 0; i < tree->child_count;) {
      char item[768] = "";
      int j = i;

      node_to_code(tree->children[j], item, sizeof(item));
      j++;
      while (j < tree->child_count &&
             is_postfix_label(tree->children[j]->label)) {
        append_postfix_node(tree->children[j], item, sizeof(item));
        j++;
      }

      if (item_idx > 0) strncat(buf, ", ", bufsize - strlen(buf) - 1);
      strncat(buf, item, bufsize - strlen(buf) - 1);

      item_idx++;
      i = j;
    }
    return;
  }
  // UNARY: ^(UNARY op expr) ->   op+expr ( -1, !x, ++i)
  if (strcmp(tree->label, "UNARY") == 0) {
    if (tree->child_count >= 2) {
      node_to_code(tree->children[0], buf, bufsize);  // op
      node_to_code(tree->children[1], buf, bufsize);  // expr
    } else if (tree->child_count == 1) {
      node_to_code(tree->children[0], buf, bufsize);
    }
    return;
  }
  if (tree->child_count == 0) {
    strncat(buf, tree->label, bufsize - strlen(buf) - 1);
    return;
  }

  if (is_wrapper_label(tree->label)) {
    // просто рекурсивно обходим детей, разделяя аргументы запятой, если их
    // несколько
    for (int i = 0; i < tree->child_count; i++) {
      if (i > 0) {
        // разделитель: если следующий ребёнок - лист или не-operator, ставим ",
        // "
        strncat(buf, (tree->child_count > 1) ? ", " : " ",
                bufsize - strlen(buf) - 1);
      }
      node_to_code(tree->children[i], buf, bufsize);
    }
  } else {
    // Метка — возможно оператор (бинарный/унарный) или имя функции в
    // CALL-поддереве
    if (tree->child_count == 2) {
      node_to_code(tree->children[0], buf, bufsize);
      strncat(buf, " ", bufsize - strlen(buf) - 1);
      strncat(buf, tree->label, bufsize - strlen(buf) - 1);
      strncat(buf, " ", bufsize - strlen(buf) - 1);
      node_to_code(tree->children[1], buf, bufsize);
    } else if (tree->child_count == 1) {
      strncat(buf, tree->label, bufsize - strlen(buf) - 1);
      node_to_code(tree->children[0], buf, bufsize);
    } else {
      // несколько детей: печатаем метку (например, имя функции) и аргументы в
      // скобках, если это CALL-like попробуем распознать CALL: если есть дети,
      // и большинство детей — не wrapper, то считать этим синтаксисом вызова
      if (is_infix_operator_label(tree->label)) {
        int wrote_operand = 0;
        for (int i = 0; i < tree->child_count; ++i) {
          TreeNode* ch = tree->children[i];
          if (is_postfix_label(ch->label) && wrote_operand) {
            append_postfix_node(ch, buf, bufsize);
            continue;
          }

          if (wrote_operand) {
            strncat(buf, " ", bufsize - strlen(buf) - 1);
            strncat(buf, tree->label, bufsize - strlen(buf) - 1);
            strncat(buf, " ", bufsize - strlen(buf) - 1);
          }
          node_to_code(ch, buf, bufsize);
          wrote_operand = 1;
        }
        return;
      }

      int nonwrapper = 0;
      for (int i = 0; i < tree->child_count; i++) {
        if (!is_wrapper_label(tree->children[i]->label)) nonwrapper++;
      }
      if (nonwrapper > 0) {
        // печатаем первый как имя, остальные как аргументы
        node_to_code(tree->children[0], buf, bufsize);
        strncat(buf, "(", bufsize - strlen(buf) - 1);
        for (int i = 1; i < tree->child_count; i++) {
          if (i > 1) strncat(buf, ", ", bufsize - strlen(buf) - 1);
          node_to_code(tree->children[i], buf, bufsize);
        }
        strncat(buf, ")", bufsize - strlen(buf) - 1);
      } else {
        // иначе просто обходим детей через пробел
        for (int i = 0; i < tree->child_count; i++) {
          if (i > 0) strncat(buf, " ", bufsize - strlen(buf) - 1);
          node_to_code(tree->children[i], buf, bufsize);
        }
      }
    }
  }
}

static void node_range_to_code(TreeNode* parent, int start_idx, char* buf,
                               int bufsize) {
  if (!parent || start_idx < 0 || start_idx >= parent->child_count) return;
  TreeNode temp = {.label = "",
                   .children = parent->children + start_idx,
                   .child_count = parent->child_count - start_idx,
                   .capacity = 0};
  node_to_code(&temp, buf, bufsize);
}

static char* dup_canonical(const char* s) {
  if (!s) return strdup("");
  char buf[512];
  int out = 0;
  int in_space = 0;
  for (const char* p = s; *p && out < (int)sizeof(buf) - 1; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (isspace(ch)) {
      in_space = 1;
      continue;
    }
    if (in_space && out > 0 && buf[out - 1] != '<' && buf[out - 1] != ',' &&
        ch != '>' && ch != ',' && ch != '[' && ch != ']') {
      buf[out++] = ' ';
    }
    in_space = 0;
    buf[out++] = (char)ch;
  }
  buf[out] = '\0';
  return strdup(buf);
}

static char* tree_to_code_dup(TreeNode* tree) {
  char buf[512] = "";
  node_to_code(tree, buf, sizeof(buf));
  return dup_canonical(buf);
}

static int is_builtin_type_name(const char* type_name) {
  if (!type_name || !*type_name) return 0;
  return strcmp(type_name, "bool") == 0 || strcmp(type_name, "byte") == 0 ||
         strcmp(type_name, "int") == 0 || strcmp(type_name, "uint") == 0 ||
         strcmp(type_name, "long") == 0 || strcmp(type_name, "ulong") == 0 ||
         strcmp(type_name, "char") == 0 || strcmp(type_name, "string") == 0;
}

/* Returns element byte size for a given type name. "byte" → 1, everything else → 4. */
static int elem_size_of_type(const char* type_name) {
  if (type_name && strcmp(type_name, "byte") == 0) return 1;
  return 4;
}

static void base_type_name(const char* type_name, char* out, size_t out_sz) {
  if (!out || out_sz == 0) return;
  out[0] = '\0';
  if (!type_name) return;
  size_t n = 0;
  while (type_name[n] && type_name[n] != '<' && type_name[n] != '[' &&
         n + 1 < out_sz) {
    out[n] = type_name[n];
    ++n;
  }
  out[n] = '\0';
}

static UserTypeInfo* find_exact_user_type(AnalysisResult* res,
                                          const char* type_name) {
  if (!res || !type_name) return NULL;
  for (int i = 0; i < res->type_count; ++i) {
    if (res->types[i].name && strcmp(res->types[i].name, type_name) == 0)
      return &res->types[i];
  }
  return NULL;
}

static UserTypeInfo* find_user_type(AnalysisResult* res, const char* type_name) {
  if (!res || !type_name) return NULL;
  UserTypeInfo* exact = find_exact_user_type(res, type_name);
  if (exact) return exact;
  char base[256];
  base_type_name(type_name, base, sizeof(base));
  for (int i = 0; i < res->type_count; ++i) {
    if (res->types[i].name && strcmp(res->types[i].name, base) == 0)
      return &res->types[i];
  }
  return NULL;
}

static int is_type_specialization_name(const char* type_name,
                                       const char* base_name) {
  size_t base_len = 0;
  if (!type_name || !base_name) return 0;
  base_len = strlen(base_name);
  return strncmp(type_name, base_name, base_len) == 0 &&
         type_name[base_len] == '<';
}

static void trim_array_suffix(char* type_name) {
  char* lb = NULL;
  if (!type_name) return;
  lb = strchr(type_name, '[');
  if (lb) *lb = '\0';
  trim_whitespace(type_name);
}

static int parse_type_arguments(const char* type_name, char out[][256],
                                int max_args) {
  const char* lt = NULL;
  const char* p = NULL;
  int depth = 0;
  int count = 0;
  char token[512];
  int token_len = 0;

  if (!type_name || !out || max_args <= 0) return 0;
  lt = strchr(type_name, '<');
  if (!lt) return 0;

  for (p = lt + 1; *p; ++p) {
    if (*p == '>' && depth == 0) {
      if (token_len > 0 && count < max_args) {
        char* canon = NULL;
        token[token_len] = '\0';
        canon = dup_canonical(token);
        strncpy(out[count], canon, 255);
        out[count][255] = '\0';
        free(canon);
        ++count;
      }
      break;
    }
    if (*p == ',' && depth == 0) {
      if (token_len > 0 && count < max_args) {
        char* canon = NULL;
        token[token_len] = '\0';
        canon = dup_canonical(token);
        strncpy(out[count], canon, 255);
        out[count][255] = '\0';
        free(canon);
        ++count;
      }
      token_len = 0;
      continue;
    }
    if (*p == '<') depth++;
    if (*p == '>' && depth > 0) depth--;
    if (token_len + 1 < (int)sizeof(token)) token[token_len++] = *p;
  }

  return count;
}

static char* substitute_template_type(const char* type_name,
                                      UserTypeInfo* template_type,
                                      char actual_args[][256],
                                      int actual_arg_count) {
  char buf[1024];
  int out = 0;
  const char* p = NULL;

  if (!type_name) return strdup("");
  if (!template_type || template_type->template_param_count == 0 ||
      actual_arg_count <= 0) {
    return strdup(type_name);
  }

  for (p = type_name; *p && out < (int)sizeof(buf) - 1;) {
    if (isalpha((unsigned char)*p) || *p == '_') {
      char ident[256];
      int ident_len = 0;
      int replaced = 0;
      while ((isalnum((unsigned char)*p) || *p == '_') &&
             ident_len + 1 < (int)sizeof(ident)) {
        ident[ident_len++] = *p++;
      }
      ident[ident_len] = '\0';
      for (int i = 0; i < template_type->template_param_count &&
                      i < actual_arg_count;
           ++i) {
        if (strcmp(template_type->template_params[i], ident) == 0) {
          const char* actual = actual_args[i];
          size_t n = strlen(actual);
          if (out + (int)n >= (int)sizeof(buf) - 1)
            n = (size_t)((int)sizeof(buf) - out - 1);
          memcpy(buf + out, actual, n);
          out += (int)n;
          replaced = 1;
          break;
        }
      }
      if (!replaced) {
        size_t n = strlen(ident);
        if (out + (int)n >= (int)sizeof(buf) - 1)
          n = (size_t)((int)sizeof(buf) - out - 1);
        memcpy(buf + out, ident, n);
        out += (int)n;
      }
      continue;
    }
    buf[out++] = *p++;
  }
  buf[out] = '\0';
  return dup_canonical(buf);
}

static UserTypeInfo* append_user_type(AnalysisResult* res, const char* name);
static void add_field_info(UserTypeInfo* type, const char* name,
                           const char* type_name, int visibility);
static void add_method_info(UserTypeInfo* type, const char* source_name,
                            const char* return_type, char** param_names,
                            char** param_types, int param_count,
                            int visibility);

static UserTypeInfo* instantiate_specialized_user_type(AnalysisResult* res,
                                                       UserTypeInfo* base_type,
                                                       const char* exact_name) {
  char actual_args[16][256];
  char base_type_name[256];
  int actual_arg_count = 0;
  int base_index = -1;
  UserTypeInfo* type = NULL;

  if (!res || !base_type || !exact_name) return NULL;
  if (find_exact_user_type(res, exact_name))
    return find_exact_user_type(res, exact_name);
  if (base_type->template_param_count <= 0) return NULL;
  strncpy(base_type_name, base_type->name ? base_type->name : "", sizeof(base_type_name) - 1);
  base_type_name[sizeof(base_type_name) - 1] = '\0';
  base_index = (int)(base_type - res->types);

  actual_arg_count =
      parse_type_arguments(exact_name, actual_args, (int)(sizeof(actual_args) /
                                                          sizeof(actual_args[0])));
  if (actual_arg_count != base_type->template_param_count) return NULL;

  type = append_user_type(res, exact_name);
  if (base_index >= 0 && base_index < res->type_count) {
    base_type = &res->types[base_index];
  } else {
    base_type = find_exact_user_type(res, base_type_name);
  }
  if (!base_type) return type;
  if (base_type->base_name && base_type->base_name[0]) {
    char* specialized_base = substitute_template_type(
        base_type->base_name, base_type, actual_args, actual_arg_count);
    type->base_name = specialized_base;
  }

  for (int i = 0; i < base_type->field_count; ++i) {
    char* field_type = substitute_template_type(base_type->fields[i].type_name,
                                                base_type, actual_args,
                                                actual_arg_count);
    add_field_info(type, base_type->fields[i].name, field_type,
                   base_type->fields[i].visibility);
    free(field_type);
  }

  for (int i = 0; i < base_type->method_count; ++i) {
    MethodInfo* src = &base_type->methods[i];
    char* return_type = substitute_template_type(src->return_type, base_type,
                                                 actual_args, actual_arg_count);
    char** param_names = NULL;
    char** param_types = NULL;

    if (src->param_count > 0) {
      param_names = malloc(sizeof(char*) * (size_t)src->param_count);
      param_types = malloc(sizeof(char*) * (size_t)src->param_count);
    }
    for (int j = 0; j < src->param_count; ++j) {
      param_names[j] = strdup(src->param_names[j]);
      param_types[j] = substitute_template_type(src->param_types[j], base_type,
                                                actual_args, actual_arg_count);
    }
    add_method_info(type, src->source_name, return_type, param_names,
                    param_types, src->param_count, src->visibility);
    free(return_type);
    for (int j = 0; j < src->param_count; ++j) {
      free(param_names[j]);
      free(param_types[j]);
    }
    free(param_names);
    free(param_types);
  }

  return type;
}

static void collect_type_specializations(TreeNode* tree, AnalysisResult* res) {
  if (!tree || !res) return;

  if (strcmp(tree->label, "TYPE_REF") == 0) {
    int has_type_args = 0;
    char full_type[512] = "";
    char base_name[256] = "";
    for (int i = 0; i < tree->child_count; ++i) {
      if (strcmp(tree->children[i]->label, "TYPE_ARGS") == 0) {
        has_type_args = 1;
        break;
      }
    }
    if (has_type_args) {
      char* canon = tree_to_code_dup(tree);
      strncpy(full_type, canon, sizeof(full_type) - 1);
      full_type[sizeof(full_type) - 1] = '\0';
      free(canon);
      trim_array_suffix(full_type);
      base_type_name(full_type, base_name, sizeof(base_name));
      {
        UserTypeInfo* base_type = find_exact_user_type(res, base_name);
        if (base_type && base_type->template_param_count > 0) {
          instantiate_specialized_user_type(res, base_type, full_type);
        }
      }
    }
  }

  for (int i = 0; i < tree->child_count; ++i) {
    collect_type_specializations(tree->children[i], res);
  }
}

static FieldInfo* find_field_in_type(AnalysisResult* res, const char* type_name,
                                     const char* field_name) {
  UserTypeInfo* type = find_user_type(res, type_name);
  while (type) {
    for (int i = 0; i < type->field_count; ++i) {
      if (strcmp(type->fields[i].name, field_name) == 0) return &type->fields[i];
    }
    type = (type->base_name && type->base_name[0])
               ? find_user_type(res, type->base_name)
               : NULL;
  }
  return NULL;
}

static char* sanitize_type_for_symbol(const char* type_name) {
  if (!type_name) return strdup("void");
  char buf[256];
  int out = 0;
  for (const char* p = type_name; *p && out < (int)sizeof(buf) - 1; ++p) {
    unsigned char ch = (unsigned char)*p;
    buf[out++] = (isalnum(ch) || ch == '_') ? (char)ch : '_';
  }
  buf[out] = '\0';
  return strdup(buf);
}

static char* mangle_method_name(const char* owner_type, const char* source_name,
                                char** param_types, int param_count) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s$%s$%d", owner_type ? owner_type : "global",
           source_name ? source_name : "method", param_count);
  for (int i = 0; i < param_count; ++i) {
    char* safe = sanitize_type_for_symbol(param_types[i]);
    strncat(buf, "$", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, safe, sizeof(buf) - strlen(buf) - 1);
    free(safe);
  }
  return strdup(buf);
}

static FunctionCFG* find_global_function(AnalysisResult* res,
                                         const char* source_name,
                                         char** arg_types, int arg_count) {
  FunctionCFG* first_arity_match = NULL;
  if (!res || !source_name) return NULL;
  for (int i = 0; i < res->files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (int j = 0; j < sf->functions_count; ++j) {
      FunctionCFG* f = sf->functions[j];
      if (!f || f->owner_type) continue;
      if (!f->source_name || strcmp(f->source_name, source_name) != 0) continue;
      if (f->param_count != arg_count) continue;
      if (!first_arity_match) first_arity_match = f;
      {
        int exact = 1;
        for (int k = 0; k < arg_count; ++k) {
          if (!arg_types || !arg_types[k] || !arg_types[k][0]) continue;
          if (strcmp(f->param_types[k], arg_types[k]) != 0) {
            exact = 0;
            break;
          }
        }
        if (exact) return f;
      }
    }
  }
  return first_arity_match;
}

static const char* resolve_global_call_label(AnalysisResult* res,
                                             const char* source_name,
                                             char** arg_types,
                                             int arg_count) {
  FunctionCFG* fn =
      find_global_function(res, source_name, arg_types, arg_count);
  return fn ? fn->func_name : source_name;
}

static MethodInfo* find_method_in_type(AnalysisResult* res, const char* type_name,
                                       const char* method_name,
                                       char** arg_types, int arg_count) {
  UserTypeInfo* type = find_user_type(res, type_name);
  MethodInfo* first_arity_match = NULL;
  while (type) {
    for (int i = 0; i < type->method_count; ++i) {
      MethodInfo* m = &type->methods[i];
      if (strcmp(m->source_name, method_name) != 0) continue;
      if (m->param_count != arg_count) continue;
      if (!first_arity_match) first_arity_match = m;
      int exact = 1;
      for (int j = 0; j < arg_count; ++j) {
        if (!arg_types || !arg_types[j] || !arg_types[j][0]) continue;
        if (strcmp(m->param_types[j], arg_types[j]) != 0) {
          exact = 0;
          break;
        }
      }
      if (exact) return m;
    }
    type = (type->base_name && type->base_name[0])
               ? find_user_type(res, type->base_name)
               : NULL;
  }
  return first_arity_match;
}

static void add_analysis_error(AnalysisResult* res, const char* fmt, ...) {
  if (!res || !fmt) return;
  va_list ap;
  va_start(ap, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  res->errors =
      realloc(res->errors, sizeof(char*) * (size_t)(res->errors_count + 1));
  res->errors[res->errors_count++] = strdup(buf);
}

static UserTypeInfo* append_user_type(AnalysisResult* res, const char* name) {
  if (!res || !name) return NULL;
  res->types =
      realloc(res->types, sizeof(UserTypeInfo) * (size_t)(res->type_count + 1));
  UserTypeInfo* type = &res->types[res->type_count++];
  memset(type, 0, sizeof(*type));
  type->name = strdup(name);
  return type;
}

static void add_template_param(UserTypeInfo* type, const char* name) {
  if (!type || !name) return;
  type->template_params =
      realloc(type->template_params,
              sizeof(char*) * (size_t)(type->template_param_count + 1));
  type->template_params[type->template_param_count++] = strdup(name);
}

static void add_field_info(UserTypeInfo* type, const char* name,
                           const char* type_name, int visibility) {
  if (!type || !name || !type_name) return;
  type->fields =
      realloc(type->fields, sizeof(FieldInfo) * (size_t)(type->field_count + 1));
  FieldInfo* field = &type->fields[type->field_count++];
  field->name = strdup(name);
  field->type_name = strdup(type_name);
  field->offset = 0;
  field->visibility = visibility;
}

static void add_method_info(UserTypeInfo* type, const char* source_name,
                            const char* return_type, char** param_names,
                            char** param_types, int param_count,
                            int visibility) {
  if (!type || !source_name) return;
  type->methods =
      realloc(type->methods,
              sizeof(MethodInfo) * (size_t)(type->method_count + 1));
  MethodInfo* method = &type->methods[type->method_count++];
  memset(method, 0, sizeof(*method));
  method->owner_type = strdup(type->name);
  method->source_name = strdup(source_name);
  method->return_type = strdup(return_type ? return_type : "int");
  method->param_count = param_count;
  method->visibility = visibility;
  method->param_names = NULL;
  method->param_types = NULL;
  for (int i = 0; i < param_count; ++i) {
    method->param_names =
        realloc(method->param_names, sizeof(char*) * (size_t)(i + 1));
    method->param_types =
        realloc(method->param_types, sizeof(char*) * (size_t)(i + 1));
    method->param_names[i] = strdup(param_names[i]);
    method->param_types[i] = strdup(param_types[i]);
  }
  method->mangled_name =
      mangle_method_name(type->name, source_name, method->param_types,
                         method->param_count);
}

static int signature_is_async(TreeNode* sig) {
  if (!sig) return 0;
  for (int i = 0; i < sig->child_count; ++i) {
    if (sig->children[i] && sig->children[i]->label &&
        strcmp(sig->children[i]->label, "ASYNC") == 0) {
      return 1;
    }
  }
  return 0;
}

static void parse_signature_info(TreeNode* sig, char** out_name,
                                 char** out_return_type, char*** out_param_names,
                                 char*** out_param_types, int* out_param_count) {
  char* name = strdup("unknown");
  char* return_type = strdup("int");
  char** param_names = NULL;
  char** param_types = NULL;
  int param_count = 0;
  int first_typeref_seen = 0;

  for (int i = 0; i < sig->child_count; ++i) {
    TreeNode* ch = sig->children[i];
    if (strcmp(ch->label, "ASYNC") == 0) {
      continue; /* handled separately by signature_is_async */
    }
    if (strcmp(ch->label, "TYPE_REF") == 0 && !first_typeref_seen) {
      free(return_type);
      return_type = tree_to_code_dup(ch);
      first_typeref_seen = 1;
      continue;
    }
    if (strcmp(ch->label, "ARG_LIST") == 0) {
      for (int a = 0; a < ch->child_count; ++a) {
        TreeNode* arg_def = ch->children[a];
        char* arg_name = strdup("arg");
        char* arg_type = strdup("int");
        for (int b = 0; b < arg_def->child_count; ++b) {
          TreeNode* leaf = arg_def->children[b];
          if (strcmp(leaf->label, "TYPE_REF") == 0) {
            free(arg_type);
            arg_type = tree_to_code_dup(leaf);
          } else if (leaf->child_count == 0) {
            free(arg_name);
            arg_name = strdup(leaf->label);
          }
        }
        param_names =
            realloc(param_names, sizeof(char*) * (size_t)(param_count + 1));
        param_types =
            realloc(param_types, sizeof(char*) * (size_t)(param_count + 1));
        param_names[param_count] = arg_name;
        param_types[param_count] = arg_type;
        ++param_count;
      }
      continue;
    }
    if (ch->child_count == 0) {
      free(name);
      name = strdup(ch->label);
    }
  }

  *out_name = name;
  *out_return_type = return_type;
  *out_param_names = param_names;
  *out_param_types = param_types;
  *out_param_count = param_count;
}

static void collect_class_defs(TreeNode* tree, AnalysisResult* res) {
  if (!tree || !res) return;
  if (strcmp(tree->label, "CLASS_DEF") == 0) {
    const char* class_name = NULL;
    TreeNode* inherit_node = NULL;
    TreeNode* template_node = NULL;
    for (int i = 0; i < tree->child_count; ++i) {
      TreeNode* ch = tree->children[i];
      if (!class_name && ch->child_count == 0) {
        class_name = ch->label;
        continue;
      }
      if (strcmp(ch->label, "INHERITANCE") == 0) inherit_node = ch;
      if (strcmp(ch->label, "TEMPLATE_PARAMS") == 0) template_node = ch;
    }
    if (class_name && !find_user_type(res, class_name)) {
      UserTypeInfo* type = append_user_type(res, class_name);
      if (template_node) {
        for (int i = 0; i < template_node->child_count; ++i) {
          if (template_node->children[i]->child_count == 0)
            add_template_param(type, template_node->children[i]->label);
        }
      }
      if (inherit_node && inherit_node->child_count > 0) {
        type->base_name = tree_to_code_dup(inherit_node->children[0]);
      }
      for (int i = 0; i < tree->child_count; ++i) {
        TreeNode* member = tree->children[i];
        if (strcmp(member->label, "MEMBER") != 0) continue;
        int visibility = 1;
        TreeNode* payload = NULL;
        for (int j = 0; j < member->child_count; ++j) {
          TreeNode* ch = member->children[j];
          if (strcmp(ch->label, "MODIFIER") == 0 && ch->child_count > 0) {
            visibility = strcmp(ch->children[0]->label, "private") != 0;
          } else {
            payload = ch;
          }
        }
        if (!payload) continue;
        if (strcmp(payload->label, "FIELD_DEF") == 0) {
          char* field_type = NULL;
          for (int j = 0; j < payload->child_count; ++j) {
            if (strcmp(payload->children[j]->label, "TYPE_REF") == 0) {
              field_type = tree_to_code_dup(payload->children[j]);
              break;
            }
          }
          if (!field_type) field_type = strdup("int");
          for (int j = 0; j < payload->child_count; ++j) {
            TreeNode* var_init = payload->children[j];
            if (strcmp(var_init->label, "VAR_INIT") != 0) continue;
            if (var_init->child_count == 0) continue;
            add_field_info(type, var_init->children[0]->label, field_type,
                           visibility);
          }
          free(field_type);
        } else if (strcmp(payload->label, "FUNC_DEF") == 0) {
          TreeNode* sig = NULL;
          for (int j = 0; j < payload->child_count; ++j) {
            if (strcmp(payload->children[j]->label, "FUNC_SIG") == 0) {
              sig = payload->children[j];
              break;
            }
          }
          if (!sig) continue;
          char* method_name = NULL;
          char* return_type = NULL;
          char** param_names = NULL;
          char** param_types = NULL;
          int param_count = 0;
          parse_signature_info(sig, &method_name, &return_type, &param_names,
                               &param_types, &param_count);
          add_method_info(type, method_name, return_type, param_names,
                          param_types, param_count, visibility);
          free(method_name);
          free(return_type);
          for (int j = 0; j < param_count; ++j) {
            free(param_names[j]);
            free(param_types[j]);
          }
          free(param_names);
          free(param_types);
        }
      }
    }
  }

  for (int i = 0; i < tree->child_count; ++i) {
    collect_class_defs(tree->children[i], res);
  }
}

static int compute_type_layout(UserTypeInfo* type, AnalysisResult* res,
                               int depth) {
  if (!type) return 0;
  if (type->instance_size > 0) return type->instance_size;
  if (depth > 32) return 0;
  int offset = 0;
  if (type->base_name && type->base_name[0]) {
    UserTypeInfo* base = find_user_type(res, type->base_name);
    if (base) offset = compute_type_layout(base, res, depth + 1);
  }
  for (int i = 0; i < type->field_count; ++i) {
    type->fields[i].offset = offset;
    offset += 4;
  }
  type->instance_size = offset;
  return offset;
}

static void finalize_type_layouts(AnalysisResult* res) {
  if (!res) return;
  for (int i = 0; i < res->type_count; ++i) {
    compute_type_layout(&res->types[i], res, 0);
  }
}

/* --- Поиск имени вызываемой функции (первый "маленький" лист в поддереве CALL)
 * --- */
static void find_callee_name_dfs(TreeNode* t, char* out, int outsize) {
  if (!t || !out || outsize <= 0) return;
  if (strcmp(t->label, "EXPR_LIST") == 0)
    return;  // не ищем имя среди аргументов
  if (t->child_count == 0) {
    if (isalpha((unsigned char)t->label[0]) &&
        islower((unsigned char)t->label[0])) {
      strncpy(out, t->label, outsize - 1);
      out[outsize - 1] = '\0';
    }
    return;
  }
  for (int i = 0; i < t->child_count && out[0] == '\0'; i++) {
    find_callee_name_dfs(t->children[i], out, outsize);
  }
}

static void collect_calls(FunctionCFG* f, TreeNode* tree) {
  if (!tree) return;

  // Специальный случай: вызов записан как соседние узлы: ID, затем CALL c
  // аргументами
  for (int idx = 0; idx + 1 < tree->child_count; idx++) {
    TreeNode* maybe_callee = tree->children[idx];
    TreeNode* maybe_call = tree->children[idx + 1];
    if (maybe_callee && maybe_call && maybe_callee->child_count == 0 &&
        strcmp(maybe_call->label, "CALL") == 0) {
      if (isalpha((unsigned char)maybe_callee->label[0]) &&
          islower((unsigned char)maybe_callee->label[0])) {
        add_called_function(f, maybe_callee->label);
      }
    }
  }

  if (strcmp(tree->label, "CALL") == 0) {
    char func_name[256] = "";
    find_callee_name_dfs(tree, func_name, sizeof(func_name));
    if (func_name[0] != '\0') {
      add_called_function(f, func_name);
    }
  }
  for (int i = 0; i < tree->child_count; i++)
    collect_calls(f, tree->children[i]);
}

static void push_loop(CfgBuilderState* st, CFGNode* cond, CFGNode* brk) {
  if (!ensure_loop_stack_capacity(st)) return;
  st->loop_stack[++st->loop_top] = (LoopCtx){.cond = cond, .brk = brk};
}
static LoopCtx top_loop(CfgBuilderState* st) {
  if (!st || st->loop_top < 0) return (LoopCtx){0};
  return st->loop_stack[st->loop_top];
}
// Backward compatibility (если где-то остались старые вызовы)
static inline CFGNode* top_loop_cond(CfgBuilderState* st) {
  return top_loop(st).cond;
}

static void pop_loop(CfgBuilderState* st) {
  if (st && st->loop_top >= 0) st->loop_top--;
}

/* --- Построение CFG из структуры дерева --- */
/* Внешняя вешалка, использующая внутреннюю рекурсивную функцию с current_block
 */
static CFGNode* build_cfg_from_tree(CfgBuilderState* st, FunctionCFG* f,
                                    TreeNode* tree, CFGNode** last_exit);
static CFGNode* build_cfg_from_tree_internal(CfgBuilderState* st,
                                             FunctionCFG* f, TreeNode* tree,
                                             CFGNode** last_exit,
                                             CFGNode** current_block);
static int func_def_has_block(TreeNode* func_def) {
  if (!func_def) return 0;
  for (int i = 0; i < func_def->child_count; i++) {
    if (strcmp(func_def->children[i]->label, "BLOCK") == 0) return 1;
  }
  return 0;
}

static CFGNode* build_cfg_from_tree(CfgBuilderState* st, FunctionCFG* f,
                                    TreeNode* tree, CFGNode** last_exit) {
  CFGNode* curr = NULL;
  return build_cfg_from_tree_internal(st, f, tree, last_exit, &curr);
}

static int block_terminates(CFGNode* n) {
  if (!n || n->ops_count <= 0) return 0;
  Operation* last = n->ops[n->ops_count - 1];
  if (!last) return 0;
  return (last->type == OP_RETURN || last->type == OP_BREAK ||
          last->type == OP_CONTINUE);
}


static CFGNode* build_cfg_from_tree_internal(CfgBuilderState* st,
                                             FunctionCFG* f, TreeNode* tree,
                                             CFGNode** last_exit,
                                             CFGNode** current_block) {
  if (!tree) return NULL;
  const char* label = tree->label;

  // PROGRAM: обрабатываем функции
  if (strcmp(label, "PROGRAM") == 0) {
    CFGNode* first = NULL;
    CFGNode* prev_exit = NULL;

    for (int i = 0; i < tree->child_count; i++) {
      CFGNode* entry = build_cfg_from_tree_internal(
          st, f, tree->children[i], &prev_exit, current_block);
      if (!first) first = entry;
      if (prev_exit && entry && prev_exit != entry) {
        int ends_with_return = 0;
        if (prev_exit->ops_count > 0) {
          Operation* last_op = prev_exit->ops[prev_exit->ops_count - 1];
          if (last_op && last_op->type == OP_RETURN) ends_with_return = 1;
        }
        if (!ends_with_return) {
          prev_exit->next_target = entry;
          add_edge(prev_exit, entry);
        }
      }
      prev_exit = prev_exit ? prev_exit : entry;
    }
    if (last_exit) *last_exit = prev_exit;
    return first;
  }

  // FUNC_DEF: функция (обрабатываем тело)
  if (strcmp(label, "FUNC_DEF") == 0) {
    for (int i = 0; i < tree->child_count; i++) {
      if (strcmp(tree->children[i]->label, "BLOCK") == 0) {
        return build_cfg_from_tree_internal(st, f, tree->children[i], last_exit,
                                            current_block);
      }
    }
    return NULL;
  }

  // BLOCK: создаём один базовый блок и собираем в него все линейные операции
  if (strcmp(label, "BLOCK") == 0) {
    CFGNode* block = create_node(st, "");
    set_cfg_node_source_from_tree(block, tree);
    add_node(f, block);
    if (*current_block == NULL) {
      // если это первая точка входа, запомним её как entry возвращаемое
      // значение
    }
    // Сохраняем старый current_block чтобы восстановить после вложенных
    // конструкций
    CFGNode* prev_current = *current_block;
    *current_block = block;

    CFGNode* first_entry = block;
    CFGNode* last_block_or_exit = block;

    for (int i = 0; i < tree->child_count; i++) {
      TreeNode* stmt = tree->children[i];

      // VAR_DECL
      if (strcmp(stmt->label, "VAR_DECL") == 0) {
        char type_str[256] = "";
        for (int j = 0; j < stmt->child_count; j++) {
          if (strcmp(stmt->children[j]->label, "TYPE_REF") == 0) {
            node_to_code(stmt->children[j], type_str, sizeof(type_str));
            break;
          }
        }
        for (int j = 0; j < stmt->child_count; j++) {
          if (strcmp(stmt->children[j]->label, "VAR_INIT") != 0) continue;
          TreeNode* var_init = stmt->children[j];
          // Внутри VAR_DECL, перед node_to_code(var_init->children[1], ...)

          char name[256] = "";
          char arr_suffix[128] = "";
          char value[512] = "";
          int value_start = 1;

          if (var_init->child_count > 0)
            node_to_code(var_init->children[0], name, sizeof(name));

          if (var_init->child_count > 1 &&
              strcmp(var_init->children[1]->label, "ARRAY_DECL") == 0) {
            node_to_code(var_init->children[1], arr_suffix, sizeof(arr_suffix));
            value_start = 2;
          }
          if (arr_suffix[0]) {
            strncat(name, arr_suffix, sizeof(name) - strlen(name) - 1);
          }
          if (var_init->child_count > value_start)
            node_range_to_code(var_init, value_start, value, sizeof(value));
          if (!value[0]) {
            char declbuf[1024];
            if (type_str[0]) {
              snprintf(declbuf, sizeof(declbuf), "decl %s %s;", type_str, name);
            } else {
              snprintf(declbuf, sizeof(declbuf), "decl int %s;", name);
            }
            append_operation(f, block, OP_ASSIGN, declbuf);
            set_last_operation_source_from_tree(block, var_init);
            continue;
          }
          char buf[1024];
          if (type_str[0]) {
            snprintf(buf, sizeof(buf), "decl %s %s = %s;", type_str, name,
                     value);
          } else {
            snprintf(buf, sizeof(buf), "decl int %s = %s;", name, value);
          }
          append_operation(f, block, OP_ASSIGN, buf);
          set_last_operation_source_from_tree(block, var_init);
        }
        collect_calls(f, stmt);
        last_block_or_exit = block;
        continue;
      }

      // EXPR_STMT
      if (strcmp(stmt->label, "EXPR_STMT") == 0) {
        char buf[1024] = "";
        node_range_to_code(stmt, 0, buf, sizeof(buf));
        if (strlen(buf) > 0 && buf[strlen(buf) - 1] != ';')
          strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
        // Если это вызов - пометим OP_CALL (попробуем определить по метке
        // дочернего узла)
        int iscall = 0;
        for (int c = 0; c < stmt->child_count; c++) {
          if (strcmp(stmt->children[c]->label, "CALL") == 0) {
            iscall = 1;
            break;
          }
        }
        append_operation(f, block, iscall ? OP_CALL : OP_EXPR, buf);
        set_last_operation_source_from_tree(block, stmt);
        // Собираем вызовы внутри выражения
        collect_calls(f, stmt);
        last_block_or_exit = block;
        continue;
      }

      // CALL как отдельный statement
      if (strcmp(stmt->label, "CALL") == 0) {
        char callbuf[1024] = "";
        node_to_code(stmt, callbuf, sizeof(callbuf));
        if (strlen(callbuf) > 0 && callbuf[strlen(callbuf) - 1] != ';')
          strncat(callbuf, ";", sizeof(callbuf) - strlen(callbuf) - 1);
        append_operation(f, block, OP_CALL, callbuf);
        set_last_operation_source_from_tree(block, stmt);
        // имя функции
        char func_name[256] = "";
        find_callee_name_dfs(stmt, func_name, sizeof(func_name));
        if (func_name[0] == '\0')
          strncpy(func_name, "unknown_call", sizeof(func_name) - 1);
        add_called_function(f, func_name);
        last_block_or_exit = block;
        continue;
      }

      // RETURN
      if (strcmp(stmt->label, "RETURN") == 0) {
        char buf[1024] = "return";
        if (stmt->child_count > 0) {
          strcat(buf, " ");
          node_to_code(stmt->children[0], buf + strlen(buf),
                       sizeof(buf) - strlen(buf));
          for (int _pi = 1; _pi < stmt->child_count; ++_pi) {
            if (is_postfix_label(stmt->children[_pi]->label))
              append_postfix_node(stmt->children[_pi], buf, sizeof(buf));
          }
        }
        if (buf[strlen(buf) - 1] != ';')
          strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
        append_operation(f, block, OP_RETURN, buf);
        set_last_operation_source_from_tree(block, stmt);
        collect_calls(f, stmt);
        // RETURN - закрывает блок (путь дальше недостижим)
        if (last_exit) *last_exit = block;
        // восстановим previous current_block перед выходом
        *current_block = prev_current;
        return first_entry;
      }
      // BREAK
      if (strcmp(stmt->label, "BREAK") == 0) {
        append_operation(f, block, OP_BREAK, "break;");
        set_last_operation_source_from_tree(block, stmt);

        LoopCtx ctx = top_loop(st);
        if (ctx.brk) {
          block->next_target = ctx.brk;
          add_edge(block, ctx.brk);
        }

        if (last_exit) *last_exit = block;
        return first_entry;
      }

      // CONTINUE
      if (strcmp(stmt->label, "CONTINUE") == 0) {
        append_operation(f, block, OP_CONTINUE, "continue;");
        set_last_operation_source_from_tree(block, stmt);

        CFGNode* loop_cond = top_loop_cond(st);
        if (loop_cond) {
          block->next_target = loop_cond;
          add_edge(block, loop_cond);
        }

        if (last_exit) *last_exit = block;
        return first_entry;
      }

      // IF / WHILE
      if (strcmp(stmt->label, "IF") == 0 || strcmp(stmt->label, "WHILE") == 0) {
        // прежде чем делать ветвление — если в текущем блоке есть операции,
        // оставляем связь
        CFGNode* cond_node = NULL;

        char condition[512] = "";
        if (stmt->child_count > 0)
          node_to_code(stmt->children[0], condition, sizeof(condition));
        if (stmt->child_count > 0) collect_calls(f, stmt->children[0]);

        cond_node = create_node(st, condition);
        set_cfg_node_source_from_tree(
            cond_node, (stmt->child_count > 0) ? stmt->children[0] : stmt);
        if (strcmp(stmt->label, "IF") == 0)
          cond_node->color = strdup("LightBlue");
        else
          cond_node->color = strdup("LightGreen");
        add_node(f, cond_node);

        // связываем текущий блок -> cond_node
        // ВСЕГДА связываем блок -> cond_node, даже если он пустой
        block->next_target = cond_node;
        add_edge(block, cond_node);

        if (strcmp(stmt->label, "IF") == 0) {
          TreeNode* then_block =
              (stmt->child_count > 1) ? stmt->children[1] : NULL;
          TreeNode* else_block =
              (stmt->child_count > 2) ? stmt->children[2] : NULL;

          CFGNode* then_entry = NULL;
          CFGNode* then_exit = NULL;
          CFGNode* else_entry = NULL;
          CFGNode* else_exit = NULL;

          // Для then и else используем новую current_block (NULL) — build
          // вернёт базовые блоки внутри
          CFGNode* saved_current = *current_block;
          *current_block = NULL;
          if (then_block)
            then_entry = build_cfg_from_tree_internal(st, f, then_block,
                                                      &then_exit, current_block);
          *current_block = NULL;
          if (else_block)
            else_entry = build_cfg_from_tree_internal(st, f, else_block,
                                                      &else_exit, current_block);
          // восстановить текущий блок как новый merge
          *current_block = saved_current;

          if (then_entry) cond_node->true_target = then_entry;
          if (else_entry) cond_node->false_target = else_entry;

          CFGNode* merge = create_node(st, "");
          add_node(f, merge);

          int then_term = block_terminates(then_exit);
          int else_term = block_terminates(else_exit);

          if (then_exit && !then_term) {
            then_exit->next_target = merge;
            add_edge(
                then_exit,
                merge);  
          }
          if (else_exit && !else_term) {
            else_exit->next_target = merge;
            add_edge(else_exit, merge);
          }

          if (!then_entry) cond_node->true_target = merge;
          if (!else_entry) cond_node->false_target = merge;


          // если одна из ветвей отсутствует — направляем соответствующий
          // переход на merge
          if (!then_entry) cond_node->true_target = merge;
          if (!else_entry) cond_node->false_target = merge;

          // после if дальше идёт merge (новый текущий блок)
          block = merge;
          *current_block = block;
          last_block_or_exit = block;
          continue;
        } else {
          // WHILE
          TreeNode* body = (stmt->child_count > 1) ? stmt->children[1] : NULL;

          // merge (куда идёт break и false ветка)
          CFGNode* merge = create_node(st, "");
          add_node(f, merge);

          // push loop context: continue -> cond_node, break -> merge
          push_loop(st, cond_node, merge);

          CFGNode* saved_current = *current_block;
          *current_block = NULL;

          CFGNode* body_entry = NULL;
          CFGNode* body_exit = NULL;
          if (body)
            body_entry = build_cfg_from_tree_internal(st, f, body, &body_exit,
                                                      current_block);

          *current_block = saved_current;

          // pop loop context after building body
          pop_loop(st);

          if (body_entry)
            cond_node->true_target = body_entry;
          else
            cond_node->true_target = cond_node;

          // back-edge: body_exit -> cond_node
          if (body_exit) {
            int ends_with_return = 0;
            if (body_exit->ops_count > 0) {
              Operation* lo = body_exit->ops[body_exit->ops_count - 1];
              if (lo && lo->type == OP_RETURN) ends_with_return = 1;
            }
            if (!ends_with_return) {
              body_exit->next_target = cond_node;
              add_edge(body_exit,
                       cond_node);  // если ты где-то реально используешь outs
            }
          } else if (body_entry) {
            // если тело есть, но exit почему-то NULL — хотя бы закольцевать
            // entry (обычно это не нужно, но как safety)
          }


          cond_node->false_target = merge;

          block = merge;
          *current_block = block;
          last_block_or_exit = merge;
          continue;
        }

      }  // IF/WHILE

      // общий случай — рекурсивно обрабатываем ребёнка
      CFGNode* child_entry = NULL;
      CFGNode* child_exit = NULL;
      child_entry =
          build_cfg_from_tree_internal(st, f, stmt, &child_exit, current_block);
      if (child_entry && child_entry != block) {
        block->next_target = child_entry;
        if (first_entry == block) first_entry = child_entry;
      }

      if (child_exit) last_block_or_exit = child_exit;
    }  // for children

    // восстановим outer current_block
    *current_block = prev_current;
    if (last_exit) *last_exit = last_block_or_exit;
    return first_entry;
  }

  // IF  BLOCK (: if (...) if (...) return ...;)
  if (strcmp(label, "IF") == 0) {
    TreeNode* cond_tree = (tree->child_count > 0) ? tree->children[0] : NULL;

    char condition[512] = "";
    if (cond_tree) node_to_code(cond_tree, condition, sizeof(condition));

    CFGNode* cond_node = create_node(st, condition);
    set_cfg_node_source_from_tree(cond_node, cond_tree ? cond_tree : tree);
    cond_node->color = strdup("LightBlue");
    add_node(f, cond_node);

    if (cond_tree) collect_calls(f, cond_tree);  // :

    CFGNode* entry = cond_node;

    //       IF
    if (current_block && *current_block) {
      (*current_block)->next_target = cond_node;
      add_edge(*current_block, cond_node);
      entry = *current_block;
    }

    TreeNode* then_tree = (tree->child_count > 1) ? tree->children[1] : NULL;
    TreeNode* else_tree = (tree->child_count > 2) ? tree->children[2] : NULL;

    CFGNode *then_entry = NULL, *then_exit = NULL;
    CFGNode *else_entry = NULL, *else_exit = NULL;

    //     current_block
    CFGNode* tmp = NULL;
    if (then_tree)
      then_entry = build_cfg_from_tree_internal(st, f, then_tree, &then_exit,
                                                &tmp);
    tmp = NULL;
    if (else_tree)
      else_entry = build_cfg_from_tree_internal(st, f, else_tree, &else_exit,
                                                &tmp);

    CFGNode* merge = create_node(st, "");
    add_node(f, merge);

    cond_node->true_target = then_entry ? then_entry : merge;
    cond_node->false_target = else_entry ? else_entry : merge;

    int then_returns = 0, else_returns = 0;
    if (then_exit && then_exit->ops_count > 0) {
      Operation* lo = then_exit->ops[then_exit->ops_count - 1];
      if (lo && lo->type == OP_RETURN) then_returns = 1;
    }
    if (else_exit && else_exit->ops_count > 0) {
      Operation* lo = else_exit->ops[else_exit->ops_count - 1];
      if (lo && lo->type == OP_RETURN) else_returns = 1;
    }

    if (then_exit && !then_returns) then_exit->next_target = merge;
    if (else_exit && !else_returns) else_exit->next_target = merge;

    if (current_block) *current_block = merge;
    if (last_exit) *last_exit = merge;

    return entry;
  }

  // WHILE  BLOCK (    statement  then/else
  // {})
  if (strcmp(label, "WHILE") == 0) {
    TreeNode* cond_tree = (tree->child_count > 0) ? tree->children[0] : NULL;

    char condition[512] = "";
    if (cond_tree) node_to_code(cond_tree, condition, sizeof(condition));

    CFGNode* cond_node = create_node(st, condition);
    set_cfg_node_source_from_tree(cond_node, cond_tree ? cond_tree : tree);
    cond_node->color = strdup("LightGreen");
    add_node(f, cond_node);

    if (cond_tree) collect_calls(f, cond_tree);  //

    CFGNode* entry = cond_node;

    if (current_block && *current_block) {
      (*current_block)->next_target = cond_node;
      add_edge(*current_block, cond_node);
      entry = *current_block;
    }

    TreeNode* body_tree = (tree->child_count > 1) ? tree->children[1] : NULL;

    CFGNode *body_entry = NULL, *body_exit = NULL;
    CFGNode* tmp = NULL;
    if (body_tree)
      body_entry = build_cfg_from_tree_internal(st, f, body_tree, &body_exit,
                                                &tmp);

    cond_node->true_target = body_entry ? body_entry : cond_node;

    // back-edge
    if (body_exit && body_exit->ops_count > 0) {
      Operation* lo = body_exit->ops[body_exit->ops_count - 1];
      if (!(lo && lo->type == OP_RETURN)) body_exit->next_target = cond_node;
    } else if (body_entry) {
      //
    }

    CFGNode* merge = create_node(st, "");
    add_node(f, merge);
    cond_node->false_target = merge;

    if (current_block) *current_block = merge;
    if (last_exit) *last_exit = merge;

    return entry;
  }

  // VAR_DECL вне блока — если есть инициализация, создаём отдельный
  // блок-операцию
  if (strcmp(label, "VAR_DECL") == 0) {
    char type_str[256] = "";
    for (int j = 0; j < tree->child_count; j++) {
      if (strcmp(tree->children[j]->label, "TYPE_REF") == 0) {
        node_to_code(tree->children[j], type_str, sizeof(type_str));
        break;
      }
    }
    CFGNode* last_node = NULL;
    for (int j = 0; j < tree->child_count; j++) {
      if (strcmp(tree->children[j]->label, "VAR_INIT") != 0) continue;
      TreeNode* var_init = tree->children[j];
      char name[256] = "";
      char arr_suffix[128] = "";
      char value[512] = "";
      int value_start = 1;
      if (var_init->child_count > 0)
        node_to_code(var_init->children[0], name, sizeof(name));
      if (var_init->child_count > 1 &&
          strcmp(var_init->children[1]->label, "ARRAY_DECL") == 0) {
        node_to_code(var_init->children[1], arr_suffix, sizeof(arr_suffix));
        value_start = 2;
      }
      if (arr_suffix[0]) {
        strncat(name, arr_suffix, sizeof(name) - strlen(name) - 1);
      }
      if (var_init->child_count > value_start)
        node_range_to_code(var_init, value_start, value, sizeof(value));
      char buf[1024];
      if (value[0]) {
        if (type_str[0])
          snprintf(buf, sizeof(buf), "decl %s %s = %s;", type_str, name, value);
        else
          snprintf(buf, sizeof(buf), "decl int %s = %s;", name, value);
      } else {
        if (type_str[0])
          snprintf(buf, sizeof(buf), "decl %s %s;", type_str, name);
        else
          snprintf(buf, sizeof(buf), "decl int %s;", name);
      }

      CFGNode* node = create_node(st, "");
      set_cfg_node_source_from_tree(node, var_init);
      add_node(f, node);
      append_operation(f, node, OP_ASSIGN, buf);
      set_last_operation_source_from_tree(node, var_init);
      last_node = node;
    }
    if (last_exit) *last_exit = last_node;
    return last_node;
  }

  // EXPR_STMT вне блока — делаем отдельный блок с операцией
  if (strcmp(label, "EXPR_STMT") == 0) {
    char buf[1024] = "";
    node_range_to_code(tree, 0, buf, sizeof(buf));
    if (strlen(buf) > 0 && buf[strlen(buf) - 1] != ';')
      strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
    CFGNode* node = create_node(st, "");
    set_cfg_node_source_from_tree(node, tree);
    add_node(f, node);
    append_operation(f, node, OP_EXPR, buf);
    set_last_operation_source_from_tree(node, tree);
    collect_calls(f, tree);
    if (last_exit) *last_exit = node;
    return node;
  }

  // CALL вне блока
  if (strcmp(label, "CALL") == 0) {
    char buf[1024] = "";
    node_to_code(tree, buf, sizeof(buf));
    if (strlen(buf) > 0 && buf[strlen(buf) - 1] != ';')
      strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
    CFGNode* node = create_node(st, "");
    set_cfg_node_source_from_tree(node, tree);
    add_node(f, node);
    append_operation(f, node, OP_CALL, buf);
    set_last_operation_source_from_tree(node, tree);
    char func_name[256] = "";
    find_callee_name_dfs(tree, func_name, sizeof(func_name));
    if (func_name[0] == '\0')
      strncpy(func_name, "unknown_call", sizeof(func_name) - 1);
    add_called_function(f, func_name);
    if (last_exit) *last_exit = node;
    return node;
  }

  // RETURN вне блока
  if (strcmp(label, "RETURN") == 0) {
    char buf[1024] = "return";
    if (tree->child_count > 0) {
      strcat(buf, " ");
      node_to_code(tree->children[0], buf + strlen(buf),
                   sizeof(buf) - strlen(buf));
      for (int _pi = 1; _pi < tree->child_count; ++_pi) {
        if (is_postfix_label(tree->children[_pi]->label))
          append_postfix_node(tree->children[_pi], buf, sizeof(buf));
      }
    }
    if (buf[strlen(buf) - 1] != ';')
      strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
    CFGNode* node = create_node(st, "");
    set_cfg_node_source_from_tree(node, tree);
    add_node(f, node);
    append_operation(f, node, OP_RETURN, buf);
    set_last_operation_source_from_tree(node, tree);
    if (last_exit) *last_exit = node;
    return node;
  }

  // Остальные узлы: рекурсивно обрабатываем детей и соединяем результаты
  if (tree->child_count > 0) {
    CFGNode* first = NULL;
    CFGNode* prev_exit = NULL;
    for (int i = 0; i < tree->child_count; i++) {
      CFGNode* child_node = build_cfg_from_tree_internal(
          st, f, tree->children[i], &prev_exit, current_block);
      if (!first) first = child_node;
      if (prev_exit && child_node && prev_exit != child_node) {
        int ends_with_return = 0;
        if (prev_exit->ops_count > 0) {
          Operation* last_op = prev_exit->ops[prev_exit->ops_count - 1];
          if (last_op && last_op->type == OP_RETURN) ends_with_return = 1;
        }
        if (!ends_with_return) prev_exit->next_target = child_node;
      }
      prev_exit = prev_exit ? prev_exit : child_node;
    }
    if (last_exit) *last_exit = prev_exit;
    return first;
  }

  return NULL;
}

static void dump_tree(FILE* out, TreeNode* t, int indent) {
  if (!t) return;

  for (int i = 0; i < indent; i++) fputc(' ', out);

  fprintf(out, "%s\n", t->label);

  for (int i = 0; i < t->child_count; i++)
    dump_tree(out, t->children[i], indent + 2);
}

static MethodInfo* find_declared_method(UserTypeInfo* type,
                                        const char* source_name,
                                        char** param_types, int param_count) {
  if (!type || !source_name) return NULL;
  for (int i = 0; i < type->method_count; ++i) {
    MethodInfo* method = &type->methods[i];
    if (strcmp(method->source_name, source_name) != 0) continue;
    if (method->param_count != param_count) continue;
    int match = 1;
    for (int j = 0; j < param_count; ++j) {
      if (strcmp(method->param_types[j], param_types[j]) != 0) {
        match = 0;
        break;
      }
    }
    if (match) return method;
  }
  return NULL;
}

static void append_source_function(SourceFileInfo* sf, FunctionCFG* func) {
  if (!sf || !func) return;
  sf->functions =
      realloc(sf->functions, sizeof(FunctionCFG*) * (size_t)(sf->functions_count + 1));
  sf->functions[sf->functions_count++] = func;
}

static char* specialize_type_in_context(AnalysisResult* res,
                                        UserTypeInfo* current_type,
                                        const char* type_name) {
  char actual_args[16][256];
  char base_name[256];
  UserTypeInfo* base_type = NULL;
  int actual_arg_count = 0;

  if (!type_name) return strdup("int");
  if (!res || !current_type || !current_type->name ||
      !strchr(current_type->name, '<')) {
    return strdup(type_name);
  }

  base_type_name(current_type->name, base_name, sizeof(base_name));
  base_type = find_exact_user_type(res, base_name);
  if (!base_type || base_type->template_param_count <= 0) {
    return strdup(type_name);
  }

  actual_arg_count = parse_type_arguments(
      current_type->name, actual_args,
      (int)(sizeof(actual_args) / sizeof(actual_args[0])));
  if (actual_arg_count != base_type->template_param_count) {
    return strdup(type_name);
  }

  return substitute_template_type(type_name, base_type, actual_args,
                                  actual_arg_count);
}

static void build_functions_from_tree(CfgBuilderState* st, TreeNode* t,
                                      SourceFileInfo* sf, AnalysisResult* res,
                                      UserTypeInfo* current_type,
                                      int* func_idx) {
  if (!t) return;

  if (strcmp(t->label, "CLASS_DEF") == 0) {
    UserTypeInfo* owner = current_type;
    const char* class_name = NULL;
    for (int i = 0; i < t->child_count; ++i) {
      if (t->children[i]->child_count == 0) {
        class_name = t->children[i]->label;
        owner = find_exact_user_type(res, class_name);
        if (!owner) owner = find_user_type(res, class_name);
        break;
      }
    }
    if (owner && owner->template_param_count > 0) {
      int emitted_specializations = 0;
      for (int i = 0; i < res->type_count; ++i) {
        UserTypeInfo* specialized = &res->types[i];
        if (!specialized->name ||
            !is_type_specialization_name(specialized->name, owner->name)) {
          continue;
        }
        emitted_specializations = 1;
        for (int j = 0; j < t->child_count; ++j) {
          build_functions_from_tree(st, t->children[j], sf, res, specialized,
                                    func_idx);
        }
      }
      if (emitted_specializations) return;
    }
    for (int i = 0; i < t->child_count; ++i) {
      build_functions_from_tree(st, t->children[i], sf, res, owner, func_idx);
    }
    return;
  }

  if (strcmp(t->label, "MEMBER") == 0) {
    for (int i = 0; i < t->child_count; ++i) {
      build_functions_from_tree(st, t->children[i], sf, res, current_type,
                                func_idx);
    }
    return;
  }

  if (strcmp(t->label, "FUNC_DEF") == 0 && func_def_has_block(t)) {
    TreeNode* func_def = t;
    TreeNode* sig = NULL;
    for (int k = 0; k < func_def->child_count; ++k) {
      if (strcmp(func_def->children[k]->label, "FUNC_SIG") == 0) {
        sig = func_def->children[k];
        break;
      }
    }
    if (!sig) return;

    char* source_name = NULL;
    char* return_type = NULL;
    char** param_names = NULL;
    char** param_types = NULL;
    int param_count = 0;
    parse_signature_info(sig, &source_name, &return_type, &param_names,
                         &param_types, &param_count);
    if (current_type) {
      char* specialized_return =
          specialize_type_in_context(res, current_type, return_type);
      free(return_type);
      return_type = specialized_return;
      for (int i = 0; i < param_count; ++i) {
        char* specialized_param =
            specialize_type_in_context(res, current_type, param_types[i]);
        free(param_types[i]);
        param_types[i] = specialized_param;
      }
    }

    MethodInfo* declared_method =
        current_type ? find_declared_method(current_type, source_name,
                                           param_types, param_count)
                     : NULL;
    char* emitted_name =
        declared_method
            ? strdup(declared_method->mangled_name)
            : (current_type
                   ? mangle_method_name(current_type->name, source_name,
                                        param_types, param_count)
                   : (strcmp(source_name, "main") == 0
                          ? strdup("main")
                          : mangle_method_name("global", source_name,
                                               param_types, param_count)));

    FunctionCFG* func = create_function(emitted_name, sf->filename);
    free(func->source_name);
    func->source_name = strdup(source_name);
    free(func->return_type);
    func->return_type = strdup(return_type);
    func->is_async = signature_is_async(sig);
    if (current_type) {
      func->is_method = 1;
      func->owner_type = strdup(current_type->name);
      add_param_typed(func, "this", current_type->name);
    }
    for (int i = 0; i < param_count; ++i) {
      add_param_typed(func, param_names[i], param_types[i]);
    }
    free(emitted_name);

    reset_function_state(st);

    CFGNode* start = create_node(st, "START");
    add_node(func, start);
    func->entry = start;

    CFGNode* exit = NULL;
    CFGNode* body = build_cfg_from_tree(st, func, func_def, &exit);
    if (body) start->next_target = body;

    CFGNode* finish = create_node(st, "FINISH");
    add_node(func, finish);
    func->exit = finish;
    for (int i = 0; i < func->node_count; i++) {
      CFGNode* n = func->nodes[i];
      if (!n || n == finish) continue;

      if (n->ops_count > 0) {
        Operation* last = n->ops[n->ops_count - 1];
        if (last && last->type == OP_RETURN) {
          //     next_target
          if (n->next_target == NULL) {
            n->next_target = finish;
            add_edge(n, finish);  //   outs
          }
        }
      }
    }
    if (exit) {
      int ends_with_return = 0;
      if (exit->ops_count > 0) {
        Operation* lo = exit->ops[exit->ops_count - 1];
        if (lo && lo->type == OP_RETURN) ends_with_return = 1;
      }
      if (!ends_with_return) exit->next_target = finish;
    }
    if (exit) {
      exit->next_target = finish;  //    FINISH,   return
    } else if (body)
      body->next_target = finish;
    else
      start->next_target = finish;

    append_source_function(sf, func);
    if (func_idx) *func_idx = sf->functions_count;
    free(source_name);
    free(return_type);
    for (int i = 0; i < param_count; ++i) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    return;
  }

  // рекурсивно обходим детей
  for (int i = 0; i < t->child_count; i++) {
    build_functions_from_tree(st, t->children[i], sf, res, current_type,
                              func_idx);
  }
}

/* --- Построение CFG для файлов --- */
AnalysisResult* build_cfg_from_parse_trees(SourceFileInfo** files,
                                           int files_count) {
  CfgBuilderState state;
  TreeNode** trees = NULL;
  init_builder_state(&state);

  AnalysisResult* res = malloc(sizeof(AnalysisResult));
  res->files_count = files_count;
  res->files = malloc(sizeof(SourceFileInfo*) * files_count);
  res->types = NULL;
  res->type_count = 0;
  res->errors = NULL;
  res->errors_count = 0;
  trees = calloc((size_t)files_count, sizeof(TreeNode*));

  for (int i = 0; i < files_count; i++) {
    SourceFileInfo* sf = malloc(sizeof(SourceFileInfo));
    sf->filename = strdup(files[i]->filename);
    sf->parse_tree = files[i]->parse_tree;
    sf->functions = NULL;
    sf->functions_count = 0;
    reset_function_state(&state);

    trees[i] = convert_antlr_tree_node((pANTLR3_BASE_TREE)sf->parse_tree);

    // Подсчитываем функции рекурсивно
    collect_class_defs(trees[i], res);

    // Нет функций — создаём одну псевдо-main по всему дереву
    res->files[i] = sf;
  }

  for (int i = 0; i < files_count; ++i) {
    collect_type_specializations(trees[i], res);
  }
  finalize_type_layouts(res);

  for (int i = 0; i < files_count; ++i) {
    SourceFileInfo* sf = res->files[i];
    TreeNode* tree = trees[i];
    int func_idx = 0;

    reset_function_state(&state);
    build_functions_from_tree(&state, tree, sf, res, NULL, &func_idx);

    if (sf->functions_count == 0) {
      FunctionCFG* func = create_function("main", sf->filename);
      reset_function_state(&state);

      CFGNode* start = create_node(&state, "START");
      add_node(func, start);
      func->entry = start;

      CFGNode* exit = NULL;
      CFGNode* body = build_cfg_from_tree(&state, func, tree, &exit);
      if (body) start->next_target = body;

      CFGNode* finish = create_node(&state, "FINISH");
      add_node(func, finish);
      func->exit = finish;

      if (exit) {
        int ends_with_return = 0;
        if (exit->ops_count > 0) {
          Operation* lo = exit->ops[exit->ops_count - 1];
          if (lo && lo->type == OP_RETURN) ends_with_return = 1;
        }
        if (!ends_with_return) exit->next_target = finish;
      } else if (body)
        body->next_target = finish;
      else
        start->next_target = finish;

      append_source_function(sf, func);
    }
  }

  for (int i = 0; i < files_count; ++i) {
    free_tree_node(trees[i]);
  }
  free(trees);

  free_builder_state(&state);
  return res;
}

/* --- Очистка --- */
void free_analysis_result(AnalysisResult* r) {
  if (!r) return;
  for (int i = 0; i < r->files_count; i++) {
    SourceFileInfo* sf = r->files[i];
    if (!sf) continue;
    for (int j = 0; j < sf->functions_count; j++) {
      FunctionCFG* f = sf->functions[j];
      if (!f) continue;
      for (int n = 0; n < f->node_count; n++) {
        CFGNode* node = f->nodes[n];
        if (!node) continue;
        if (node->label) free(node->label);
        if (node->color) free(node->color);
        // free operations
        if (node->ops) {
          for (int o = 0; o < node->ops_count; o++) {
            Operation* op = node->ops[o];
            if (!op) continue;
            if (op->text) free(op->text);
            free(op);
          }
          free(node->ops);
        }
        free(node);
      }
      free(f->nodes);
      for (int p = 0; p < f->param_count; p++) free(f->params[p]);
      free(f->params);
      for (int p = 0; p < f->param_count; p++) free(f->param_types[p]);
      free(f->param_types);
      for (int c = 0; c < f->called_count; c++) free(f->called_functions[c]);
      free(f->called_functions);
      free(f->func_name);
      free(f->source_name);
      free(f->owner_type);
      free(f->return_type);
      free(f->signature);
      free(f->source_filename);
      free(f);
    }
    free(sf->functions);
    free(sf->filename);
    free(sf);
  }
  for (int t = 0; t < r->type_count; ++t) {
    UserTypeInfo* type = &r->types[t];
    free(type->name);
    free(type->base_name);
    for (int i = 0; i < type->template_param_count; ++i)
      free(type->template_params[i]);
    free(type->template_params);
    for (int i = 0; i < type->field_count; ++i) {
      free(type->fields[i].name);
      free(type->fields[i].type_name);
    }
    free(type->fields);
    for (int i = 0; i < type->method_count; ++i) {
      MethodInfo* m = &type->methods[i];
      free(m->owner_type);
      free(m->source_name);
      free(m->mangled_name);
      free(m->return_type);
      for (int j = 0; j < m->param_count; ++j) {
        free(m->param_names[j]);
        free(m->param_types[j]);
      }
      free(m->param_names);
      free(m->param_types);
    }
    free(type->methods);
  }
  free(r->types);
  for (int e = 0; e < r->errors_count; e++) free(r->errors[e]);
  free(r->errors);
  free(r->files);
  free(r);
}

/* --- DGML для CFG + CallGraph --- */
void write_cfg_callgraph_dgml(AnalysisResult* res, const char* base_filename) {
  for (int i = 0; i < res->files_count; i++) {
    SourceFileInfo* sf = res->files[i];
    for (int j = 0; j < sf->functions_count; j++) {
      FunctionCFG* f = sf->functions[j];

      // ************ DGML FILE NAME ************
      char filename_dgml[512];
      snprintf(filename_dgml, sizeof(filename_dgml), "%s_%s.dgml",
               base_filename, f->func_name ? f->func_name : "unknown");
      FILE* out = fopen(filename_dgml, "w");
      if (!out) {
        fprintf(stderr, "Cannot open file '%s' for writing\n", filename_dgml);
        continue;
      }

      // ************ DGML OUTPUT ************
      fprintf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
      fprintf(out,
              "<DirectedGraph Title=\"CFG+CallGraph for %s\" "
              "xmlns=\"http://schemas.microsoft.com/vs/2009/dgml\">\n",
              f->func_name ? f->func_name : "unknown");
      fprintf(out, "  <Nodes>\n");

      // CFG Nodes
      for (int n = 0; n < f->node_count; n++) {
        CFGNode* node = f->nodes[n];
        // Build a short label: if ops exist, concatenate first N chars of ops
        char shortlabel[1024] = "";
        if (node->ops_count > 0) {
          for (int o = 0; o < node->ops_count; o++) {
            if (o > 0)
              strncat(shortlabel, "; ",
                      sizeof(shortlabel) - strlen(shortlabel) - 1);
            strncat(shortlabel, node->ops[o]->text,
                    sizeof(shortlabel) - strlen(shortlabel) - 1);
            if (strlen(shortlabel) > 800) break;
          }
        } else {
          strncpy(shortlabel, node->label ? node->label : "",
                  sizeof(shortlabel) - 1);
          shortlabel[sizeof(shortlabel) - 1] = '\0';
        }

        fprintf(out, "    <Node Id=\"n%d\" Label=\"", node->id);
        for (char* c = shortlabel; c && *c; ++c) {
          if (*c == '&')
            fprintf(out, "&amp;");
          else if (*c == '<')
            fprintf(out, "&lt;");
          else if (*c == '>')
            fprintf(out, "&gt;");
          else if (*c == '"')
            fprintf(out, "&quot;");
          else
            fputc(*c, out);
        }
        fprintf(out, "\"");

        if (node->color) {
          char safe_color[64];
          int idx = 0;
          for (char* c = node->color; c && *c && idx < 63; ++c) {
            if ((unsigned char)*c >= 32) safe_color[idx++] = *c;
          }
          safe_color[idx] = '\0';
          if (idx > 0) fprintf(out, " Background=\"%s\"", safe_color);
        }

        fprintf(out, " />\n");
      }

      // Call nodes
      for (int c = 0; c < f->called_count; c++) {
        fprintf(out, "    <Node Id=\"call_%d_%d\" Label=\"", i, c);
        for (char* ch = f->called_functions[c]; ch && *ch; ++ch) {
          if (*ch == '&')
            fprintf(out, "&amp;");
          else if (*ch == '<')
            fprintf(out, "&lt;");
          else if (*ch == '>')
            fprintf(out, "&gt;");
          else if (*ch == '"')
            fprintf(out, "&quot;");
          else
            fputc(*ch, out);
        }
        fprintf(out, "\" />\n");
      }

      fprintf(out, "  </Nodes>\n  <Links>\n");

      // CFG edges
      for (int n = 0; n < f->node_count; n++) {
        CFGNode* node = f->nodes[n];
        if (node->next_target)
          fprintf(out, "    <Link Source=\"n%d\" Target=\"n%d\" />\n", node->id,
                  node->next_target->id);

        if (node->true_target)
          fprintf(out,
                  "    <Link Source=\"n%d\" Target=\"n%d\" Label=\"true\" />\n",
                  node->id, node->true_target->id);

        if (node->false_target)
          fprintf(
              out,
              "    <Link Source=\"n%d\" Target=\"n%d\" Label=\"false\" />\n",
              node->id, node->false_target->id);
      }

      // Call edges
      for (int c = 0; c < f->called_count; c++) {
        if (f->entry) {
          fprintf(out,
                  "    <Link Source=\"n%d\" Target=\"call_%d_%d\" "
                  "Label=\"calls\" />\n",
                  f->entry->id, i, c);
        }
      }

      fprintf(out, "  </Links>\n</DirectedGraph>\n");
      fclose(out);

      fprintf(stderr, "CFG written to %s\n", filename_dgml);

      // ================================================================
      // EXT FORMAT
      // ================================================================
      char filename_ext[512];
      snprintf(filename_ext, sizeof(filename_ext), "%s_%s.ext", base_filename,
               f->func_name ? f->func_name : "unknown");

      FILE* ext = fopen(filename_ext, "w");
      if (!ext) {
        fprintf(stderr, "Cannot open file '%s' for writing\n", filename_ext);
        continue;
      }

      fprintf(ext, "FUNCTION: %s\n", f->func_name ? f->func_name : "unknown");

      fprintf(ext, "\nNODES:\n");
      for (int n = 0; n < f->node_count; n++) {
        CFGNode* node = f->nodes[n];
        fprintf(ext, "  NODE %d:\n", node->id);
        if (node->ops_count > 0) {
          for (int o = 0; o < node->ops_count; o++) {
            fprintf(ext, "    %s\n",
                    node->ops[o]->text ? node->ops[o]->text : "");
          }
        } else {
          fprintf(ext, "    %s\n", node->label ? node->label : "");
        }
      }

      fprintf(ext, "\nEDGES:\n");
      for (int n = 0; n < f->node_count; n++) {
        CFGNode* node = f->nodes[n];
        if (node->next_target)
          fprintf(ext, "  %d -> %d  (next)\n", node->id, node->next_target->id);

        if (node->true_target)
          fprintf(ext, "  %d -> %d  (true)\n", node->id, node->true_target->id);

        if (node->false_target)
          fprintf(ext, "  %d -> %d  (false)\n", node->id,
                  node->false_target->id);
      }

      fprintf(ext, "\nCALLS:\n");
      for (int c = 0; c < f->called_count; c++) {
        fprintf(ext, "  call %s\n", f->called_functions[c]);
      }

      fclose(ext);
      fprintf(stderr, "EXT written to %s\n", filename_ext);
    }
  }
}

/* --- Запись DGML только для CallGraph --- */
static void print_xml_escaped(FILE* out, const char* s) {
  if (!s) return;
  for (const char* c = s; *c; ++c) {
    if (*c == '&')
      fputs("&amp;", out);
    else if (*c == '<')
      fputs("&lt;", out);
    else if (*c == '>')
      fputs("&gt;", out);
    else if (*c == '"')
      fputs("&quot;", out);
    else if ((unsigned char)*c < 32) {
      /* пропускаем управляющие символы */
    } else
      fputc(*c, out);
  }
}

/* ===================================================================
   TAC-VM: линейзация CFG в трехадресный код и генерация .tac
   =================================================================== */

/* Константы для размеров буферов и локальных переменных */
#define VAR_SIZE 4
#define MAX_BUFFER_LEN 512
#define MAX_SMALL_BUFFER_LEN 256
#define MAX_VAR_NAME_LEN 128
#define DEFAULT_SYMTAB_CAPACITY 8
#define DEFAULT_ARRAY_ELEMS 256
#define BUILTIN_HEAP_PTR_ADDR 180000
#define BUILTIN_HEAP_START_ADDR 180004

static int g_heap_label_counter = 0;

/* Небольшой trim-helpers */
static void trim_whitespace(char* s) {
  if (!s) return;
  char* start = s;
  while (*start && isspace((unsigned char)*start)) start++;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) {
    s[--len] = '\0';
  }
}

static int split_array_suffix(const char* text, char* base, size_t base_sz,
                              char* index_expr, size_t index_sz) {
  char tmp[256];
  char* lb = NULL;
  char* rb = NULL;
  size_t n;

  if (!text || !base || base_sz == 0) return 0;
  base[0] = '\0';
  if (index_expr && index_sz) index_expr[0] = '\0';

  strncpy(tmp, text, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  trim_whitespace(tmp);
  if (tmp[0] == '\0') return 0;

  lb = strchr(tmp, '[');
  rb = strrchr(tmp, ']');
  if (!lb || !rb || rb < lb) return 0;
  if (rb[1] != '\0') return 0;

  *lb = '\0';
  trim_whitespace(tmp);
  if (tmp[0] == '\0') return 0;
  strncpy(base, tmp, base_sz - 1);
  base[base_sz - 1] = '\0';

  if (index_expr && index_sz) {
    const char* start = lb + 1;
    n = (size_t)(rb - start);
    if (n >= index_sz) n = index_sz - 1;
    memcpy(index_expr, start, n);
    index_expr[n] = '\0';
    trim_whitespace(index_expr);
  }
  return 1;
}

static int type_is_array_name(const char* type_name) {
  const char* lb = NULL;
  const char* rb = NULL;
  if (!type_name) return 0;
  lb = strchr(type_name, '[');
  rb = strrchr(type_name, ']');
  return lb && rb && rb > lb;
}

static int type_without_array_dim(const char* type_name, char* out,
                                  size_t out_sz) {
  const char* lb = NULL;
  size_t n = 0;
  if (!type_name || !out || out_sz == 0) return 0;
  out[0] = '\0';
  lb = strchr(type_name, '[');
  if (!lb) {
    /* No array dimension suffix — type_name IS already the element type. */
    strncpy(out, type_name, out_sz - 1);
    out[out_sz - 1] = '\0';
    trim_whitespace(out);
    return out[0] != '\0';
  }
  n = (size_t)(lb - type_name);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, type_name, n);
  out[n] = '\0';
  trim_whitespace(out);
  return out[0] != '\0';
}

static int split_decl_type_and_name(const char* decl_text, char* out_type,
                                    size_t type_sz, char* out_name,
                                    size_t name_sz) {
  const char* p = decl_text;
  const char* sep = NULL;
  size_t n = 0;

  if (!decl_text || !out_type || !out_name || type_sz == 0 || name_sz == 0) {
    return 0;
  }
  out_type[0] = '\0';
  out_name[0] = '\0';

  while (*p && isspace((unsigned char)*p)) p++;
  sep = p;
  while (*sep && !isspace((unsigned char)*sep)) sep++;
  if (sep == p) return 0;

  n = (size_t)(sep - p);
  if (n >= type_sz) n = type_sz - 1;
  memcpy(out_type, p, n);
  out_type[n] = '\0';

  while (*sep && isspace((unsigned char)*sep)) sep++;
  if (*sep == '\0') return 0;
  strncpy(out_name, sep, name_sz - 1);
  out_name[name_sz - 1] = '\0';
  trim_whitespace(out_name);
  return out_name[0] != '\0';
}

/* ---------- Операнды и инструкции TAC-VM ---------- */

typedef enum {
  OP_REG,   // r5
  OP_IMM,   // #123
  OP_MEM,   // [fp-8]
  OP_LABEL  // L10
} OpKind;

typedef struct {
  OpKind kind;
  union {
    int reg;  // 0..31
    int imm;
    struct {
      int base;  // FP/SP/rX или -1 (абсолютный адрес)
      int offset;
    } mem;
    char* label;
  };
} Operand;

typedef struct Instr {
  char* mnemonic;
  Operand dst, src1, src2;  // для инструкций без операндов – игнорируются
  char* label;              // метка перед инструкцией (может быть NULL)
  int src_file_index;
  int src_line;
  int src_col;
  struct Instr* next;
} Instr;

typedef struct {
  char* name;
  Instr* head;
  Instr* tail;
  int instr_count;
  int local_size;  // байты под локальные переменные
  FuncSyms syms;
} FunctionCode;

static FunctionCode* current_fc = NULL;
static int g_current_src_file_index = -1;
static int g_current_src_line = 0;
static int g_current_src_col = 0;

static void clear_current_source_location(void) {
  g_current_src_file_index = -1;
  g_current_src_line = 0;
  g_current_src_col = 0;
}

static void set_current_source_location(int file_index, int line, int col) {
  g_current_src_file_index = file_index;
  g_current_src_line = line;
  g_current_src_col = col;
}

/* Регистры VM */
#define SP 30
#define FP 29

/* ---------- Таблица символов (локальные переменные) ---------- */

typedef struct {
  char* name;
  char* type_name;
  int offset;
  int is_param;
  int is_object;
  int is_array;
  int array_length;
  int storage_offset;
  int storage_size;
} Symbol;

typedef struct {
  Symbol* symbols;
  int count;
  int capacity;
  int local_vars_size;
  AnalysisResult* analysis;
  FunctionCFG* function;
} SymbolTable;

static void load_simple_to_reg(const char* tok, int regno, SymbolTable* st,
                               Instr** head, Instr** tail,
                               char** pending_label_ptr);
static void emit_call_with_args(const char* func_name, char args[][64], int argc,
                                SymbolTable* st, Instr** head, Instr** tail,
                                char** pending_label);
static int split_call_text(const char* text, char* fname, size_t fname_size,
                           char args[][64], int max_args);
static int emit_builtin_call_to_reg(const char* fname, char args[][64], int argc,
                                    int dst_reg, SymbolTable* st, Instr** head,
                                    Instr** tail, char** pending_label_ptr);

static int reserve_local_bytes(SymbolTable* st, int size) {
  if (!st || size <= 0) return 0;
  st->local_vars_size += size;
  return -st->local_vars_size;
}

static void init_symtab(SymbolTable* st, AnalysisResult* analysis,
                        FunctionCFG* function) {
  st->symbols = NULL;
  st->count = 0;
  st->capacity = 0;
  st->local_vars_size = 0;
  st->analysis = analysis;
  st->function = function;
}

static void free_symtab(SymbolTable* st) {
  for (int i = 0; i < st->count; i++) {
    free(st->symbols[i].name);
    free(st->symbols[i].type_name);
  }
  free(st->symbols);
}

static Symbol* find_symbol(SymbolTable* st, const char* name) {
  if (!st || !name) return NULL;
  for (int i = 0; i < st->count; ++i) {
    if (strcmp(st->symbols[i].name, name) == 0) return &st->symbols[i];
  }
  return NULL;
}

static Symbol* append_symbol(SymbolTable* st) {
  if (!st) return NULL;
  if (st->count >= st->capacity) {
    st->capacity =
        (st->capacity == 0) ? DEFAULT_SYMTAB_CAPACITY : st->capacity * 2;
    st->symbols = realloc(st->symbols, st->capacity * sizeof(Symbol));
  }
  Symbol* s = &st->symbols[st->count++];
  memset(s, 0, sizeof(*s));
  return s;
}

static int is_object_type(SymbolTable* st, const char* type_name) {
  return st && st->analysis && type_name && find_user_type(st->analysis, type_name);
}

static int is_implicit_field_name(SymbolTable* st, const char* name) {
  if (!st || !name || !st->function || !st->function->owner_type) return 0;
  if (find_symbol(st, name)) return 0;
  return find_field_in_type(st->analysis, st->function->owner_type, name) != NULL;
}

static Symbol* add_local_symbol(SymbolTable* st, const char* name,
                                const char* type_name) {
  char parsed_name[128];
  char array_base[128];
  char array_decl_expr[128];
  int has_array_suffix = 0;
  int array_length = 0;

  if (!st || !name) return NULL;
  strncpy(parsed_name, name, sizeof(parsed_name) - 1);
  parsed_name[sizeof(parsed_name) - 1] = '\0';
  trim_whitespace(parsed_name);
  has_array_suffix =
      split_array_suffix(parsed_name, array_base, sizeof(array_base),
                         array_decl_expr, sizeof(array_decl_expr));
  if (has_array_suffix) {
    strncpy(parsed_name, array_base, sizeof(parsed_name) - 1);
    parsed_name[sizeof(parsed_name) - 1] = '\0';
  }

  Symbol* existing = find_symbol(st, parsed_name);
  if (existing) return existing;

  Symbol* s = append_symbol(st);
  s->name = strdup(parsed_name);
  s->type_name = strdup(type_name ? type_name : "int");
  s->is_param = 0;
  s->offset = reserve_local_bytes(st, VAR_SIZE);
  s->is_array = has_array_suffix || type_is_array_name(s->type_name);
  if (has_array_suffix) {
    if (array_decl_expr[0]) {
      parse_int_literal_strict(array_decl_expr, &array_length);
    } else {
      array_length = DEFAULT_ARRAY_ELEMS;
    }
  }
  s->array_length = array_length;
  s->is_object = is_object_type(st, s->type_name);
  if (s->is_object && !s->is_array) {
    UserTypeInfo* type = find_user_type(st->analysis, s->type_name);
    s->storage_size = type ? type->instance_size : 0;
    if (s->storage_size > 0) {
      s->storage_offset = reserve_local_bytes(st, s->storage_size);
    }
  }
  return s;
}

static int get_var_offset(SymbolTable* st, const char* name) {
  if (!name) return 0;
  char buf[128];
  strncpy(buf, name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  trim_whitespace(buf);

  if (!is_simple_ident(buf)) {
    return 0;  // <-- КРИТИЧНО
  }
  if (strcmp(buf, "continue") == 0 || strcmp(buf, "break") == 0 ||
      strcmp(buf, "return") == 0)
    return 0;

  // критично: не заводим "пустые" или мусорные имена
  while (*name && isspace((unsigned char)*name)) name++;
  if (*name == '\0') return 0;

  // можно ещё жёстче: имя должно начинаться с буквы или '_'
  if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) return 0;
  Symbol* existing = find_symbol(st, name);
  if (existing) return existing->offset;
  Symbol* s = add_local_symbol(st, name, "int");
  return s ? s->offset : 0;
}

static void add_param_symbol(SymbolTable* st, const char* name, int offset,
                             const char* type_name) {
  char array_base[128];
  char array_decl_expr[128];
  if (!st || !name) return;
  if (find_symbol(st, name)) return;
  Symbol* s = append_symbol(st);
  s->name = strdup(name);
  s->type_name = strdup(type_name ? type_name : "int");
  s->offset = offset;
  s->is_param = 1;
  s->is_array = type_is_array_name(s->type_name);
  if (s->is_array &&
      split_array_suffix(s->type_name, array_base, sizeof(array_base),
                         array_decl_expr, sizeof(array_decl_expr)) &&
      array_decl_expr[0]) {
    parse_int_literal_strict(array_decl_expr, &s->array_length);
  }
  s->is_object = is_object_type(st, s->type_name);
}

static int split_member_ref(const char* expr, char* owner, size_t owner_sz,
                            char* member, size_t member_sz) {
  if (!expr || !owner || !member) return 0;
  const char* dot = strrchr(expr, '.');
  if (!dot || dot == expr) return 0;
  size_t left = (size_t)(dot - expr);
  size_t right = strlen(dot + 1);
  if (left + 1 > owner_sz || right + 1 > member_sz) return 0;
  memcpy(owner, expr, left);
  owner[left] = '\0';
  memcpy(member, dot + 1, right + 1);
  trim_whitespace(owner);
  trim_whitespace(member);
  return owner[0] && member[0];
}

static Symbol* resolve_owner_symbol(SymbolTable* st, const char* owner_name) {
  if (!st || !owner_name) return NULL;
  Symbol* sym = find_symbol(st, owner_name);
  if (sym) return sym;
  if (strcmp(owner_name, "this") == 0) return find_symbol(st, "this");
  return NULL;
}

static FieldInfo* resolve_field_access(SymbolTable* st, const char* expr,
                                       Symbol** owner_sym_out) {
  if (owner_sym_out) *owner_sym_out = NULL;
  if (!st || !expr) return NULL;

  char owner[128];
  char member[128];
  if (split_member_ref(expr, owner, sizeof(owner), member, sizeof(member))) {
    Symbol* owner_sym = resolve_owner_symbol(st, owner);
    if (!owner_sym || !owner_sym->type_name) return NULL;
    if (owner_sym_out) *owner_sym_out = owner_sym;
    return find_field_in_type(st->analysis, owner_sym->type_name, member);
  }

  if (st->function && st->function->owner_type && !find_symbol(st, expr)) {
    Symbol* this_sym = find_symbol(st, "this");
    if (!this_sym) return NULL;
    if (owner_sym_out) *owner_sym_out = this_sym;
    return find_field_in_type(st->analysis, st->function->owner_type, expr);
  }
  return NULL;
}

static const char* infer_simple_type(SymbolTable* st, const char* expr) {
  static char inferred_type[4][256];
  static int inferred_slot = 0;
  if (!expr || !*expr) return NULL;
  if (strcmp(expr, "true") == 0 || strcmp(expr, "false") == 0) return "bool";
  if (expr[0] == '\'' && expr[strlen(expr) - 1] == '\'') return "char";
  if (expr[0] == '"' && expr[strlen(expr) - 1] == '"') return "string";
  if (isdigit((unsigned char)expr[0]) ||
      (expr[0] == '-' && isdigit((unsigned char)expr[1]))) {
    return "int";
  }
  {
    char array_base[128];
    char array_index[128];
    if (split_array_suffix(expr, array_base, sizeof(array_base), array_index,
                           sizeof(array_index)) &&
        array_index[0]) {
      const char* array_type = NULL;
      Symbol* owner = NULL;
      FieldInfo* field =
          resolve_field_access(st, array_base, &owner);
      if (field) {
        array_type = field->type_name;
      } else {
        Symbol* sym = find_symbol(st, array_base);
        if (sym) array_type = sym->type_name;
      }
      if (array_type &&
          type_without_array_dim(array_type, inferred_type[inferred_slot],
                                 sizeof(inferred_type[inferred_slot]))) {
        const char* result = inferred_type[inferred_slot];
        inferred_slot = (inferred_slot + 1) % 4;
        return result;
      }
    }
  }
  Symbol* owner = NULL;
  FieldInfo* field = resolve_field_access(st, expr, &owner);
  if (field) return field->type_name;
  Symbol* sym = find_symbol(st, expr);
  if (sym) return sym->type_name;
  return NULL;
}

/* ---------- Конструкторы операндов ---------- */

static Operand reg(int r) {
  Operand o;
  o.kind = OP_REG;
  o.reg = r;
  return o;
}
static Operand imm(int v) {
  Operand o;
  o.kind = OP_IMM;
  o.imm = v;
  return o;
}
static Operand label_op(char* l) {
  Operand o;
  o.kind = OP_LABEL;
  o.label = l;
  return o;
}
static Operand mem_op(int base, int off) {
  Operand o;
  o.kind = OP_MEM;
  o.mem.base = base;
  o.mem.offset = off;
  return o;
}
static Operand zero_reg(void) { return reg(0); }

/* ---------- strdupf для меток ---------- */

static char* strdupf(const char* fmt, ...) {
  char buf[MAX_VAR_NAME_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return strdup(buf);
}

/* ---------- emit одной инструкции ---------- */

static void emit_instr(Instr** head_ref, Instr** tail_ref, const char* mnemonic,
                       Operand dst, Operand src1, Operand src2, char* label) {
  Instr* i = calloc(1, sizeof(Instr));
  i->mnemonic = strdup(mnemonic);
  i->dst = dst;
  i->src1 = src1;
  i->src2 = src2;
  i->label = label;
  i->src_file_index = g_current_src_file_index;
  i->src_line = g_current_src_line;
  i->src_col = g_current_src_col;

  if (*tail_ref) {
    (*tail_ref)->next = i;
  } else {
    *head_ref = i;
  }
  *tail_ref = i;
  if (current_fc) {
    current_fc->instr_count++;
  }
}

/* ---------- DFS по CFG для порядка блоков ---------- */

static void dfs_postorder(CFGNode* node, bool* visited, int max_id, int* order,
                          int* idx) {
  if (!node) return;
  if (node->id < 0 || node->id > max_id) return;
  if (visited[node->id]) return;
  visited[node->id] = true;

  if (node->true_target)
    dfs_postorder(node->true_target, visited, max_id, order, idx);
  if (node->false_target)
    dfs_postorder(node->false_target, visited, max_id, order, idx);
  if (node->next_target && node->next_target != node->true_target &&
      node->next_target != node->false_target)
    dfs_postorder(node->next_target, visited, max_id, order, idx);

  order[(*idx)++] = node->id;  // пост-визит
}

static char* take_pending_label(char** pending_label);
static int resolve_array_element_type(SymbolTable* st, const char* array_expr,
                                      char* out_type, size_t out_type_sz);
static void eval_arg_to_reg(const char* tok, int dst_reg, SymbolTable* st,
                            Instr** head, Instr** tail,
                            char** pending_label_ptr);

static void emit_load_array_base_ptr(const char* base_expr, int dst_reg,
                                     SymbolTable* st, Instr** head,
                                     Instr** tail, char* label) {
  Symbol* owner_sym = NULL;
  FieldInfo* field = resolve_field_access(st, base_expr, &owner_sym);
  if (field && owner_sym) {
    emit_instr(head, tail, "mov", reg(6), mem_op(FP, owner_sym->offset),
               zero_reg(), label);
    emit_instr(head, tail, "mov", reg(dst_reg), mem_op(6, field->offset),
               zero_reg(), NULL);
    return;
  }

  {
    int off = get_var_offset(st, base_expr);
    emit_instr(head, tail, "mov", reg(dst_reg), mem_op(FP, off), zero_reg(),
               label);
  }
}

static void load_simple_to_reg(const char* tok, int regno, SymbolTable* st,
                               Instr** head, Instr** tail,
                               char** pending_label_ptr) {
  if (!tok) return;
  trim_whitespace((char*)tok);  // если tok не const в твоей версии
  if (tok[0] == '\0') return;

  char* lbl = *pending_label_ptr;

  {
    char fname[64];
    char args_arr[16][64];
    int call_info = split_call_text(tok, fname, sizeof(fname), args_arr, 16);
    if (call_info > 0) {
      int argc = call_info - 1;

      if (emit_builtin_call_to_reg(fname, args_arr, argc, regno, st, head, tail,
                                   pending_label_ptr)) {
        return;
      }

      {
        char owner_name[64];
        char method_name[64];
        if (split_member_ref(fname, owner_name, sizeof(owner_name), method_name,
                             sizeof(method_name))) {
          Symbol* owner_sym = resolve_owner_symbol(st, owner_name);
          if (owner_sym && owner_sym->type_name) {
            char* arg_types[16] = {0};
            for (int i = 0; i < argc; ++i)
              arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
            MethodInfo* method =
                find_method_in_type(st->analysis, owner_sym->type_name,
                                    method_name, arg_types, argc);
            if (method) {
              char call_args[17][64];
              snprintf(call_args[0], sizeof(call_args[0]), "%s", owner_name);
              for (int i = 0; i < argc; ++i) {
                snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                         args_arr[i]);
              }
              emit_call_with_args(method->mangled_name, call_args, argc + 1, st,
                                  head, tail, pending_label_ptr);
              if (regno != 1) {
                emit_instr(head, tail, "mov", reg(regno), reg(1), zero_reg(),
                           NULL);
              }
              return;
            }
          }
        }
      }

      if (st->function && st->function->owner_type) {
        char* arg_types[16] = {0};
        for (int i = 0; i < argc; ++i)
          arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
        MethodInfo* method =
            find_method_in_type(st->analysis, st->function->owner_type, fname,
                                arg_types, argc);
        if (method) {
          char call_args[17][64];
          snprintf(call_args[0], sizeof(call_args[0]), "%s", "this");
          for (int i = 0; i < argc; ++i) {
            snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                     args_arr[i]);
          }
          emit_call_with_args(method->mangled_name, call_args, argc + 1, st,
                              head, tail, pending_label_ptr);
          if (regno != 1) {
            emit_instr(head, tail, "mov", reg(regno), reg(1), zero_reg(), NULL);
          }
          return;
        }
      }

      {
        char* arg_types[16] = {0};
        const char* target = NULL;
        for (int i = 0; i < argc; ++i)
          arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
        target = resolve_global_call_label(st->analysis, fname, arg_types, argc);
        emit_call_with_args(target, args_arr, argc, st, head, tail,
                            pending_label_ptr);
        if (regno != 1) {
          emit_instr(head, tail, "mov", reg(regno), reg(1), zero_reg(), NULL);
        }
        return;
      }
    }
  }

  if (tok[0] == '-' && !isdigit((unsigned char)tok[1])) {
    load_simple_to_reg(tok + 1, regno, st, head, tail, pending_label_ptr);
    // regno = -regno  => sub regno, #0, regno
    emit_instr(head, tail, "sub", reg(regno), zero_reg(), reg(regno), NULL);
    return;
  }

  // Число типа -123
  if (isdigit((unsigned char)tok[0]) ||
      (tok[0] == '-' && isdigit((unsigned char)tok[1]))) {
    emit_instr(head, tail, "mov", reg(regno), imm(atoi(tok)), zero_reg(), lbl);
    *pending_label_ptr = NULL;
    return;
  }

  // Литерал вида '0' или '\n'
  {
    int val = 0;
    if (parse_char_literal_value(tok, &val)) {
    emit_instr(head, tail, "mov", reg(regno), imm(val), zero_reg(), lbl);
    *pending_label_ptr = NULL;
    return;
    }
  }

  {
    char array_base[128];
    char array_index[128];
    if (split_array_suffix(tok, array_base, sizeof(array_base), array_index,
                           sizeof(array_index)) &&
        array_index[0]) {
      char _elem_type[128];
      int _stride;
      const char* _load_mn;
      emit_load_array_base_ptr(array_base, 10, st, head, tail, lbl);
      *pending_label_ptr = NULL;
      eval_arg_to_reg(array_index, 11, st, head, tail, pending_label_ptr);
      resolve_array_element_type(st, array_base, _elem_type, sizeof(_elem_type));
      _stride = elem_size_of_type(_elem_type);
      _load_mn = (_stride == 1) ? "movb" : "mov";
      emit_instr(head, tail, "mul", reg(11), reg(11), imm(_stride), NULL);
      emit_instr(head, tail, "add", reg(10), reg(10), reg(11), NULL);
      emit_instr(head, tail, _load_mn, reg(regno), mem_op(10, 0), zero_reg(),
                 NULL);
      return;
    }
  }

  Symbol* owner_sym = NULL;
  FieldInfo* field = resolve_field_access(st, tok, &owner_sym);
  if (field && owner_sym) {
    int base_reg = 6;
    emit_instr(head, tail, "mov", reg(base_reg), mem_op(FP, owner_sym->offset),
               zero_reg(), lbl);
    emit_instr(head, tail, "mov", reg(regno), mem_op(base_reg, field->offset),
               zero_reg(), NULL);
    *pending_label_ptr = NULL;
    return;
  }

  int off = get_var_offset(st, tok);
  emit_instr(head, tail, "mov", reg(regno), mem_op(FP, off), zero_reg(), lbl);
  *pending_label_ptr = NULL;
}

/* ---   --- */
static void emit_call_with_args(const char* func_name, char args[][64],
                                int argc, SymbolTable* st, Instr** head,
                                Instr** tail, char** pending_label) {
  for (int i = argc - 1; i >= 0; --i) {
    eval_arg_to_reg(args[i], 2, st, head, tail, pending_label);
    emit_instr(head, tail, "push", reg(2), zero_reg(), zero_reg(), NULL);
  }

  emit_instr(head, tail, "call", label_op(strdup(func_name)), zero_reg(),
             zero_reg(), *pending_label);
  *pending_label = NULL;

  if (argc > 0) {
    emit_instr(head, tail, "add", reg(SP), reg(SP), imm(argc * VAR_SIZE), NULL);
  }
}

static char* take_pending_label(char** pending_label) {
  if (pending_label && *pending_label) {
    char* lbl = *pending_label;
    *pending_label = NULL;
    return lbl;
  }
  return NULL;
}

static int resolve_array_element_type(SymbolTable* st, const char* array_expr,
                                      char* out_type, size_t out_type_sz) {
  const char* array_type = NULL;
  Symbol* owner = NULL;
  FieldInfo* field = NULL;
  if (!st || !array_expr || !out_type || out_type_sz == 0) return 0;
  out_type[0] = '\0';

  field = resolve_field_access(st, array_expr, &owner);
  if (field) {
    array_type = field->type_name;
  } else {
    Symbol* sym = find_symbol(st, array_expr);
    if (sym) array_type = sym->type_name;
  }
  if (!array_type) return 0;
  return type_without_array_dim(array_type, out_type, out_type_sz);
}

static int resolve_object_word_count(SymbolTable* st, const char* type_name) {
  UserTypeInfo* type = NULL;
  if (!st || !type_name) return 0;
  type = find_user_type(st->analysis, type_name);
  if (!type || type->instance_size <= 0) return 0;
  return (type->instance_size + VAR_SIZE - 1) / VAR_SIZE;
}

static void emit_heap_words_alloc_const(int word_count, int dst_reg,
                                        Instr** head, Instr** tail,
                                        char** pending_label_ptr) {
  char* heap_ready = strdupf("__obj_heap_ready_%d", g_heap_label_counter++);
  int byte_count = (word_count > 0 ? word_count : 1) * VAR_SIZE;

  emit_instr(head, tail, "mov", reg(2), imm(byte_count), zero_reg(),
             take_pending_label(pending_label_ptr));
  emit_instr(head, tail, "mov", reg(5), imm(BUILTIN_HEAP_PTR_ADDR), zero_reg(),
             NULL);
  emit_instr(head, tail, "mov", reg(6), mem_op(5, 0), zero_reg(), NULL);
  emit_instr(head, tail, "cmp", zero_reg(), reg(6), imm(0), NULL);
  emit_instr(head, tail, "bne", label_op(strdup(heap_ready)), zero_reg(),
             zero_reg(), NULL);
  emit_instr(head, tail, "mov", reg(6), imm(BUILTIN_HEAP_START_ADDR), zero_reg(),
             NULL);
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(6), zero_reg(), NULL);
  emit_instr(head, tail, "mov", reg(dst_reg), reg(6), zero_reg(), heap_ready);
  emit_instr(head, tail, "add", reg(6), reg(6), reg(2), NULL);
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(6), zero_reg(), NULL);
}

static void emit_copy_object_words(int dst_ptr_reg, int src_ptr_reg,
                                   int byte_size, Instr** head, Instr** tail) {
  for (int off = 0; off < byte_size; off += VAR_SIZE) {
    emit_instr(head, tail, "mov", reg(13), mem_op(src_ptr_reg, off), zero_reg(),
               NULL);
    emit_instr(head, tail, "mov", mem_op(dst_ptr_reg, off), reg(13), zero_reg(),
               NULL);
  }
}

static void emit_clone_object_to_heap(const char* type_name, int src_ptr_reg,
                                      int dst_ptr_reg, SymbolTable* st,
                                      Instr** head, Instr** tail,
                                      char** pending_label_ptr) {
  int word_count = resolve_object_word_count(st, type_name);
  int source_reg = src_ptr_reg;
  int scratch_reg = (dst_ptr_reg == 14) ? 15 : 14;
  if (word_count <= 0) {
    emit_instr(head, tail, "mov", reg(dst_ptr_reg), reg(src_ptr_reg), zero_reg(),
               take_pending_label(pending_label_ptr));
    return;
  }
  if (src_ptr_reg != scratch_reg) {
    emit_instr(head, tail, "mov", reg(scratch_reg), reg(src_ptr_reg), zero_reg(),
               take_pending_label(pending_label_ptr));
    source_reg = scratch_reg;
  }
  emit_heap_words_alloc_const(word_count, dst_ptr_reg, head, tail,
                              pending_label_ptr);
  emit_copy_object_words(dst_ptr_reg, source_reg, word_count * VAR_SIZE, head,
                         tail);
}

static void emit_copy_object_to_local(Symbol* sym, int src_ptr_reg,
                                      SymbolTable* st, Instr** head,
                                      Instr** tail,
                                      char** pending_label_ptr) {
  if (!sym || !st || !head || !tail) return;
  if (sym->storage_offset == 0 || sym->storage_size <= 0) {
    emit_instr(head, tail, "mov", mem_op(FP, sym->offset), reg(src_ptr_reg),
               zero_reg(), take_pending_label(pending_label_ptr));
    return;
  }

  emit_instr(head, tail, "add", reg(12), reg(FP), imm(sym->storage_offset),
             take_pending_label(pending_label_ptr));
  emit_instr(head, tail, "mov", mem_op(FP, sym->offset), reg(12), zero_reg(),
             NULL);
  emit_copy_object_words(12, src_ptr_reg, sym->storage_size, head, tail);
}

static void store_reg_to_lvalue(const char* lhs, int src_reg, SymbolTable* st,
                                Instr** head, Instr** tail,
                                char** pending_label_ptr) {
  if (!lhs || !st || !head || !tail) return;
  {
    char array_base[128];
    char array_index[128];
    if (split_array_suffix(lhs, array_base, sizeof(array_base), array_index,
                           sizeof(array_index)) &&
        array_index[0]) {
      char element_type[256];
      int _stride;
      const char* _store_mn;
      char* lbl = take_pending_label(pending_label_ptr);
      emit_load_array_base_ptr(array_base, 10, st, head, tail, lbl);
      eval_arg_to_reg(array_index, 11, st, head, tail, pending_label_ptr);
      resolve_array_element_type(st, array_base, element_type, sizeof(element_type));
      _stride = elem_size_of_type(element_type);
      _store_mn = (_stride == 1) ? "movb" : "mov";
      emit_instr(head, tail, "mul", reg(11), reg(11), imm(_stride), NULL);
      emit_instr(head, tail, "add", reg(10), reg(10), reg(11), NULL);
      if (is_object_type(st, element_type)) {
        emit_clone_object_to_heap(element_type, src_reg, 12, st, head, tail,
                                  pending_label_ptr);
        emit_instr(head, tail, "mov", mem_op(10, 0), reg(12), zero_reg(), NULL);
      } else {
        emit_instr(head, tail, _store_mn, mem_op(10, 0), reg(src_reg), zero_reg(),
                   NULL);
      }
      return;
    }
  }

  Symbol* owner_sym = NULL;
  FieldInfo* field = resolve_field_access(st, lhs, &owner_sym);
  if (field && owner_sym) {
    int base_reg = 15;
    char* lbl = take_pending_label(pending_label_ptr);
    emit_instr(head, tail, "mov", reg(base_reg), mem_op(FP, owner_sym->offset),
               zero_reg(), lbl);
    if (is_object_type(st, field->type_name) &&
        !type_is_array_name(field->type_name)) {
      emit_clone_object_to_heap(field->type_name, src_reg, 12, st, head, tail,
                                pending_label_ptr);
      emit_instr(head, tail, "mov", mem_op(base_reg, field->offset), reg(12),
                 zero_reg(), NULL);
    } else {
      emit_instr(head, tail, "mov", mem_op(base_reg, field->offset), reg(src_reg),
                 zero_reg(), NULL);
    }
    return;
  }
  {
    Symbol* sym = find_symbol(st, lhs);
    if (sym && sym->is_object && !sym->is_array) {
      emit_copy_object_to_local(sym, src_reg, st, head, tail, pending_label_ptr);
      return;
    }
  }
  {
    int off = get_var_offset(st, lhs);
    emit_instr(head, tail, "mov", mem_op(FP, off), reg(src_reg), zero_reg(),
               take_pending_label(pending_label_ptr));
  }
}

/* ---------- helpers for binary expressions ---------- */

static int op_precedence(const char* op) {
  if (!op) return 0;
  if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) return 1;
  if (strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0)
    return 2;
  return 0;
}

static const char* mnemonic_for_op(const char* op) {
  if (!op) return NULL;
  if (strcmp(op, "+") == 0) return "add";
  if (strcmp(op, "-") == 0) return "sub";
  if (strcmp(op, "*") == 0) return "mul";
  if (strcmp(op, "/") == 0) return "div";
  return NULL;
}

static int split_simple_binary_expr(const char* expr, char* lhs, size_t lhs_sz,
                                    char* op, size_t op_sz, char* rhs,
                                    size_t rhs_sz) {
  int round_depth = 0;
  int square_depth = 0;
  int angle_depth = 0;
  int in_char = 0;
  int in_string = 0;
  int split_at = -1;
  if (!expr || !lhs || !op || !rhs || lhs_sz == 0 || op_sz == 0 ||
      rhs_sz == 0) {
    return 0;
  }

  lhs[0] = '\0';
  op[0] = '\0';
  rhs[0] = '\0';

  for (int i = 0; expr[i]; ++i) {
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
    if (ch == '\'') {
      in_char = 1;
      continue;
    }
    if (ch == '"') {
      in_string = 1;
      continue;
    }

    if (ch == '(') ++round_depth;
    else if (ch == ')' && round_depth > 0) --round_depth;
    else if (ch == '[') ++square_depth;
    else if (ch == ']' && square_depth > 0) --square_depth;
    else if (ch == '<') ++angle_depth;
    else if (ch == '>' && angle_depth > 0) --angle_depth;

    if (round_depth != 0 || square_depth != 0 || angle_depth != 0) continue;
    if (ch != '+' && ch != '-' && ch != '*' && ch != '/' && ch != '%') continue;

    if (ch == '-' &&
        (i == 0 || prev == '+' || prev == '-' || prev == '*' || prev == '/' ||
         prev == '%' || prev == '(' || prev == '[' || prev == ',')) {
      continue;
    }

    split_at = i;
    break;
  }

  if (split_at <= 0 || expr[split_at + 1] == '\0') return 0;

  strncpy(lhs, expr, lhs_sz - 1);
  lhs[lhs_sz - 1] = '\0';
  lhs[split_at < (int)lhs_sz - 1 ? split_at : (int)lhs_sz - 1] = '\0';
  trim_whitespace(lhs);

  op[0] = expr[split_at];
  op[1] = '\0';

  strncpy(rhs, expr + split_at + 1, rhs_sz - 1);
  rhs[rhs_sz - 1] = '\0';
  trim_whitespace(rhs);

  return lhs[0] != '\0' && rhs[0] != '\0';
}

static void emit_binary_calc(const char* op, int dst_reg, int lhs_reg,
                             int rhs_reg, Instr** head, Instr** tail,
                             char** pending_label) {
  if (!op || !head || !tail) return;

  if (strcmp(op, "%") == 0) {
    char* lbl = take_pending_label(pending_label);
    emit_instr(head, tail, "div", reg(dst_reg), reg(lhs_reg), reg(rhs_reg),
               lbl);
    emit_instr(head, tail, "mul", reg(dst_reg), reg(dst_reg), reg(rhs_reg),
               NULL);
    emit_instr(head, tail, "sub", reg(dst_reg), reg(lhs_reg), reg(dst_reg),
               NULL);
    return;
  }

  const char* mnem = mnemonic_for_op(op);
  if (mnem) {
    char* lbl = take_pending_label(pending_label);
    emit_instr(head, tail, mnem, reg(dst_reg), reg(lhs_reg), reg(rhs_reg), lbl);
  }
}

static void eval_arg_to_reg(const char* tok, int dst_reg, SymbolTable* st,
                            Instr** head, Instr** tail,
                            char** pending_label_ptr) {
  char a[64], op[4], b[64];
  if (sscanf(tok, "%63s %3s %63s", a, op, b) == 3 ||
      split_simple_binary_expr(tok, a, sizeof(a), op, sizeof(op), b,
                               sizeof(b))) {
    load_simple_to_reg(a, 2, st, head, tail, pending_label_ptr);
    load_simple_to_reg(b, 3, st, head, tail, pending_label_ptr);
    if (!strcmp(op, "+"))
      emit_instr(head, tail, "add", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "-"))
      emit_instr(head, tail, "sub", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "*"))
      emit_instr(head, tail, "mul", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "/"))
      emit_instr(head, tail, "div", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "%")) {
      emit_instr(head, tail, "div", reg(4), reg(2), reg(3), NULL);
      emit_instr(head, tail, "mul", reg(4), reg(4), reg(3), NULL);
      emit_instr(head, tail, "sub", reg(4), reg(2), reg(4), NULL);
    }
    else if (!strcmp(op, "&"))
      emit_instr(head, tail, "band", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "|"))
      emit_instr(head, tail, "bor",  reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "^"))
      emit_instr(head, tail, "bxor", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, "<<"))
      emit_instr(head, tail, "bshl", reg(4), reg(2), reg(3), NULL);
    else if (!strcmp(op, ">>"))
      emit_instr(head, tail, "bshr", reg(4), reg(2), reg(3), NULL);
    emit_instr(head, tail, "mov", reg(dst_reg), reg(4), zero_reg(), NULL);
    return;
  }

  load_simple_to_reg(tok, dst_reg, st, head, tail, pending_label_ptr);
}

static int parse_call_arg_list(char* inside, char args[][64], int max_args) {
  int argc = 0;
  char* p = inside;
  if (!inside || max_args <= 0) return 0;

  while (*p && argc < max_args) {
    char* start = NULL;
    char* end = NULL;
    int round_depth = 0;
    int square_depth = 0;
    int angle_depth = 0;
    int in_char = 0;
    int in_string = 0;
    size_t n = 0;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;

    start = p;
    while (*p) {
      char ch = *p;
      char prev = (p > start) ? *(p - 1) : '\0';

      if (in_char) {
        if (ch == '\'' && prev != '\\') in_char = 0;
        ++p;
        continue;
      }
      if (in_string) {
        if (ch == '"' && prev != '\\') in_string = 0;
        ++p;
        continue;
      }

      if (ch == '\'') {
        in_char = 1;
        ++p;
        continue;
      }
      if (ch == '"') {
        in_string = 1;
        ++p;
        continue;
      }

      if (ch == '(') ++round_depth;
      else if (ch == ')' && round_depth > 0) --round_depth;
      else if (ch == '[') ++square_depth;
      else if (ch == ']' && square_depth > 0) --square_depth;
      else if (ch == '<') ++angle_depth;
      else if (ch == '>' && angle_depth > 0) --angle_depth;

      if (ch == ',' && round_depth == 0 && square_depth == 0 &&
          angle_depth == 0) {
        break;
      }
      ++p;
    }
    end = p;

    n = (size_t)(end - start);
    if (n >= 63) n = 63;
    memcpy(args[argc], start, n);
    args[argc][n] = '\0';
    trim_whitespace(args[argc]);
    argc++;

    if (*p == ',') {
      p++;
    } else {
      break;
    }
  }

  return argc;
}

static int split_call_text(const char* text, char* fname, size_t fname_size,
                           char args[][64], int max_args) {
  const char* lpar = NULL;
  const char* rpar = NULL;
  size_t fnlen = 0;
  char inside[256];
  size_t alen = 0;

  if (!text || !fname || fname_size == 0) return 0;

  lpar = strchr(text, '(');
  if (!lpar) return 0;
  {
    int depth = 0;
    int in_char = 0;
    int in_string = 0;
    const char* p = lpar;
    while (*p) {
      char ch = *p;
      char prev = (p > text) ? *(p - 1) : '\0';
      if (in_char) {
        if (ch == '\'' && prev != '\\') in_char = 0;
        ++p;
        continue;
      }
      if (in_string) {
        if (ch == '"' && prev != '\\') in_string = 0;
        ++p;
        continue;
      }
      if (ch == '\'') {
        in_char = 1;
        ++p;
        continue;
      }
      if (ch == '"') {
        in_string = 1;
        ++p;
        continue;
      }
      if (ch == '(') ++depth;
      else if (ch == ')') {
        --depth;
        if (depth == 0) {
          rpar = p;
          break;
        }
      }
      ++p;
    }
  }
  if (!rpar || rpar < lpar) return 0;
  {
    const char* p = rpar + 1;
    while (*p) {
      if (!isspace((unsigned char)*p)) return 0;
      ++p;
    }
  }

  fnlen = (size_t)(lpar - text);
  while (fnlen > 0 &&
         (text[fnlen - 1] == ' ' || text[fnlen - 1] == '\t')) {
    fnlen--;
  }
  if (fnlen == 0) return 0;
  if (fnlen >= fname_size) fnlen = fname_size - 1;
  memcpy(fname, text, fnlen);
  fname[fnlen] = '\0';
  trim_whitespace(fname);

  alen = (size_t)(rpar - (lpar + 1));
  if (alen >= sizeof(inside)) alen = sizeof(inside) - 1;
  memcpy(inside, lpar + 1, alen);
  inside[alen] = '\0';
  trim_whitespace(inside);

  if (inside[0] == '\0') return 1;
  return 1 + parse_call_arg_list(inside, args, max_args);
}

enum {
  BUILTIN_MMIO_TIME_ADDR = 1000000,
  BUILTIN_MMIO_TIMER_PERIOD_ADDR = 1000004,
  BUILTIN_MMIO_HANDLER_ADDR = 1000008,
  BUILTIN_MMIO_IRQ_ENABLE_ADDR = 1000012,
  BUILTIN_MMIO_IRQ_CAUSE_ADDR = 1000016,
  BUILTIN_MMIO_FRAME_SP_ADDR = 1000020,

  BUILTIN_SCHED_CURRENT_ADDR = 131072,
  BUILTIN_SCHED_FINISHED_ADDR = 131076,
  BUILTIN_SCHED_QUANTUM_ADDR = 131080,
  BUILTIN_SCHED_QUEUE_HEAD_ADDR = 131084,
  BUILTIN_SCHED_QUEUE_TAIL_ADDR = 131088,
  BUILTIN_SCHED_QUEUE_COUNT_ADDR = 131092,
  BUILTIN_SCHED_PENDING_ADDR = 131104,
  BUILTIN_SCHED_MODE_ADDR = 131108,
  BUILTIN_SAVED_HOST_FRAME_ADDR = 131112,
  BUILTIN_SCHED_START_TIME_ADDR = 131116,
  BUILTIN_SCHED_SYSCALL_CODE_ADDR = 131120,
  BUILTIN_SCHED_SYSCALL_ARG0_ADDR = 131124,
  BUILTIN_SCHED_SYSCALL_ARG1_ADDR = 131128,
  BUILTIN_SCHED_STEP_COUNT_ADDR = 131132,
  BUILTIN_TRACE_COUNT_ADDR = 131136,
  BUILTIN_RESULT_COUNT_ADDR = 131140,
  BUILTIN_BLOCKED_READ_COUNT_ADDR = 131144,
  BUILTIN_BLOCKED_WRITE_COUNT_ADDR = 131148,

  BUILTIN_TASK_ARRIVAL_BASE = 131200,
  BUILTIN_TASK_BURST_BASE = 131328,
  BUILTIN_TASK_REMAINING_BASE = 131456,
  BUILTIN_TASK_STATE_BASE = 131584,
  BUILTIN_TASK_COMPLETION_BASE = 131712,
  BUILTIN_TASK_FRAME_BASE = 131840,
  BUILTIN_READY_QUEUE_BASE = 132096,
  BUILTIN_TASK_RESULT_BASE = 132224,
  BUILTIN_TASK_WAIT_NEXT_BASE = 132352,
  BUILTIN_TASK_WAIT_VALUE_BASE = 132480,
  BUILTIN_STREAM_HEAD_BASE = 132608,
  BUILTIN_STREAM_TAIL_BASE = 132736,
  BUILTIN_STREAM_COUNT_BASE = 132864,
  BUILTIN_STREAM_CLOSED_BASE = 132992,
  BUILTIN_STREAM_READ_HEAD_BASE = 133120,
  BUILTIN_STREAM_READ_TAIL_BASE = 133248,
  BUILTIN_STREAM_WRITE_HEAD_BASE = 133376,
  BUILTIN_STREAM_WRITE_TAIL_BASE = 133504,
  BUILTIN_STREAM_BUFFER_ENTRY_BASE = 133632,
  BUILTIN_TRACE_ENTRY_BASE = 133888,
  BUILTIN_RESULT_ENTRY_BASE = 134144,

  BUILTIN_KERNEL_STACK_TOP = 240000,
  BUILTIN_TASK_STACK_BASE = 220000,
  BUILTIN_TASK_STACK_STRIDE = 256,
  BUILTIN_IDLE_STACK_TOP = 214000,
  BUILTIN_CONTEXT_FRAME_SIZE = 128
};

typedef struct {
  const char* getter_name;
  const char* setter_name;
  int address;
} BuiltinWordSlot;

typedef struct {
  const char* getter_name;
  const char* setter_name;
  int base_address;
} BuiltinIndexedWordSlot;

typedef struct {
  const char* getter_name;
  int value;
} BuiltinConstantSlot;

static const BuiltinWordSlot kBuiltinWordSlots[] = {
    {"schedCurrent", "setSchedCurrent", BUILTIN_SCHED_CURRENT_ADDR},
    {"schedFinished", "setSchedFinished", BUILTIN_SCHED_FINISHED_ADDR},
    {"schedQuantum", "setSchedQuantum", BUILTIN_SCHED_QUANTUM_ADDR},
    {"schedQueueHead", "setSchedQueueHead", BUILTIN_SCHED_QUEUE_HEAD_ADDR},
    {"schedQueueTail", "setSchedQueueTail", BUILTIN_SCHED_QUEUE_TAIL_ADDR},
    {"schedQueueCount", "setSchedQueueCount", BUILTIN_SCHED_QUEUE_COUNT_ADDR},
    {"schedPending", "setSchedPending", BUILTIN_SCHED_PENDING_ADDR},
    {"schedMode", "setSchedMode", BUILTIN_SCHED_MODE_ADDR},
    {"savedHostFrame", "setSavedHostFrame", BUILTIN_SAVED_HOST_FRAME_ADDR},
    {"schedStartTime", "setSchedStartTime", BUILTIN_SCHED_START_TIME_ADDR},
    {"schedSyscallCode", "setSchedSyscallCode",
     BUILTIN_SCHED_SYSCALL_CODE_ADDR},
    {"schedSyscallArg0", "setSchedSyscallArg0",
     BUILTIN_SCHED_SYSCALL_ARG0_ADDR},
    {"schedSyscallArg1", "setSchedSyscallArg1",
     BUILTIN_SCHED_SYSCALL_ARG1_ADDR},
    {"schedStepCount", "setSchedStepCount", BUILTIN_SCHED_STEP_COUNT_ADDR},
    {"traceCount", "setTraceCount", BUILTIN_TRACE_COUNT_ADDR},
    {"resultCount", "setResultCount", BUILTIN_RESULT_COUNT_ADDR},
    {"blockedReadCount", "setBlockedReadCount",
     BUILTIN_BLOCKED_READ_COUNT_ADDR},
    {"blockedWriteCount", "setBlockedWriteCount",
     BUILTIN_BLOCKED_WRITE_COUNT_ADDR},
    {"vmTime", NULL, BUILTIN_MMIO_TIME_ADDR},
    {"irqCause", NULL, BUILTIN_MMIO_IRQ_CAUSE_ADDR},
    {"irqFrameSp", NULL, BUILTIN_MMIO_FRAME_SP_ADDR},
    {NULL, NULL, 0}};

static const BuiltinIndexedWordSlot kBuiltinIndexedWordSlots[] = {
    {"taskArrival", "setTaskArrival", BUILTIN_TASK_ARRIVAL_BASE},
    {"taskBurst", "setTaskBurst", BUILTIN_TASK_BURST_BASE},
    {"taskRemaining", "setTaskRemaining", BUILTIN_TASK_REMAINING_BASE},
    {"taskState", "setTaskState", BUILTIN_TASK_STATE_BASE},
    {"taskCompletion", "setTaskCompletion", BUILTIN_TASK_COMPLETION_BASE},
    {"taskFrame", "setTaskFrame", BUILTIN_TASK_FRAME_BASE},
    {"queueEntry", "setQueueEntry", BUILTIN_READY_QUEUE_BASE},
    {"taskResult", "setTaskResult", BUILTIN_TASK_RESULT_BASE},
    {"taskWaitNext", "setTaskWaitNext", BUILTIN_TASK_WAIT_NEXT_BASE},
    {"taskWaitValue", "setTaskWaitValue", BUILTIN_TASK_WAIT_VALUE_BASE},
    {"streamHead", "setStreamHead", BUILTIN_STREAM_HEAD_BASE},
    {"streamTail", "setStreamTail", BUILTIN_STREAM_TAIL_BASE},
    {"streamCount", "setStreamCount", BUILTIN_STREAM_COUNT_BASE},
    {"streamClosed", "setStreamClosed", BUILTIN_STREAM_CLOSED_BASE},
    {"streamReadHead", "setStreamReadHead", BUILTIN_STREAM_READ_HEAD_BASE},
    {"streamReadTail", "setStreamReadTail", BUILTIN_STREAM_READ_TAIL_BASE},
    {"streamWriteHead", "setStreamWriteHead", BUILTIN_STREAM_WRITE_HEAD_BASE},
    {"streamWriteTail", "setStreamWriteTail", BUILTIN_STREAM_WRITE_TAIL_BASE},
    {"streamBufferEntry", "setStreamBufferEntry",
     BUILTIN_STREAM_BUFFER_ENTRY_BASE},
    {"traceEntry", "setTraceEntry", BUILTIN_TRACE_ENTRY_BASE},
    {"resultEntry", "setResultEntry", BUILTIN_RESULT_ENTRY_BASE},
    {NULL, NULL, 0}};

static const BuiltinConstantSlot kBuiltinConstantSlots[] = {
    {"kernelStackTop", BUILTIN_KERNEL_STACK_TOP},
    {"idleStackTop", BUILTIN_IDLE_STACK_TOP},
    {"contextFrameSize", BUILTIN_CONTEXT_FRAME_SIZE},
    {NULL, 0}};

static void emit_load_absolute_word(int dst_reg, int address, Instr** head,
                                    Instr** tail,
                                    char** pending_label_ptr) {
  emit_instr(head, tail, "mov", reg(5), imm(address), zero_reg(),
             take_pending_label(pending_label_ptr));
  emit_instr(head, tail, "mov", reg(dst_reg), mem_op(5, 0), zero_reg(), NULL);
}

static void emit_store_absolute_word(int address, const char* value_expr,
                                     SymbolTable* st, Instr** head,
                                     Instr** tail,
                                     char** pending_label_ptr) {
  eval_arg_to_reg(value_expr, 3, st, head, tail, pending_label_ptr);
  emit_instr(head, tail, "mov", reg(5), imm(address), zero_reg(),
             take_pending_label(pending_label_ptr));
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(3), zero_reg(), NULL);
}

static void emit_load_indexed_word(int dst_reg, const char* index_expr, int base,
                                   SymbolTable* st, Instr** head,
                                   Instr** tail,
                                   char** pending_label_ptr) {
  emit_instr(head, tail, "mov", reg(5), imm(base), zero_reg(),
             take_pending_label(pending_label_ptr));
  eval_arg_to_reg(index_expr, 2, st, head, tail, pending_label_ptr);
  emit_instr(head, tail, "mul", reg(2), reg(2), imm(VAR_SIZE), NULL);
  emit_instr(head, tail, "add", reg(5), reg(5), reg(2), NULL);
  emit_instr(head, tail, "mov", reg(dst_reg), mem_op(5, 0), zero_reg(), NULL);
}

static void emit_store_indexed_word(const char* index_expr, int base,
                                    const char* value_expr, SymbolTable* st,
                                    Instr** head, Instr** tail,
                                    char** pending_label_ptr) {
  emit_instr(head, tail, "mov", reg(5), imm(base), zero_reg(),
             take_pending_label(pending_label_ptr));
  eval_arg_to_reg(index_expr, 2, st, head, tail, pending_label_ptr);
  emit_instr(head, tail, "mul", reg(2), reg(2), imm(VAR_SIZE), NULL);
  emit_instr(head, tail, "add", reg(5), reg(5), reg(2), NULL);
  eval_arg_to_reg(value_expr, 3, st, head, tail, pending_label_ptr);
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(3), zero_reg(), NULL);
}

static int emit_builtin_constant_assignment_call(
    const char* lhs, const char* fname, SymbolTable* st, Instr** head,
    Instr** tail, char** pending_label_ptr) {
  for (int i = 0; kBuiltinConstantSlots[i].getter_name; ++i) {
    if (strcmp(fname, kBuiltinConstantSlots[i].getter_name) == 0) {
      emit_instr(head, tail, "mov", reg(1), imm(kBuiltinConstantSlots[i].value),
                 zero_reg(), take_pending_label(pending_label_ptr));
      store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
      return 1;
    }
  }
  return 0;
}

static int emit_builtin_scalar_assignment_call(
    const char* lhs, const char* fname, SymbolTable* st, Instr** head,
    Instr** tail, char** pending_label_ptr) {
  for (int i = 0; kBuiltinWordSlots[i].getter_name; ++i) {
    if (strcmp(fname, kBuiltinWordSlots[i].getter_name) == 0) {
      emit_load_absolute_word(1, kBuiltinWordSlots[i].address, head, tail,
                              pending_label_ptr);
      store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
      return 1;
    }
  }
  return 0;
}

static int emit_builtin_indexed_assignment_call(
    const char* lhs, const char* fname, const char* index_expr, SymbolTable* st,
    Instr** head, Instr** tail, char** pending_label_ptr) {
  for (int i = 0; kBuiltinIndexedWordSlots[i].getter_name; ++i) {
    if (strcmp(fname, kBuiltinIndexedWordSlots[i].getter_name) == 0) {
      emit_load_indexed_word(1, index_expr, kBuiltinIndexedWordSlots[i].base_address,
                             st, head, tail, pending_label_ptr);
      store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
      return 1;
    }
  }

  if (strcmp(fname, "taskStackTop") == 0) {
    emit_instr(head, tail, "mov", reg(5), imm(BUILTIN_TASK_STACK_BASE),
               zero_reg(), take_pending_label(pending_label_ptr));
    eval_arg_to_reg(index_expr, 2, st, head, tail, pending_label_ptr);
    emit_instr(head, tail, "mul", reg(2), reg(2),
               imm(BUILTIN_TASK_STACK_STRIDE), NULL);
    emit_instr(head, tail, "sub", reg(1), reg(5), reg(2), NULL);
    store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
    return 1;
  }

  return 0;
}

static int emit_builtin_scalar_statement_call(const char* fname,
                                              const char* value_expr,
                                              SymbolTable* st, Instr** head,
                                              Instr** tail,
                                              char** pending_label_ptr) {
  for (int i = 0; kBuiltinWordSlots[i].getter_name; ++i) {
    if (kBuiltinWordSlots[i].setter_name &&
        strcmp(fname, kBuiltinWordSlots[i].setter_name) == 0) {
      emit_store_absolute_word(kBuiltinWordSlots[i].address, value_expr, st,
                               head, tail, pending_label_ptr);
      return 1;
    }
  }

  if (strcmp(fname, "setTimerPeriod") == 0) {
    emit_store_absolute_word(BUILTIN_MMIO_TIMER_PERIOD_ADDR, value_expr, st, head,
                             tail, pending_label_ptr);
    return 1;
  }

  if (strcmp(fname, "setIrqHandler") == 0) {
    emit_store_absolute_word(BUILTIN_MMIO_HANDLER_ADDR, value_expr, st, head,
                             tail, pending_label_ptr);
    return 1;
  }

  if (strcmp(fname, "setIrqEnable") == 0) {
    emit_store_absolute_word(BUILTIN_MMIO_IRQ_ENABLE_ADDR, value_expr, st, head,
                             tail, pending_label_ptr);
    return 1;
  }

  return 0;
}

static int emit_builtin_indexed_statement_call(
    const char* fname, const char* index_expr, const char* value_expr,
    SymbolTable* st, Instr** head, Instr** tail, char** pending_label_ptr) {
  for (int i = 0; kBuiltinIndexedWordSlots[i].getter_name; ++i) {
    if (kBuiltinIndexedWordSlots[i].setter_name &&
        strcmp(fname, kBuiltinIndexedWordSlots[i].setter_name) == 0) {
      emit_store_indexed_word(index_expr, kBuiltinIndexedWordSlots[i].base_address,
                              value_expr, st, head, tail, pending_label_ptr);
      return 1;
    }
  }
  return 0;
}

static int emit_builtin_call_to_reg(const char* fname, char args[][64], int argc,
                                    int dst_reg, SymbolTable* st, Instr** head,
                                    Instr** tail,
                                    char** pending_label_ptr) {
  if (!fname || !st || !head || !tail || !pending_label_ptr) return 0;

  if (argc == 0) {
    for (int i = 0; kBuiltinConstantSlots[i].getter_name; ++i) {
      if (strcmp(fname, kBuiltinConstantSlots[i].getter_name) == 0) {
        emit_instr(head, tail, "mov", reg(dst_reg),
                   imm(kBuiltinConstantSlots[i].value), zero_reg(),
                   take_pending_label(pending_label_ptr));
        return 1;
      }
    }

    for (int i = 0; kBuiltinWordSlots[i].getter_name; ++i) {
      if (strcmp(fname, kBuiltinWordSlots[i].getter_name) == 0) {
        emit_load_absolute_word(dst_reg, kBuiltinWordSlots[i].address, head,
                                tail, pending_label_ptr);
        return 1;
      }
    }
  }

  if (argc == 1) {
    for (int i = 0; kBuiltinIndexedWordSlots[i].getter_name; ++i) {
      if (strcmp(fname, kBuiltinIndexedWordSlots[i].getter_name) == 0) {
        emit_load_indexed_word(dst_reg, args[0],
                               kBuiltinIndexedWordSlots[i].base_address, st,
                               head, tail, pending_label_ptr);
        return 1;
      }
    }

    if (strcmp(fname, "taskStackTop") == 0) {
      emit_instr(head, tail, "mov", reg(5), imm(BUILTIN_TASK_STACK_BASE),
                 zero_reg(), take_pending_label(pending_label_ptr));
      eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
      emit_instr(head, tail, "mul", reg(2), reg(2),
                 imm(BUILTIN_TASK_STACK_STRIDE), NULL);
      emit_instr(head, tail, "sub", reg(dst_reg), reg(5), reg(2), NULL);
      return 1;
    }

    if (strcmp(fname, "memLoad") == 0) {
      eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
      emit_instr(head, tail, "mov", reg(dst_reg), mem_op(2, 0), zero_reg(),
                 take_pending_label(pending_label_ptr));
      return 1;
    }

    if (strcmp(fname, "memLoadByte") == 0) {
      eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
      emit_instr(head, tail, "movb", reg(dst_reg), mem_op(2, 0), zero_reg(),
                 take_pending_label(pending_label_ptr));
      return 1;
    }

    if (strcmp(fname, "memLoadHalf") == 0) {
      eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
      emit_instr(head, tail, "movh", reg(dst_reg), mem_op(2, 0), zero_reg(),
                 take_pending_label(pending_label_ptr));
      return 1;
    }

    if (strcmp(fname, "funcAddr") == 0) {
      const char* target =
          resolve_global_call_label(st->analysis, args[0], NULL, 0);
      emit_instr(head, tail, "mov", reg(dst_reg), label_op(strdup(target)),
                 zero_reg(), take_pending_label(pending_label_ptr));
      return 1;
    }
  }

  if (strcmp(fname, "getSp") == 0 && argc == 0) {
    emit_instr(head, tail, "mov", reg(dst_reg), reg(SP), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "getFp") == 0 && argc == 0) {
    emit_instr(head, tail, "mov", reg(dst_reg), reg(FP), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  return 0;
}

static int emit_builtin_assignment_call(const char* lhs, const char* fname,
                                        char args[][64], int argc,
                                        SymbolTable* st, Instr** head,
                                        Instr** tail,
                                        char** pending_label_ptr) {
  if (!lhs || !fname || !st || !head || !tail || !pending_label_ptr) return 0;

  if (argc == 0) {
    if (emit_builtin_constant_assignment_call(lhs, fname, st, head, tail,
                                              pending_label_ptr)) {
      return 1;
    }
    if (emit_builtin_scalar_assignment_call(lhs, fname, st, head, tail,
                                            pending_label_ptr)) {
      return 1;
    }
  }

  if (argc == 1 &&
      emit_builtin_indexed_assignment_call(lhs, fname, args[0], st, head, tail,
                                           pending_label_ptr)) {
    return 1;
  }

  if (strcmp(fname, "memLoad") == 0 && argc == 1) {
    eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
    emit_instr(head, tail, "mov", reg(1), mem_op(2, 0), zero_reg(),
               take_pending_label(pending_label_ptr));
    store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
    return 1;
  }

  if (strcmp(fname, "funcAddr") == 0 && argc == 1) {
    const char* target =
        resolve_global_call_label(st->analysis, args[0], NULL, 0);
    emit_instr(head, tail, "mov", reg(1), label_op(strdup(target)), zero_reg(),
               take_pending_label(pending_label_ptr));
    store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
    return 1;
  }

  if (strcmp(fname, "getSp") == 0 && argc == 0) {
    emit_instr(head, tail, "mov", reg(1), reg(SP), zero_reg(),
               take_pending_label(pending_label_ptr));
    store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
    return 1;
  }

  if (strcmp(fname, "getFp") == 0 && argc == 0) {
    emit_instr(head, tail, "mov", reg(1), reg(FP), zero_reg(),
               take_pending_label(pending_label_ptr));
    store_reg_to_lvalue(lhs, 1, st, head, tail, pending_label_ptr);
    return 1;
  }

  return 0;
}

static int emit_builtin_statement_call(const char* fname, char args[][64],
                                       int argc, SymbolTable* st, Instr** head,
                                       Instr** tail,
                                       char** pending_label_ptr) {
  if (!fname || !st || !head || !tail || !pending_label_ptr) return 0;

  if (argc == 1 &&
      emit_builtin_scalar_statement_call(fname, args[0], st, head, tail,
                                         pending_label_ptr)) {
    return 1;
  }

  if (argc == 2 &&
      emit_builtin_indexed_statement_call(fname, args[0], args[1], st, head,
                                          tail, pending_label_ptr)) {
    return 1;
  }

  if (strcmp(fname, "memStore") == 0 && argc == 2) {
    eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
    eval_arg_to_reg(args[1], 3, st, head, tail, pending_label_ptr);
    emit_instr(head, tail, "mov", mem_op(2, 0), reg(3), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "setSp") == 0 && argc == 1) {
    eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
    emit_instr(head, tail, "mov", reg(SP), reg(2), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "setFp") == 0 && argc == 1) {
    eval_arg_to_reg(args[0], 2, st, head, tail, pending_label_ptr);
    emit_instr(head, tail, "mov", reg(FP), reg(2), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "vmTrap") == 0 && argc == 0) {
    emit_instr(head, tail, "trap", zero_reg(), zero_reg(), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "enterKernelStack") == 0 && argc == 0) {
    char* lbl = take_pending_label(pending_label_ptr);
    emit_instr(head, tail, "mov", reg(SP), imm(BUILTIN_KERNEL_STACK_TOP),
               zero_reg(), lbl);
    emit_instr(head, tail, "mov", reg(FP), imm(BUILTIN_KERNEL_STACK_TOP),
               zero_reg(), NULL);
    return 1;
  }

  if (strcmp(fname, "restorePendingSp") == 0 && argc == 0) {
    char* lbl = take_pending_label(pending_label_ptr);
    emit_instr(head, tail, "mov", reg(5), imm(BUILTIN_SCHED_PENDING_ADDR),
               zero_reg(), lbl);
    emit_instr(head, tail, "mov", reg(2), mem_op(5, 0), zero_reg(), NULL);
    emit_instr(head, tail, "mov", reg(SP), reg(2), zero_reg(), NULL);
    return 1;
  }

  if (strcmp(fname, "vmIret") == 0 && argc == 0) {
    emit_instr(head, tail, "iret", zero_reg(), zero_reg(), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  if (strcmp(fname, "vmHalt") == 0 && argc == 0) {
    emit_instr(head, tail, "halt", zero_reg(), zero_reg(), zero_reg(),
               take_pending_label(pending_label_ptr));
    return 1;
  }

  return 0;
}

static void emit_heap_array_alloc(const char* elem_count_expr, int dst_reg,
                                  int elem_bytes,
                                  SymbolTable* st, Instr** head, Instr** tail,
                                  char** pending_label_ptr,
                                  CfgBuilderState* state) {
  (void)state;
  char* heap_ready = strdupf("__heap_ready_%d", g_heap_label_counter++);
  const char* count_expr =
      (elem_count_expr && elem_count_expr[0]) ? elem_count_expr : "1";

  eval_arg_to_reg(count_expr, 2, st, head, tail, pending_label_ptr);
  emit_instr(head, tail, "mul", reg(2), reg(2), imm(elem_bytes), NULL);
  emit_instr(head, tail, "mov", reg(5), imm(BUILTIN_HEAP_PTR_ADDR), zero_reg(),
             take_pending_label(pending_label_ptr));
  emit_instr(head, tail, "mov", reg(6), mem_op(5, 0), zero_reg(), NULL);
  emit_instr(head, tail, "cmp", zero_reg(), reg(6), imm(0), NULL);
  emit_instr(head, tail, "bne", label_op(strdup(heap_ready)), zero_reg(),
             zero_reg(), NULL);
  emit_instr(head, tail, "mov", reg(6), imm(BUILTIN_HEAP_START_ADDR), zero_reg(),
             NULL);
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(6), zero_reg(), NULL);
  emit_instr(head, tail, "mov", reg(dst_reg), reg(6), zero_reg(), heap_ready);
  emit_instr(head, tail, "add", reg(6), reg(6), reg(2), NULL);
  emit_instr(head, tail, "mov", mem_op(5, 0), reg(6), zero_reg(), NULL);
}

static char* generate_single_statement(char* stmt, char* pending_label,
                                       Instr** head, Instr** tail,
                                       const char* exit_label, SymbolTable* st,
                                       CfgBuilderState* state,
                                       bool* did_return) {
  if (did_return) *did_return = false;

  if (strncmp(stmt, "decl ", 5) == 0) {
    char decl[512];
    char decl_type[256];
    char decl_name[256];
    Symbol* decl_sym = NULL;
    strncpy(decl, stmt + 5, sizeof(decl) - 1);
    decl[sizeof(decl) - 1] = '\0';
    char* eq = strchr(decl, '=');
    if (!eq) {
      trim_whitespace(decl);
      if (split_decl_type_and_name(decl, decl_type, sizeof(decl_type), decl_name,
                                   sizeof(decl_name))) {
        decl_sym =
            add_local_symbol(st, decl_name, decl_type[0] ? decl_type : "int");
        if (decl_sym && decl_sym->is_object && !decl_sym->is_array &&
            decl_sym->storage_offset != 0) {
          emit_instr(head, tail, "add", reg(2), reg(FP),
                     imm(decl_sym->storage_offset), pending_label);
          emit_instr(head, tail, "mov", mem_op(FP, decl_sym->offset), reg(2),
                     zero_reg(), NULL);
          pending_label = NULL;
        }
        char array_name[128];
        char array_len[128];
        if (split_array_suffix(decl_name, array_name, sizeof(array_name),
                               array_len, sizeof(array_len))) {
          if (!array_len[0]) {
            snprintf(array_len, sizeof(array_len), "%d", DEFAULT_ARRAY_ELEMS);
          }
          emit_heap_array_alloc(array_len, 1,
                                elem_size_of_type(decl_type[0] ? decl_type : "int"),
                                st, head, tail, &pending_label, state);
          store_reg_to_lvalue(array_name, 1, st, head, tail, &pending_label);
          return NULL;
        }
      }
      return pending_label;
    }

    *eq = '\0';
    char rhs[256];
    strncpy(rhs, eq + 1, sizeof(rhs) - 1);
    rhs[sizeof(rhs) - 1] = '\0';
    trim_whitespace(decl);
    trim_whitespace(rhs);

    char lhs[128];
    if (!split_decl_type_and_name(decl, decl_type, sizeof(decl_type), decl_name,
                                  sizeof(decl_name))) {
      return pending_label;
    }
    decl_sym = add_local_symbol(st, decl_name, decl_type[0] ? decl_type : "int");
    if (decl_sym && decl_sym->is_object && !decl_sym->is_array &&
        decl_sym->storage_offset != 0) {
      emit_instr(head, tail, "add", reg(2), reg(FP), imm(decl_sym->storage_offset),
                 pending_label);
      emit_instr(head, tail, "mov", mem_op(FP, decl_sym->offset), reg(2),
                 zero_reg(), NULL);
      pending_label = NULL;
    }

    {
      char array_name[128];
      char array_len[128];
      if (split_array_suffix(decl_name, array_name, sizeof(array_name), array_len,
                             sizeof(array_len))) {
        strncpy(lhs, array_name, sizeof(lhs) - 1);
        lhs[sizeof(lhs) - 1] = '\0';
      } else {
        strncpy(lhs, decl_name, sizeof(lhs) - 1);
        lhs[sizeof(lhs) - 1] = '\0';
      }
    }

    char assign_stmt[512];
    snprintf(assign_stmt, sizeof(assign_stmt), "%s = %s", lhs, rhs);
    return generate_single_statement(assign_stmt, pending_label, head, tail,
                                     exit_label, st, state, did_return);
  }

  // ---------- return ... ----------
  if (strncmp(stmt, "return", 6) == 0) {
    char expr[128] = "";
    char* p = stmt + 6;
    while (*p && isspace((unsigned char)*p)) p++;

    // просто "return;" без выражения
    if (*p == ';' || *p == '\0') {
      emit_instr(head, tail, "mov", reg(1), imm(0), zero_reg(), pending_label);
      if (exit_label)
        emit_instr(head, tail, "jmp", label_op(strdup(exit_label)), zero_reg(),
                   zero_reg(), NULL);
      if (did_return) *did_return = true;
      return NULL;
    }

    if (sscanf(p, "%127[^;]", expr) == 1) {
      trim_whitespace(expr);
      char* lbl = pending_label;
      const char* expr_type = infer_simple_type(st, expr);

      {
        int val = 0;
        if (parse_int_literal_strict(expr, &val)) {
          emit_instr(head, tail, "mov", reg(1), imm(val), zero_reg(), lbl);
          if (exit_label)
            emit_instr(head, tail, "jmp", label_op(strdup(exit_label)),
                       zero_reg(), zero_reg(), NULL);
          if (did_return) *did_return = true;
          return NULL;
        }
      }

      // 2) бинарное выражение: a op b  (наш случай c - '0')
      char a[64], op[4], b[64];
      if (sscanf(expr, "%63s %3s %63s", a, op, b) == 3) {
        // загрузить a → r2
        load_simple_to_reg(a, 2, st, head, tail, &lbl);
        // загрузить b → r3
        load_simple_to_reg(b, 3, st, head, tail, &lbl);

        const char* mnem = NULL;
        if (!strcmp(op, "+"))
          mnem = "add";
        else if (!strcmp(op, "-"))
          mnem = "sub";
        else if (!strcmp(op, "*"))
          mnem = "mul";
        else if (!strcmp(op, "/"))
          mnem = "div";
        else {
          // на всякий случай fallback — пока просто вернём первый операнд
          mnem = NULL;
        }

        if (mnem) {
          emit_instr(head, tail, mnem, reg(1), reg(2), reg(3), lbl);
        } else {
          // fallback: просто вернуть a (редкий случай)
          emit_instr(head, tail, "mov", reg(1), reg(2), zero_reg(), lbl);
        }
        if (exit_label)
          emit_instr(head, tail, "jmp", label_op(strdup(exit_label)),
                     zero_reg(), zero_reg(), NULL);
        if (did_return) *did_return = true;
        return NULL;
      }

      // 3) просто идентификатор: return num;
      load_simple_to_reg(expr, 1, st, head, tail, &lbl);
      if (expr_type && is_object_type(st, expr_type) &&
          !type_is_array_name(expr_type)) {
        emit_clone_object_to_heap(expr_type, 1, 1, st, head, tail, &lbl);
      }
      if (exit_label)
        emit_instr(head, tail, "jmp", label_op(strdup(exit_label)), zero_reg(),
                   zero_reg(), NULL);
      if (did_return) *did_return = true;
      return NULL;
    }

    // если ничего не распознали — дефолт: return 0;
    emit_instr(head, tail, "mov", reg(1), imm(0), zero_reg(), pending_label);
    if (exit_label)
      emit_instr(head, tail, "jmp", label_op(strdup(exit_label)), zero_reg(),
                 zero_reg(), NULL);
    if (did_return) *did_return = true;
    return NULL;
  }
  
  // ------- x = f(...) --------
  {
    char lhs[64];

    // ищем "lhs = something"
    if (sscanf(stmt, "%63s =", lhs) == 1) {
      char* eq = strchr(stmt, '=');
      if (eq) {
        char* rhs = eq + 1;
        char fname[64];
        char args_arr[16][64];
        int call_info = 0;
        int argc = 0;
        while (*rhs == ' ' || *rhs == '\t') rhs++;

        call_info = split_call_text(rhs, fname, sizeof(fname), args_arr, 16);
        if (call_info > 0) {
          argc = call_info - 1;

          if (emit_builtin_assignment_call(lhs, fname, args_arr, argc, st, head,
                                           tail, &pending_label)) {
            return NULL;
          }

          {
            char owner_name[64];
            char method_name[64];
            if (split_member_ref(fname, owner_name, sizeof(owner_name),
                                 method_name, sizeof(method_name))) {
              Symbol* owner_sym = resolve_owner_symbol(st, owner_name);
              if (owner_sym && owner_sym->type_name) {
                char* arg_types[16] = {0};
                for (int i = 0; i < argc; ++i)
                  arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
                MethodInfo* method =
                    find_method_in_type(st->analysis, owner_sym->type_name,
                                        method_name, arg_types, argc);
                if (method) {
                  char call_args[17][64];
                  snprintf(call_args[0], sizeof(call_args[0]), "%s", owner_name);
                  for (int i = 0; i < argc; ++i) {
                    snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                             args_arr[i]);
                  }
                  emit_call_with_args(method->mangled_name, call_args, argc + 1,
                                      st, head, tail, &pending_label);
                  store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
                  return NULL;
                }
              }
            } else if (st->function && st->function->owner_type) {
              char* arg_types[16] = {0};
              for (int i = 0; i < argc; ++i)
                arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
              MethodInfo* method =
                  find_method_in_type(st->analysis, st->function->owner_type,
                                      fname, arg_types, argc);
              if (method) {
                char call_args[17][64];
                snprintf(call_args[0], sizeof(call_args[0]), "%s", "this");
                for (int i = 0; i < argc; ++i) {
                  snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                           args_arr[i]);
                }
                emit_call_with_args(method->mangled_name, call_args, argc + 1,
                                    st, head, tail, &pending_label);
                store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
                return NULL;
              }
            }
          }

          {
            char* arg_types[16] = {0};
            const char* target = NULL;
            for (int i = 0; i < argc; ++i)
              arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
            target = resolve_global_call_label(st->analysis, fname, arg_types,
                                               argc);
            emit_call_with_args(target, args_arr, argc, st, head, tail,
                                &pending_label);
          }

          store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
          return NULL;
        }

        // rhs должно выглядеть как fname(...)
        char* lpar = strchr(rhs, '(');
        char* rpar = lpar ? strchr(lpar, ')') : NULL;

        if (lpar && rpar && rpar >= lpar) {
          // имя функции
          char fname[64];
          size_t fnlen = (size_t)(lpar - rhs);
          while (fnlen > 0 && (rhs[fnlen - 1] == ' ' || rhs[fnlen - 1] == '\t'))
            fnlen--;
          if (fnlen >= sizeof(fname)) fnlen = sizeof(fname) - 1;
          memcpy(fname, rhs, fnlen);
          fname[fnlen] = '\0';
          trim_whitespace(fname);

          // аргументы (могут быть пустыми!)
          char inside[256];
          size_t alen = (size_t)(rpar - (lpar + 1));
          if (alen >= sizeof(inside)) alen = sizeof(inside) - 1;
          memcpy(inside, lpar + 1, alen);
          inside[alen] = '\0';
          trim_whitespace(inside);

          // распарсить аргументы, если они есть
          char args_arr[16][64];
          int argc = 0;

          if (inside[0] != '\0') {
            char* p = inside;
            while (*p && argc < 16) {
              while (*p == ' ' || *p == '\t') p++;
              if (*p == '\0') break;

              char* comma = strchr(p, ',');
              if (!comma) comma = p + strlen(p);

              size_t n = (size_t)(comma - p);
              if (n >= sizeof(args_arr[0])) n = sizeof(args_arr[0]) - 1;
              memcpy(args_arr[argc], p, n);
              args_arr[argc][n] = '\0';
              trim_whitespace(args_arr[argc]);
              argc++;

              if (*comma == ',')
                p = comma + 1;
              else
                break;
            }
          }

          if (emit_builtin_assignment_call(lhs, fname, args_arr, argc, st, head,
                                           tail, &pending_label)) {
            return NULL;
          }

          char owner_name[64];
          char method_name[64];
          if (split_member_ref(fname, owner_name, sizeof(owner_name), method_name,
                               sizeof(method_name))) {
            Symbol* owner_sym = resolve_owner_symbol(st, owner_name);
            if (owner_sym && owner_sym->type_name) {
              char* arg_types[16] = {0};
              for (int i = 0; i < argc; ++i)
                arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
              MethodInfo* method =
                  find_method_in_type(st->analysis, owner_sym->type_name,
                                      method_name, arg_types, argc);
              if (method) {
                char call_args[17][64];
                snprintf(call_args[0], sizeof(call_args[0]), "%s", owner_name);
                for (int i = 0; i < argc; ++i) {
                  snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                           args_arr[i]);
                }
                emit_call_with_args(method->mangled_name, call_args, argc + 1,
                                    st, head, tail, &pending_label);
                store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
                return NULL;
              }
            }
          } else if (st->function && st->function->owner_type) {
            char* arg_types[16] = {0};
            for (int i = 0; i < argc; ++i)
              arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
            MethodInfo* method =
                find_method_in_type(st->analysis, st->function->owner_type,
                                    fname, arg_types, argc);
            if (method) {
              char call_args[17][64];
              snprintf(call_args[0], sizeof(call_args[0]), "%s", "this");
              for (int i = 0; i < argc; ++i) {
                snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                         args_arr[i]);
              }
              emit_call_with_args(method->mangled_name, call_args, argc + 1, st,
                                  head, tail, &pending_label);
              store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
              return NULL;
            }
          }

          {
            char* arg_types[16] = {0};
            const char* target = NULL;
            for (int i = 0; i < argc; ++i)
              arg_types[i] = (char*)infer_simple_type(st, args_arr[i]);
            target = resolve_global_call_label(st->analysis, fname, arg_types,
                                               argc);
            emit_call_with_args(target, args_arr, argc, st, head, tail,
                                &pending_label);
          }

          // save return value r1 -> lhs
          store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
          return NULL;
        }
      }
    }
  }


  // -----------------------
  // f(...)
  // -----------------------
  {
    char fname[64];
    char parsed_args[32][64];
    int call_info = split_call_text(stmt, fname, sizeof(fname), parsed_args, 32);
    if (call_info > 0) {
      char* arg_tokens[32];
      int arg_count = call_info - 1;

      for (int i = 0; i < arg_count; ++i) {
        arg_tokens[i] = strdup(parsed_args[i]);
      }

      {
        char builtin_args[32][64];
        for (int i = 0; i < arg_count; ++i) {
          snprintf(builtin_args[i], sizeof(builtin_args[i]), "%s",
                   arg_tokens[i]);
        }
        if (emit_builtin_statement_call(fname, builtin_args, arg_count, st,
                                        head, tail, &pending_label)) {
          for (int i = 0; i < arg_count; ++i) free(arg_tokens[i]);
          return NULL;
        }
      }

      char owner_name[64];
      char method_name[64];
      if (split_member_ref(fname, owner_name, sizeof(owner_name), method_name,
                           sizeof(method_name))) {
        Symbol* owner_sym = resolve_owner_symbol(st, owner_name);
        if (owner_sym && owner_sym->type_name) {
          char* arg_types[32] = {0};
          for (int i = 0; i < arg_count; ++i)
            arg_types[i] = (char*)infer_simple_type(st, arg_tokens[i]);
          MethodInfo* method =
              find_method_in_type(st->analysis, owner_sym->type_name,
                                  method_name, arg_types, arg_count);
          if (method) {
            char call_args[33][64];
            snprintf(call_args[0], sizeof(call_args[0]), "%s", owner_name);
            for (int i = 0; i < arg_count; ++i) {
              snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                       arg_tokens[i]);
              free(arg_tokens[i]);
            }
            emit_call_with_args(method->mangled_name, call_args, arg_count + 1,
                                st, head, tail, &pending_label);
            return NULL;
          }
        }
      } else if (st->function && st->function->owner_type) {
        char* arg_types[32] = {0};
        for (int i = 0; i < arg_count; ++i)
          arg_types[i] = (char*)infer_simple_type(st, arg_tokens[i]);
        MethodInfo* method =
            find_method_in_type(st->analysis, st->function->owner_type, fname,
                                arg_types, arg_count);
        if (method) {
          char call_args[33][64];
          snprintf(call_args[0], sizeof(call_args[0]), "%s", "this");
          for (int i = 0; i < arg_count; ++i) {
            snprintf(call_args[i + 1], sizeof(call_args[i + 1]), "%s",
                     arg_tokens[i]);
            free(arg_tokens[i]);
          }
          emit_call_with_args(method->mangled_name, call_args, arg_count + 1,
                              st, head, tail, &pending_label);
          return NULL;
        }
      }

      for (int idx = arg_count - 1; idx >= 0; --idx) {
        eval_arg_to_reg(arg_tokens[idx], 7 + idx, st, head, tail,
                        &pending_label);
        emit_instr(head, tail, "push", reg(7 + idx), zero_reg(), zero_reg(),
                   NULL);
        free(arg_tokens[idx]);
      }

      {
        char* arg_types[32] = {0};
        const char* target = NULL;
        for (int i = 0; i < arg_count; ++i)
          arg_types[i] = (char*)infer_simple_type(st, parsed_args[i]);
        target =
            resolve_global_call_label(st->analysis, fname, arg_types, arg_count);
        emit_instr(head, tail, "call", label_op(strdup(target)), zero_reg(),
                   zero_reg(), pending_label);
      }

      if (arg_count > 0)
        emit_instr(head, tail, "add", reg(SP), reg(SP), imm(arg_count * 4),
                   NULL);
      return NULL;
    }
  }

  // -----------------------
  // x = number
  // -----------------------
  {
    char lhs[64];
    char rhs[128];
    int num;
    if (sscanf(stmt, "%63s = %127[^\n]", lhs, rhs) == 2) {
      trim_whitespace(rhs);
      if (parse_int_literal_strict(rhs, &num)) {
        emit_instr(head, tail, "mov", reg(1), imm(num), zero_reg(),
                   pending_label);
        pending_label = NULL;
        store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
        return NULL;
      }
    }
  }

  // -----------------------
  // x = y  (простое присваивание/унарный минус)
  // -----------------------
  {
    char lhs[64], rhs[64], extra[64];
    if (sscanf(stmt, "%63s = %63s %63s", lhs, rhs, extra) != 3 &&
        sscanf(stmt, "%63s = %63s", lhs, rhs) == 2) {
      /* попытка: вызов функции без аргументов, если скобки потеряны */
      size_t rlen = strlen(rhs);
      if (rlen >= 2 && rhs[rlen - 2] == '(' && rhs[rlen - 1] == ')') {
        rhs[rlen - 2] = '\0';  // убрали "()"
        {
          const char* target =
              resolve_global_call_label(st->analysis, rhs, NULL, 0);
          emit_instr(head, tail, "call", label_op(strdup(target)), zero_reg(),
                     zero_reg(), pending_label);
        }
        pending_label = NULL;
        store_reg_to_lvalue(lhs, 1, st, head, tail, &pending_label);
        return NULL;
      }


      if (rhs[0] == '-' && isalpha((unsigned char)rhs[1])) {
        int off_rhs = get_var_offset(st, rhs + 1);
        emit_instr(head, tail, "mov", reg(2), mem_op(FP, off_rhs), zero_reg(),
                   pending_label);
        pending_label = NULL;
        emit_instr(head, tail, "sub", reg(2), zero_reg(), reg(2), NULL);
        store_reg_to_lvalue(lhs, 2, st, head, tail, &pending_label);
      } else {
        load_simple_to_reg(rhs, 2, st, head, tail, &pending_label);
        store_reg_to_lvalue(lhs, 2, st, head, tail, &pending_label);
      }
      return NULL;
    }
  }

  // -----------------------
  // x = a op1 b op2 c  ( )
  // -----------------------
  {
    char lhs[64], a[64], op1[4], b[64], op2[4], c[64];
    if (sscanf(stmt, "%63s = %63s %3s %63s %3s %63s", lhs, a, op1, b, op2, c) ==
        6) {
      int p1 = op_precedence(op1);
      int p2 = op_precedence(op2);
      if (p1 > 0 && p2 > 0) {
        if (p1 >= p2) {
          load_simple_to_reg(a, 2, st, head, tail, &pending_label);
          load_simple_to_reg(b, 3, st, head, tail, &pending_label);
          emit_binary_calc(op1, 4, 2, 3, head, tail, &pending_label);

          load_simple_to_reg(c, 3, st, head, tail, &pending_label);
          emit_binary_calc(op2, 4, 4, 3, head, tail, &pending_label);
        } else {
          load_simple_to_reg(b, 2, st, head, tail, &pending_label);
          load_simple_to_reg(c, 3, st, head, tail, &pending_label);
          emit_binary_calc(op2, 4, 2, 3, head, tail, &pending_label);

          load_simple_to_reg(a, 2, st, head, tail, &pending_label);
          emit_binary_calc(op1, 4, 2, 4, head, tail, &pending_label);
        }

        store_reg_to_lvalue(lhs, 4, st, head, tail, &pending_label);
        return NULL;
      }
    }
  }

  // -----------------------
  // x = y op z
  // -----------------------
  {
    char lhs[64], a[64], op[4], b[64];
    if (sscanf(stmt, "%63s = %63s %3s %63s", lhs, a, op, b) == 4) {
      if (!strcmp(op, "%")) {
        // эмулируем mod через div/mul/sub
        char tmp_name[64];
        int counter = state ? state->tmp_mod_counter++ : 0;
        snprintf(tmp_name, sizeof(tmp_name), "__tmp_mod_%d", counter);
        int off_tmp = get_var_offset(st, tmp_name);

        load_simple_to_reg(a, 2, st, head, tail, &pending_label);
        load_simple_to_reg(b, 3, st, head, tail, &pending_label);
        emit_instr(head, tail, "div", reg(4), reg(2), reg(3), NULL);
        emit_instr(head, tail, "mov", mem_op(FP, off_tmp), reg(4), zero_reg(),
                   NULL);

        emit_instr(head, tail, "mov", reg(2), mem_op(FP, off_tmp), zero_reg(),
                   NULL);
        load_simple_to_reg(b, 3, st, head, tail, &pending_label);
        emit_instr(head, tail, "mul", reg(4), reg(2), reg(3), NULL);
        emit_instr(head, tail, "mov", mem_op(FP, off_tmp), reg(4), zero_reg(),
                   NULL);

        load_simple_to_reg(a, 2, st, head, tail, &pending_label);
        emit_instr(head, tail, "mov", reg(3), mem_op(FP, off_tmp), zero_reg(),
                   NULL);
        emit_instr(head, tail, "sub", reg(4), reg(2), reg(3), NULL);
        store_reg_to_lvalue(lhs, 4, st, head, tail, &pending_label);
        return NULL;
      }

      load_simple_to_reg(a, 2, st, head, tail, &pending_label);
      load_simple_to_reg(b, 3, st, head, tail, &pending_label);

      if (!strcmp(op, "+"))
        emit_instr(head, tail, "add", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "-"))
        emit_instr(head, tail, "sub", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "*"))
        emit_instr(head, tail, "mul", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "/"))
        emit_instr(head, tail, "div", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "&"))
        emit_instr(head, tail, "band", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "|"))
        emit_instr(head, tail, "bor",  reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "^"))
        emit_instr(head, tail, "bxor", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, "<<"))
        emit_instr(head, tail, "bshl", reg(4), reg(2), reg(3), NULL);
      else if (!strcmp(op, ">>"))
        emit_instr(head, tail, "bshr", reg(4), reg(2), reg(3), NULL);

      store_reg_to_lvalue(lhs, 4, st, head, tail, &pending_label);

      return NULL;
    }
  }

  return pending_label;
}

/* ---------- Генерация кода для одного CFG-блока ---------- */
static char* func_label(const char* func, int id) {
  return strdupf("%s_L%d", func, id);
}

static void gen_block(CFGNode* node, FunctionCFG* cfg, Instr** head,
                      Instr** tail, char* block_label, const char* exit_label,
                      SymbolTable* st, CfgBuilderState* state,
                      int source_file_index) {

  // START / FINISH игнорируем
  // START не печатаем как блок,
  // FINISH печатаем метку, но без операций
  if (node->label && strcmp(node->label, "START") == 0) {
    if (block_label) free(block_label);
    return;
  }

  if (node->label && strcmp(node->label, "FINISH") == 0) {
    //   FINISH
    emit_instr(head, tail, "nop", zero_reg(), zero_reg(), zero_reg(),
               block_label ? block_label : strdup("FINISH"));

    // :
    if (exit_label) {
      emit_instr(head, tail, "jmp", label_op(strdup(exit_label)), zero_reg(),
                 zero_reg(), NULL);
    }
    return;
  }

  char* pending_label = block_label;
  bool block_has_return = false;

  // ======================================================
  // 1. Разбиваем op->text по ';' и генерируем TAC по строкам
  // ======================================================
  for (int o = 0; o < node->ops_count; ++o) {
    Operation* op = node->ops[o];
    if (!op || !op->text) continue;

    char* full = strdup(op->text);
    char* line = strtok(full, ";");

    while (line && !block_has_return) {
      char stmt[512];
      strncpy(stmt, line, sizeof(stmt) - 1);
      stmt[sizeof(stmt) - 1] = 0;

      trim_whitespace(stmt);
      if (stmt[0]) {
        int src_line = op->line > 0 ? op->line : node->line;
        int src_col = op->col > 0 ? op->col : node->col;
        set_current_source_location(source_file_index, src_line, src_col);
        // ---------- break ----------
        if (strncmp(stmt, "break", 5) == 0) {
          if (node->next_target) {
            char* lab = func_label(cfg->func_name, node->next_target->id);
            emit_instr(head, tail, "jmp", label_op(strdup(lab)), zero_reg(),
                       zero_reg(), pending_label);
            pending_label = NULL;
          }
          clear_current_source_location();
          break;
        }
        // ===== FIX continue =====
        if (strncmp(stmt, "continue", 8) == 0) {
          if (node->next_target) {
            char* lab = func_label(cfg->func_name, node->next_target->id);
            emit_instr(head, tail, "jmp", label_op(strdup(lab)), zero_reg(),
                       zero_reg(), pending_label);
            pending_label = NULL;
          }
          // continue   :  stmts
          clear_current_source_location();
          break;
        }
        // ========================

        pending_label = generate_single_statement(
            stmt, pending_label, head, tail, exit_label, st, state,
            &block_has_return);
        clear_current_source_location();
      }

      line = strtok(NULL, ";");
    }

    free(full);
    if (block_has_return) break;
  }

  if (block_has_return) return;

  // ======================================================
  // 2. Если это IF/WHILE узел с true/false
  // ======================================================
  if (node->true_target && node->false_target && node->label) {
    set_current_source_location(source_file_index, node->line, node->col);
    char lhs[64], rhs[64], op[4];
    char assign_lhs[64], assign_op[4], assign_rhs[128], cmp_op[4], cmp_rhs[64];

    /* (var = expr) cmp rhs */
    if (sscanf(node->label, "%63s %3s %127s %3s %63s", assign_lhs, assign_op,
               assign_rhs, cmp_op, cmp_rhs) == 5 &&
        strcmp(assign_op, "=") == 0) {
      int is_cmp = (!strcmp(cmp_op, "<")) || (!strcmp(cmp_op, "<=")) ||
                   (!strcmp(cmp_op, ">")) || (!strcmp(cmp_op, ">=")) ||
                   (!strcmp(cmp_op, "==")) || (!strcmp(cmp_op, "!="));
      if (is_cmp) {
        int off_assign = get_var_offset(st, assign_lhs);
        char fname[64];
        char parsed_args[32][64];
        int call_info =
            split_call_text(assign_rhs, fname, sizeof(fname), parsed_args, 32);

        if (call_info > 0) {
          int arg_count = call_info - 1;
          char* arg_types[32] = {0};
          const char* target = NULL;
          for (int i = 0; i < arg_count; ++i)
            arg_types[i] = (char*)infer_simple_type(st, parsed_args[i]);
          target = resolve_global_call_label(st->analysis, fname, arg_types,
                                             arg_count);

          for (int idx = arg_count - 1; idx >= 0; --idx) {
            eval_arg_to_reg(parsed_args[idx], 7 + idx, st, head, tail,
                            &pending_label);
            emit_instr(head, tail, "push", reg(7 + idx), zero_reg(), zero_reg(),
                       NULL);
          }

          emit_instr(head, tail, "call", label_op(strdup(target)), zero_reg(),
                     zero_reg(), pending_label);
          pending_label = NULL;

          if (arg_count > 0)
            emit_instr(head, tail, "add", reg(SP), reg(SP), imm(arg_count * 4),
                       NULL);

          emit_instr(head, tail, "mov", reg(2), reg(1), zero_reg(), NULL);
        } else {
          load_simple_to_reg(assign_rhs, 2, st, head, tail, &pending_label);
        }

        emit_instr(head, tail, "mov", mem_op(FP, off_assign), reg(2),
                   zero_reg(), NULL);
        load_simple_to_reg(cmp_rhs, 3, st, head, tail, &pending_label);

        emit_instr(head, tail, "cmp", zero_reg(), reg(2), reg(3), NULL);

        char* Ltrue = func_label(cfg->func_name, node->true_target->id);
        char* Lfalse = func_label(cfg->func_name, node->false_target->id);


        if (strcmp(cmp_op, "<") == 0)
          emit_instr(head, tail, "blt", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);
        else if (strcmp(cmp_op, "<=") == 0)
          emit_instr(head, tail, "ble", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);
        else if (strcmp(cmp_op, ">") == 0)
          emit_instr(head, tail, "bgt", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);
        else if (strcmp(cmp_op, ">=") == 0)
          emit_instr(head, tail, "bge", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);
        else if (strcmp(cmp_op, "==") == 0)
          emit_instr(head, tail, "beq", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);
        else if (strcmp(cmp_op, "!=") == 0)
          emit_instr(head, tail, "bne", label_op(Ltrue), zero_reg(), zero_reg(),
                     NULL);

        emit_instr(head, tail, "jmp", label_op(Lfalse), zero_reg(), zero_reg(),
                   NULL);
        clear_current_source_location();
        return;
      }
    }

    /* constant condition: while(1), while(0) */
    {
      char cond_buf[MAX_SMALL_BUFFER_LEN];
      strncpy(cond_buf, node->label, sizeof(cond_buf) - 1);
      cond_buf[sizeof(cond_buf) - 1] = '\0';
      trim_whitespace(cond_buf);

      if (cond_buf[0]) {
        char* endp = NULL;
        long cval = strtol(cond_buf, &endp, 0);
        if (endp && *endp == '\0') {
          char* Ltrue = func_label(cfg->func_name, node->true_target->id);
          char* Lfalse = func_label(cfg->func_name, node->false_target->id);

          char* lbl = take_pending_label(&pending_label);

          if (cval)
            emit_instr(head, tail, "jmp", label_op(Ltrue), zero_reg(),
                       zero_reg(), lbl);
          else
            emit_instr(head, tail, "jmp", label_op(Lfalse), zero_reg(),
                       zero_reg(), lbl);
          clear_current_source_location();
          return;
        }
      }
    }

    /* truthy condition: if(flag), while(running) */
    {
      char cond_buf[MAX_SMALL_BUFFER_LEN];
      char simple[MAX_SMALL_BUFFER_LEN];
      strncpy(cond_buf, node->label, sizeof(cond_buf) - 1);
      cond_buf[sizeof(cond_buf) - 1] = '\0';
      trim_whitespace(cond_buf);
      if (cond_buf[0] &&
          sscanf(cond_buf, "%127s", simple) == 1 &&
          strcmp(cond_buf, simple) == 0) {
        load_simple_to_reg(simple, 2, st, head, tail, &pending_label);
        emit_instr(head, tail, "cmp", zero_reg(), reg(2), imm(0), NULL);

        char* Ltrue = func_label(cfg->func_name, node->true_target->id);
        char* Lfalse = func_label(cfg->func_name, node->false_target->id);

        emit_instr(head, tail, "bne", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
        emit_instr(head, tail, "jmp", label_op(Lfalse), zero_reg(), zero_reg(),
                   NULL);
        clear_current_source_location();
        return;
      }
    }

    /*    lhs op rhs */
    if (sscanf(node->label, "%63s %3s %63s", lhs, op, rhs) == 3) {
      /* load lhs/rhs into registers, respecting immediates and char literals */
      load_simple_to_reg(lhs, 2, st, head, tail, &pending_label);
      load_simple_to_reg(rhs, 3, st, head, tail, &pending_label);

      emit_instr(head, tail, "cmp", zero_reg(), reg(2), reg(3), NULL);

      char* Ltrue = func_label(cfg->func_name, node->true_target->id);
      char* Lfalse = func_label(cfg->func_name, node->false_target->id);


      if (strcmp(op, "<") == 0)
        emit_instr(head, tail, "blt", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
      else if (strcmp(op, "<=") == 0)
        emit_instr(head, tail, "ble", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
      else if (strcmp(op, ">") == 0)
        emit_instr(head, tail, "bgt", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
      else if (strcmp(op, ">=") == 0)
        emit_instr(head, tail, "bge", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
      else if (strcmp(op, "==") == 0)
        emit_instr(head, tail, "beq", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);
      else if (strcmp(op, "!=") == 0)
        emit_instr(head, tail, "bne", label_op(Ltrue), zero_reg(), zero_reg(),
                   NULL);

      emit_instr(head, tail, "jmp", label_op(Lfalse), zero_reg(), zero_reg(),
                 NULL);
      clear_current_source_location();
      return;
    }

    clear_current_source_location();
  }

  // ======================================================
  // 3. Обычный переход next
  // ======================================================
  if (node->next_target) {
    char* L = func_label(cfg->func_name, node->next_target->id);
    emit_instr(head, tail, "jmp", label_op(L), zero_reg(), zero_reg(),
               pending_label);
    return;
  }
  // после блока "3. Обычный переход next" и перед "4. Если метка осталась"
  if (!node->next_target && !(node->true_target && node->false_target) &&
      !(node->label && strcmp(node->label, "FINISH") == 0)) {
    // чтобы не провалиться в следующий лейбл случайно
    emit_instr(head, tail, "jmp", label_op(strdup(exit_label)), zero_reg(),
               zero_reg(), pending_label);
    return;
  }

  // ======================================================
  // 4. Если метка осталась — ставим nop
  // ======================================================
  if (pending_label) {
    emit_instr(head, tail, "nop", zero_reg(), zero_reg(), zero_reg(),
               pending_label);
  }
}

/* ---------- Линеаризация одной функции в TAC ---------- */

static int is_naked_function(const char* func_name) {
  return func_name &&
         (strcmp(func_name, "irq_handler") == 0 ||
          strstr(func_name, "$irq_handler$") != NULL);
}

static void collect_symbols_from_stmt(SymbolTable* symtab, const char* stmt_text) {
  char text[MAX_BUFFER_LEN];
  char var_name[MAX_VAR_NAME_LEN];
  int val;
  char* eq;

  if (!symtab || !stmt_text) return;

  strncpy(text, stmt_text, sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  trim_whitespace(text);
  if (text[0] == '\0') return;

  eq = strchr(text, '=');

  if (strncmp(text, "decl ", 5) == 0) {
    char decl[512];
    char decl_type[256];
    char decl_name[256];
    strncpy(decl, text + 5, sizeof(decl) - 1);
    decl[sizeof(decl) - 1] = '\0';
    eq = strchr(decl, '=');
    if (eq) *eq = '\0';
    trim_whitespace(decl);
    if (split_decl_type_and_name(decl, decl_type, sizeof(decl_type), decl_name,
                                 sizeof(decl_name))) {
      add_local_symbol(symtab, decl_name, decl_type[0] ? decl_type : "int");
    }
    return;
  }

  if (eq) {
    char lhs_buf[MAX_VAR_NAME_LEN];
    char rhs_buf[MAX_BUFFER_LEN];
    size_t lhs_len = (size_t)(eq - text);
    if (lhs_len >= sizeof(lhs_buf)) lhs_len = sizeof(lhs_buf) - 1;
    memcpy(lhs_buf, text, lhs_len);
    lhs_buf[lhs_len] = '\0';
    snprintf(rhs_buf, sizeof(rhs_buf), "%s", eq + 1);
    trim_whitespace(lhs_buf);
    trim_whitespace(rhs_buf);

    if (split_array_suffix(lhs_buf, var_name, sizeof(var_name), rhs_buf,
                           sizeof(rhs_buf))) {
      get_var_offset(symtab, var_name);
    } else if (is_simple_ident(lhs_buf) &&
               !is_implicit_field_name(symtab, lhs_buf)) {
      get_var_offset(symtab, lhs_buf);
    }
    return;
  }
}

FunctionCode linearize_function(FunctionCFG* cfg, AnalysisResult* analysis,
                                CfgBuilderState* state) {
  FunctionCode code = {0};
  int source_file_index = -1;
  code.name = strdup(cfg->func_name ? cfg->func_name : "unknown");
  current_fc = &code;
  code.instr_count = 0;
  clear_current_source_location();

  if (analysis && cfg && cfg->source_filename) {
    for (int i = 0; i < analysis->files_count; ++i) {
      SourceFileInfo* sf = analysis->files[i];
      if (sf && sf->filename && strcmp(sf->filename, cfg->source_filename) == 0) {
        source_file_index = i;
        break;
      }
    }
  }
  if (source_file_index < 0 && analysis && analysis->files_count == 1) {
    source_file_index = 0;
  }

  if (state) {
    state->tmp_mod_counter = 0;
  }

  if (cfg->node_count == 0) return code;

  SymbolTable symtab;
  init_symtab(&symtab, analysis, cfg);
  for (int p = 0; p < cfg->param_count; ++p) {
    int offset = 8 + p * VAR_SIZE;
    add_param_symbol(&symtab, cfg->params[p], offset, cfg->param_types[p]);
  }

  Instr* head = NULL;
  Instr* tail = NULL;
  char* entry_label = strdupf("%s:", code.name);
  char* exit_label = strdupf("%s_exit", code.name);
  char* exit_label_def = strdupf("%s_exit:", code.name);
  int naked = is_naked_function(cfg->func_name);

  /* 1. Первый проход: собираем переменные по текстам операций */
  for (int i = 0; i < cfg->node_count; i++) {
    CFGNode* node = cfg->nodes[i];
    for (int o = 0; o < node->ops_count; ++o) {
      Operation* op = node->ops[o];
      if (!op || !op->text) continue;

      char text[MAX_BUFFER_LEN];
      strncpy(text, op->text, sizeof(text) - 1);
      text[sizeof(text) - 1] = '\0';
      trim_whitespace(text);
      size_t len = strlen(text);
      if (len > 0 && text[len - 1] == ';') {
        text[len - 1] = '\0';
        trim_whitespace(text);
      }
      if (text[0] == '\0') continue;

      {
        char* stmt = strtok(text, ";");
        while (stmt) {
          char one_stmt[MAX_BUFFER_LEN];
          strncpy(one_stmt, stmt, sizeof(one_stmt) - 1);
          one_stmt[sizeof(one_stmt) - 1] = '\0';
          trim_whitespace(one_stmt);
          if (one_stmt[0]) {
            collect_symbols_from_stmt(&symtab, one_stmt);
          }
          stmt = strtok(NULL, ";");
        }
      }
      continue;

      char var_name[MAX_VAR_NAME_LEN];
      int val;
      char* eq = strchr(text, '=');

      if (strncmp(text, "decl ", 5) == 0) {
        char decl[512];
        char decl_type[256];
        char decl_name[256];
        strncpy(decl, text + 5, sizeof(decl) - 1);
        decl[sizeof(decl) - 1] = '\0';
        char* eq = strchr(decl, '=');
        if (eq) *eq = '\0';
        trim_whitespace(decl);
        if (split_decl_type_and_name(decl, decl_type, sizeof(decl_type),
                                     decl_name, sizeof(decl_name))) {
          add_local_symbol(&symtab, decl_name,
                           decl_type[0] ? decl_type : "int");
        }
      }
      /* var = in() */
      else if (eq) {
        char lhs_buf[MAX_VAR_NAME_LEN];
        char rhs_buf[MAX_BUFFER_LEN];
        size_t lhs_len = (size_t)(eq - text);
        if (lhs_len >= sizeof(lhs_buf)) lhs_len = sizeof(lhs_buf) - 1;
        memcpy(lhs_buf, text, lhs_len);
        lhs_buf[lhs_len] = '\0';
        snprintf(rhs_buf, sizeof(rhs_buf), "%s", eq + 1);
        trim_whitespace(lhs_buf);
        trim_whitespace(rhs_buf);
        if (sscanf(text, "%127[^= ] = %d", var_name, &val) == 2) {
          get_var_offset(&symtab, var_name);
        }
      }
      /* out(...) с переменной внутри */
      else if (strncmp(text, "out", 3) == 0) {
        char* paren = strchr(text, '(');
        char* close = paren ? strrchr(paren, ')') : NULL;
        if (paren && close && close > paren + 1) {
          char inner[MAX_VAR_NAME_LEN];
          size_t ilen = (size_t)(close - paren - 1);
          if (ilen >= sizeof(inner)) ilen = sizeof(inner) - 1;
          memcpy(inner, paren + 1, ilen);
          inner[ilen] = '\0';
          trim_whitespace(inner);
          if ((inner[0] == '\'' && strlen(inner) >= 3 &&
               inner[strlen(inner) - 1] == '\''))
            ;  // ничего не делаем
          else if (is_simple_ident(inner)) {
            get_var_offset(&symtab, inner);
          }
        }
      }
    }
  }

  /* 2. Пролог функции: push fp; mov fp, sp; sub sp, local_size */
  Instr* sub_sp_instr = NULL;
  if (!naked) {
  emit_instr(&head, &tail, "push", reg(FP), zero_reg(), zero_reg(),
             entry_label);
  emit_instr(&head, &tail, "mov", reg(FP), reg(SP), zero_reg(), NULL);
  emit_instr(&head, &tail, "sub", reg(SP), reg(SP), imm(0), NULL);
  sub_sp_instr = tail;
  for (int i = 0; i < symtab.count; ++i) {
    Symbol* s = &symtab.symbols[i];
    if (!s->is_param && s->is_object && s->storage_offset != 0) {
      emit_instr(&head, &tail, "add", reg(2), reg(FP), imm(s->storage_offset),
                 NULL);
      emit_instr(&head, &tail, "mov", mem_op(FP, s->offset), reg(2),
                 zero_reg(), NULL);
    }
  }

  /* 3. Строим порядок обхода блоков (reverse postorder) */
  } else {
    emit_instr(&head, &tail, "nop", zero_reg(), zero_reg(), zero_reg(),
               entry_label);
  }

  int max_id = 0;
  for (int i = 0; i < cfg->node_count; i++) {
    if (cfg->nodes[i]->id > max_id) max_id = cfg->nodes[i]->id;
  }
  bool* visited = calloc((size_t)max_id + 1, sizeof(bool));
  int* order = malloc(sizeof(int) * (size_t)cfg->node_count);
  int order_idx = 0;
  dfs_postorder(cfg->entry, visited, max_id, order, &order_idx);

  /* 4. Генерируем код для каждого блока */
  for (int i = order_idx - 1; i >= 0; i--) {
    int id = order[i];
    CFGNode* node = NULL;
    for (int j = 0; j < cfg->node_count; ++j) {
      if (cfg->nodes[j]->id == id) {
        node = cfg->nodes[j];
        break;
      }
    }
    if (!node) continue;

    bool need_label = (node->id != cfg->entry->id);
    char* block_label =
        need_label ? func_label(cfg->func_name, node->id) : NULL;


    gen_block(node, cfg, &head, &tail, block_label, exit_label, &symtab,
              state, source_file_index);
  }

  /* 5. Эпилог: mov sp, fp; pop fp; ret */
  Instr* add_sp_back = NULL;
  if (!naked) {
  emit_instr(&head, &tail, "add", reg(SP), reg(SP), imm(0), exit_label_def);
  add_sp_back = tail;
  emit_instr(&head, &tail, "pop", reg(FP), zero_reg(), zero_reg(), NULL);
  emit_instr(&head, &tail, "ret", zero_reg(), zero_reg(), zero_reg(), NULL);

  /* 6. Проставляем реальный размер локалов в прологе */
  } else {
    emit_instr(&head, &tail, "halt", zero_reg(), zero_reg(), zero_reg(),
               exit_label_def);
  }

  code.local_size = naked ? 0 : symtab.local_vars_size;
  if (sub_sp_instr) {
    sub_sp_instr->src2.imm = code.local_size;
  }
  if (add_sp_back) {
    add_sp_back->src2.imm = code.local_size;
  }

  code.head = head;
  code.tail = tail;

  memset(&code.syms, 0, sizeof(code.syms));
  strncpy(code.syms.func, cfg->func_name ? cfg->func_name : "unknown",
          sizeof(code.syms.func) - 1);
  code.syms.frame_size = symtab.local_vars_size;

  /* параметры (arg) */
  for (int p = 0; p < cfg->param_count; ++p) {
    int off = 8 + p * VAR_SIZE;
    add_named_entry_typed(&code.syms, cfg->params[p], cfg->param_types[p], off,
                          is_object_type(&symtab, cfg->param_types[p]),
                          type_is_array_name(cfg->param_types[p]), 0, 0, 0);
  }

  /* локальные переменные (var) */
  for (int i = 0; i < symtab.count; ++i) {
    Symbol* s = &symtab.symbols[i];
    if (!s->name || !s->name[0]) continue;

    /* фильтр временных */
    if (strncmp(s->name, "__tmp_", 6) == 0) continue;

    add_named_entry_typed(&code.syms, s->name, s->type_name, s->offset,
                          s->is_object, s->is_array, s->array_length,
                          s->storage_offset, s->storage_size);
  }


  current_fc = NULL;


  free_symtab(&symtab);
  free(visited);
  free(order);
  return code;
}

/* ---------- Печать операнда в .tac ---------- */

static void print_operand(FILE* f, Operand op) {
  const char* p;
  switch (op.kind) {
    case OP_REG:
      if (op.reg == FP)
        fprintf(f, "fp");
      else if (op.reg == SP)
        fprintf(f, "sp");
      else
        fprintf(f, "r%d", op.reg);
      break;
    case OP_IMM:
      fprintf(f, "#%d", op.imm);
      break;
    case OP_LABEL:
      if (!op.label) {
        fprintf(f, "<null_label>");
        break;
      }
      for (p = op.label; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        fputc((isalnum(ch) || ch == '_') ? (int)ch : '_', f);
      }
      break;
    case OP_MEM:
      if (op.mem.base == FP)
        fprintf(f, "[fp + %d]", op.mem.offset);
      else if (op.mem.base == SP)
        fprintf(f, "[sp + %d]", op.mem.offset);
      else if (op.mem.base >= 0)
        fprintf(f, "[r%d + %d]", op.mem.base, op.mem.offset);
      else
        fprintf(f, "[%d]", op.mem.offset);
      break;
  }
}

static void print_label_definition(FILE* f, const char* label) {
  size_t len;
  size_t stop;
  size_t i;

  if (!f || !label) return;

  len = strlen(label);
  stop = (len > 0 && label[len - 1] == ':') ? len - 1 : len;

  for (i = 0; i < stop; ++i) {
    unsigned char ch = (unsigned char)label[i];
    fputc((isalnum(ch) || ch == '_') ? (int)ch : '_', f);
  }

  if (stop < len) {
    fputc(':', f);
  }
}

typedef struct {
  char* name;
  int address;
} AsmLabelAddress;

static char* trim_ascii_inplace(char* s) {
  char* end;
  if (!s) return s;
  while (*s && isspace((unsigned char)*s)) s++;
  if (*s == '\0') return s;
  end = s + strlen(s) - 1;
  while (end >= s && isspace((unsigned char)*end)) {
    *end = '\0';
    --end;
  }
  return s;
}

static int append_label_address(AsmLabelAddress** items, int* count, int* cap,
                                const char* name, int address) {
  if (!items || !count || !cap || !name || !*name) return 0;
  for (int i = 0; i < *count; ++i) {
    if (strcmp((*items)[i].name, name) == 0) {
      (*items)[i].address = address;
      return 1;
    }
  }
  if (*count >= *cap) {
    int new_cap = (*cap == 0) ? 32 : (*cap * 2);
    AsmLabelAddress* next =
        realloc(*items, sizeof(AsmLabelAddress) * new_cap);
    if (!next) return 0;
    *items = next;
    *cap = new_cap;
  }
  (*items)[*count].name = strdup(name);
  (*items)[*count].address = address;
  (*count)++;
  return 1;
}

static int find_label_address(const AsmLabelAddress* items, int count,
                              const char* name, int* out_address) {
  if (!items || !name || !out_address) return 0;
  for (int i = 0; i < count; ++i) {
    if (strcmp(items[i].name, name) == 0) {
      *out_address = items[i].address;
      return 1;
    }
  }
  return 0;
}

static int is_label_reference_token(const char* token) {
  const unsigned char* p = (const unsigned char*)token;
  if (!p || !*p) return 0;
  if (!(isalpha(*p) || *p == '_')) return 0;
  for (++p; *p; ++p) {
    if (!(isalnum(*p) || *p == '_')) return 0;
  }
  return 1;
}

static int resolve_generated_asm_labels(const char* path) {
  enum { ASM_LINE_MAX = 8192, INSTR_SIZE_BYTES = 8 };
  FILE* in = NULL;
  FILE* out = NULL;
  char tmp_path[1024];
  char line[ASM_LINE_MAX];
  AsmLabelAddress* labels = NULL;
  int label_count = 0;
  int label_cap = 0;
  int pc = 0;
  int ok = 0;

  if (!path) return 0;

  in = fopen(path, "rb");
  if (!in) return 0;

  while (fgets(line, sizeof(line), in)) {
    char work[ASM_LINE_MAX];
    char* trimmed;
    size_t len;
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    trimmed = trim_ascii_inplace(work);
    if (*trimmed == '\0' || *trimmed == ';' || *trimmed == '[') continue;
    len = strlen(trimmed);
    if (len > 0 && trimmed[len - 1] == ':') {
      trimmed[len - 1] = '\0';
      if (!append_label_address(&labels, &label_count, &label_cap, trimmed,
                                pc)) {
        goto cleanup;
      }
      continue;
    }
    pc += INSTR_SIZE_BYTES;
  }

  fclose(in);
  in = NULL;

  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  in = fopen(path, "rb");
  if (!in) goto cleanup;
  out = fopen(tmp_path, "wb");
  if (!out) goto cleanup;

  while (fgets(line, sizeof(line), in)) {
    char raw[ASM_LINE_MAX];
    char original[ASM_LINE_MAX];
    char body[ASM_LINE_MAX];
    char comment[ASM_LINE_MAX];
    char* trimmed;
    char* mnemonic;
    char* rest;
    char* sep;
    size_t indent_len = 0;
    int replaced = 0;

    strncpy(raw, line, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';
    raw[strcspn(raw, "\r\n")] = '\0';
    strncpy(original, raw, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';
    body[0] = '\0';
    comment[0] = '\0';

    {
      char* semicolon = strchr(raw, ';');
      if (semicolon) {
        snprintf(comment, sizeof(comment), "%s", semicolon);
        *semicolon = '\0';
      }
    }

    snprintf(body, sizeof(body), "%s", raw);
    trimmed = trim_ascii_inplace(body);

    while (line[indent_len] == ' ' || line[indent_len] == '\t') {
      indent_len++;
    }

    if (*trimmed == '\0' || *trimmed == '[' || *trimmed == ';') {
      fprintf(out, "%s\n", original);
      continue;
    }

    {
      size_t len = strlen(trimmed);
      if (len > 0 && trimmed[len - 1] == ':') {
        fprintf(out, "%s\n", original);
        continue;
      }
    }

    mnemonic = trimmed;
    rest = mnemonic;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) {
      *rest++ = '\0';
    }
    rest = trim_ascii_inplace(rest);

    if ((strcmp(mnemonic, "jmp") == 0 || strcmp(mnemonic, "beq") == 0 ||
         strcmp(mnemonic, "bne") == 0 || strcmp(mnemonic, "blt") == 0 ||
         strcmp(mnemonic, "bgt") == 0 || strcmp(mnemonic, "ble") == 0 ||
         strcmp(mnemonic, "bge") == 0 || strcmp(mnemonic, "call") == 0) &&
        is_label_reference_token(rest)) {
      int address = 0;
      if (find_label_address(labels, label_count, rest, &address)) {
        fprintf(out, "%.*s%-7s %d", (int)indent_len, line, mnemonic, address);
        if (comment[0]) fprintf(out, " %s", comment);
        fprintf(out, "\n");
        replaced = 1;
      }
    } else if (strcmp(mnemonic, "mov") == 0) {
      sep = strchr(rest, ',');
      if (sep) {
        char dst[ASM_LINE_MAX];
        char src[ASM_LINE_MAX];
        int address = 0;
        size_t dst_len;

        dst_len = (size_t)(sep - rest);
        if (dst_len >= sizeof(dst)) dst_len = sizeof(dst) - 1;
        memcpy(dst, rest, dst_len);
        dst[dst_len] = '\0';
        snprintf(src, sizeof(src), "%s", sep + 1);

        trim_ascii_inplace(dst);
        trim_ascii_inplace(src);

        if (is_label_reference_token(src) &&
            find_label_address(labels, label_count, src, &address)) {
          fprintf(out, "%.*s%-7s %s, %d", (int)indent_len, line, mnemonic, dst,
                  address);
          if (comment[0]) fprintf(out, " %s", comment);
          fprintf(out, "\n");
          replaced = 1;
        }
      }
    }

    if (!replaced) {
      fprintf(out, "%s", raw);
      if (comment[0]) fprintf(out, "%s", comment);
      fprintf(out, "\n");
    }
  }

  fclose(out);
  out = NULL;
  fclose(in);
  in = NULL;

  if (remove(path) != 0) goto cleanup;
  if (rename(tmp_path, path) != 0) goto cleanup;

  ok = 1;

cleanup:
  if (in) fclose(in);
  if (out) fclose(out);
  if (!ok) {
    remove(tmp_path);
  }
  for (int i = 0; i < label_count; ++i) {
    free(labels[i].name);
  }
  free(labels);
  return ok;
}

/* ---------- Выгрузка FunctionCode в .tac ---------- */
static void dump_function_sym(FILE* out, FuncSyms* fs) {
  fprintf(out, "func %s start %d end %d frame %d\n", fs->func, fs->start_addr,
          fs->end_addr, fs->frame_size);

  qsort(fs->entries, fs->entry_count, sizeof(SymEntry), sym_cmp);

  for (int i = 0; i < fs->entry_count; ++i) {
    int off = fs->entries[i].offset;
    if (off == 0) continue;
    char type_token[128];
    compact_type_token(fs->entries[i].type_name[0] ? fs->entries[i].type_name
                                                   : "int",
                       type_token, sizeof(type_token));

    fprintf(out, "%s %s %d type %s", off > 0 ? "arg" : "var",
            fs->entries[i].name, off, type_token);
    if (fs->entries[i].is_array) {
      fprintf(out, " arr %d", fs->entries[i].array_length);
    }
    if (fs->entries[i].is_object) {
      fprintf(out, " obj %d %d", fs->entries[i].storage_offset,
              fs->entries[i].storage_size);
    }
    fprintf(out, "\n");

  }
  fprintf(out, "\n");
}

static void dump_type_syms(FILE* out, AnalysisResult* res) {
  if (!out || !res) return;
  for (int i = 0; i < res->type_count; ++i) {
    UserTypeInfo* type = &res->types[i];
    char base_token[128];
    compact_type_token(type->base_name ? type->base_name : "-", base_token,
                       sizeof(base_token));
    fprintf(out, "type %s size %d base %s\n", type->name ? type->name : "",
            type->instance_size, base_token[0] ? base_token : "-");
    for (int j = 0; j < type->field_count; ++j) {
      FieldInfo* field = &type->fields[j];
      char field_type[128];
      compact_type_token(field->type_name ? field->type_name : "int",
                         field_type, sizeof(field_type));
      fprintf(out, "field %s %s %d %s\n", type->name ? type->name : "",
              field->name ? field->name : "", field->offset, field_type);
    }
  }
  if (res->type_count > 0) fprintf(out, "\n");
}

void dump_function_asm(FILE* f, FunctionCode* fc) {
  int last_src_file_index = -2;
  int last_src_line = -1;
  int last_src_col = -1;
  fprintf(f, "\n; ====================================\n");
  fprintf(f, "; Function: %s\n", fc->name);
  fprintf(f, "; Local space: %d bytes\n", fc->local_size);
  fprintf(f, "; ====================================\n");

  for (Instr* i = fc->head; i; i = i->next) {
    if (i->label) {
      if (strchr(i->label, ':')) {
        print_label_definition(f, i->label);
        fprintf(f, "\n");
      } else {
        fprintf(f, "  ");
        print_label_definition(f, i->label);
        fprintf(f, ":\n");
      }
    }

    if (i->src_file_index >= 0 && i->src_line > 0 &&
        (i->src_file_index != last_src_file_index ||
         i->src_line != last_src_line || i->src_col != last_src_col)) {
      fprintf(f, ";#src %d %d %d\n", i->src_file_index, i->src_line,
              i->src_col);
      last_src_file_index = i->src_file_index;
      last_src_line = i->src_line;
      last_src_col = i->src_col;
    }

    fprintf(f, "    %-7s ", i->mnemonic);

    if (strcmp(i->mnemonic, "ret") == 0 || strcmp(i->mnemonic, "nop") == 0 ||
        strcmp(i->mnemonic, "halt") == 0 ||
        strcmp(i->mnemonic, "trap") == 0 ||
        strcmp(i->mnemonic, "iret") == 0) {
      /* 0 операндов */
    } else if (strcmp(i->mnemonic, "jmp") == 0 ||
               strcmp(i->mnemonic, "beq") == 0 ||
               strcmp(i->mnemonic, "bne") == 0 ||
               strcmp(i->mnemonic, "blt") == 0 ||
               strcmp(i->mnemonic, "bgt") == 0 ||
               strcmp(i->mnemonic, "ble") == 0 ||
               strcmp(i->mnemonic, "bge") == 0) {
      print_operand(f, i->dst);
    } else if (strcmp(i->mnemonic, "call") == 0) {
      print_operand(f, i->dst);
    } else if (strcmp(i->mnemonic, "cmp") == 0) {
      print_operand(f, i->src1);
      fprintf(f, ", ");
      print_operand(f, i->src2);
    } else if (strcmp(i->mnemonic, "in") == 0) {
      print_operand(f, i->dst);
    } else if (strcmp(i->mnemonic, "push") == 0 ||
               strcmp(i->mnemonic, "pop") == 0) {
      print_operand(f, i->dst);
    } else if (strcmp(i->mnemonic, "out") == 0) {
      /* out печатаем по src1 */
      print_operand(f, i->src1);
    } else if (strcmp(i->mnemonic, "mov") == 0 ||
               strcmp(i->mnemonic, "movb") == 0) {
      print_operand(f, i->dst);
      fprintf(f, ", ");
      print_operand(f, i->src1);
    } else {
      /* по умолчанию 3-операндная форма: add, sub, ... */
      print_operand(f, i->dst);
      fprintf(f, ", ");
      print_operand(f, i->src1);
      fprintf(f, ", ");
      print_operand(f, i->src2);
    }
    fprintf(f, "\n");
  }
}

static void free_function_code(FunctionCode* fc) {
  if (!fc) return;

  Instr* i = fc->head;
  while (i) {
    Instr* next = i->next;
    free(i->mnemonic);
    if (i->label) free(i->label);
    free(i);
    i = next;
  }

  if (fc->name) free(fc->name);
}

static void append_runtime_asm_file(FILE* out, const char* path) {
  FILE* in = NULL;
  char line[1024];

  if (!out || !path) return;

  in = fopen(path, "rb");
  if (!in) {
    fprintf(stderr, "Warning: failed to open runtime asm %s\n", path);
    return;
  }

  fprintf(out, ";; runtime: %s\n", path);
  while (fgets(line, sizeof(line), in)) {
    fputs(line, out);
    if (!strchr(line, '\n')) fputc('\n', out);
  }
  fprintf(out, "\n");
  fclose(in);
}

static void append_runtime_asm(FILE* out) {
  // rt_threads.asm is NOT auto-included because it contains a bridge stub
  // (taskBody: jmp global_taskBody_1_int) that requires the source program
  // to define `void taskBody(int)`. Programs that use funcAddr(taskBody) must
  // link rt_threads.asm explicitly via the assembler's ExtraAsmFiles option.
  static const char* runtime_files[] = {
      "src/runtime/in.asm",
      "src/runtime/out.asm",
      "src/runtime/pipe_in.asm",
      "src/runtime/pipe_in0.asm",
      "src/runtime/pipe_in1.asm",
      "src/runtime/pipe_in2.asm",
      "src/runtime/pipe_typed.asm",
      "src/runtime/pipe_block.asm",
      "src/runtime/pipe_out.asm",
      "src/runtime/timer.asm",
      "src/runtime/rt_ctx.asm",
      NULL,
  };

  if (!out) return;
  for (int i = 0; runtime_files[i]; ++i) {
    append_runtime_asm_file(out, runtime_files[i]);
  }
}


/* ---------- Главная функция для генерации .tac ---------- */

void generate_tac_assembly(AnalysisResult* res, const char* outfile) {
  const int instr_size_bytes = 8;
  const int bootstrap_instrs = 7;
  const int stack_top = 1040000;
  FILE* f = fopen(outfile, "w");
  if (!f) {
    perror("fopen");
    return;
  }

  char sym_path[1024];
  snprintf(sym_path, sizeof(sym_path), "%s.sym", outfile);
  FILE* sym = fopen(sym_path, "w");
  if (!sym) {
    perror("fopen sym");
    fclose(f);
    return;
  }

  CfgBuilderState tac_state;
  init_builder_state(&tac_state);

  fprintf(f, ";; TacVm13 assembly listing\n");
  if (res && res->files_count > 0 && res->files[0])
    fprintf(f, ";; source: %s\n\n", res->files[0]->filename);
  if (res) {
    for (int i = 0; i < res->files_count; ++i) {
      SourceFileInfo* sf = res->files[i];
      if (sf && sf->filename) {
        fprintf(f, ";; source-file %d %s\n", i, sf->filename);
      }
    }
    if (res->files_count > 0) {
      fprintf(f, "\n");
    }
  }
  fprintf(f, "[section ram, code]\n\n");

  dump_type_syms(sym, res);

  int has_main = 0;
  for (int i = 0; i < res->files_count && !has_main; ++i) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;
    for (int j = 0; j < sf->functions_count; ++j) {
      if (sf->functions[j] && sf->functions[j]->func_name &&
          strcmp(sf->functions[j]->func_name, "main") == 0) {
        has_main = 1;
        break;
      }
    }
  }

  int global_pc = 0;
  if (has_main) {
    fprintf(f, "start:\n");
    fprintf(f, "    mov     sp, #%d\n", stack_top);
    fprintf(f, "    mov     fp, sp\n");
    fprintf(f, "    mov     r5, #%d\n", BUILTIN_HEAP_PTR_ADDR);
    fprintf(f, "    mov     r6, #%d\n", BUILTIN_HEAP_START_ADDR);
    fprintf(f, "    mov     [r5 + 0], r6\n");
    fprintf(f, "    call    main\n");
    fprintf(f, "    halt\n\n");
    global_pc = bootstrap_instrs * instr_size_bytes;
  }

  for (int i = 0; i < res->files_count; i++) {
    SourceFileInfo* sf = res->files[i];
    if (!sf) continue;

    /* ---- найти main ---- */
    int main_idx = -1;
    for (int j = 0; j < sf->functions_count; j++) {
      if (sf->functions[j] && sf->functions[j]->func_name &&
          strcmp(sf->functions[j]->func_name, "main") == 0) {
        main_idx = j;
        break;
      }
    }

    /* ---- сначала main ---- */
    if (main_idx >= 0) {
      FunctionCFG* cfg = sf->functions[main_idx];
      if (cfg) {
        reset_function_state(&tac_state);

        FunctionCode fc = linearize_function(cfg, res, &tac_state);

        fc.syms.start_addr = global_pc;

        dump_function_asm(f, &fc);

        global_pc += fc.instr_count * instr_size_bytes;

        fc.syms.end_addr = global_pc;

        dump_function_sym(sym, &fc.syms);

        free_function_code(&fc);
      }
    }

    /* ---- остальные функции ---- */
    for (int j = 0; j < sf->functions_count; j++) {
      if (j == main_idx) continue;

      FunctionCFG* cfg = sf->functions[j];
      if (!cfg) continue;

      reset_function_state(&tac_state);

      FunctionCode fc = linearize_function(cfg, res, &tac_state);

      fc.syms.start_addr = global_pc;

      dump_function_asm(f, &fc);

      global_pc += fc.instr_count * instr_size_bytes;

      fc.syms.end_addr = global_pc;

      dump_function_sym(sym, &fc.syms);

      free_function_code(&fc);
    }
  }

  fclose(sym);
  free_builder_state(&tac_state);
  append_runtime_asm(f);
  fclose(f);

  if (!resolve_generated_asm_labels(outfile)) {
    fprintf(stderr, "Warning: failed to resolve labels in %s\n", outfile);
  }
}
