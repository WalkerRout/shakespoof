#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct rng {
  uint64_t raw;
};

struct rng rng_new_seeded(uint64_t s) {
  return (struct rng) {.raw = s};
}

uint32_t rng_next(struct rng *r) {
  assert(r);
  // knuths mmix constant+1
  r->raw = r->raw * 0x3243f6a8885a308d + 1;
  return (uint32_t)(r->raw >> 32);
}

uint32_t rng_limit(struct rng *r, uint32_t limit) {
  assert(r);
  return (uint32_t)(((uint64_t)rng_next(r) * limit) >> 32);
}

// a key is 4 char bytes (a 4-gram) packed into a uint32_t
struct key {
  uint32_t raw;
};

struct key key_new(const char *p) {
  assert(p);
  return (struct key) {
    .raw = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]),
  };
}

uint64_t key_hash(struct key key) {
  return (((uint64_t)key.raw * 0xaef76aef8979ff25ull) >> 32);
}

bool key_empty(struct key key) {
  return key.raw == 0;
}

bool key_eq(struct key a, struct key b) {
  return a.raw == b.raw;
}

// represents an index into a bucket
struct value {
  size_t raw;
};

struct value value_new(size_t v) {
  return (struct value) {.raw = v};
}

size_t value_get(struct value value) {
  return value.raw;
}

struct entry {
  // 4-gram
  struct key key;
  // bucket index
  struct value value;
};

struct entry entry_new(struct key key, struct value value) {
  return (struct entry) {.key = key, .value = value};
}

// each unique 4-gram (key) gets a bucket
struct bucket {
  uint32_t total;
  uint32_t counts[256];
};

struct model {
  size_t corpus_len;
  const char *corpus;
  // map 4-gram -> bucket index
  struct entry *table;
  size_t buckets_cap;
  size_t buckets_len;
  struct bucket *buckets;
};

#define N (4u)
#define EXP (20u)

void model_init(struct model *m, const char *corpus, size_t corpus_len) {
  assert(m);
  assert(corpus);
  m->corpus = corpus;
  m->corpus_len = corpus_len;

  m->table = calloc((size_t)1 << EXP, sizeof(struct entry));
  assert(m->table);

  // initial cap
  m->buckets_cap = 1 << 10;
  m->buckets_len = 0;
  m->buckets = calloc(m->buckets_cap, sizeof(struct bucket));
  assert(m->buckets);
}

void model_free(struct model *m) {
  assert(m);
  free(m->table);
  free(m->buckets);
}

static struct entry *lookup(struct model *m, struct key key) {
  // mask-step-index api stolen from https://nullprogram.com/blog/2022/08/08/
  uint32_t hash = (uint32_t)key_hash(key);
  uint32_t mask = (1u << EXP) - 1u;
  uint32_t step = (hash >> (32u - EXP)) | 1u;
  for (uint32_t idx = hash;;) {
    idx = (idx + step) & mask;
    if (key_empty(m->table[idx].key) || key_eq(m->table[idx].key, key)) {
      return &m->table[idx];
    }
  }
}

static void grow_buckets(struct model *m) {
  size_t new_cap = m->buckets_cap << 1;
  struct bucket *grown = realloc(m->buckets, new_cap * sizeof(struct bucket));
  assert(grown);
  m->buckets = grown;
  m->buckets_cap = new_cap;
}

void model_train(struct model *m) {
  assert(m);
  assert(m->corpus_len > N);
  for (size_t i = 0; i + N < m->corpus_len; ++i) {
    struct key k = key_new(m->corpus + i);
    struct entry *e = lookup(m, k);
    // is this a new 4-gram sequence?
    if (key_empty(e->key)) {
      // sure is... before doing anything, try grow buckets
      if (m->buckets_len >= m->buckets_cap) {
        grow_buckets(m);
      }
      // add a new unique entry into the map and bump bucket count
      *e = entry_new(k, value_new(m->buckets_len));
      m->buckets_len += 1;
    }
    // record stats
    struct bucket *b = &m->buckets[value_get(e->value)];
    b->counts[(uint8_t)m->corpus[i + N]] += 1;
    b->total += 1;
  }
}

void model_dump(struct model *m, FILE *out) {
  assert(m);
  assert(out);
  fprintf(out, "trained %zu unique %u-grams\n", m->buckets_len, N);
}

static bool contains(struct model *m, const char *ctx) {
  return !key_empty(lookup(m, key_new(ctx))->key);
}

static char step(struct model *m, const char *ctx, struct rng *rng) {
  struct entry *e = lookup(m, key_new(ctx));
  assert(!key_empty(e->key));
  struct bucket *b = &m->buckets[value_get(e->value)];
  assert(b->total != 0);
  uint32_t r = rng_limit(rng, b->total);
  uint32_t acc = 0;
  for (int i = 0; i < 256; ++i) {
    acc += b->counts[i];
    if (r < acc) {
      return (char)i;
    }
  }
  assert(0 && "unreachable");
  return 0;
}

void model_generate(struct model *m, size_t count, FILE *out, struct rng *rng) {
  assert(m);
  assert(out);
  assert(rng);
  size_t span = m->corpus_len - N;
  size_t start = (size_t)(rng_next(rng) % span);
  char ctx[N];
  memcpy(ctx, m->corpus + start, N);

  fwrite(ctx, 1, N, out);
  for (size_t i = 0; i < count; ++i) {
    // does the model contain our current 'gram? if not, we gotta retry...
    if (!contains(m, ctx)) {
      // find new start, new line, new life...
      start = (size_t)(rng_next(rng) % span);
      memcpy(ctx, m->corpus + start, N);
      fputc('\n', out);
      fwrite(ctx, 1, N, out);
      continue;
    }
    char c = step(m, ctx, rng);
    fputc(c, out);
    memmove(ctx, ctx + 1, N - 1);
    ctx[N - 1] = c;
  }
  fputc('\n', out);
}

// file path to cstr
static char *slurp(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  assert(f);
  int sook = fseek(f, 0, SEEK_END);
  assert(sook == 0);
#ifdef NDEBUG
  (void)sook;
#endif // NDEBUG
  long n = ftell(f);
  assert(n >= 0);
  rewind(f);
  char *buf = malloc((size_t)n);
  assert(buf);
  size_t got = fread(buf, 1, (size_t)n, f);
  assert(got == (size_t)n);
#ifdef NDEBUG
  (void)got;
#endif // NDEBUG
  fclose(f);
  *out_len = (size_t)n;
  return buf;
}

int main(int argc, char *argv[]) {
  // usage: shakespoof <corpus.txt> [out_chars]
  assert(argc >= 2);

  size_t corpus_len;
  char *corpus = slurp(argv[1], &corpus_len);
  assert(corpus_len > N);

  // default to spitting out 2k chars as a sample
  size_t out_chars =
    (argc >= 3) ? (size_t)strtoull(argv[2], NULL, 10) : (size_t)2000;

  struct model m;
  model_init(&m, corpus, corpus_len);
  model_train(&m);
  model_dump(&m, stderr);

  struct rng rng = rng_new_seeded((uint64_t)time(NULL) ^ 0xdeadbeefcafef00dull);
  model_generate(&m, out_chars, stdout, &rng);

  model_free(&m);
  free(corpus);

  return 0;
}
