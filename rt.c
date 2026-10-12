#include "rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t uj_result;
int uj_halt;
Frame *uj_next;
int uj_live_frames;
int uj_live_conts;

static Frame *frame_free;
static Cont *cont_free;
static Frame *uj_alloc_frame(void);
static Frame *uj_fresh(Fn fn);

typedef struct Obj {
  int tag;
  int n;
  uint64_t field[];
} Obj;

static char *bump_p;
static size_t bump_used, bump_cap;

static void *bump_mem(size_t n) {
  n = (n + 7u) & ~(size_t)7u;
  if (bump_used + n > bump_cap) {
    size_t cap = bump_cap ? bump_cap * 2 : (size_t)1 << 20;
    while (cap < bump_used + n) cap *= 2;
    char *p = malloc(cap);
    if (!p) {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
    bump_p = p;
    bump_used = 0;
    bump_cap = cap;
  }
  void *r = bump_p + bump_used;
  bump_used += n;
  memset(r, 0, n);
  return r;
}

uint64_t uj_obj_n(int tag, int n, const uint64_t *fields) {
  if (n < 0) n = 0;
  Obj *o = bump_mem(sizeof(Obj) + (size_t)n * sizeof(uint64_t));
  o->tag = tag;
  o->n = n;
  for (int i = 0; i < n; i++) o->field[i] = fields[i];
  return (uint64_t)(uintptr_t)o;
}

uint64_t uj_obj(int tag, int n, uint64_t field) {
  if (n <= 0) return uj_obj_n(tag, 0, 0);
  return uj_obj_n(tag, 1, &field);
}

typedef struct UjCell {
  uint64_t head;
  uint64_t tail;
} UjCell;

uint64_t uj_list_empty(void) { return 0; }

uint64_t uj_list_cons(uint64_t head, uint64_t tail) {
  UjCell *c = malloc(sizeof(UjCell));
  if (!c) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  c->head = head;
  c->tail = tail;
  return (uint64_t)(uintptr_t)c;
}

uint64_t uj_list_view(uint64_t xs) {
  if (!xs) return uj_obj_n(1, 0, 0);
  UjCell *c = (UjCell *)(uintptr_t)xs;
  uint64_t fs[2];
  fs[0] = c->head;
  fs[1] = c->tail;
  return uj_obj_n(0, 2, fs);
}

uint64_t uj_clos(Fn fn, int need) {
  UjClos *c = bump_mem(sizeof(UjClos));
  c->fn = fn;
  c->need = need;
  return (uint64_t)(uintptr_t)c;
}

uint64_t uj_clos_kind(Fn fn, int need, int plain) {
  uint64_t v = uj_clos(fn, need);
  ((UjClos *)(uintptr_t)v)->plain = plain;
  return v;
}

uint64_t uj_apply(uint64_t clos, uint64_t arg) {
  UjClos *c = (UjClos *)(uintptr_t)clos;
  if (!c || !c->fn) {
    fprintf(stderr, "apply empty\n");
    exit(1);
  }
  if (c->have + 1 < c->need) {
    if (c->have >= 8) {
      fprintf(stderr, "closure full\n");
      exit(1);
    }
    UjClos *n = bump_mem(sizeof(UjClos));
    *n = *c;
    n->arg[n->have] = arg;
    n->have++;
    return (uint64_t)(uintptr_t)n;
  }
  Frame *fr = uj_fresh(c->fn);
  for (int i = 0; i < c->have; i++) fr->slot[i] = c->arg[i];
  fr->slot[c->have] = arg;
  int halt = uj_halt;
  Frame *next = uj_next;
  uint64_t saved = uj_result;
  uj_run(fr);
  uint64_t out = uj_result;
  uj_halt = halt;
  uj_next = next;
  uj_result = saved;
  return out;
}

uint64_t uj_push(uint64_t clos, uint64_t arg) {
  UjClos *c = (UjClos *)(uintptr_t)clos;
  if (!c || !c->fn) {
    fprintf(stderr, "push empty\n");
    exit(1);
  }
  if (c->have >= 8) {
    fprintf(stderr, "closure full\n");
    exit(1);
  }
  UjClos *n = bump_mem(sizeof(UjClos));
  *n = *c;
  n->arg[n->have] = arg;
  n->have++;
  return (uint64_t)(uintptr_t)n;
}

void uj_force_clos(Frame *f) {
  UjClos *c = (UjClos *)(uintptr_t)f->slot[0];
  if (!c || !c->fn) {
    fprintf(stderr, "force empty\n");
    exit(1);
  }
  Frame *b = uj_new(c->fn);
  int i;
  for (i = 0; i < c->have; i++) b->slot[i] = c->arg[i];
  b->next = f->next;
  b->dest_slot = 0;
  uj_next = b;
  uj_free_frame(f);
}

#define UJ_TEXT_TAG (1ull << 63)

typedef struct UjText {
  uint32_t len;
  uint32_t kind; /* 0 = one slice at data, 1 = cat of left and right */
  const char *data;
  uint64_t left;
  uint64_t right;
} UjText;

static int uj_is_text(uint64_t v) { return (v & UJ_TEXT_TAG) != 0; }

static UjText *uj_text_ptr(uint64_t v) {
  return (UjText *)(uintptr_t)(v & ~UJ_TEXT_TAG);
}

static uint64_t uj_text_tag(UjText *t) {
  return (uint64_t)(uintptr_t)t | UJ_TEXT_TAG;
}

static uint64_t uj_text_slice(const char *data, uint32_t n) {
  UjText *t = bump_mem(sizeof(UjText));
  if (!t) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  t->len = n;
  t->kind = 0;
  t->data = data;
  t->left = 0;
  t->right = 0;
  return uj_text_tag(t);
}

static void uj_text_flatten(UjText *t, char *dst) {
  UjText **st = malloc(sizeof(UjText *) * 64);
  size_t cap = 64;
  size_t n = 0;
  if (!st) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  st[n++] = t;
  char *p = dst;
  while (n) {
    UjText *c = st[--n];
    if (!c || c->len == 0) continue;
    if (c->kind == 0) {
      memcpy(p, c->data, c->len);
      p += c->len;
    } else {
      if (n + 2 > cap) {
        cap *= 2;
        st = realloc(st, cap * sizeof(UjText *));
        if (!st) {
          fprintf(stderr, "out of memory\n");
          exit(1);
        }
      }
      st[n++] = uj_text_ptr(c->right);
      st[n++] = uj_text_ptr(c->left);
    }
  }
  free(st);
}

static uint64_t uj_text_flat(uint64_t v) {
  UjText *t = uj_text_ptr(v);
  if (!t || t->kind == 0) return v;
  char *buf = malloc((size_t)t->len + 1);
  if (!buf) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  if (t->len) uj_text_flatten(t, buf);
  buf[t->len] = 0;
  return uj_text_slice(buf, t->len);
}

static uint64_t uj_text_copy(const char *s, uint32_t n) {
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  if (n && s) memcpy(buf, s, n);
  buf[n] = 0;
  return uj_text_slice(buf, n);
}

uint64_t uj_text_lit(const char *s) {
  return uj_text_copy(s, (uint32_t)strlen(s));
}

uint64_t uj_text_nat(uint64_t n) {
  char buf[32];
  int i = 32;
  if (n == 0) buf[--i] = '0';
  else {
    while (n && i > 0) {
      buf[--i] = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return uj_text_copy(buf + i, (uint32_t)(32 - i));
}

uint64_t uj_text_cat(uint64_t a, uint64_t b) {
  UjText *x = uj_text_ptr(a);
  UjText *y = uj_text_ptr(b);
  if (!x || x->len == 0) return b;
  if (!y || y->len == 0) return a;
  if (x->len > UINT32_MAX - y->len) {
    fprintf(stderr, "text too long\n");
    exit(1);
  }
  UjText *t = malloc(sizeof(UjText));
  if (!t) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  t->len = x->len + y->len;
  t->kind = 1;
  t->data = 0;
  t->left = a;
  t->right = b;
  return uj_text_tag(t);
}

typedef struct UjBytes {
  uint32_t len;
  uint8_t b[];
} UjBytes;

static uint64_t uj_tag_ptr(void *p) {
  return (uint64_t)(uintptr_t)p | UJ_TEXT_TAG;
}

static void *uj_untag(uint64_t v) {
  return (void *)(uintptr_t)(v & ~UJ_TEXT_TAG);
}

uint64_t uj_bytes_of(uint32_t n, const uint8_t *p) {
  UjBytes *b = malloc(sizeof(UjBytes) + n);
  if (!b) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  b->len = n;
  if (n && p) memcpy(b->b, p, n);
  return uj_tag_ptr(b);
}

uint64_t uj_bytes_cat(uint64_t a, uint64_t c) {
  UjBytes *x = uj_untag(a);
  UjBytes *y = uj_untag(c);
  uint32_t n = x->len + y->len;
  UjBytes *o = malloc(sizeof(UjBytes) + n);
  if (!o) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  o->len = n;
  if (x->len) memcpy(o->b, x->b, x->len);
  if (y->len) memcpy(o->b + x->len, y->b, y->len);
  return uj_tag_ptr(o);
}

uint64_t uj_bytes_size(uint64_t a) {
  UjBytes *x = uj_untag(a);
  return x->len;
}

uint64_t uj_bytes_at(uint64_t missing, uint64_t index, uint64_t a) {
  UjBytes *x = uj_untag(a);
  if (index >= x->len) return missing;
  return x->b[index];
}

typedef struct UjMap {
  uint32_t n;
  uint64_t *k;
  uint64_t *v;
} UjMap;

uint64_t uj_map_empty(void) {
  UjMap *m = calloc(1, sizeof(UjMap));
  if (!m) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  return uj_tag_ptr(m);
}

uint64_t uj_map_put(uint64_t key, uint64_t val, uint64_t src) {
  UjMap *m = uj_untag(src);
  uint32_t n = m ? m->n : 0;
  uint32_t i;
  int found = -1;
  for (i = 0; i < n; i++) {
    if (m->k[i] == key) {
      found = (int)i;
      break;
    }
  }
  uint32_t nn = found >= 0 ? n : n + 1;
  UjMap *o = calloc(1, sizeof(UjMap));
  uint64_t *kk = nn ? calloc(nn, sizeof(uint64_t)) : 0;
  uint64_t *vv = nn ? calloc(nn, sizeof(uint64_t)) : 0;
  if (!o || (nn && (!kk || !vv))) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  o->n = nn;
  o->k = kk;
  o->v = vv;
  if (m && n) {
    memcpy(o->k, m->k, n * sizeof(uint64_t));
    memcpy(o->v, m->v, n * sizeof(uint64_t));
  }
  if (found >= 0) o->v[found] = val;
  else {
    o->k[n] = key;
    o->v[n] = val;
  }
  return uj_tag_ptr(o);
}

uint64_t uj_map_size(uint64_t src) {
  UjMap *m = uj_untag(src);
  return m ? m->n : 0;
}

uint64_t uj_map_get(uint64_t missing, uint64_t key, uint64_t src) {
  UjMap *m = uj_untag(src);
  uint32_t i;
  if (!m) return missing;
  for (i = 0; i < m->n; i++) {
    if (m->k[i] == key) return m->v[i];
  }
  return missing;
}

uint64_t uj_text_take(uint64_t n, uint64_t v) {
  UjText *t = uj_text_ptr(v);
  if (!t || t->len == 0) return v;
  if (t->kind != 0) return uj_text_take(n, uj_text_flat(v));
  uint32_t k = n >= t->len ? t->len : (uint32_t)n;
  return uj_text_slice(t->data, k);
}

uint64_t uj_text_drop(uint64_t n, uint64_t v) {
  UjText *t = uj_text_ptr(v);
  if (!t || t->len == 0) return v;
  if (t->kind != 0) return uj_text_drop(n, uj_text_flat(v));
  uint32_t k = n >= t->len ? t->len : (uint32_t)n;
  return uj_text_slice(t->data + k, t->len - k);
}

uint64_t uj_text_eq(uint64_t a, uint64_t b) {
  UjText *x = uj_text_ptr(a);
  UjText *y = uj_text_ptr(b);
  if (!x || !y) return 0;
  if (x->len != y->len) return 0;
  if (x->kind != 0) a = uj_text_flat(a);
  if (y->kind != 0) b = uj_text_flat(b);
  x = uj_text_ptr(a);
  y = uj_text_ptr(b);
  int eq = x->len == 0 || memcmp(x->data, y->data, x->len) == 0;
  return eq ? 1 : 0;
}

uint64_t uj_text_size(uint64_t v) {
  return uj_text_ptr(v)->len;
}

uint64_t uj_text_index(uint64_t needle, uint64_t hay) {
  if (uj_text_ptr(needle)->kind != 0) needle = uj_text_flat(needle);
  if (uj_text_ptr(hay)->kind != 0) hay = uj_text_flat(hay);
  UjText *n = uj_text_ptr(needle);
  UjText *h = uj_text_ptr(hay);
  uint32_t i;
  if (n->len == 0) return uj_obj(0, 1, 0);
  if (n->len > h->len) return uj_obj(1, 0, 0);
  for (i = 0; i + n->len <= h->len; i++) {
    if (memcmp(h->data + i, n->data, n->len) == 0) return uj_obj(0, 1, i);
  }
  return uj_obj(1, 0, 0);
}

uint64_t uj_nat_from_text(uint64_t v) {
  if (uj_text_ptr(v)->kind != 0) v = uj_text_flat(v);
  UjText *t = uj_text_ptr(v);
  uint64_t n = 0;
  uint32_t i;
  if (t->len == 0) return uj_obj(1, 0, 0);
  for (i = 0; i < t->len; i++) {
    unsigned char c = (unsigned char)t->data[i];
    if (c < '0' || c > '9') return uj_obj(1, 0, 0);
    n = n * 10u + (uint64_t)(c - '0');
  }
  return uj_obj(0, 1, n);
}

uint64_t uj_nat_sub(uint64_t a, uint64_t b) {
  return a < b ? 0 : a - b;
}

static char *uj_text_cstr(uint64_t v) {
  if (uj_text_ptr(v)->kind != 0) v = uj_text_flat(v);
  UjText *t = uj_text_ptr(v);
  char *s = malloc((size_t)t->len + 1);
  if (!s) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  if (t->len) memcpy(s, t->data, t->len);
  s[t->len] = 0;
  return s;
}

uint64_t uj_read_file(uint64_t path) {
  char *s = uj_text_cstr(uj_field(path, 0));
  FILE *f = fopen(s, "rb");
  free(s);
  if (!f) {
    fprintf(stderr, "read failed\n");
    exit(1);
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fprintf(stderr, "read failed\n");
    exit(1);
  }
  long n = ftell(f);
  if (n < 0) {
    fprintf(stderr, "read failed\n");
    exit(1);
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fprintf(stderr, "read failed\n");
    exit(1);
  }
  uint8_t *buf = n ? malloc((size_t)n) : 0;
  if (n && !buf) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "read failed\n");
    exit(1);
  }
  fclose(f);
  uint64_t b = uj_bytes_of((uint32_t)n, buf);
  free(buf);
  return b;
}

uint64_t uj_utf8_decode(uint64_t bytes) {
  UjBytes *b = uj_untag(bytes);
  return uj_text_copy((const char *)b->b, b->len);
}

uint64_t uj_text_to_utf8(uint64_t text) {
  if (uj_text_ptr(text)->kind != 0) text = uj_text_flat(text);
  UjText *t = uj_text_ptr(text);
  return uj_bytes_of(t->len, (const uint8_t *)t->data);
}

uint64_t uj_write_file(uint64_t path, uint64_t bytes) {
  char *s = uj_text_cstr(uj_field(path, 0));
  UjBytes *b = uj_untag(bytes);
  FILE *f = fopen(s, "wb");
  free(s);
  if (!f) {
    fprintf(stderr, "write failed\n");
    exit(1);
  }
  if (b->len && fwrite(b->b, 1, b->len, f) != b->len) {
    fprintf(stderr, "write failed\n");
    exit(1);
  }
  fclose(f);
  return 0;
}

static void uj_write_payload(uint64_t v) {
  if (uj_is_text(v)) {
    if (uj_text_ptr(v)->kind != 0) v = uj_text_flat(v);
    UjText *t = uj_text_ptr(v);
    if (t->len) fwrite(t->data, 1, t->len, stdout);
    fputc('\n', stdout);
  } else {
    printf("%llu\n", (unsigned long long)v);
  }
  fflush(stdout);
}

int uj_tag(uint64_t v) {
  if (v < 2) return (int)v;
  return ((Obj *)(uintptr_t)v)->tag;
}

uint64_t uj_field(uint64_t v, int i) {
  return ((Obj *)(uintptr_t)v)->field[i];
}

static Frame *uj_alloc_frame(void) {
  Frame *f = frame_free;
  if (f) frame_free = f->next;
  else {
    f = calloc(1, sizeof(Frame));
    if (!f) {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
  }
  uj_live_frames++;
  return f;
}

static Cont *uj_alloc_cont(void) {
  Cont *c = cont_free;
  if (c) {
    cont_free = (Cont *)(uintptr_t)c->top;
    c->top = NULL;
  } else {
    c = calloc(1, sizeof(Cont));
    if (!c) {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
  }
  uj_live_conts++;
  return c;
}

static void uj_pool_cont(Cont *c) {
  c->top = (Frame *)(uintptr_t)cont_free;
  cont_free = c;
  uj_live_conts--;
}

Frame *uj_new(Fn fn) {
  Frame *f = uj_alloc_frame();
  memset(f, 0, sizeof(Frame));
  f->fn = fn;
  return f;
}

static Frame *uj_fresh(Fn fn) {
  Frame *f = uj_alloc_frame();
  f->fn = fn;
  f->body = NULL;
  f->pc = 0;
  f->is_handler = 0;
  f->affine = 0;
  f->ability = 0;
  f->req = 0;
  f->has_payload = 0;
  f->payload = 0;
  f->pure_value = 0;
  f->resume_slot = 0;
  f->dest_slot = 0;
  f->bargn = 0;
  memset(f->barg, 0, sizeof f->barg);
  for (int i = 0; i < 8; i++) f->ks[i] = NULL;
  f->next = NULL;
  f->kont = NULL;
  return f;
}

uint64_t uj_call(Fn fn, int n, const uint64_t *a) {
  Frame *fr = uj_fresh(fn);
  if (n < 0) n = 0;
  if (n > 8192) n = 8192;
  for (int i = 0; i < n; i++) fr->slot[i] = a ? a[i] : 0;
  int halt = uj_halt;
  Frame *next = uj_next;
  uint64_t saved = uj_result;
  uj_run(fr);
  uint64_t out = uj_result;
  uj_halt = halt;
  uj_next = next;
  uj_result = saved;
  return out;
}

static Cont *uj_clone_cont(Cont *k);

static Frame *uj_clone_frame(Frame *f, Frame *new_next) {
  Frame *c = uj_alloc_frame();
  memcpy(c, f, sizeof(Frame));
  c->next = new_next;
  for (int i = 0; i < 8; i++) c->ks[i] = NULL;
  for (int i = 0; i < 8; i++) {
    if (f->ks[i]) c->ks[i] = uj_clone_cont(f->ks[i]);
  }
  return c;
}

static Frame *uj_clone_until(Frame *from, Frame *stop) {
  int n = 0;
  for (Frame *f = from; f && f != stop; f = f->next) n++;
  if (n == 0) return NULL;
  Frame **arr = calloc((size_t)n, sizeof(Frame *));
  if (!arr) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  int i = 0;
  for (Frame *f = from; f && f != stop; f = f->next) arr[i++] = f;
  Frame *next = NULL;
  for (int j = n - 1; j >= 0; j--) {
    next = uj_clone_frame(arr[j], next);
  }
  free(arr);
  return next;
}

static Cont *uj_clone_cont(Cont *k) {
  if (!k) return NULL;
  Cont *c = uj_alloc_cont();
  c->top = uj_clone_until(k->top, NULL);
  return c;
}

static void uj_drop_cont(Cont *k) {
  if (!k) return;
  Frame *f = k->top;
  while (f) {
    Frame *n = f->next;
    f->next = NULL;
    uj_free_frame(f);
    f = n;
  }
  uj_pool_cont(k);
}

void uj_free_frame(Frame *f) {
  if (!f) return;
  for (int i = 0; i < 8; i++) {
    if (f->ks[i]) uj_drop_cont(f->ks[i]);
    f->ks[i] = NULL;
  }
  f->fn = NULL;
  f->next = frame_free;
  frame_free = f;
  uj_live_frames--;
}

static void uj_free_until(Frame *from, Frame *stop) {
  Frame *f = from;
  while (f && f != stop) {
    Frame *n = f->next;
    f->next = NULL;
    uj_free_frame(f);
    f = n;
  }
}

void uj_perform(Frame *self, int ability, int req, int has_payload, uint64_t payload, int resume_slot) {
  self->resume_slot = resume_slot;
  Frame *h = self->next;
  while (h && !(h->is_handler && h->ability == ability)) h = h->next;
  if (!h) {
    fprintf(stderr, "unhandled ability %d req %d\n", ability, req);
    exit(1);
  }
  Cont *k = uj_alloc_cont();
  if (h->affine) {
    Frame *bot = self;
    while (bot->next && bot->next != h) bot = bot->next;
    if (bot->next != h) {
      fprintf(stderr, "affine cut\n");
      exit(1);
    }
    bot->next = NULL;
    k->top = self;
  } else {
    k->top = uj_clone_until(self, h);
    uj_free_until(self, h);
  }
  if (h->ks[0]) uj_drop_cont(h->ks[0]);
  h->ks[0] = k;
  h->req = req;
  h->has_payload = has_payload;
  h->payload = payload;
  h->pc = 1;
  uj_next = h;
}

void uj_resume_ks(Frame *handler, uint64_t value) {
  Cont *k = handler->ks[0];
  if (!k || !k->top) {
    fprintf(stderr, "resume of empty continuation\n");
    exit(1);
  }
  handler->ks[0] = NULL;
  Frame *top = k->top;
  k->top = NULL;
  uj_pool_cont(k);
  top->slot[top->resume_slot] = value;
  Frame *bot = top;
  while (bot->next) bot = bot->next;
  bot->next = handler;
  handler->pc = 2;
  uj_next = top;
}

void uj_resume_affine(Frame *handler, uint64_t value) {
  Cont *k = handler->ks[0];
  if (!k || !k->top) {
    fprintf(stderr, "resume of empty continuation\n");
    exit(1);
  }
  handler->ks[0] = NULL;
  Frame *top = k->top;
  k->top = NULL;
  uj_pool_cont(k);
  top->slot[top->resume_slot] = value;
  Frame *bot = top;
  while (bot->next) bot = bot->next;
  bot->next = handler;
  handler->pc = 2;
  uj_next = top;
}

static void fn_builtin_exception(Frame *f) {
  if (f->pc == 1 && f->req == 0) {
    uj_stm_abort();
    if (f->payload) {
      uint64_t p = f->payload;
      const char *msg = uj_is_text(p) ? uj_text_cstr(p) : (const char *)(uintptr_t)p;
      fprintf(stderr, "exception %s\n", msg);
    } else
      fprintf(stderr, "exception\n");
    uj_halt = 1;
    uj_next = NULL;
    uj_free_frame(f);
    return;
  }
  if (f->pc == 3) {
    uj_result = f->pure_value;
    uj_halt = 1;
    uj_next = NULL;
    uj_free_frame(f);
    return;
  }
  fprintf(stderr, "bad exception pc %d\n", f->pc);
  exit(1);
}

static void fn_builtin_io(Frame *f) {
  if (f->pc == 1 && f->req == 0) {
    uj_write_payload(f->payload);
    uj_resume_affine(f, 0);
    return;
  }
  if (f->pc == 3) {
    uj_return(f, f->pure_value);
    return;
  }
  fprintf(stderr, "bad io pc %d req %d\n", f->pc, f->req);
  exit(1);
}

Frame *uj_link_builtins(Frame *user) {
  Frame *ex = uj_new(fn_builtin_exception);
  ex->is_handler = 1;
  ex->ability = 20;
  ex->affine = 1;
  ex->pc = 2;
  Frame *io = uj_new(fn_builtin_io);
  io->is_handler = 1;
  io->ability = 21;
  io->affine = 1;
  io->pc = 2;
  io->next = ex;
  user->next = io;
  return user;
}

typedef struct {
  uint64_t val;
} UjRef;

uint64_t uj_ref_new(uint64_t init) {
  UjRef *r = calloc(1, sizeof(UjRef));
  if (!r) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  r->val = init;
  return (uint64_t)(uintptr_t)r;
}

uint64_t uj_ref_read(uint64_t r) {
  return ((UjRef *)(uintptr_t)r)->val;
}

void uj_ref_write(uint64_t r, uint64_t v) {
  ((UjRef *)(uintptr_t)r)->val = v;
}

typedef struct {
  uint64_t val;
  uint64_t scratch;
  int dirty;
  int inlog;
} UjTVar;

#define UJ_STM_LOG 64
static UjTVar *uj_stm_log[UJ_STM_LOG];
static int uj_stm_n;
static int uj_stm_on;

uint64_t uj_tvar_new(uint64_t init) {
  UjTVar *t = calloc(1, sizeof(UjTVar));
  if (!t) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  t->val = init;
  return (uint64_t)(uintptr_t)t;
}

void uj_stm_begin(void) {
  uj_stm_on = 1;
  uj_stm_n = 0;
}

void uj_stm_commit(void) {
  for (int i = 0; i < uj_stm_n; i++) {
    UjTVar *t = uj_stm_log[i];
    if (t->dirty) t->val = t->scratch;
    t->dirty = 0;
    t->inlog = 0;
  }
  uj_stm_n = 0;
  uj_stm_on = 0;
}

void uj_stm_abort(void) {
  for (int i = 0; i < uj_stm_n; i++) {
    uj_stm_log[i]->dirty = 0;
    uj_stm_log[i]->inlog = 0;
  }
  uj_stm_n = 0;
  uj_stm_on = 0;
}

void uj_stm_retry(void) {
  uj_stm_abort();
  for (;;) pause();
}

uint64_t uj_tvar_read(uint64_t p) {
  UjTVar *t = (UjTVar *)(uintptr_t)p;
  if (uj_stm_on && t->dirty) return t->scratch;
  return t->val;
}

uint64_t uj_tvar_read_io(uint64_t p) {
  return ((UjTVar *)(uintptr_t)p)->val;
}

void uj_tvar_write(uint64_t p, uint64_t v) {
  UjTVar *t = (UjTVar *)(uintptr_t)p;
  if (!uj_stm_on) {
    t->val = v;
    return;
  }
  t->scratch = v;
  t->dirty = 1;
  if (!t->inlog) {
    if (uj_stm_n >= UJ_STM_LOG) {
      fprintf(stderr, "stm log full\n");
      exit(1);
    }
    uj_stm_log[uj_stm_n++] = t;
    t->inlog = 1;
  }
}

void uj_resume(Frame *handler, Cont *k, uint64_t value) {
  if (!k || !k->top) {
    fprintf(stderr, "resume of empty continuation\n");
    exit(1);
  }
  Frame *copy = uj_clone_until(k->top, NULL);
  Frame *bot = copy;
  while (bot->next) bot = bot->next;
  bot->next = handler;
  copy->slot[copy->resume_slot] = value;
  handler->pc = 2;
  uj_next = copy;
}

struct timespec uj_loop0, uj_loop1;
int uj_loop_live;

void uj_stamp(void) {
  if (uj_loop_live == 1) {
    clock_gettime(CLOCK_MONOTONIC, &uj_loop1);
    uj_loop_live = 2;
  }
}

void uj_return(Frame *self, uint64_t value) {
  Frame *n = self->next;
  if (!n) {
    uj_result = value;
    uj_halt = 1;
    uj_next = NULL;
    uj_free_frame(self);
    return;
  }
  /* pc 2 means the handler is waiting for its own body.
     A handler frame can also be paused mid-branch; that return is a normal value. */
  if (n->is_handler && n->pc == 2) {
    n->pure_value = value;
    n->pc = 3;
    uj_next = n;
    uj_free_frame(self);
    return;
  }
  n->slot[self->dest_slot] = value;
  uj_next = n;
  uj_free_frame(self);
}

void uj_run(Frame *start) {
  uj_halt = 0;
  uj_result = 0;
  uj_next = start;
  while (!uj_halt && uj_next) {
    Frame *cur = uj_next;
    uj_next = NULL;
    cur->fn(cur);
  }
}
