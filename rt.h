#ifndef UJ_RT_H
#define UJ_RT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Frame Frame;
typedef struct Cont Cont;
typedef void (*Fn)(Frame *);

struct Cont {
  Frame *top;
};

struct Frame {
  Fn fn;
  Fn body;
  int pc;
  int is_handler;
  int affine;
  int ability;
  int req;
  int has_payload;
  uint64_t payload;
  uint64_t pure_value;
  int resume_slot;
  int dest_slot;
  int bargn;
  uint64_t barg[8];
  uint64_t slot[8192];
  Cont *ks[8];
  Frame *next;
  Frame *kont;
};

extern uint64_t uj_result;
extern int uj_halt;
extern Frame *uj_next;

Frame *uj_new(Fn fn);
void uj_run(Frame *start);
void uj_perform(Frame *self, int ability, int req, int has_payload, uint64_t payload, int resume_slot);
void uj_resume(Frame *handler, Cont *k, uint64_t value);
void uj_resume_affine(Frame *handler, uint64_t value);
Frame *uj_link_builtins(Frame *user);
uint64_t uj_ref_new(uint64_t init);
uint64_t uj_ref_read(uint64_t r);
void uj_ref_write(uint64_t r, uint64_t v);
uint64_t uj_tvar_new(uint64_t init);
uint64_t uj_tvar_read(uint64_t t);
uint64_t uj_tvar_read_io(uint64_t t);
void uj_tvar_write(uint64_t t, uint64_t v);
void uj_stm_begin(void);
void uj_stm_commit(void);
void uj_stm_abort(void);
void uj_stm_retry(void);
void uj_resume_ks(Frame *handler, uint64_t value);
static inline void uj_resume_once(Frame *handler, uint64_t value) {
  Frame *top = handler->kont;
  if (!top) {
    uj_resume_ks(handler, value);
    return;
  }
  handler->kont = NULL;
  top->slot[top->resume_slot] = value;
  handler->pc = 2;
}
void uj_return(Frame *self, uint64_t value);
void uj_free_frame(Frame *f);
extern int uj_live_frames;
extern int uj_live_conts;
void uj_stamp(void);
extern struct timespec uj_loop0, uj_loop1;
extern int uj_loop_live;
uint64_t uj_obj(int tag, int n, uint64_t field);
uint64_t uj_obj_n(int tag, int n, const uint64_t *fields);
uint64_t uj_list_empty(void);
uint64_t uj_list_cons(uint64_t head, uint64_t tail);
uint64_t uj_list_view(uint64_t xs);

typedef struct UjClos {
  Fn fn;
  int need;
  int have;
  uint64_t arg[8];
  int plain;
} UjClos;
uint64_t uj_clos(void (*fn)(Frame *), int need);
uint64_t uj_clos_kind(void (*fn)(Frame *), int need, int plain);
uint64_t uj_apply(uint64_t clos, uint64_t arg);
uint64_t uj_call(void (*fn)(Frame *), int n, const uint64_t *a);
uint64_t uj_push(uint64_t clos, uint64_t arg);
void uj_force_clos(Frame *f);
uint64_t uj_text_lit(const char *s);
uint64_t uj_text_nat(uint64_t n);
uint64_t uj_text_cat(uint64_t a, uint64_t b);
uint64_t uj_bytes_of(uint32_t n, const uint8_t *p);
uint64_t uj_bytes_cat(uint64_t a, uint64_t b);
uint64_t uj_bytes_size(uint64_t a);
uint64_t uj_bytes_at(uint64_t missing, uint64_t index, uint64_t a);
uint64_t uj_map_empty(void);
uint64_t uj_map_put(uint64_t key, uint64_t val, uint64_t src);
uint64_t uj_map_size(uint64_t src);
uint64_t uj_map_get(uint64_t missing, uint64_t key, uint64_t src);
uint64_t uj_text_take(uint64_t n, uint64_t v);
uint64_t uj_text_drop(uint64_t n, uint64_t v);
uint64_t uj_text_eq(uint64_t a, uint64_t b);
uint64_t uj_text_size(uint64_t v);
uint64_t uj_text_index(uint64_t needle, uint64_t hay);
uint64_t uj_nat_from_text(uint64_t v);
uint64_t uj_nat_sub(uint64_t a, uint64_t b);
uint64_t uj_read_file(uint64_t path);
uint64_t uj_utf8_decode(uint64_t bytes);
uint64_t uj_text_to_utf8(uint64_t text);
uint64_t uj_write_file(uint64_t path, uint64_t bytes);
int uj_tag(uint64_t v);
static inline int uj_bool(uint64_t v) {
  if (v < 2) return (int)v;
  return uj_tag(v) != 0;
}
uint64_t uj_field(uint64_t v, int i);

#endif
