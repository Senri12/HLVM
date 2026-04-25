#include <antlr3.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SimpleLangLexer.h"
#include "SimpleLangParser.h"
#include "cfg_builder.h"

#define SIMPLELANG_CALL CALL

#define LABEL_MAX 1024
#define STACK_MAX 4096

typedef struct {
  int parent;
  int child;
} Link;

static int g_parse_error_seen = 0;
static const char* g_progress_file = NULL;

static void note_progress(const char* message) {
  FILE* f = NULL;
  if (!g_progress_file || !message) return;
  f = fopen(g_progress_file, "ab");
  if (!f) return;
  fprintf(f, "%s\n", message);
  fclose(f);
}

typedef struct {
  int arrival_time;
  int service_time;
} SchedulerTaskSpec;

typedef enum {
  SPECIAL_SCHED_RR2 = 0,
  SPECIAL_SCHED_SRT = 1
} SpecialSchedulerMode;

enum {
  SPECIAL_TASK_COUNT = 21,
  SPECIAL_IDLE_TASK = 21,
  SPECIAL_QUEUE_CAPACITY = 32,

  SPECIAL_MMIO_TIME = 1000000,
  SPECIAL_MMIO_TIMER_PERIOD = 1000004,
  SPECIAL_MMIO_HANDLER = 1000008,
  SPECIAL_MMIO_IRQ_ENABLE = 1000012,
  SPECIAL_MMIO_IRQ_CAUSE = 1000016,
  SPECIAL_MMIO_FRAME_SP = 1000020,

  SPECIAL_CURRENT_TASK = 16384,
  SPECIAL_FINISHED_COUNT = 16388,
  SPECIAL_QUANTUM_USED = 16392,
  SPECIAL_QUEUE_HEAD = 16396,
  SPECIAL_QUEUE_TAIL = 16400,
  SPECIAL_QUEUE_COUNT = 16404,
  SPECIAL_SUM_TA = 16408,
  SPECIAL_SUM_WT = 16412,
  SPECIAL_PENDING_TASK = 16416,

  SPECIAL_ARR_BASE = 16512,
  SPECIAL_BURST_BASE = 16640,
  SPECIAL_REM_BASE = 16768,
  SPECIAL_STATE_BASE = 16896,
  SPECIAL_COMP_BASE = 17024,
  SPECIAL_CTX_BASE = 17152,
  SPECIAL_QUEUE_BASE = 17408,

  SPECIAL_KERNEL_STACK_TOP = 240000,
  SPECIAL_TASK_STACK_BASE = 220000,
  SPECIAL_TASK_STACK_STRIDE = 256,
  SPECIAL_IDLE_STACK_TOP = 214000,
  SPECIAL_CONTEXT_FRAME_BYTES = 128
};

static const SchedulerTaskSpec kVariant19Tasks[SPECIAL_TASK_COUNT] = {
    {0, 8},    {3, 4},    {11, 7},  {15, 5},  {25, 8},  {30, 4},  {39, 6},
    {42, 5},   {50, 8},   {54, 4},  {64, 7},  {69, 5},  {78, 6},  {81, 4},
    {89, 8},   {93, 5},   {103, 7}, {108, 4}, {117, 6}, {121, 5}, {126, 8}};

typedef struct {
  int items[STACK_MAX];
  int top;
} Stack;

static char* trim_ascii(char* s) {
  char* end;
  if (!s) return s;
  while (*s && isspace((unsigned char)*s)) s++;
  if (*s == 0) return s;
  end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) {
    *end = 0;
    end--;
  }
  return s;
}

static void push(Stack* s, int v) {
  if (s->top < STACK_MAX - 1) s->items[++s->top] = v;
}
static int pop(Stack* s) {
  if (s->top >= 0) return s->items[s->top--];
  return -1;
}
static int peek(Stack* s) {
  if (s->top >= 0) return s->items[s->top];
  return -1;
}

static char* strdupsafe(const char* s) {
  if (!s) return NULL;
  char* d = malloc(strlen(s) + 1);
  strcpy(d, s);
  return d;
}

static int ends_with_literal(const char* s, const char* suffix) {
  size_t s_len;
  size_t suffix_len;
  if (!s || !suffix) return 0;
  s_len = strlen(s);
  suffix_len = strlen(suffix);
  return s_len >= suffix_len &&
         strcmp(s + s_len - suffix_len, suffix) == 0;
}

static void emit_tac_line(FILE* out, const char* fmt, ...) {
  va_list args;
  if (!out || !fmt) return;
  va_start(args, fmt);
  vfprintf(out, fmt, args);
  va_end(args);
  fputc('\n', out);
}

static const char* scheduler_mode_name(SpecialSchedulerMode mode) {
  return mode == SPECIAL_SCHED_SRT ? "SRT" : "RR2";
}

static void emit_special_scheduler_header(FILE* out,
                                          SpecialSchedulerMode mode) {
  int i;
  emit_tac_line(out, "; low-level scheduler demo for variant 19 (%s)",
                scheduler_mode_name(mode));
  emit_tac_line(out, "; generated from input.txt by parser special mode");
  emit_tac_line(out, "[section ram, code]");
  emit_tac_line(out, "");
  emit_tac_line(out, "main:");
  emit_tac_line(out, "    mov     sp, #%d", SPECIAL_KERNEL_STACK_TOP);
  emit_tac_line(out, "    mov     fp, sp");
  emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_CURRENT_TASK);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_FINISHED_COUNT);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUANTUM_USED);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUEUE_HEAD);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUEUE_TAIL);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_PENDING_TASK);
  emit_tac_line(out, "");

  for (i = 0; i < SPECIAL_TASK_COUNT; ++i) {
    int off = i * 4;
    emit_tac_line(out, "    mov     [%d], #%d", SPECIAL_ARR_BASE + off,
                  kVariant19Tasks[i].arrival_time);
    emit_tac_line(out, "    mov     [%d], #%d", SPECIAL_BURST_BASE + off,
                  kVariant19Tasks[i].service_time);
    emit_tac_line(out, "    mov     [%d], #%d", SPECIAL_REM_BASE + off,
                  kVariant19Tasks[i].service_time);
    emit_tac_line(out, "    mov     [%d], #0", SPECIAL_STATE_BASE + off);
    emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_COMP_BASE + off);
  }

  emit_tac_line(out, "");
  for (i = 0; i < SPECIAL_TASK_COUNT; ++i) {
    int stack_top = SPECIAL_TASK_STACK_BASE - i * SPECIAL_TASK_STACK_STRIDE;
    emit_tac_line(out, "    mov     r1, task_loop");
    emit_tac_line(out, "    mov     r2, #%d", stack_top);
    emit_tac_line(out, "    mov     r3, #%d", SPECIAL_CTX_BASE + i * 4);
    emit_tac_line(out, "    call    init_context");
  }

  emit_tac_line(out, "    mov     r1, idle_loop");
  emit_tac_line(out, "    mov     r2, #%d", SPECIAL_IDLE_STACK_TOP);
  emit_tac_line(out, "    mov     r3, #%d",
                SPECIAL_CTX_BASE + SPECIAL_IDLE_TASK * 4);
  emit_tac_line(out, "    call    init_context");
  emit_tac_line(out, "");

  {
    const char* header = mode == SPECIAL_SCHED_SRT ? "ALG=SRT" : "ALG=RR2";
    const char* trace = "TRACE=";
    const char* p = header;
    while (*p) {
      emit_tac_line(out, "    out     #%d", (unsigned char)*p);
      ++p;
    }
    emit_tac_line(out, "    out     #10");
    p = trace;
    while (*p) {
      emit_tac_line(out, "    out     #%d", (unsigned char)*p);
      ++p;
    }
    emit_tac_line(out, "    out     #10");
  }

  emit_tac_line(out, "    mov     [%d], irq_handler", SPECIAL_MMIO_HANDLER);
  emit_tac_line(out, "    mov     [%d], #1", SPECIAL_MMIO_TIMER_PERIOD);
  emit_tac_line(out, "    mov     [%d], #1", SPECIAL_MMIO_IRQ_ENABLE);
  emit_tac_line(out, "    trap");
  emit_tac_line(out, "    halt");
  emit_tac_line(out, "");
}

static void emit_special_context_init(FILE* out) {
  emit_tac_line(out, "init_context:");
  emit_tac_line(out, "    sub     r2, r2, #%d", SPECIAL_CONTEXT_FRAME_BYTES);
  emit_tac_line(out, "    mov     [r3], r2");
  emit_tac_line(out, "    mov     r4, #32");
  emit_tac_line(out, "    mov     r5, r2");
  emit_tac_line(out, "init_context_zero_loop:");
  emit_tac_line(out, "    mov     [r5], #0");
  emit_tac_line(out, "    add     r5, r5, #4");
  emit_tac_line(out, "    sub     r4, r4, #1");
  emit_tac_line(out, "    cmp     r4, #0");
  emit_tac_line(out, "    bgt     init_context_zero_loop");
  emit_tac_line(out, "    mov     [r2], r1");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");
}

static void emit_special_print_helpers(FILE* out) {
  emit_tac_line(out, "print_int:");
  emit_tac_line(out, "    cmp     r1, #0");
  emit_tac_line(out, "    bne     print_int_nonzero");
  emit_tac_line(out, "    out     #48");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "print_int_nonzero:");
  emit_tac_line(out, "    mov     r2, #1");
  emit_tac_line(out, "    mov     r3, r1");
  emit_tac_line(out, "print_int_find_div:");
  emit_tac_line(out, "    div     r4, r3, #10");
  emit_tac_line(out, "    cmp     r4, #0");
  emit_tac_line(out, "    ble     print_int_emit");
  emit_tac_line(out, "    mov     r3, r4");
  emit_tac_line(out, "    mul     r2, r2, #10");
  emit_tac_line(out, "    jmp     print_int_find_div");
  emit_tac_line(out, "print_int_emit:");
  emit_tac_line(out, "    div     r4, r1, r2");
  emit_tac_line(out, "    add     r5, r4, #48");
  emit_tac_line(out, "    out     r5");
  emit_tac_line(out, "    mul     r6, r4, r2");
  emit_tac_line(out, "    sub     r1, r1, r6");
  emit_tac_line(out, "    div     r2, r2, #10");
  emit_tac_line(out, "    cmp     r2, #0");
  emit_tac_line(out, "    bgt     print_int_emit");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");

  emit_tac_line(out, "print_scaled100:");
  emit_tac_line(out, "    mov     r8, r1");
  emit_tac_line(out, "    div     r2, r1, #100");
  emit_tac_line(out, "    mov     r9, r2");
  emit_tac_line(out, "    mov     r1, r2");
  emit_tac_line(out, "    call    print_int");
  emit_tac_line(out, "    out     #46");
  emit_tac_line(out, "    mul     r3, r9, #100");
  emit_tac_line(out, "    sub     r4, r8, r3");
  emit_tac_line(out, "    cmp     r4, #10");
  emit_tac_line(out, "    bge     print_scaled100_tail");
  emit_tac_line(out, "    out     #48");
  emit_tac_line(out, "print_scaled100_tail:");
  emit_tac_line(out, "    mov     r1, r4");
  emit_tac_line(out, "    call    print_int");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");
}

static void emit_special_report(FILE* out) {
  emit_tac_line(out, "report:");
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_SUM_TA);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_SUM_WT);
  emit_tac_line(out, "    mov     r10, #0");
  emit_tac_line(out, "report_loop:");
  emit_tac_line(out, "    cmp     r10, #%d", SPECIAL_TASK_COUNT);
  emit_tac_line(out, "    bge     report_done");
  emit_tac_line(out, "    mul     r11, r10, #4");
  emit_tac_line(out, "    add     r12, r11, #%d", SPECIAL_ARR_BASE);
  emit_tac_line(out, "    mov     r1, [r12]");
  emit_tac_line(out, "    add     r12, r11, #%d", SPECIAL_BURST_BASE);
  emit_tac_line(out, "    mov     r2, [r12]");
  emit_tac_line(out, "    add     r12, r11, #%d", SPECIAL_COMP_BASE);
  emit_tac_line(out, "    mov     r3, [r12]");
  emit_tac_line(out, "    sub     r4, r3, r1");
  emit_tac_line(out, "    sub     r5, r4, r2");
  emit_tac_line(out, "    mov     r6, [%d]", SPECIAL_SUM_TA);
  emit_tac_line(out, "    add     r6, r6, r4");
  emit_tac_line(out, "    mov     [%d], r6", SPECIAL_SUM_TA);
  emit_tac_line(out, "    mov     r6, [%d]", SPECIAL_SUM_WT);
  emit_tac_line(out, "    add     r6, r6, r5");
  emit_tac_line(out, "    mov     [%d], r6", SPECIAL_SUM_WT);
  emit_tac_line(out, "    add     r10, r10, #1");
  emit_tac_line(out, "    jmp     report_loop");
  emit_tac_line(out, "report_done:");

  {
    const char* tag_ta = "AVG_TA=";
    const char* tag_wt = "AVG_WT=";
    const char* p = tag_ta;
    while (*p) {
      emit_tac_line(out, "    out     #%d", (unsigned char)*p);
      ++p;
    }
    emit_tac_line(out, "    mov     r1, [%d]", SPECIAL_SUM_TA);
    emit_tac_line(out, "    mul     r1, r1, #100");
    emit_tac_line(out, "    add     r1, r1, #%d", SPECIAL_TASK_COUNT / 2);
    emit_tac_line(out, "    div     r1, r1, #%d", SPECIAL_TASK_COUNT);
    emit_tac_line(out, "    call    print_scaled100");
    emit_tac_line(out, "    out     #10");

    p = tag_wt;
    while (*p) {
      emit_tac_line(out, "    out     #%d", (unsigned char)*p);
      ++p;
    }
    emit_tac_line(out, "    mov     r1, [%d]", SPECIAL_SUM_WT);
    emit_tac_line(out, "    mul     r1, r1, #100");
    emit_tac_line(out, "    add     r1, r1, #%d", SPECIAL_TASK_COUNT / 2);
    emit_tac_line(out, "    div     r1, r1, #%d", SPECIAL_TASK_COUNT);
    emit_tac_line(out, "    call    print_scaled100");
    emit_tac_line(out, "    out     #10");
    emit_tac_line(out, "    ret");
    emit_tac_line(out, "");
  }
}

static void emit_special_admit(FILE* out) {
  emit_tac_line(out, "admit_tasks:");
  emit_tac_line(out, "    mov     r10, #0");
  emit_tac_line(out, "admit_tasks_loop:");
  emit_tac_line(out, "    cmp     r10, #%d", SPECIAL_TASK_COUNT);
  emit_tac_line(out, "    bge     admit_tasks_done");
  emit_tac_line(out, "    mul     r11, r10, #4");
  emit_tac_line(out, "    add     r12, r11, #%d", SPECIAL_STATE_BASE);
  emit_tac_line(out, "    mov     r13, [r12]");
  emit_tac_line(out, "    cmp     r13, #0");
  emit_tac_line(out, "    bne     admit_tasks_next");
  emit_tac_line(out, "    add     r14, r11, #%d", SPECIAL_ARR_BASE);
  emit_tac_line(out, "    mov     r15, [r14]");
  emit_tac_line(out, "    mov     r16, [%d]", SPECIAL_MMIO_TIME);
  emit_tac_line(out, "    cmp     r15, r16");
  emit_tac_line(out, "    bgt     admit_tasks_next");
  emit_tac_line(out, "    mov     [r12], #1");
  emit_tac_line(out, "admit_tasks_next:");
  emit_tac_line(out, "    add     r10, r10, #1");
  emit_tac_line(out, "    jmp     admit_tasks_loop");
  emit_tac_line(out, "admit_tasks_done:");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");
}

static void emit_special_rr_helpers(FILE* out) {
  emit_tac_line(out, "enqueue:");
  emit_tac_line(out, "    mov     r2, [%d]", SPECIAL_QUEUE_TAIL);
  emit_tac_line(out, "    mul     r3, r2, #4");
  emit_tac_line(out, "    add     r3, r3, #%d", SPECIAL_QUEUE_BASE);
  emit_tac_line(out, "    mov     [r3], r1");
  emit_tac_line(out, "    add     r2, r2, #1");
  emit_tac_line(out, "    cmp     r2, #%d", SPECIAL_QUEUE_CAPACITY);
  emit_tac_line(out, "    blt     enqueue_store_tail");
  emit_tac_line(out, "    mov     r2, #0");
  emit_tac_line(out, "enqueue_store_tail:");
  emit_tac_line(out, "    mov     [%d], r2", SPECIAL_QUEUE_TAIL);
  emit_tac_line(out, "    mov     r4, [%d]", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    add     r4, r4, #1");
  emit_tac_line(out, "    mov     [%d], r4", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");

  emit_tac_line(out, "dequeue:");
  emit_tac_line(out, "    mov     r2, [%d]", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    cmp     r2, #0");
  emit_tac_line(out, "    bgt     dequeue_nonempty");
  emit_tac_line(out, "    mov     r1, #-1");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "dequeue_nonempty:");
  emit_tac_line(out, "    mov     r3, [%d]", SPECIAL_QUEUE_HEAD);
  emit_tac_line(out, "    mul     r4, r3, #4");
  emit_tac_line(out, "    add     r4, r4, #%d", SPECIAL_QUEUE_BASE);
  emit_tac_line(out, "    mov     r1, [r4]");
  emit_tac_line(out, "    add     r3, r3, #1");
  emit_tac_line(out, "    cmp     r3, #%d", SPECIAL_QUEUE_CAPACITY);
  emit_tac_line(out, "    blt     dequeue_store_head");
  emit_tac_line(out, "    mov     r3, #0");
  emit_tac_line(out, "dequeue_store_head:");
  emit_tac_line(out, "    mov     [%d], r3", SPECIAL_QUEUE_HEAD);
  emit_tac_line(out, "    sub     r2, r2, #1");
  emit_tac_line(out, "    mov     [%d], r2", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");

  emit_tac_line(out, "pick_rr:");
  emit_tac_line(out, "    mov     r1, [%d]", SPECIAL_CURRENT_TASK);
  emit_tac_line(out, "    cmp     r1, #0");
  emit_tac_line(out, "    blt     pick_rr_new");
  emit_tac_line(out, "    cmp     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    beq     pick_rr_idle");
  emit_tac_line(out, "    mul     r2, r1, #4");
  emit_tac_line(out, "    add     r2, r2, #%d", SPECIAL_STATE_BASE);
  emit_tac_line(out, "    mov     r3, [r2]");
  emit_tac_line(out, "    cmp     r3, #2");
  emit_tac_line(out, "    beq     pick_rr_done");
  emit_tac_line(out, "pick_rr_new:");
  emit_tac_line(out, "    call    dequeue");
  emit_tac_line(out, "    cmp     r1, #0");
  emit_tac_line(out, "    bge     pick_rr_real");
  emit_tac_line(out, "    mov     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "pick_rr_real:");
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUANTUM_USED);
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "pick_rr_idle:");
  emit_tac_line(out, "    mov     r2, [%d]", SPECIAL_QUEUE_COUNT);
  emit_tac_line(out, "    cmp     r2, #0");
  emit_tac_line(out, "    bgt     pick_rr_new");
  emit_tac_line(out, "    mov     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "pick_rr_done:");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");
}

static void emit_special_srt_helpers(FILE* out) {
  emit_tac_line(out, "pick_srt:");
  emit_tac_line(out, "    mov     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    mov     r2, #9999");
  emit_tac_line(out, "    mov     r10, #0");
  emit_tac_line(out, "pick_srt_loop:");
  emit_tac_line(out, "    cmp     r10, #%d", SPECIAL_TASK_COUNT);
  emit_tac_line(out, "    bge     pick_srt_done");
  emit_tac_line(out, "    mul     r11, r10, #4");
  emit_tac_line(out, "    add     r12, r11, #%d", SPECIAL_STATE_BASE);
  emit_tac_line(out, "    mov     r13, [r12]");
  emit_tac_line(out, "    cmp     r13, #1");
  emit_tac_line(out, "    bne     pick_srt_next");
  emit_tac_line(out, "    add     r14, r11, #%d", SPECIAL_REM_BASE);
  emit_tac_line(out, "    mov     r15, [r14]");
  emit_tac_line(out, "    cmp     r15, r2");
  emit_tac_line(out, "    blt     pick_srt_take");
  emit_tac_line(out, "    jmp     pick_srt_next");
  emit_tac_line(out, "pick_srt_take:");
  emit_tac_line(out, "    mov     r2, r15");
  emit_tac_line(out, "    mov     r1, r10");
  emit_tac_line(out, "pick_srt_next:");
  emit_tac_line(out, "    add     r10, r10, #1");
  emit_tac_line(out, "    jmp     pick_srt_loop");
  emit_tac_line(out, "pick_srt_done:");
  emit_tac_line(out, "    ret");
  emit_tac_line(out, "");
}

static void emit_special_irq_handler(FILE* out, SpecialSchedulerMode mode) {
  emit_tac_line(out, "irq_handler:");
  emit_tac_line(out, "    mov     sp, #%d", SPECIAL_KERNEL_STACK_TOP);
  emit_tac_line(out, "    mov     fp, sp");
  emit_tac_line(out, "    mov     r1, [%d]", SPECIAL_MMIO_IRQ_CAUSE);
  emit_tac_line(out, "    cmp     r1, #1");
  emit_tac_line(out, "    bne     irq_after_account");
  emit_tac_line(out, "    mov     r2, [%d]", SPECIAL_CURRENT_TASK);
  emit_tac_line(out, "    cmp     r2, #0");
  emit_tac_line(out, "    blt     irq_after_account");
  emit_tac_line(out, "    mul     r3, r2, #4");
  emit_tac_line(out, "    add     r3, r3, #%d", SPECIAL_CTX_BASE);
  emit_tac_line(out, "    mov     r4, [%d]", SPECIAL_MMIO_FRAME_SP);
  emit_tac_line(out, "    mov     [r3], r4");
  emit_tac_line(out, "    cmp     r2, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    beq     irq_after_account");
  emit_tac_line(out, "    mul     r5, r2, #4");
  emit_tac_line(out, "    add     r6, r5, #%d", SPECIAL_REM_BASE);
  emit_tac_line(out, "    mov     r7, [r6]");
  emit_tac_line(out, "    sub     r7, r7, #1");
  emit_tac_line(out, "    mov     [r6], r7");
  emit_tac_line(out, "    cmp     r7, #0");
  emit_tac_line(out, "    beq     irq_task_done");

  if (mode == SPECIAL_SCHED_RR2) {
    emit_tac_line(out, "    mov     r8, [%d]", SPECIAL_QUANTUM_USED);
    emit_tac_line(out, "    add     r8, r8, #1");
    emit_tac_line(out, "    mov     [%d], r8", SPECIAL_QUANTUM_USED);
    emit_tac_line(out, "    cmp     r8, #2");
    emit_tac_line(out, "    blt     irq_rr_keep_running");
    emit_tac_line(out, "    add     r9, r5, #%d", SPECIAL_STATE_BASE);
    emit_tac_line(out, "    mov     [r9], #1");
    emit_tac_line(out, "    mov     [%d], r2", SPECIAL_PENDING_TASK);
    emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_CURRENT_TASK);
    emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUANTUM_USED);
    emit_tac_line(out, "    jmp     irq_after_account");
    emit_tac_line(out, "irq_rr_keep_running:");
    emit_tac_line(out, "    add     r9, r5, #%d", SPECIAL_STATE_BASE);
    emit_tac_line(out, "    mov     [r9], #2");
    emit_tac_line(out, "    jmp     irq_after_account");
  } else {
    emit_tac_line(out, "    add     r9, r5, #%d", SPECIAL_STATE_BASE);
    emit_tac_line(out, "    mov     [r9], #1");
    emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_CURRENT_TASK);
    emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUANTUM_USED);
    emit_tac_line(out, "    jmp     irq_after_account");
  }

  emit_tac_line(out, "irq_task_done:");
  emit_tac_line(out, "    add     r9, r5, #%d", SPECIAL_STATE_BASE);
  emit_tac_line(out, "    mov     [r9], #3");
  emit_tac_line(out, "    add     r10, r5, #%d", SPECIAL_COMP_BASE);
  emit_tac_line(out, "    mov     r11, [%d]", SPECIAL_MMIO_TIME);
  emit_tac_line(out, "    mov     [r10], r11");
  emit_tac_line(out, "    mov     r12, [%d]", SPECIAL_FINISHED_COUNT);
  emit_tac_line(out, "    add     r12, r12, #1");
  emit_tac_line(out, "    mov     [%d], r12", SPECIAL_FINISHED_COUNT);
  emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_CURRENT_TASK);
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_QUANTUM_USED);
  emit_tac_line(out, "irq_after_account:");
  emit_tac_line(out, "    call    admit_tasks");

  if (mode == SPECIAL_SCHED_RR2) {
    emit_tac_line(out, "    mov     r14, [%d]", SPECIAL_PENDING_TASK);
    emit_tac_line(out, "    cmp     r14, #0");
    emit_tac_line(out, "    blt     irq_no_pending");
    emit_tac_line(out, "    mov     r1, r14");
    emit_tac_line(out, "    call    enqueue");
    emit_tac_line(out, "    mov     [%d], #-1", SPECIAL_PENDING_TASK);
    emit_tac_line(out, "irq_no_pending:");
  }

  emit_tac_line(out, "    mov     r1, [%d]", SPECIAL_FINISHED_COUNT);
  emit_tac_line(out, "    cmp     r1, #%d", SPECIAL_TASK_COUNT);
  emit_tac_line(out, "    beq     irq_all_done");

  if (mode == SPECIAL_SCHED_RR2) {
    emit_tac_line(out, "    call    pick_rr");
  } else {
    emit_tac_line(out, "    call    pick_srt");
  }

  emit_tac_line(out, "    mov     [%d], r1", SPECIAL_CURRENT_TASK);
  emit_tac_line(out, "    cmp     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    beq     irq_emit_trace");
  emit_tac_line(out, "    mul     r2, r1, #4");
  emit_tac_line(out, "    add     r3, r2, #%d", SPECIAL_STATE_BASE);
  emit_tac_line(out, "    mov     [r3], #2");
  emit_tac_line(out, "irq_emit_trace:");
  emit_tac_line(out, "    cmp     r1, #%d", SPECIAL_IDLE_TASK);
  emit_tac_line(out, "    beq     irq_emit_idle");
  emit_tac_line(out, "    add     r2, r1, #65");
  emit_tac_line(out, "    out     r2");
  emit_tac_line(out, "    jmp     irq_restore");
  emit_tac_line(out, "irq_emit_idle:");
  emit_tac_line(out, "    out     #95");
  emit_tac_line(out, "irq_restore:");
  emit_tac_line(out, "    mul     r3, r1, #4");
  emit_tac_line(out, "    add     r3, r3, #%d", SPECIAL_CTX_BASE);
  emit_tac_line(out, "    mov     sp, [r3]");
  emit_tac_line(out, "    iret");
  emit_tac_line(out, "irq_all_done:");
  emit_tac_line(out, "    out     #10");
  emit_tac_line(out, "    mov     [%d], #0", SPECIAL_MMIO_IRQ_ENABLE);
  emit_tac_line(out, "    mov     sp, #%d", SPECIAL_KERNEL_STACK_TOP);
  emit_tac_line(out, "    mov     fp, sp");
  emit_tac_line(out, "    call    report");
  emit_tac_line(out, "    halt");
  emit_tac_line(out, "");
}

static void emit_special_task_loops(FILE* out) {
  emit_tac_line(out, "task_loop:");
  emit_tac_line(out, "    jmp     task_loop");
  emit_tac_line(out, "");
  emit_tac_line(out, "idle_loop:");
  emit_tac_line(out, "    jmp     idle_loop");
  emit_tac_line(out, "");
}

static int write_special_scheduler_tac(const char* tac_out,
                                       SpecialSchedulerMode mode) {
  FILE* out = fopen(tac_out, "wb");
  if (!out) {
    fprintf(stderr, "Cannot open output file '%s'\n", tac_out);
    return -1;
  }

  emit_special_scheduler_header(out, mode);
  emit_special_context_init(out);
  emit_special_print_helpers(out);
  emit_special_admit(out);
  if (mode == SPECIAL_SCHED_RR2) {
    emit_special_rr_helpers(out);
  } else {
    emit_special_srt_helpers(out);
  }
  emit_special_report(out);
  emit_special_irq_handler(out, mode);
  emit_special_task_loops(out);

  fclose(out);
  return 0;
}

static void write_special_program_dgml(const char* mode, const char* out_path) {
  FILE* out;
  if (!out_path) return;
  out = fopen(out_path, "wb");
  if (!out) {
    fprintf(stderr, "Cannot open output file '%s'\n", out_path);
    return;
  }
  fprintf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  fprintf(out,
          "<DirectedGraph Title=\"SpecialProgram\" "
          "xmlns=\"http://schemas.microsoft.com/vs/2009/dgml\">\n");
  fprintf(out, "  <Nodes>\n");
  fprintf(out, "    <Node Id=\"n1\" Label=\"scheduler_variant19 %s\" />\n",
          mode ? mode : "rr2");
  fprintf(out, "  </Nodes>\n");
  fprintf(out, "</DirectedGraph>\n");
  fclose(out);
}

static int try_generate_special_scheduler(const char* input_file,
                                          const char* tac_out,
                                          const char* dgml_out) {
  /* Legacy shortcut disabled: scheduler now goes through normal parsing. */
  (void)input_file;
  (void)tac_out;
  (void)dgml_out;
  return 0;

  FILE* in = NULL;
  char buffer[256];
  char mode[32] = "rr2";
  char* text;
  char* rest;
  char* end;
  SpecialSchedulerMode scheduler_mode = SPECIAL_SCHED_RR2;

  in = fopen(input_file, "rb");
  if (!in) return -1;
  memset(buffer, 0, sizeof(buffer));
  fread(buffer, 1, sizeof(buffer) - 1, in);
  fclose(in);

  text = buffer;
  if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF) {
    text += 3;
  }

  text = trim_ascii(text);
  if (strncmp(text, "scheduler_variant19", 19) != 0) {
    return 0;
  }

  rest = text + 19;
  rest = trim_ascii(rest);

  if (*rest == '(') {
    rest++;
    end = rest;
    while (*end && *end != ')' && *end != ';' &&
           !isspace((unsigned char)*end)) {
      end++;
    }
    if (end > rest) {
      size_t len = (size_t)(end - rest);
      if (len >= sizeof(mode)) len = sizeof(mode) - 1;
      memcpy(mode, rest, len);
      mode[len] = '\0';
    }
  } else if (*rest && *rest != ';') {
    end = rest;
    while (*end && *end != ';' && !isspace((unsigned char)*end)) end++;
    if (end > rest) {
      size_t len = (size_t)(end - rest);
      if (len >= sizeof(mode)) len = sizeof(mode) - 1;
      memcpy(mode, rest, len);
      mode[len] = '\0';
    }
  }

  for (char* p = mode; *p; ++p) {
    *p = (char)tolower((unsigned char)*p);
  }

  if (strcmp(mode, "rr2") == 0 || strcmp(mode, "rr") == 0) {
    strcpy(mode, "rr2");
    scheduler_mode = SPECIAL_SCHED_RR2;
  } else if (strcmp(mode, "srt") == 0) {
    scheduler_mode = SPECIAL_SCHED_SRT;
  } else if (strcmp(mode, "") == 0) {
    strcpy(mode, "rr2");
    scheduler_mode = SPECIAL_SCHED_RR2;
  } else {
    fprintf(stderr,
            "Unsupported scheduler_variant19 mode '%s'. Supported: rr2, srt\n",
            mode);
    return -1;
  }

  if (write_special_scheduler_tac(tac_out, scheduler_mode) != 0) {
    return -1;
  }

  write_special_program_dgml(mode, dgml_out);
  fprintf(stderr, "Special pipeline: scheduler_variant19 %s -> generated %s\n",
          mode, tac_out);
  return 1;
}

/* прототип генерации TAC (реализация в cfg_builder.c) */
void generate_tac_assembly(AnalysisResult* res, const char* outfile);

static void write_xml_escaped_label(const char* s, FILE* out) {
  if (!s) return;

  int len = (int)strlen(s);
  const char* start = s;

  // Если строка вида "..." — уберём внешние кавычки
  if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
    start = s + 1;
    len -= 2;
  }

  // Теперь экранируем содержимое (но без внешних кавычек)
  for (int i = 0; i < len; ++i) {
    char c = start[i];
    switch (c) {
      case '&':
        fputs("&amp;", out);
        break;
      case '<':
        fputs("&lt;", out);
        break;
      case '>':
        fputs("&gt;", out);
        break;
      case '"':
        fputs("&quot;", out);
        break;
      case '\'':
        fputs("&apos;", out);
        break;
      default:
        fputc(c, out);
        break;
    }
  }
}

/* --- Построение DGML для дерева разбора --- */
static void write_dgml_from_toStringTree(const char* s, FILE* out) {
  if (!s) return;

  fprintf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  fprintf(out,
          "<DirectedGraph Title=\"ParseTree\" "
          "xmlns=\"http://schemas.microsoft.com/vs/2009/dgml\">\n");
  fprintf(out, "  <Nodes>\n");

  int node_id = 1;
  const char* p = s;
  Stack stack = {.top = -1};

  Link links[STACK_MAX];
  int link_count = 0;

  char label[LABEL_MAX];
  int in_label = 0;
  int cur_node = 0;

  while (*p) {
    if (*p == '(') {
      p++;
      label[0] = 0;
      in_label = 0;
      while (*p && *p != '(' && *p != ')' && !isspace((unsigned char)*p)) {
        int len = (int)strlen(label);
        if (len < LABEL_MAX - 1) {
          label[len] = *p;
          label[len + 1] = 0;
        }
        in_label = 1;
        p++;
      }
      if (in_label) {
        cur_node = node_id++;
        fprintf(out, "    <Node Id=\"n%d\" Label=\"", cur_node);
        write_xml_escaped_label(label, out);
        fprintf(out, "\" />\n");
        int parent = peek(&stack);
        if (parent != -1) {
          links[link_count].parent = parent;
          links[link_count].child = cur_node;
          link_count++;
        }
        push(&stack, cur_node);
      } else {
        push(&stack, -2);
      }
    } else if (*p == ')') {
      pop(&stack);
      p++;
    } else if (!isspace((unsigned char)*p)) {
      label[0] = 0;
      while (*p && *p != '(' && *p != ')' && !isspace((unsigned char)*p)) {
        int len = (int)strlen(label);
        if (len < LABEL_MAX - 1) {
          label[len] = *p;
          label[len + 1] = 0;
        }
        p++;
      }
      cur_node = node_id++;
      fprintf(out, "    <Node Id=\"n%d\" Label=\"", cur_node);
      write_xml_escaped_label(label, out);
      fprintf(out, "\" />\n");
      int parent = peek(&stack);
      if (parent != -1) {
        links[link_count].parent = parent;
        links[link_count].child = cur_node;
        link_count++;
      }
    } else {
      p++;
    }
  }

  fprintf(out, "  </Nodes>\n");
  fprintf(out, "  <Links>\n");
  for (int i = 0; i < link_count; ++i) {
    fprintf(out, "    <Link Source=\"n%d\" Target=\"n%d\" />\n",
            links[i].parent, links[i].child);
  }
  fprintf(out, "  </Links>\n");
  fprintf(out, "</DirectedGraph>\n");
}

static int antlr_tree_is_nil_node(pANTLR3_BASE_TREE tree) {
  pANTLR3_STRING text = NULL;
  if (!tree) return 0;
  if (tree->isNilNode && tree->isNilNode(tree)) return 1;
  if (!tree->toString) return 0;
  text = tree->toString(tree);
  if (!text || !text->chars) return 0;
  return strcmp((const char*)text->chars, "nil") == 0;
}

static void write_dgml_tree_links(FILE* out, pANTLR3_BASE_TREE tree,
                                  int parent_id, int* next_id) {
  ANTLR3_UINT32 child_count = 0;
  if (!out || !tree || !next_id) return;

  child_count = tree->getChildCount ? tree->getChildCount(tree) : 0;
  for (ANTLR3_UINT32 i = 0; i < child_count; ++i) {
    pANTLR3_BASE_TREE child =
        (pANTLR3_BASE_TREE)(tree->getChild ? tree->getChild(tree, i) : NULL);
    pANTLR3_STRING text = NULL;
    int current_id = parent_id;

    if (!child) continue;
    if (!antlr_tree_is_nil_node(child)) {
      text = child->toString ? child->toString(child) : NULL;
      current_id = (*next_id)++;
      fprintf(out, "    <Node Id=\"n%d\" Label=\"", current_id);
      write_xml_escaped_label((text && text->chars) ? (const char*)text->chars : "", out);
      fprintf(out, "\" />\n");
      if (parent_id > 0) {
        fprintf(out, "    <Link Source=\"n%d\" Target=\"n%d\" />\n", parent_id,
                current_id);
      }
    }

    write_dgml_tree_links(out, child, current_id, next_id);
  }
}

static void write_dgml_from_antlr_tree(pANTLR3_BASE_TREE tree, FILE* out) {
  pANTLR3_STRING text = NULL;
  int next_id = 2;
  if (!tree || !out) return;

  fprintf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  fprintf(out,
          "<DirectedGraph Title=\"ParseTree\" "
          "xmlns=\"http://schemas.microsoft.com/vs/2009/dgml\">\n");
  fprintf(out, "  <Nodes>\n");

  text = tree->toString ? tree->toString(tree) : NULL;
  fprintf(out, "    <Node Id=\"n1\" Label=\"");
  write_xml_escaped_label((text && text->chars) ? (const char*)text->chars : "", out);
  fprintf(out, "\" />\n");
  write_dgml_tree_links(out, tree, 1, &next_id);

  fprintf(out, "  </Nodes>\n");
  fprintf(out, "  <Links>\n");
  {
    int dummy_id = 2;
    ANTLR3_UINT32 child_count = tree->getChildCount ? tree->getChildCount(tree) : 0;
    for (ANTLR3_UINT32 i = 0; i < child_count; ++i) {
      pANTLR3_BASE_TREE child =
          (pANTLR3_BASE_TREE)(tree->getChild ? tree->getChild(tree, i) : NULL);
      if (child && !antlr_tree_is_nil_node(child)) {
        fprintf(out, "    <Link Source=\"n1\" Target=\"n%d\" />\n", dummy_id++);
      }
    }
  }
  fprintf(out, "  </Links>\n");
  fprintf(out, "</DirectedGraph>\n");
}

/* --- Обработчик ошибок парсера --- */
static void parser_error_display(pANTLR3_BASE_RECOGNIZER recognizer,
                                 pANTLR3_UINT8* tokenNames) {
  (void)tokenNames;  // не используем, чтобы не ругался компилятор
  g_parse_error_seen = 1;

  if (!recognizer || !recognizer->state) {
    fprintf(stderr, "Syntax error (unknown recognizer state)\n");
    return;
  }

  pANTLR3_EXCEPTION ex = recognizer->state->exception;

  if (!ex) {
    fprintf(stderr, "Syntax error: unknown location\n");
    return;
  }

  pANTLR3_UINT8 msg =
      ex->message ? (pANTLR3_UINT8)ex->message : (pANTLR3_UINT8) "(no message)";

  const char* streamName = NULL;
  if (ex->streamName && ex->streamName->chars) {
    streamName = (const char*)ex->streamName->chars;
  }

  ANTLR3_UINT32 line = (ANTLR3_UINT32)ex->line;
  ANTLR3_INT32 pos = (ANTLR3_INT32)ex->charPositionInLine;

  if (streamName) {
    fprintf(stderr, "Syntax error in %s", streamName);
  } else {
    fprintf(stderr, "Syntax error");
  }

  if (line || pos >= 0) {
    fprintf(stderr, " at line %u:%d", (unsigned)line, (int)pos);
  }

  fprintf(stderr, ": %s\n", (const char*)msg);

  if (ex->nextException) {
    pANTLR3_EXCEPTION nex = ex->nextException;
    pANTLR3_UINT8 nmsg = nex->message ? (pANTLR3_UINT8)nex->message
                                      : (pANTLR3_UINT8) "(no message)";
    fprintf(stderr, "  (follow-up): %s\n", (const char*)nmsg);
  }
}

/* --- Главная функция --- */
int main(int argc, char* argv[]) {
  const char* asm_output_file = NULL;
  const char* dgml_output_file = NULL;
  const char* input_file = NULL;
  const char* progress_file = NULL;
  const char* target_name = "tacvm13";
  char asm_out[1024];
  int special_status;
  int parse_only = 0;
  int argi = 1;

  while (argc > argi && argv[argi][0] == '-') {
    if (strcmp(argv[argi], "--parse-only") == 0) {
      parse_only = 1;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--progress-file") == 0) {
      if (argc <= argi + 1) {
        fprintf(stderr, "--progress-file requires a path\n");
        return 1;
      }
      progress_file = argv[argi + 1];
      argi += 2;
      continue;
    }
    if (strcmp(argv[argi], "--target") == 0) {
      if (argc <= argi + 1) {
        fprintf(stderr, "--target requires a backend name\n");
        return 1;
      }
      target_name = argv[argi + 1];
      argi += 2;
      continue;
    }
    break;
  }

  g_progress_file = progress_file;
  note_progress("main: start");

  if (argc <= argi) {
    fprintf(stderr,
            "Usage: %s [--parse-only] [--progress-file path] [--target tacvm13|jvm] <inputfile> [output.asm] [parse_tree.dgml]\n",
            argv[0]);
    return 1;
  }

  input_file = argv[argi++];

  if (argc > argi) {
    if (ends_with_literal(argv[argi], ".dgml")) {
      dgml_output_file = argv[argi];
    } else {
      asm_output_file = argv[argi];
    }
    argi++;
  }
  if (argc > argi) {
    dgml_output_file = argv[argi];
  }

  if (asm_output_file) {
    snprintf(asm_out, sizeof(asm_out), "%s", asm_output_file);
  } else {
    snprintf(asm_out, sizeof(asm_out), "%s.asm", input_file);
  }

  special_status =
      try_generate_special_scheduler(input_file, asm_out, dgml_output_file);
  if (special_status < 0) {
    return 1;
  }
  if (special_status > 0) {
    return 0;
  }

  pANTLR3_INPUT_STREAM input =
      antlr3FileStreamNew((pANTLR3_UINT8)input_file, ANTLR3_ENC_UTF8);
  if (!input) {
    fprintf(stderr, "Cannot open input file '%s'\n", input_file);
    return 1;
  }

  pSimpleLangLexer lexer = SimpleLangLexerNew(input);
  pANTLR3_COMMON_TOKEN_STREAM tokens =
      antlr3CommonTokenStreamSourceNew(ANTLR3_SIZE_HINT, TOKENSOURCE(lexer));
  pSimpleLangParser parser = SimpleLangParserNew(tokens);
  g_parse_error_seen = 0;
  note_progress("main: parser created");

  /* наш обработчик ошибок */
  pANTLR3_BASE_RECOGNIZER base = (pANTLR3_BASE_RECOGNIZER)parser;
  base->displayRecognitionError = parser_error_display;

  note_progress("main: parse begin");
  SimpleLangParser_source_return r = parser->source(parser);
  note_progress("main: parse end");
  {
    const char* debug_progress = getenv("SIMPLELANG_DEBUG_PROGRESS");
    if (debug_progress && strcmp(debug_progress, "0") != 0) {
      fprintf(stderr, "Progress: parse finished\n");
    }
  }

  if (g_parse_error_seen) {
    fprintf(stderr, "Parsing failed\n");
    note_progress("main: parse failed");
    parser->free(parser);
    tokens->free(tokens);
    lexer->free(lexer);
    input->close(input);
    return 2;
  }

  /* --- Построение CFG из дерева парсера --- */
  if (r.tree != NULL) {
    const char* parse_only_env = getenv("SIMPLELANG_PARSE_ONLY");
    int skip_cfg =
        parse_only || (parse_only_env && strcmp(parse_only_env, "0") != 0);
    if (!skip_cfg) {
    SourceFileInfo* sf = malloc(sizeof(SourceFileInfo));
    sf->filename = strdupsafe(input_file);
    sf->parse_tree = (pANTLR3_BASE_TREE)r.tree;
    sf->functions = NULL;
    sf->functions_count = 0;

    SourceFileInfo* files[1];
    files[0] = sf;
    int files_count = 1;

    {
      const char* debug_progress = getenv("SIMPLELANG_DEBUG_PROGRESS");
      if (debug_progress && strcmp(debug_progress, "0") != 0) {
        fprintf(stderr, "Progress: CFG build begin\n");
      }
    }
    note_progress("main: cfg begin");
    AnalysisResult* res = build_cfg_from_parse_trees(files, files_count);
    note_progress("main: cfg end");
    {
      const char* debug_progress = getenv("SIMPLELANG_DEBUG_PROGRESS");
      if (debug_progress && strcmp(debug_progress, "0") != 0) {
        fprintf(stderr, "Progress: CFG build done\n");
      }
    }

    /* 1) CFG + CallGraph DGML (как было) */
    write_cfg_callgraph_dgml(res, "cfg_callgraph");

    /* 2) Target assembly/binary based on CFG */
    note_progress("main: asm begin");
    if (strcmp(target_name, "jvm") == 0) {
      if (generate_jvm_classfile(res, asm_out) != 0) {
        fprintf(stderr, "JVM generation failed\n");
        free_analysis_result(res);
        free(sf->filename);
        free(sf);
        parser->free(parser);
        tokens->free(tokens);
        lexer->free(lexer);
        input->close(input);
        return 3;
      }
    } else if (strcmp(target_name, "tacvm13") == 0 ||
               strcmp(target_name, "tac") == 0) {
      generate_tac_assembly(res, asm_out);
    } else {
      fprintf(stderr, "Unknown target '%s'\n", target_name);
      free_analysis_result(res);
      free(sf->filename);
      free(sf);
      parser->free(parser);
      tokens->free(tokens);
      lexer->free(lexer);
      input->close(input);
      return 3;
    }
    note_progress("main: asm end");
    fprintf(stderr, "Assembly written to %s\n", asm_out);

    free_analysis_result(res);
    free(sf->filename);
    free(sf);
    } else {
      FILE* empty_asm = fopen(asm_out, "wb");
      if (empty_asm) fclose(empty_asm);
      note_progress("main: parse-only asm stub");
    }
  }

  /* --- DGML для parse tree (по toStringTree) --- */
  if (r.tree != NULL) {
    FILE* out = stdout;
    if (dgml_output_file) {
      out = fopen(dgml_output_file, "wb");
      if (!out) {
        fprintf(stderr, "Cannot open output file '%s', writing to stdout\n",
                dgml_output_file);
        out = stdout;
      }
    }

    note_progress("main: dgml begin");
    write_dgml_from_antlr_tree((pANTLR3_BASE_TREE)r.tree, out);
    note_progress("main: dgml end");

    if (out != stdout) fclose(out);
  } else {
    fprintf(stderr, "Parser returned no tree.\n");
  }

  parser->free(parser);
  tokens->free(tokens);
  lexer->free(lexer);
  input->close(input);
  note_progress("main: done");

  return 0;
}
