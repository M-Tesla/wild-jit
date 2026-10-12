#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* chooseBody, printed by Link.Term.toText after update. */
static const char *kChooseHash = "02r9ma64e57ht0nu1onm1tmr4b4hmt9gn34rmu0f719los1qnplfi";

typedef int (*UjMain)(void);

struct Slot {
  int used;
  char hash[80];
  void *lib;
  UjMain fn;
};

static struct Slot table[4];

static struct Slot *find_slot(const char *hash) {
  for (int i = 0; i < 4; i++) {
    if (table[i].used && strcmp(table[i].hash, hash) == 0) return &table[i];
  }
  return NULL;
}

static struct Slot *empty_slot(void) {
  for (int i = 0; i < 4; i++) {
    if (!table[i].used) return &table[i];
  }
  return NULL;
}

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static int plain_name(const char *s) {
  if (!s || !s[0]) return 0;
  for (const char *p = s; *p; p++) {
    char c = *p;
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_';
    if (!ok) return 0;
  }
  return 1;
}

static int ends_with(const char *s, const char *suf) {
  size_t n = strlen(s);
  size_t m = strlen(suf);
  return n >= m && strcmp(s + n - m, suf) == 0;
}

static int emit_native(const char *hash) {
  FILE *req = fopen("anf/request.txt", "w");
  if (!req) {
    fprintf(stderr, "request\n");
    return 1;
  }
  fprintf(req, "%s\n", hash);
  fclose(req);
  fprintf(stderr, "native run %s\n", hash);
  return system(
      "./out/anf-cc > /run/media/honinbou/HD/UnisonDB/tmp/cache-native.txt 2>&1");
}

static int uj_cached_call(const char *hash) {
  char path[512];
  char cfile[512];
  snprintf(path, sizeof path, "out/cache/%s.so", hash);
  snprintf(cfile, sizeof cfile, "out/anf-%s.c", hash);
  mkdir("out", 0755);
  mkdir("out/cache", 0755);

  struct Slot *s = find_slot(hash);
  if (s && !exists(path)) {
    dlclose(s->lib);
    s->used = 0;
    s->lib = NULL;
    s->fn = NULL;
    s = NULL;
  }
  if (s) {
    fprintf(stderr, "hit #%s compiler idle\n", hash);
    return s->fn();
  }
  if (!exists(path)) {
    fprintf(stderr, "miss #%s\n", hash);
    if (!exists(cfile)) {
      int urc = emit_native(hash);
      if (urc != 0 || !exists(cfile)) {
        fprintf(stderr, "native failed rc %d file %d\n", urc, exists(cfile));
        return 1;
      }
      fprintf(stderr, "native wrote\n");
    } else {
      fprintf(stderr, "compiler idle\n");
    }
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "gcc -shared -fPIC -O0 -I. -o out/cache/%s.so %s rt.c",
             hash, cfile);
    int rc = system(cmd);
    if (rc != 0) {
      fprintf(stderr, "gcc failed %d\n", rc);
      return 1;
    }
  } else {
    fprintf(stderr, "hit #%s compiler idle\n", hash);
  }
  void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    fprintf(stderr, "dlopen %s\n", dlerror());
    return 1;
  }
  UjMain fn = (UjMain)dlsym(lib, "main");
  if (!fn) {
    fprintf(stderr, "dlsym %s\n", dlerror());
    return 1;
  }
  s = empty_slot();
  if (!s) {
    fprintf(stderr, "cache full\n");
    return 1;
  }
  s->used = 1;
  snprintf(s->hash, sizeof s->hash, "%s", hash);
  s->lib = lib;
  s->fn = fn;
  return fn();
}

static unsigned long long since(struct timespec t0, struct timespec t1) {
  return (unsigned long long)(t1.tv_sec - t0.tv_sec) * 1000000000ull
       + (unsigned long long)(t1.tv_nsec - t0.tv_nsec);
}

static int read_hash_line(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r");
  char line[256];
  if (!f) return 0;
  out[0] = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "HASH ", 5) == 0) {
      char *p = line + 5;
      size_t k = strcspn(p, "\r\n");
      if (k + 1 > n) k = n - 1;
      memcpy(out, p, k);
      out[k] = 0;
    }
  }
  fclose(f);
  return plain_name(out);
}

static int dump_source(const char *file, const char *symbol, char *hash) {
  char side[512];
  char cmd[2048];
  struct stat src, mark;
  snprintf(side, sizeof side, "out/cache/src/%s.hash", symbol);
  mkdir("out", 0755);
  mkdir("out/cache", 0755);
  mkdir("out/cache/src", 0755);
  if (stat(file, &src) == 0 && stat(side, &mark) == 0 && mark.st_mtime >= src.st_mtime) {
    FILE *f = fopen(side, "r");
    if (f && fgets(hash, 80, f)) {
      hash[strcspn(hash, "\r\n")] = 0;
      fclose(f);
      if (plain_name(hash)) {
        char anf[512];
        snprintf(anf, sizeof anf, "anf/%s.anf", hash);
        if (exists(anf)) {
          fprintf(stderr, "anf ready %s\n", hash);
          return 0;
        }
      }
    } else if (f) {
      fclose(f);
    }
  }
  FILE *tpl = fopen("/run/media/honinbou/HD/UnisonDB/tmp/dump-term.u", "r");
  FILE *out = fopen("/run/media/honinbou/HD/UnisonDB/tmp/dump-one.u", "w");
  FILE *up = fopen("/run/media/honinbou/HD/UnisonDB/tmp/upd-src.md", "w");
  if (!tpl || !out || !up) {
    fprintf(stderr, "dump files\n");
    if (tpl) fclose(tpl);
    if (out) fclose(out);
    if (up) fclose(up);
    return 1;
  }
  fprintf(up, "```ucm\n> load %s\n> update\n```\n", file);
  fclose(up);
  char buf[1024];
  while (fgets(buf, sizeof buf, tpl)) {
    char *hit = strstr(buf, "__SYM__");
    if (!hit) fputs(buf, out);
    else {
      fwrite(buf, 1, (size_t)(hit - buf), out);
      fputs(symbol, out);
      fputs(hit + 7, out);
    }
  }
  fclose(tpl);
  fclose(out);
  fprintf(stderr, "dump %s %s\n", file, symbol);
  snprintf(cmd, sizeof cmd,
           "export HOME=/run/media/honinbou/HD/UnisonDB/bench/home "
           "TMPDIR=/run/media/honinbou/HD/UnisonDB/tmp "
           "UCM_WEB_UI=/run/media/honinbou/HD/UnisonDB/bench/ui; "
           "cd /run/media/honinbou/HD/UnisonDB/wild-jit && "
           "stdbuf -oL -eL /run/media/honinbou/HD/UnisonDB/bench/unison/unison "
           "-C /run/media/honinbou/HD/UnisonDB/wild-jit transcript.in-place "
           "/run/media/honinbou/HD/UnisonDB/tmp/upd-src.md "
           "> /run/media/honinbou/HD/UnisonDB/tmp/upd-src.out 2>&1 && "
           "stdbuf -oL -eL /run/media/honinbou/HD/UnisonDB/bench/unison/unison "
           "-C /run/media/honinbou/HD/UnisonDB/wild-jit run.file "
           "/run/media/honinbou/HD/UnisonDB/tmp/dump-one.u dumpOne "
           "> /run/media/honinbou/HD/UnisonDB/tmp/dump-one.out 2>&1");
  int rc = system(cmd);
  if (rc != 0 || !read_hash_line("/run/media/honinbou/HD/UnisonDB/tmp/dump-one.out", hash, 80)) {
    fprintf(stderr, "dump failed %d\n", rc);
    return 1;
  }
  FILE *sf = fopen(side, "w");
  if (sf) {
    fprintf(sf, "%s\n", hash);
    fclose(sf);
  }
  fprintf(stderr, "dumped %s\n", hash);
  return 0;
}

static void once(const char *hash) {
  char path[512];
  snprintf(path, sizeof path, "out/cache/%s.so", hash);
  struct stat st;
  long before = 0;
  int had = stat(path, &st) == 0;
  if (had) before = (long)st.st_mtime;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int rc = uj_cached_call(hash);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  int has = stat(path, &st) == 0;
  const char *gcc = "idle";
  if (!had && has) gcc = "ran";
  else if (had && has && (long)st.st_mtime != before) gcc = "ran";
  else if (had && !has) gcc = "missing";
  fprintf(stderr, "call_ns %llu rc %d gcc %s\n", since(t0, t1), rc, gcc);
}

int main(int argc, char **argv) {
  int reset = 0;
  int drop = 0;
  int twice = 0;
  const char *hash_arg = kChooseHash;
  const char *file = NULL;
  const char *symbol = NULL;
  int npos = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "reset") == 0) reset = 1;
    else if (strcmp(argv[i], "drop") == 0) drop = 1;
    else if (strcmp(argv[i], "twice") == 0) twice = 1;
    else if (npos == 0) {
      hash_arg = argv[i];
      npos = 1;
    } else if (npos == 1) {
      symbol = argv[i];
      npos = 2;
    } else {
      fprintf(stderr, "args\n");
      return 1;
    }
  }
  char hash_buf[80];
  const char *hash = hash_arg;
  if (ends_with(hash_arg, ".u")) {
    if (!symbol || !plain_name(symbol)) {
      fprintf(stderr, "args\n");
      return 1;
    }
    file = hash_arg;
    if (dump_source(file, symbol, hash_buf) != 0) return 1;
    hash = hash_buf;
  } else if (!plain_name(hash) || symbol) {
    fprintf(stderr, "args\n");
    return 1;
  }
  char path[512];
  char cfile[512];
  snprintf(path, sizeof path, "out/cache/%s.so", hash);
  snprintf(cfile, sizeof cfile, "out/anf-%s.c", hash);
  if (reset) {
    unlink(path);
    unlink(cfile);
  }
  if (drop) unlink(path);
  once(hash);
  if (twice) once(hash);
  return 0;
}
