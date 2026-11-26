#include "anki.h"
#include "bobo_app.h"
#include "sqlite.h"
#include "zip.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint8_t s_buf[384 * 1024];

static deck_t* deck_at(anki_t* a, int64_t did);

typedef struct {
  char* conf;
  char* decks;
  char* dconf;
  char* models;
} col_row_t;
int anki_col_scan(sqlite_db_t* s, uint32_t root, col_row_t* out);

/* ---------- small helpers ---------- */

static const uint8_t* skip_ws(const uint8_t* p) {
  if (!p) return p;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

static const uint8_t* skip_string(const uint8_t* p) {
  if (*p != '"') return p;
  p++;
  while (*p) {
    if (*p == '\\' && p[1]) {
      p += 2;
      continue;
    }
    if (*p == '"') return p + 1;
    p++;
  }
  return p;
}

static const uint8_t* skip_value(const uint8_t* p) {
  p = skip_ws(p);
  if (*p == '"') return skip_string(p);
  if (*p == '{' || *p == '[') {
    char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = skip_string(p);
        continue;
      }
      if (*p == open) depth++;
      if (*p == close) {
        depth--;
        if (depth == 0) return p + 1;
      }
      p++;
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}

/* find "key" inside object [p, end) and return the value pointer */
static const uint8_t* obj_get(const uint8_t* p, const char* key) {
  p = skip_ws(p);
  if (*p != '{') return NULL;
  p++;
  size_t klen = strlen(key);
  while (*p) {
    p = skip_ws(p);
    if (*p == '}') return NULL;
    if (*p != '"') return NULL;
    int match = 1;
    const uint8_t* k = p + 1;
    for (size_t i = 0; i < klen; i++) {
      if (k[i] != (uint8_t)key[i]) {
        match = 0;
        break;
      }
    }
    p = skip_string(p);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    if (match) return p;
    p = skip_value(p);
    p = skip_ws(p);
    if (*p == ',') p++;
  }
  return NULL;
}

static int json_int(const uint8_t* p, int def) {
  if (!p) return def;
  p = skip_ws(p);
  if (!p) return def;
  int neg = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  }
  if (*p < '0' || *p > '9') return def;
  int v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return neg ? -v : v;
}

static void json_str(const uint8_t* p, char* out, int cap) {
  out[0] = 0;
  if (!p) return;
  p = skip_ws(p);
  if (!p || *p != '"') return;
  p++;
  int i = 0;
  while (*p && *p != '"' && i < cap - 1) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n') out[i++] = '\n';
      else if (*p == 't') out[i++] = '\t';
      else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
        int cp = 0;
        for (int k = 1; k <= 4; k++) {
          char h = (char)p[k];
          cp = cp * 16 + (h >= '0' && h <= '9' ? h - '0' : (h >= 'a' && h <= 'f' ? h - 'a' + 10 : h - 'A' + 10));
        }
        p += 5;
        if (cp < 0x80 && i < cap - 1) out[i++] = (char)cp;
        else if (cp < 0x800 && i < cap - 2) {
          out[i++] = (char)(0xc0 | (cp >> 6));
          out[i++] = (char)(0x80 | (cp & 0x3f));
        } else if (i < cap - 3) {
          out[i++] = (char)(0xe0 | (cp >> 12));
          out[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
          out[i++] = (char)(0x80 | (cp & 0x3f));
        }
        continue;
      }
      else out[i++] = (char)*p;
      p++;
      continue;
    }
    /* keep UTF-8 bytes as they are */
    out[i++] = (char)*p++;
  }
  out[i] = 0;
}

/* ---------- SD read ---------- */

static int read_file(const char* path, uint8_t** out, size_t* out_len) {
  int h = bobo_sd_open(path);
  if (h <= 0) return -1;
  int32_t sz = bobo_sd_size(h);
  if (sz <= 0) {
    bobo_sd_close(h);
    return -2;
  }
  /* big packages (usually embedded media) are read as a prefix: the
   * collection database sits at the start, pictures at the end. */
  int32_t want = sz > (4 << 20) ? (4 << 20) : sz;
  uint8_t* buf = (uint8_t*)malloc((size_t)want);
  if (!buf) {
    bobo_sd_close(h);
    return -3;
  }
  int got = 0;
  while (got < want) {
    int n = bobo_sd_read(h, buf + got, (size_t)(want - got));
    if (n <= 0) break;
    got += n;
  }
  bobo_sd_close(h);
  if (got != want) {
    free(buf);
    return -4;
  }
  *out = buf;
  *out_len = (size_t)want;
  return 0;
}

/* ---------- text cleanup ---------- */



/* Turns Anki field HTML into plain text; caps the result. */
static int clean_html(const uint8_t* src, int len, char* out, int cap) {
  int n = 0;
  for (int i = 0; i < len && n < cap - 1; i++) {
    char c = (char)src[i];
    if (c == '<') {
      int j = i + 1;
      /* find '>' */
      while (j < len && src[j] != '>') j++;
      if (j >= len) break;
      /* <br>, <div>, <p>, </div>, </p> -> newline */
      if ((src[i + 1] == 'b' && src[i + 2] == 'r') || (src[i + 1] == '/' && (src[i + 2] == 'd' || src[i + 2] == 'p')) ||
          src[i + 1] == 'd' || src[i + 1] == 'p') {
        if (n > 0 && out[n - 1] != '\n' && n < cap - 1) out[n++] = '\n';
      }
      i = j;
      continue;
    }
    if (c == '&') {
      if (i + 5 < len && memcmp(src + i, "&nbsp;", 6) == 0) {
        out[n++] = ' ';
        i += 5;
        continue;
      }
      if (i + 4 < len && memcmp(src + i, "&amp;", 5) == 0) {
        out[n++] = '&';
        i += 4;
        continue;
      }
      if (i + 3 < len && memcmp(src + i, "&lt;", 4) == 0) {
        out[n++] = '<';
        i += 3;
        continue;
      }
      if (i + 3 < len && memcmp(src + i, "&gt;", 4) == 0) {
        out[n++] = '>';
        i += 3;
        continue;
      }
      out[n++] = '&';
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n' && n > 0 && out[n - 1] == '\n') continue;
    out[n++] = c;
  }
  out[n] = 0;
  /* collapse blank lines at the ends */
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
  return n;
}

/* Remove cloze markers: {{c1::answer::hint}} -> answer */
static void strip_cloze(char* s) {
  char* p = s;
  while ((p = strstr(p, "{{c")) != NULL) {
    char* open = strstr(p, "::");
    char* close = strstr(p, "}}");
    if (!open || !close || open > close) break;
    /* delete "{{cN::" */
    memmove(p, open + 2, strlen(open + 2) + 1);
    /* delete "::hint}}"? find closing */
    close = strstr(p, "}}");
    if (!close) break;
    char* hint = NULL;
    for (char* q = p; q < close; q++)
      if (q[0] == ':' && q[1] == ':') {
        hint = q;
        break;
      }
    if (hint && hint < close) {
      memmove(hint, close + 2, strlen(close + 2) + 1);
    } else {
      memmove(close, close + 2, strlen(close + 2) + 1);
    }
  }
}

/* ---------- sqlite row callbacks ---------- */

typedef struct {
  anki_t* a;
  card_t* cards;
  int cap;
  int n;
} card_ctx;

static int card_cb(int64_t rowid, sqlite_rec_t* r, void* vctx) {
  card_ctx* ctx = (card_ctx*)vctx;
  if (ctx->n >= ctx->cap) return 0;
  if ((ctx->n & 511) == 0) {
    bobo_event_t ev;
    bobo_wait_event(&ev, 0); /* keep the watchdog fed while parsing */
  }
  sqlite_val_t did, type, queue, due, ivl, factor, reps, lapses, left;
  if (sqlite_rec_get(r, 2, &did) || sqlite_rec_get(r, 6, &type)) return 0;
  card_t* c = &ctx->cards[ctx->n];
  memset(c, 0, sizeof(*c));
  c->id = (uint32_t)rowid;
  c->nid = 0;
  sqlite_val_t nid;
  if (sqlite_rec_get(r, 1, &nid) == 0 && nid.type == 1) c->nid = nid.i;
  c->did = did.type == 1 ? did.i : 1;
  c->type = type.type == 1 ? (uint8_t)type.i : 0;
  c->queue = sqlite_rec_get(r, 7, &queue) == 0 && queue.type == 1 ? (uint8_t)queue.i : 0;
  c->due = sqlite_rec_get(r, 8, &due) == 0 && due.type == 1 ? (int32_t)due.i : 0;
  c->ivl = sqlite_rec_get(r, 9, &ivl) == 0 && ivl.type == 1 ? (int32_t)ivl.i : 0;
  c->factor = sqlite_rec_get(r, 10, &factor) == 0 && factor.type == 1 ? (uint16_t)factor.i : 2500;
  c->reps = sqlite_rec_get(r, 11, &reps) == 0 && reps.type == 1 ? (uint16_t)reps.i : 0;
  c->lapses = sqlite_rec_get(r, 12, &lapses) == 0 && lapses.type == 1 ? (uint16_t)lapses.i : 0;
  c->left = sqlite_rec_get(r, 13, &left) == 0 && left.type == 1 ? (uint16_t)left.i : 0;
  ctx->n++;
  return 0;
}

/* nid -> card index chaining */
#define ANKI_MAX_MODELS 24
#define ANKI_MAX_FIELDS 20
#define ANKI_NAME_LEN 20
#define ANKI_FLD_LEN 1536

typedef struct {
  int64_t mid;
  int nf;
  int q;        /* question field, -1 if unknown */
  int ans;      /* answer-key field */
  int back;     /* back field for plain two-sided cards */
  int basic;    /* simple front/back card, no options */
  int opt[4];   /* option fields */
  int nopt;
  char names[ANKI_MAX_FIELDS][ANKI_NAME_LEN];
  int valid;
} model_t;

static model_t g_models[ANKI_MAX_MODELS];
static int g_nmodels;

static int lower_ch(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int name_has(const char* name, const char* key) {
  int nl = (int)strlen(name), kl = (int)strlen(key);
  for (int i = 0; i + kl <= nl; i++) {
    int ok = 1;
    for (int j = 0; j < kl; j++)
      if (lower_ch(name[i + j]) != lower_ch(key[j])) {
        ok = 0;
        break;
      }
    if (ok) return 1;
  }
  return 0;
}

static void classify_model(model_t* m) {
  m->q = -1;
  m->ans = -1;
  m->back = -1;
  m->basic = 0;
  m->nopt = 0;
  for (int i = 0; i < m->nf; i++) {
    const char* n = m->names[i];
    if (strlen(n) == 1) {
      char ch = n[0] >= 'a' && n[0] <= 'z' ? n[0] - 32 : n[0];
      if (ch >= 'A' && ch <= 'D') {
        m->opt[ch - 'A'] = i;
        if (m->nopt < ch - 'A' + 1) m->nopt = ch - 'A' + 1;
        continue;
      }
      if (ch == 'Q') {
        if (m->q < 0) m->q = i;
        continue;
      }
      if (ch == 'T') {
        if (m->ans < 0) m->ans = i;
        continue;
      }
    }
    if (m->q < 0 && (name_has(n, "问题") || name_has(n, "题干") || name_has(n, "question") || name_has(n, "front")))
      m->q = i;
    else if (m->ans < 0 && (name_has(n, "答案") || name_has(n, "answer") || name_has(n, "correct")))
      m->ans = i;
    else if (m->back < 0 && (name_has(n, "背面") || name_has(n, "back") || name_has(n, "释义")))
      m->back = i;
    else if (m->nopt < 4 && (name_has(n, "选项") || name_has(n, "option")))
      m->opt[m->nopt++] = i;
  }
  for (int i = 0; i < m->nopt; i++)
    if (m->opt[i] < 0) m->opt[i] = 0;
  if (m->q >= 0 && m->back < 0 && m->ans >= 0) m->back = m->ans;
  if (m->q >= 0 && m->back >= 0 && m->nopt < 2) m->basic = 1;
  m->valid = m->q >= 0 && m->q < m->nf && ((m->ans >= 0 && m->nopt >= 2) || (m->basic && m->back < m->nf));
}

static void parse_models(const uint8_t* json) {
  g_nmodels = 0;
  memset(g_models, 0, sizeof(g_models));
  if (!json) return;
  const uint8_t* p = skip_ws(json);
  if (!p || *p != '{') return;
  p++;
  while (*p && g_nmodels < ANKI_MAX_MODELS) {
    p = skip_ws(p);
    if (!p || *p == '}') break;
    if (*p != '"') break;
    const uint8_t* k = p + 1;
    long long mid = 0;
    while (*k >= '0' && *k <= '9') mid = mid * 10 + (*k++ - '0');
    p = skip_string(p);
    p = skip_ws(p);
    if (!p || *p != ':') break;
    p++;
    model_t* m = &g_models[g_nmodels];
    m->mid = mid;
    const uint8_t* flds = obj_get(p, "flds");
    if (flds) {
      const uint8_t* q = skip_ws(flds);
      if (q && *q == '[') {
        q++;
        while (*q && m->nf < ANKI_MAX_FIELDS) {
          q = skip_ws(q);
          if (!q || *q == ']') break;
          if (*q == '{') {
            const uint8_t* nm = obj_get(q, "name");
            if (nm) json_str(nm, m->names[m->nf], ANKI_NAME_LEN);
            m->nf++;
          }
          q = skip_value(q);
          q = skip_ws(q);
          if (q && *q == ',') q++;
        }
      }
    }
    classify_model(m);
    g_nmodels++;
    p = skip_value(p);
    p = skip_ws(p);
    if (p && *p == ',') p++;
  }
  for (int i = 0; i < g_nmodels; i++)
    bobo_log("anki: model %lld fields=%d q=%d ans=%d opts=%d valid=%d", (long long)g_models[i].mid, g_models[i].nf,
             g_models[i].q, g_models[i].ans, g_models[i].nopt, g_models[i].valid);
}

static model_t* model_for(int64_t mid) {
  for (int i = 0; i < g_nmodels; i++)
    if (g_models[i].mid == mid) return &g_models[i];
  return NULL;
}

static int note_cb(int64_t rowid, sqlite_rec_t* r, void* vctx);

typedef struct {
  anki_t* a;
  int* head; /* hash of nid -> card index + 1 */
  int* next; /* card index -> next card index with same nid, -1 */
  int hash_size;
} note_ctx;

static int hash_nid(int64_t nid, int size) { return (int)(((uint64_t)nid * 2654435761u) % (uint32_t)size); }

static char s_fld[ANKI_MAX_FIELDS][ANKI_FLD_LEN];
static char s_rawq[8192];

static int build_blanks(const char* raw, char* out, int cap, card_line_t** lines_out, uint8_t* nlines_out) {
  int n = 0, found = 0;
  card_line_t* lines = NULL;
  int cap_lines = 40;
  lines = (card_line_t*)calloc((size_t)cap_lines, sizeof(card_line_t));
  const char* p = raw;
  while (p && *p) {
    const char* lab = strstr(p, "<label");
    if (!lab) break;
    const char* end = strstr(lab, "</label>");
    const char* w = strstr(lab, "width:");
    int chars = 4;
    if (w && (!end || w < end)) {
      const char* q = w + 6;
      int whole = 0, frac = 0, fd = 0;
      while (*q >= '0' && *q <= '9') whole = whole * 10 + (*q++ - '0');
      if (*q == '.') {
        q++;
        while (*q >= '0' && *q <= '9' && fd < 2) {
          frac = frac * 10 + (*q++ - '0');
          fd++;
        }
      }
      while (fd < 2) {
        frac *= 10;
        fd++;
      }
      chars = whole + (frac >= 50 ? 1 : 0);
      if (chars < 2) chars = 2;
      if (chars > 32) chars = 32;
    }
    if (n > 0 && n < cap - 1) out[n++] = '\n';
    for (int i = 0; i < chars && n < cap - 4; i++) {
      memcpy(out + n, "＿", 3);
      n += 3;
    }
    out[n] = 0;
    if (lines && found < cap_lines) {
      card_line_t* L = &lines[found];
      L->blanks = (uint16_t)chars;
      const char* t = strstr(lab, "<span class=\"t\">");
      const char* gt = t ? strchr(t, '>') : NULL;
      const char* te = t ? strstr(t, "</span>") : NULL;
      if (gt && te && (!end || te < end) && gt < te) {
        char tmp[ANKI_FLD_LEN];
        clean_html((const uint8_t*)(gt + 1), (int)(te - (gt + 1)), tmp, ANKI_FLD_LEN);
        int tl = (int)strlen(tmp);
        L->text = (char*)malloc((size_t)tl + 1);
        if (L->text) memcpy(L->text, tmp, (size_t)tl + 1);
      }
      if (!L->text) {
        L->text = (char*)calloc(1, 1);
      }
    }
    found++;
    p = end ? end + 8 : lab + 6;
  }
  if (nlines_out) *nlines_out = (uint8_t)(found > cap_lines ? cap_lines : found);
  if (lines_out) *lines_out = lines;
  else free(lines);
  return found;
}

static char s_front[ANKI_TEXT_MAX];
static char s_back[ANKI_TEXT_MAX];

static void append_str(char* buf, int* n, int cap, const char* s) {
  while (*s && *n < cap - 1) buf[(*n)++] = *s++;
  buf[*n] = 0;
}

static void append_line(char* buf, int* n, int cap, const char* s) {
  if (*n > 0 && buf[*n - 1] != '\n') append_str(buf, n, cap, "\n");
  append_str(buf, n, cap, s);
}

static const char* option_prefix(int i) {
  static const char* p[4] = {"A. ", "B. ", "C. ", "D. "};
  return p[i];
}

static int has_letter_prefix(const char* s, int i) {
  if (!s[0] || !s[1]) return 0;
  char ch = s[0] >= 'a' && s[0] <= 'z' ? s[0] - 32 : s[0];
  if (ch != 'A' + i) return 0;
  return s[1] == '.' || s[1] == ' ' || s[1] == ':';
}

/* Does the answer field name this option? Accepts "AC", "A,C", "A、C"
 * or the option text itself. */
static int answer_matches(const char* ans, int opt, const char* text) {
  if (!ans || !ans[0]) return 0;
  if (text[0] && strstr(ans, text)) return 1;
  for (const char* p = ans; *p; p++) {
    char ch = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
    if (ch == 'A' + opt) return 1;
  }
  return 0;
}

static void render_note(card_t* c, model_t* m, const char* flds, int flen) {
  int nf = 0, start = 0, rawidx = 0;
  s_rawq[0] = 0;
  for (int i = 0; i <= flen && nf < ANKI_MAX_FIELDS; i++) {
    if (i == flen || flds[i] == 0x1f) {
      if (m && rawidx == m->q) {
        int len = i - start;
        if (len > (int)sizeof(s_rawq) - 1) len = (int)sizeof(s_rawq) - 1;
        if (len > 0) memcpy(s_rawq, flds + start, (size_t)len);
        s_rawq[len > 0 ? len : 0] = 0;
      }
      rawidx++;
      clean_html((const uint8_t*)flds + start, i - start, s_fld[nf], ANKI_FLD_LEN);
      strip_cloze(s_fld[nf]);
      /* trim */
      char* b = s_fld[nf];
      while (*b == ' ' || *b == '\n' || *b == '\r') b++;
      int len = (int)strlen(b);
      while (len > 0 && (b[len - 1] == ' ' || b[len - 1] == '\n' || b[len - 1] == '\r')) b[--len] = 0;
      if (b != s_fld[nf]) memmove(s_fld[nf], b, (size_t)len + 1);
      nf++;
      start = i + 1;
    }
  }

  s_front[0] = 0;
  s_back[0] = 0;
  if (!m || !m->valid || m->q >= nf || m->ans >= nf) {
    /* unknown layout: first field on the front, the rest on the back */
    int fn = 0, bk = 0;
    for (int i = 0; i < nf; i++) {
      if (i == 0) {
        append_str(s_front, &fn, sizeof(s_front), s_fld[i]);
        append_str(s_back, &bk, sizeof(s_back), s_fld[i]);
      } else {
        append_line(s_back, &bk, sizeof(s_back), s_fld[i]);
      }
    }
    goto out;
  }
  if (m->basic) {
    int fn = 0, bk = 0;
    for (int i = 0; i < m->q && i < nf; i++) {
      if (!s_fld[i][0]) continue;
      if ((strcmp(s_fld[i], "单选") == 0) || (strcmp(s_fld[i], "多选") == 0)) continue;
      if (fn) append_str(s_front, &fn, sizeof(s_front), " · ");
      append_str(s_front, &fn, sizeof(s_front), s_fld[i]);
      if (bk) append_str(s_back, &bk, sizeof(s_back), " · ");
      append_str(s_back, &bk, sizeof(s_back), s_fld[i]);
    }
    if (fn) append_line(s_front, &fn, sizeof(s_front), "");
    if (bk) append_line(s_back, &bk, sizeof(s_back), "");
    /* templates that blank out every line: show the blanks, reveal on answer */
    int nb = 0;
    if (strstr(s_rawq, "class=\"d\"") && strstr(s_rawq, "class=\"t\"")) {
      nb = build_blanks(s_rawq, s_front + fn, (int)sizeof(s_front) - fn, &c->lines, &c->nlines);
      if (nb > 0) fn = (int)strlen(s_front);
    }
    if (nb == 0) append_str(s_front, &fn, sizeof(s_front), s_fld[m->q]);
    if (m->back < nf) append_line(s_back, &bk, sizeof(s_back), s_fld[m->back]);
    else append_line(s_back, &bk, sizeof(s_back), s_fld[m->q]);
    goto out;
  }
  /* header: everything before the question, minus the type word */
  int hn = 0;
  for (int i = 0; i < m->q && i < nf; i++) {
    if (!s_fld[i][0]) continue;
    if ((strcmp(s_fld[i], "单选") == 0) || (strcmp(s_fld[i], "多选") == 0)) continue;
    if (hn) append_str(s_front, &hn, sizeof(s_front), " · ");
    append_str(s_front, &hn, sizeof(s_front), s_fld[i]);
  }
  if (hn) append_line(s_front, &hn, sizeof(s_front), s_fld[m->q][0] ? s_fld[m->q] : "");
  else append_str(s_front, &hn, sizeof(s_front), s_fld[m->q]);
  /* options, always lettered */
  int show = 0;
  for (int i = 0; i < m->nopt && m->opt[i] < nf; i++) {
    int fi = m->opt[i];
    if (!s_fld[fi][0]) continue;
    char line[ANKI_FLD_LEN + 8];
    if (has_letter_prefix(s_fld[fi], i))
      snprintf(line, sizeof(line), "%s", s_fld[fi]);
    else
      snprintf(line, sizeof(line), "%s%s", option_prefix(i), s_fld[fi]);
    append_line(s_front, &hn, sizeof(s_front), line);
    show++;
  }
  if (show == 0) append_line(s_front, &hn, sizeof(s_front), s_fld[m->q]);
  /* answer side: question + only the correct options */
  int bn = 0;
  const char* ansf = s_fld[m->ans];
  char key[32];
  int kn = 0;
  for (int i = 0; i < m->nopt && m->opt[i] < nf; i++) {
    if (!answer_matches(ansf, i, s_fld[m->opt[i]])) continue;
    if (kn) {
      memcpy(key + kn, "、", 3);
      kn += 3;
    }
    key[kn++] = (char)('A' + i);
    key[kn] = 0;
  }
  if (kn == 0) {
    /* the answer field does not name any option: keep the plain answer text */
    for (int i = 0; i < m->q && i < nf; i++) {
      if (!s_fld[i][0]) continue;
      if ((strcmp(s_fld[i], "单选") == 0) || (strcmp(s_fld[i], "多选") == 0)) continue;
      if (bn) append_str(s_back, &bn, sizeof(s_back), " · ");
      append_str(s_back, &bn, sizeof(s_back), s_fld[i]);
    }
    append_line(s_back, &bn, sizeof(s_back), s_fld[m->q]);
    append_line(s_back, &bn, sizeof(s_back), ansf);
  } else {
    for (int i = 0; i < m->q && i < nf; i++) {
      if (!s_fld[i][0]) continue;
      if ((strcmp(s_fld[i], "单选") == 0) || (strcmp(s_fld[i], "多选") == 0)) continue;
      if (bn) append_str(s_back, &bn, sizeof(s_back), " · ");
      append_str(s_back, &bn, sizeof(s_back), s_fld[i]);
    }
    if (bn) append_line(s_back, &bn, sizeof(s_back), s_fld[m->q]);
    else append_str(s_back, &bn, sizeof(s_back), s_fld[m->q]);
    char line[64];
    snprintf(line, sizeof(line), "正确答案：%s", key);
    append_line(s_back, &bn, sizeof(s_back), line);
    for (int i = 0; i < m->nopt && m->opt[i] < nf; i++) {
      if (!answer_matches(ansf, i, s_fld[m->opt[i]])) continue;
      char ol[ANKI_FLD_LEN + 8];
      if (has_letter_prefix(s_fld[m->opt[i]], i))
        snprintf(ol, sizeof(ol), "%s", s_fld[m->opt[i]]);
      else
        snprintf(ol, sizeof(ol), "%s%s", option_prefix(i), s_fld[m->opt[i]]);
      append_line(s_back, &bn, sizeof(s_back), ol);
    }
  }
out:
  c->front = (char*)malloc(strlen(s_front) + 1);
  c->back = (char*)malloc(strlen(s_back) + 1);
  if (c->front && c->back) {
    strcpy(c->front, s_front);
    strcpy(c->back, s_back);
  } else {
    free(c->front);
    free(c->back);
    c->front = c->back = NULL;
  }
}

static int note_cb(int64_t rowid, sqlite_rec_t* r, void* vctx) {
  note_ctx* ctx = (note_ctx*)vctx;
  static int seen = 0;
  if ((seen++ & 511) == 0) {
    bobo_event_t ev;
    bobo_wait_event(&ev, 0);
  }
  sqlite_val_t flds, midv;
  if (sqlite_rec_get(r, 6, &flds) || flds.type != 2) return 0;
  int64_t mid = 0;
  if (sqlite_rec_get(r, 2, &midv) == 0 && midv.type == 1) mid = midv.i;
  model_t* m = model_for(mid);
  int h = hash_nid(rowid, ctx->hash_size);
  int ci = ctx->head[h] - 1;
  while (ci >= 0) {
    card_t* c = &ctx->a->cards[ci];
    if (c->nid == rowid) {
      render_note(c, m, (const char*)flds.text, (int)flds.len);
      break;
    }
    ci = ctx->next[ci];
  }
  return 0;
}

/* ---------- load ---------- */

static void parse_decks(anki_t* a, const uint8_t* decks) {
  if (!decks) return;
  const uint8_t* p = skip_ws(decks);
  if (!p || *p != '{') return;
  p++;
  while (*p) {
    p = skip_ws(p);
    if (!p || *p == '}') break;
    if (*p != '"') break;
    const uint8_t* k = p + 1;
    long long id = 0;
    int neg = 0;
    if (*k == '-') {
      neg = 1;
      k++;
    }
    while (*k >= '0' && *k <= '9') id = id * 10 + (*k++ - '0');
    if (neg) id = -id;
    p = skip_string(p);
    p = skip_ws(p);
    if (!p || *p != ':') break;
    p++;
    const uint8_t* namev = obj_get(p, "name");
    if (id != 0 && a->ndecks < ANKI_MAX_DECKS && namev) {
      deck_t* d = &a->decks[a->ndecks];
      d->id = id;
      json_str(namev, d->name, sizeof(d->name));
      if (d->name[0]) a->ndecks++;
    }
    p = skip_value(p);
    p = skip_ws(p);
    if (*p == ',') p++;
  }
}

static void parse_dconf(anki_t* a, const uint8_t* dconf) {
  if (!dconf) return;
  const uint8_t* p = skip_ws(dconf);
  if (!p || *p != '{') return;
  p++;
  p = skip_ws(p);
  if (!p || *p != '"') return;
  p = skip_string(p);
  p = skip_ws(p);
  if (!p || *p != ':') return;
  p++;
  const uint8_t* cfg = skip_ws(p);
  if (!cfg || *cfg != '{') return;
  const uint8_t* nw = obj_get(cfg, "new");
  if (nw) {
    int per = json_int(obj_get(nw, "perDay"), a->new_per_day);
    if (per > 0) a->new_per_day = per;
    const uint8_t* delays = obj_get(nw, "delays");
    if (delays) {
      const uint8_t* q = skip_ws(delays);
      if (q && *q == '[') {
        q++;
        int m1 = json_int(q, 1);
        q = skip_value(q);
        q = skip_ws(q);
        int m2 = m1;
        if (q && *q == ',') {
          q++;
          m2 = json_int(q, 10);
        }
        a->step1 = (m1 > 0 ? m1 : 1) * 60;
        a->step2 = (m2 > 0 ? m2 : 10) * 60;
      }
    }
    const uint8_t* ints = obj_get(nw, "ints");
    if (ints) {
      const uint8_t* q = skip_ws(ints);
      if (q && *q == '[') {
        q++;
        int g = json_int(q, 1);
        q = skip_value(q);
        q = skip_ws(q);
        int ez = g;
        if (q && *q == ',') {
          q++;
          ez = json_int(q, 4);
        }
        if (g > 0) a->graduating_ivl = g;
        if (ez > 0) a->easy_ivl = ez;
      }
    }
  }
  const uint8_t* rev = obj_get(cfg, "rev");
  if (rev) {
    int per = json_int(obj_get(rev, "perDay"), a->rev_per_day);
    if (per > 0) a->rev_per_day = per;
  }
}

int anki_load(anki_t* a, const char* path, char* err, int errcap) {
  memset(a, 0, sizeof(*a));
  a->new_per_day = 20;
  a->rev_per_day = 200;
  a->step1 = 60;
  a->step2 = 600;
  a->graduating_ivl = 1;
  a->easy_ivl = 4;
  a->relearn_step = 600;
  strncpy(a->path, path, sizeof(a->path) - 1);
  uint8_t* buf = NULL;
  size_t len = 0;
  int r = read_file(path, &buf, &len);
  if (r != 0) {
    snprintf(err, (size_t)errcap, "%s", r == -2 ? "读不到卡组文件" : "读不到文件，检查 TF 卡与 /Anki 路径");
    return -1;
  }
  bobo_log("anki: open %s (%u bytes)", path, (unsigned)len);
  zip_t z;
  if (zip_open(&z, buf, len) != 0) {
    snprintf(err, (size_t)errcap, "不是有效的 apkg（zip 结构损坏）");
    free(buf);
    return -1;
  }
  zip_entry_t e;
  int found = 0;
  /* 2.1 legacy exports keep an empty collection.anki2 and put the real deck
   * in collection.anki21, so the latter wins when both exist. */
  if (zip_find(&z, "collection.anki21", &e) == 0) found = 1;
  else if (zip_find(&z, "collection.anki2", &e) == 0) found = 1;
  bobo_log("anki: collection hit=%d entry=%s size=%u", found, found ? e.name : "-", found ? (unsigned)e.comp_size : 0);
  if (!found) {
    if (zip_find(&z, "collection.anki21b", &e) == 0)
      snprintf(err, (size_t)errcap, "这是 Anki 2.1.50+ 的 zstd 卡组，请在 Anki 里用「导出 › 支持旧版 Anki」重新导出");
    else
      snprintf(err, (size_t)errcap, "apkg 里没有 collection.anki2");
    free(buf);
    return -1;
  }
  size_t dblen = 0;
  uint8_t* db = zip_extract(&z, &e, &dblen);
  bobo_log("anki: extract ret=%d len=%u", db ? 0 : -1, (unsigned)dblen);
  if (!db && found && strcmp(e.name, "collection.anki21") == 0 &&
      zip_find(&z, "collection.anki2", &e) == 0) {
    bobo_log("anki: fallback to collection.anki2");
    db = zip_extract(&z, &e, &dblen);
  }
  if (!db) {
    snprintf(err, (size_t)errcap, "解压卡组数据库失败");
    free(buf);
    return -1;
  }
  sqlite_db_t s;
  int sopen = sqlite_open(&s, db, dblen);
  bobo_log("anki: sqlite_open=%d pages=%d", sopen, sopen ? -1 : (int)s.page_count);
  if (sopen != 0) {
    snprintf(err, (size_t)errcap, "卡组数据库格式不支持");
    free(db);
    free(buf);
    return -1;
  }
  /* col: decks and daily limits */
  uint32_t root = sqlite_table_root(&s, "col");
  if (root) {
    col_row_t row;
    anki_col_scan(&s, root, &row);
    if (row.models) parse_models((const uint8_t*)row.models);
    if (row.decks) parse_decks(a, (const uint8_t*)row.decks);
    if (row.dconf) parse_dconf(a, (const uint8_t*)row.dconf);
    free(row.models);
    free(row.conf);
    free(row.decks);
    free(row.dconf);
  }
  bobo_log("anki: decks=%d cards=%d", a->ndecks, a->ncards);
  if (a->ndecks == 0) {
    a->decks[0].id = 1;
    strcpy(a->decks[0].name, "默认牌组");
    a->ndecks = 1;
  }
  /* cards */
  root = sqlite_table_root(&s, "cards");
  if (!root) {
    snprintf(err, (size_t)errcap, "卡组里没有 cards 表");
    free(db);
    free(buf);
    return -1;
  }
  card_ctx cc = {a, NULL, ANKI_MAX_CARDS, 0};
  a->cards = (card_t*)calloc(ANKI_MAX_CARDS, sizeof(card_t));
  if (!a->cards) {
    snprintf(err, (size_t)errcap, "内存不足（卡组太大）");
    free(db);
    free(buf);
    return -1;
  }
  cc.cards = a->cards;
  sqlite_table_scan(&s, root, card_cb, &cc);
  a->ncards = cc.n;
  /* notes -> fields */
  root = sqlite_table_root(&s, "notes");
  if (root && a->ncards > 0) {
    int hs = 32768;
    int* head = (int*)calloc((size_t)hs, sizeof(int));
    int* next = (int*)malloc((size_t)a->ncards * sizeof(int));
    if (!head || !next) {
      bobo_log("anki: oom note index head=%p next=%p", (void*)head, (void*)next);
      free(head);
      free(next);
      free(db);
      free(buf);
      snprintf(err, (size_t)errcap, "内存不足（卡组太大）");
      return -1;
    }
    note_ctx nc = {a, head, next, hs};
    for (int i = 0; i < a->ncards; i++) {
      int h = hash_nid(a->cards[i].nid, hs);
      next[i] = head[h] - 1;
      head[h] = i + 1;
    }
    sqlite_table_scan(&s, root, note_cb, &nc);
    free(head);
    free(next);
  }
  free(db);
  free(buf);
  bobo_log("anki: ready cards=%d new/day=%d rev/day=%d", a->ncards, a->new_per_day, a->rev_per_day);
  for (int i = 0; i < a->ncards; i++) {
    if (!a->cards[i].front) {
      a->cards[i].front = (char*)calloc(1, 1);
      a->cards[i].back = (char*)calloc(1, 1);
    }
  }
  return 0;
}

/* col callback: copy the conf text of the first row */
static char* dup_text(sqlite_rec_t* r, int idx) {
  sqlite_val_t v;
  if (sqlite_rec_get(r, idx, &v) != 0 || v.type != 2) return NULL;
  char* buf = (char*)malloc((size_t)v.len + 1);
  if (!buf) return NULL;
  memcpy(buf, v.text, (size_t)v.len);
  buf[v.len] = 0;
  return buf;
}

static int col_cb(int64_t rowid, sqlite_rec_t* r, void* vctx) {
  (void)rowid;
  col_row_t* row = (col_row_t*)vctx;
  row->conf = dup_text(r, 8);
  row->models = dup_text(r, 9);
  row->decks = dup_text(r, 10);
  row->dconf = dup_text(r, 11);
  return 1;
}

int anki_col_scan(sqlite_db_t* s, uint32_t root, col_row_t* out) {
  memset(out, 0, sizeof(*out));
  sqlite_table_scan(s, root, col_cb, out);
  return 0;
}

void anki_free(anki_t* a) {
  for (int i = 0; i < a->ncards; i++) {
    if (a->cards[i].lines) {
      for (int j = 0; j < a->cards[i].nlines; j++) free(a->cards[i].lines[j].text);
      free(a->cards[i].lines);
    }
  }
  if (!a->cards) return;
  for (int i = 0; i < a->ncards; i++) {
    free(a->cards[i].front);
    free(a->cards[i].back);
  }
  free(a->cards);
  a->cards = NULL;
  a->ncards = 0;
}

/* ---------- time ---------- */

uint32_t anki_today(void) {
  int64_t t = bobo_time();
  if (t == 0) return 0;
  return (uint32_t)((t + bobo_tz_offset()) / 86400);
}

/* ---------- scheduling ---------- */

static int cap_ivl(int v) {
  if (v < 1) v = 1;
  if (v > 36500) v = 36500;
  return v;
}

static void graduate(anki_t* a, card_t* c, int ivl) {
  c->type = 2;
  c->queue = 2;
  c->ivl = cap_ivl(ivl);
  c->due = (int32_t)(a->today + (uint32_t)c->ivl);
  c->left = 0;
}

void anki_answer(anki_t* a, card_t* c, int rating) {
  deck_t* d = deck_at(a, c->did);
  uint8_t before = c->type;
  if (d) {
    if (before == 0) d->new_done++;
    else if (before == 2) d->rev_done++;
    d->ans_today++;
    if (rating == 1) d->again_today++;
  }
  if (before == 0) a->session_new++;
  else if (before == 2) a->session_rev++;
  c->reps++;
  if (c->type == 0) { /* new */
    if (rating == 1) {
      c->type = 1;
      c->queue = 1;
      c->left = 0;
      c->due = (int32_t)(a->now + (uint32_t)a->step1);
    } else if (rating == 2) {
      c->type = 1;
      c->queue = 1;
      c->left = 0;
      c->due = (int32_t)(a->now + (uint32_t)(a->step1 * 3 / 2));
    } else if (rating == 3) {
      if (a->step2 > a->step1) {
        c->type = 1;
        c->queue = 1;
        c->left = 1;
        c->due = (int32_t)(a->now + (uint32_t)a->step2);
      } else {
        c->factor = c->factor ? c->factor : 2500;
        graduate(a, c, a->graduating_ivl);
      }
    } else {
      c->factor = 2500;
      graduate(a, c, a->easy_ivl);
    }
  } else if (c->type == 1) { /* learning */
    if (rating == 1) {
      c->left = 0;
      c->due = (int32_t)(a->now + (uint32_t)a->step1);
    } else if (rating == 2) {
      c->due = (int32_t)(a->now + (uint32_t)((a->step1 + a->step2) / 2));
    } else if (rating == 3) {
      if (c->left + 1 < 2 && a->step2 > a->step1) {
        c->left++;
        c->due = (int32_t)(a->now + (uint32_t)a->step2);
      } else {
        c->factor = c->factor ? c->factor : 2500;
        graduate(a, c, a->graduating_ivl);
      }
    } else {
      c->factor = c->factor ? c->factor : 2500;
      graduate(a, c, a->easy_ivl);
    }
  } else if (c->type == 3) { /* relearning */
    if (rating == 1) {
      c->due = (int32_t)(a->now + (uint32_t)a->relearn_step);
    } else if (rating == 2) {
      c->due = (int32_t)(a->now + (uint32_t)a->relearn_step);
    } else {
      int base = c->ivl ? c->ivl : 1;
      if (rating == 4) base = (int)((long)base * (long)c->factor * 13 / 10000);
      else base = (int)((long)base * (long)c->factor / 1000);
      graduate(a, c, base);
    }
  } else { /* review */
    int factor = c->factor ? c->factor : 2500;
    if (rating == 1) {
      c->lapses++;
      factor -= 200;
      if (factor < 1300) factor = 1300;
      c->factor = (uint16_t)factor;
      c->type = 3;
      c->queue = 3;
      c->due = (int32_t)(a->now + (uint32_t)a->relearn_step);
    } else {
      long long iv = c->ivl ? c->ivl : 1;
      if (rating == 2) {
        iv = iv * 6 / 5; /* hard: 1.2x */
        factor -= 150;
        if (factor < 1300) factor = 1300;
      } else if (rating == 3) {
        iv = iv * factor / 1000;
      } else {
        iv = iv * factor / 1000 * 13 / 10;
        factor += 150;
      }
      c->factor = (uint16_t)factor;
      c->ivl = cap_ivl((int)iv);
      c->due = (int32_t)(a->today + (uint32_t)c->ivl);
    }
  }
}

int anki_counts(anki_t* a, int64_t did, int* new_n, int* learn_n, int* due_n, int* total) {
  int nn = 0, ln = 0, dn = 0, tt = 0;
  for (int i = 0; i < a->ncards; i++) {
    card_t* c = &a->cards[i];
    if (c->did != did) continue;
    tt++;
    if (c->type == 0) nn++;
    else if (c->type == 1 || c->type == 3) {
      if ((uint32_t)c->due <= a->now) ln++;
    } else if (c->type == 2) {
      if ((uint32_t)c->due <= a->today) dn++;
    }
  }
  *new_n = nn;
  *learn_n = ln;
  *due_n = dn;
  *total = tt;
  return 0;
}

static deck_t* deck_at(anki_t* a, int64_t did) {
  for (int i = 0; i < a->ndecks; i++)
    if (a->decks[i].id == did) return &a->decks[i];
  return NULL;
}

int anki_queue(anki_t* a, int64_t did, uint32_t* queue, int cap) {
  int n = 0;
  deck_t* d = deck_at(a, did);
  int new_left = a->new_per_day + (d ? d->extra_new : 0) - (d ? d->new_done : 0);
  int rev_left = a->rev_per_day - (d ? d->rev_done : 0);
  if (new_left < 0) new_left = 0;
  if (rev_left < 0) rev_left = 0;
  /* learning first (most overdue), then review, then new by position */
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < a->ncards && n < cap; i++) {
      card_t* c = &a->cards[i];
      if (c->did != did) continue;
      if (pass == 0) {
        if ((c->type != 1 && c->type != 3) || (uint32_t)c->due > a->now) continue;
      } else if (pass == 1) {
        if (c->type != 2 || (uint32_t)c->due > a->today) continue;
        if (rev_left <= 0) continue;
        rev_left--;
      } else {
        if (c->type != 0 || new_left <= 0) continue;
        new_left--;
      }
      queue[n++] = (uint32_t)i;
    }
  }
  return n;
}

/* ---------- progress ---------- */

static const char SRS_MAGIC[8] = "BSRS4";
static const char SRS_MAGIC_OLD[8] = "BSRS3";

typedef struct {
  char magic[8];
  uint32_t today;
  uint32_t count;
  uint32_t path_hash;
  uint32_t ndecks;
} srs_header_t;

typedef struct {
  int64_t id;
  int32_t new_done;
  int32_t rev_done;
  int32_t extra_new;
  int32_t ans_today;
  int32_t again_today;
} srs_deck_t;

typedef struct {
  uint32_t id;
  uint8_t type;
  uint8_t queue;
  uint16_t left;
  int32_t due;
  int32_t ivl;
  uint16_t factor;
  uint16_t reps;
  uint16_t lapses;
  uint16_t pad;
} srs_entry_t;

static uint32_t path_hash(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h;
}

static int find_card(anki_t* a, uint32_t id) {
  for (int i = 0; i < a->ncards; i++)
    if (a->cards[i].id == id) return i;
  return -1;
}

static int build_srs(anki_t* a, uint8_t* buf, size_t cap, size_t* outlen) {
  size_t need = sizeof(srs_header_t) + (size_t)a->ndecks * sizeof(srs_deck_t) +
                (size_t)a->ncards * sizeof(srs_entry_t);
  if (need > cap) return -1;
  srs_header_t h;
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, SRS_MAGIC, 8);
  h.today = a->today;
  h.count = (uint32_t)a->ncards;
  h.path_hash = path_hash(a->path);
  h.ndecks = (uint32_t)a->ndecks;
  memcpy(buf, &h, sizeof(h));
  size_t off = sizeof(h);
  for (int i = 0; i < a->ndecks; i++) {
    srs_deck_t e;
    memset(&e, 0, sizeof(e));
    e.id = a->decks[i].id;
    e.new_done = a->decks[i].new_done;
    e.rev_done = a->decks[i].rev_done;
    e.extra_new = a->decks[i].extra_new;
    e.ans_today = a->decks[i].ans_today;
    e.again_today = a->decks[i].again_today;
    memcpy(buf + off, &e, sizeof(e));
    off += sizeof(e);
  }
  for (int i = 0; i < a->ncards; i++) {
    card_t* c = &a->cards[i];
    srs_entry_t e;
    memset(&e, 0, sizeof(e));
    e.id = c->id;
    e.type = c->type;
    e.queue = c->queue;
    e.left = c->left;
    e.due = c->due;
    e.ivl = c->ivl;
    e.factor = c->factor;
    e.reps = c->reps;
    e.lapses = c->lapses;
    memcpy(buf + off, &e, sizeof(e));
    off += sizeof(e);
  }
  *outlen = off;
  return 0;
}

static int apply_srs(anki_t* a, const uint8_t* buf, size_t n) {
  if (n < sizeof(srs_header_t)) return -1;
  srs_header_t h;
  memcpy(&h, buf, sizeof(h));
  int old = memcmp(h.magic, SRS_MAGIC_OLD, 8) == 0;
  if (!old && memcmp(h.magic, SRS_MAGIC, 8) != 0) return -1;
  size_t off = sizeof(h);
  if (!old) {
    if (h.path_hash != path_hash(a->path)) return -1;
    int nd = (int)h.ndecks;
    if (nd > ANKI_MAX_DECKS) nd = ANKI_MAX_DECKS;
    if (off + (size_t)nd * sizeof(srs_deck_t) > n) return -1;
    for (int i = 0; i < nd; i++) {
      srs_deck_t e;
      memcpy(&e, buf + off, sizeof(e));
      off += sizeof(e);
      if (h.today != a->today) continue; /* new day: counters start at zero */
      deck_t* d = deck_at(a, e.id);
      if (!d) continue;
      d->new_done = e.new_done;
      d->rev_done = e.rev_done;
      d->extra_new = e.extra_new;
      d->ans_today = e.ans_today;
      d->again_today = e.again_today;
    }
  } else {
    /* BSRS3 had a global session counter instead of per-deck ones */
    uint32_t pathh = 0;
    memcpy(&pathh, buf + 20, 4);
    if (pathh != path_hash(a->path)) return -1;
    off = 28;
    if (h.today == a->today) {
      uint32_t sn = 0, sr = 0;
      memcpy(&sn, buf + 8, 4);
      memcpy(&sr, buf + 12, 4);
      deck_t* d = a->ndecks ? &a->decks[0] : NULL;
      if (d) {
        d->new_done = (int)sn;
        d->rev_done = (int)sr;
      }
    }
  }
  int count = (int)h.count;
  size_t avail = (n - off) / sizeof(srs_entry_t);
  if ((size_t)count > avail) count = (int)avail;
  for (int i = 0; i < count; i++) {
    srs_entry_t e;
    memcpy(&e, buf + off + (size_t)i * sizeof(srs_entry_t), sizeof(e));
    int idx = find_card(a, e.id);
    if (idx < 0) continue;
    card_t* c = &a->cards[idx];
    c->type = e.type;
    c->queue = e.queue;
    c->left = e.left;
    c->due = e.due;
    c->ivl = e.ivl;
    c->factor = e.factor ? e.factor : 2500;
    c->reps = e.reps;
    c->lapses = e.lapses;
  }
  return 0;
}

static void progress_name(anki_t* a, char* out, int cap, char kind) {
  snprintf(out, (size_t)cap, "user/%c%08lx.bin", kind, (unsigned long)path_hash(a->path));
}

int anki_progress_load(anki_t* a) {
  char name[40];
  progress_name(a, name, sizeof(name), 's');
  bobo_log("anki: progress load %s", name);
  int n = bobo_file_read(name, s_buf, sizeof(s_buf));
  if (n <= 0) {
    /* 0.2.3 and earlier kept one shared user/srs.bin; adopt it once */
    n = bobo_file_read("user/srs.bin", s_buf, sizeof(s_buf));
    if (n <= 0) return -1;
    bobo_log("anki: migrating shared srs.bin");
  }
  return apply_srs(a, s_buf, (size_t)n);
}

int anki_progress_save(anki_t* a) {
  size_t len = 0;
  char name[40];
  if (build_srs(a, s_buf, sizeof(s_buf), &len) != 0) return -1;
  progress_name(a, name, sizeof(name), 's');
  return bobo_file_write(name, s_buf, len) ? 0 : -1;
}

int anki_backup(anki_t* a) {
  size_t len = 0;
  char name[40];
  if (build_srs(a, s_buf, sizeof(s_buf), &len) != 0) return -1;
  progress_name(a, name, sizeof(name), 'b');
  return bobo_file_write(name, s_buf, len) ? 0 : -1;
}

#define HIDDEN_MAGIC "BHID"

int anki_hidden_load(anki_t* a) {
  char name[40];
  progress_name(a, name, sizeof(name), 'h');
  int n = bobo_file_read(name, s_buf, sizeof(s_buf));
  if (n < 8 || memcmp(s_buf, HIDDEN_MAGIC, 4) != 0) return -1;
  int cnt;
  memcpy(&cnt, s_buf + 4, sizeof(cnt));
  if (cnt < 0 || cnt > ANKI_MAX_DECKS) return -1;
  if ((size_t)n < 8 + (size_t)cnt * sizeof(int64_t)) return -1;
  for (int i = 0; i < cnt; i++) {
    int64_t id;
    memcpy(&id, s_buf + 8 + (size_t)i * sizeof(int64_t), sizeof(id));
    for (int j = 0; j < a->ndecks; j++)
      if (a->decks[j].id == id) a->decks[j].hidden = 1;
  }
  bobo_log("anki: hidden decks=%d", cnt);
  return 0;
}

int anki_hidden_save(anki_t* a) {
  char name[40];
  int cnt = 0;
  memcpy(s_buf, HIDDEN_MAGIC, 4);
  for (int i = 0; i < a->ndecks; i++) {
    if (!a->decks[i].hidden) continue;
    memcpy(s_buf + 8 + (size_t)cnt * sizeof(int64_t), &a->decks[i].id, sizeof(int64_t));
    cnt++;
  }
  memcpy(s_buf + 4, &cnt, sizeof(cnt));
  size_t len = 8 + (size_t)cnt * sizeof(int64_t);
  progress_name(a, name, sizeof(name), 'h');
  return bobo_file_write(name, s_buf, len) ? 0 : -1;
}

int anki_restore(anki_t* a) {
  char name[40];
  progress_name(a, name, sizeof(name), 'b');
  int n = bobo_file_read(name, s_buf, sizeof(s_buf));
  if (n <= 0) return -1;
  return apply_srs(a, s_buf, (size_t)n);
}

void anki_more_new(anki_t* a, int64_t did, int extra) {
  deck_t* d = deck_at(a, did);
  if (d) d->extra_new += extra;
}
