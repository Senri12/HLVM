#ifndef CFG_BUILDER_H
#define CFG_BUILDER_H

#include <stdlib.h>
#include <stdio.h>
#include <antlr3.h>

/* Operation kinds */
typedef enum {
  OP_ASSIGN,
  OP_EXPR,
  OP_CALL,
  OP_RETURN,
  OP_CONTINUE,
  OP_BREAK,
  OP_EMPTY,
  OP_UNKNOWN
} OpType;

typedef struct FieldInfo {
  char* name;
  char* type_name;
  int offset;
  int visibility;
} FieldInfo;

typedef struct MethodInfo {
  char* owner_type;
  char* source_name;
  char* mangled_name;
  char* return_type;
  char** param_names;
  char** param_types;
  int param_count;
  int visibility;
} MethodInfo;

typedef struct UserTypeInfo {
  char* name;
  char* base_name;
  char** template_params;
  int template_param_count;
  FieldInfo* fields;
  int field_count;
  MethodInfo* methods;
  int method_count;
  int instance_size;
} UserTypeInfo;


/* Operation node */
typedef struct Operation {
    OpType type;
    char* text;                 /* textual representation */
    struct Operation** args;
    int argc;
    int line;
    int col;
} Operation;

/* CFG structures */
typedef int NodeId;

typedef struct CFGNode {
    NodeId id;
    char* label;
    char* color;
    Operation** ops;
    int ops_count;
    struct CFGNode* true_target;
    struct CFGNode* false_target;
    struct CFGNode* next_target;
    struct CFGNode** outs;
    int outs_count;
    int line;
    int col;
} CFGNode;

typedef struct FunctionCFG {
    char* func_name;
    char* source_name;
    char* owner_type;
    char* return_type;
    char* signature;
    char* source_filename;
    CFGNode* entry;
    CFGNode* exit;
    CFGNode** nodes;
    int node_count;
    char** params;
    char** param_types;
    int param_count;
    int is_method;
    char** called_functions;
    int called_count;
} FunctionCFG;

typedef struct SourceFileInfo {
    char* filename;
    pANTLR3_BASE_TREE parse_tree; /* ANTLR tree pointer */
    FunctionCFG** functions;
    int functions_count;
} SourceFileInfo;

typedef struct AnalysisResult {
    SourceFileInfo** files;
    int files_count;
    UserTypeInfo* types;
    int type_count;
    char** errors;
    int errors_count;
} AnalysisResult;

/* API */
AnalysisResult* build_cfg_from_parse_trees(SourceFileInfo** files, int files_count);

/* Export CFG + callgraph to DGML */
void write_cfg_callgraph_dgml(struct AnalysisResult* res, const char* filename);

/* Export JVM text listing and a real .class file for the simple JVM backend */
int generate_jvm_classfile(struct AnalysisResult* res, const char* asm_outfile);

/* Free analysis result */
void free_analysis_result(AnalysisResult* r);

/* Export a single function to DOT */
int write_function_cfg_dot(FunctionCFG* f, const char* outpath);
void write_callgraph_dgml(AnalysisResult* res, const char* filename);

/* Export call-graph (DOT) */
int write_callgraph_dot(AnalysisResult* res, const char* outpath);

#endif /* CFG_BUILDER_H */
