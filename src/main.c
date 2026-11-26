#include "anki.h"
#include "bobo_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_local(void);

static anki_t g;
static char g_err[200];
static int g_page; /* 0 files, 1 decks, 2 study, 3 done, 4 stats */
static char g_msg[96];
static char g_files[24][72];
static int g_nfiles;
static uint32_t g_queue[2048];
static int g_qn, g_qi;
static int g_show_back;
static int g_scroll;
static uint32_t g_reveal;
static int64_t g_deck;
static char g_title[64];
static int g_dirty;
typedef struct {
  int valid;
  int qi;
  uint8_t type, queue, left;
  int32_t due, ivl;
  uint16_t factor, reps, lapses;
  int new_done, rev_done, ans_today, again_today;
} undo_t;
static undo_t g_undo;

typedef struct {
  int x, y, w, h, id;
} zone_t;
static zone_t g_zones[48];
static int g_nzones;

static int px(int n) { return bobo_screen_width() > 600 ? n * 5 / 4 : n; }

static void zone_clear(void) { g_nzones = 0; }
static void zone_add(int x, int y, int w, int h, int id) {
  if (g_nzones < 48) {
    g_zones[g_nzones].x = x;
    g_zones[g_nzones].y = y;
    g_zones[g_nzones].w = w;
    g_zones[g_nzones].h = h;
    g_zones[g_nzones].id = id;
    g_nzones++;
  }
}
static int zone_hit(int x, int y) {
  for (int i = g_nzones - 1; i >= 0; i--) {
    zone_t* z = &g_zones[i];
    if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h) return z->id;
  }
  return -1;
}

/* ---------- text drawing ---------- */

static int utf8_len(const char* s) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) return 1;
  if ((c >> 5) == 6) return 2;
  if ((c >> 4) == 14) return 3;
  if ((c >> 3) == 30) return 4;
  return 1;
}

/* Draw wrapped text inside the box; returns the y after the last drawn line. */
/* Draw wrapped text; skip the first `skip` lines. Returns the y after the last painted line. */
static int draw_wrapped_skip(const char* text, int x, int y, int w, int max_y, int font, bool bold, int skip, int* drawn) {
  int lh = bobo_line_height(font) + px(4);
  int cur_y = y, line_w = 0, idx = 0, n_drawn = 0;
  char buf[1024];
  const char* line_start = text;
  for (const char* p = text;;) {
    if (!*p || *p == '\n') {
      int n = (int)(p - line_start);
      if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
      memcpy(buf, line_start, (size_t)n);
      buf[n] = 0;
      if (idx >= skip && cur_y + lh <= max_y) {
        bobo_draw_text(font, x, cur_y, buf, true, bold);
        cur_y += lh;
        n_drawn++;
      }
      if (idx >= skip && cur_y + lh > max_y && n_drawn > 0) {
        /* space exhausted: fall through to count remaining lines only */
      }
      idx++;
      line_w = 0;
      if (!*p) break;
      p++;
      line_start = p;
      continue;
    }
    int cl = utf8_len(p);
    char ch[8];
    memcpy(ch, p, (size_t)cl);
    ch[cl] = 0;
    int cw = bobo_text_width(font, ch, bold);
    if (line_w + cw > w && p != line_start) {
      int n = (int)(p - line_start);
      if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
      memcpy(buf, line_start, (size_t)n);
      buf[n] = 0;
      if (idx >= skip && cur_y + lh <= max_y) {
        bobo_draw_text(font, x, cur_y, buf, true, bold);
        cur_y += lh;
        n_drawn++;
      }
      idx++;
      line_w = 0;
      line_start = p;
    }
    line_w += cw;
    p += cl;
  }
  if (drawn) *drawn = n_drawn;
  return cur_y;
}

static int draw_wrapped(const char* text, int x, int y, int w, int max_y, int font, bool bold) {
  return draw_wrapped_skip(text, x, y, w, max_y, font, bold, 0, NULL);
}

static int g_refresh = BOBO_REFRESH_FAST;   /* 首屏/重画用 HALF，其余交互用 FAST */

/* ---------- file list ---------- */

static int ends_with_apkg(const char* name) {
  int n = (int)strlen(name);
  if (n < 5) return 0;
  const char* e = name + n - 5;
  return (e[0] == '.' && (e[1] == 'a' || e[1] == 'A') && (e[2] == 'p' || e[2] == 'P') && (e[3] == 'k' || e[3] == 'K') &&
          (e[4] == 'g' || e[4] == 'G'));
}

static void scan_files(void) {
  g_nfiles = 0;
  int d = bobo_sd_dir_open("/Anki");
  if (d <= 0) {
    snprintf(g_err, sizeof(g_err), "没有找到 TF 卡的 /Anki 目录：请在卡上建这个文件夹再放 .apkg");
    return;
  }
  bobo_sd_entry_t e;
  while (bobo_sd_dir_next(d, &e) && g_nfiles < 24) {
    if (e.is_dir || !ends_with_apkg(e.name)) continue;
    snprintf(g_files[g_nfiles], sizeof(g_files[0]), "%.71s", e.name);
    g_nfiles++;
  }
  bobo_sd_dir_close(d);
  if (g_nfiles == 0) snprintf(g_err, sizeof(g_err), "/Anki 里没有 .apkg 文件");
}

static void draw_files(void) {
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header("Anki 卡片");
  int w = bobo_screen_width();
  int margin = px(16);
  if (g_nfiles == 0) {
    draw_wrapped(g_err[0] ? g_err : "正在读取…", margin, top + px(24), w - margin * 2, bobo_screen_height() - px(40),
                 BOBO_FONT_BODY, false);
  }
  int y = top + px(12);
  if (g_err[0] && g_nfiles > 0) {
    y = draw_wrapped(g_err, margin, y, w - margin * 2, bobo_screen_height() - px(20), BOBO_FONT_SMALL, false) + px(10);
    bobo_draw_line(margin, y - px(6), w - margin, y - px(6), 1, false);
  }
  int rowh = px(76);
  for (int i = 0; i < g_nfiles; i++) {
    if (y + rowh > bobo_screen_height()) break;
    bobo_draw_text(BOBO_FONT_TITLE, margin, y + px(8), g_files[i], true, false);
    bobo_draw_line(margin, y + rowh - px(8), w - margin, y + rowh - px(8), 1, false);
    zone_add(margin, y, w - margin * 2, rowh, 100 + i);
    y += rowh;
  }
  bobo_refresh(g_refresh);
}

/* ---------- deck list ---------- */

static int deck_index_by_id(int64_t id) {
  for (int i = 0; i < g.ndecks; i++)
    if (g.decks[i].id == id) return i;
  return -1;
}

static void draw_decks(void) {
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header(g_title);
  int w = bobo_screen_width();
  int h = bobo_screen_height();
  int margin = px(16);
  int y = top + px(8);
  int footer = px(76);
  int rowh = px(96);
  for (int i = 0; i < g.ndecks; i++) {
    if (y + rowh > h - footer) break;
    int nn, ln, dn, tt;
    anki_counts(&g, g.decks[i].id, &nn, &ln, &dn, &tt);
    deck_t* d = &g.decks[i];
    bobo_draw_text(BOBO_FONT_TITLE, margin, y + px(2), d->name, true, d->new_done + d->rev_done > 0);
    char sub[96];
    snprintf(sub, sizeof(sub), "新 %d  学 %d  待复习 %d  共 %d", nn, ln, dn, tt);
    bobo_draw_text(BOBO_FONT_BODY, margin, y + px(38), sub, true, false);
    snprintf(sub, sizeof(sub), "今日 新卡 %d/%d  复习完成 %d / %d", d->new_done, g.new_per_day + d->extra_new,
             d->rev_done, g.rev_per_day);
    bobo_draw_text(BOBO_FONT_SMALL, margin, y + px(66), sub, true, false);
    bobo_draw_line(margin, y + rowh - px(6), w - margin, y + rowh - px(6), 1, false);
    zone_add(margin, y, w - margin * 2, rowh, 600 + i);
    y += rowh;
  }
  if (g_msg[0]) {
    bobo_draw_text(BOBO_FONT_SMALL, margin, h - footer - px(18), g_msg, true, false);
  }
  int gap = px(8);
  int bw = (w - margin * 2 - gap * 2) / 3;
  int by = h - px(64);
  const char* labels[3] = {"统计", "备份进度", "恢复进度"};
  for (int i = 0; i < 3; i++) {
    int bx = margin + i * (bw + gap);
    bobo_draw_round_rect(bx, by, bw, px(48), 1, px(8), true);
    int tw = bobo_text_width(BOBO_FONT_BODY, labels[i], true);
    bobo_draw_text(BOBO_FONT_BODY, bx + (bw - tw) / 2, by + px(10), labels[i], true, true);
    zone_add(bx, by, bw, px(48), 220 + i);
  }
  bobo_refresh(g_refresh);
}

/* ---------- study ---------- */

static void start_deck(int64_t did) {
  g_deck = did;
  g_qi = 0;
  g_undo.valid = 0;
  g_qn = anki_queue(&g, did, g_queue, 2048);
  g_show_back = 0;
  g_scroll = 0;
  g_reveal = 0;
  g_page = g_qn ? 2 : 3;
}

static void draw_done(void);

static void draw_line_card(card_t* c, int margin, int y, int w, int bottom) {
  int i, n = c->nlines;
  int lh = bobo_line_height(BOBO_FONT_TITLE) + px(4);
  /* title / author line kept at the top of the front text */
  char head[ANKI_TEXT_MAX];
  int hn = 0;
  if (c->front) {
    while (c->front[hn] && c->front[hn] != '\n' && hn < (int)sizeof(head) - 1) {
      head[hn] = c->front[hn];
      hn++;
    }
  }
  head[hn] = 0;
  if (hn) {
    bobo_draw_text(BOBO_FONT_TITLE, margin, y, head, true, true);
    y += lh + px(6);
  }
  for (i = 0; i < n && y + lh <= bottom; i++) {
    card_line_t* L = &c->lines[i];
    int start_y = y;
    if (g_reveal & (1u << i)) {
      int used = draw_wrapped(L->text ? L->text : "", margin, y, w - margin * 2 - px(8), bottom,
                             BOBO_FONT_TITLE, false);
      y = used + px(2);
    } else {
      char blank[80];
      int bn = 0;
      int nb = L->blanks > 0 ? L->blanks : 4;
      if (nb > 12) nb = 12;
      for (int k = 0; k < nb && bn < (int)sizeof(blank) - 4; k++) {
        memcpy(blank + bn, "＿", 3);
        bn += 3;
      }
      blank[bn] = 0;
      bobo_draw_text(BOBO_FONT_TITLE, margin, y, blank, true, false);
      y += lh;
    }
    zone_add(margin, start_y - px(2), w - margin * 2, y - start_y + px(4), 700 + i);
  }
  /* keep line ids stable: add zones for the visible ones only */
  const char* hint = "点空白行显示该行";
  /* shortcut: reveal all lines and rate */
  int bw = bobo_text_width(BOBO_FONT_SMALL, "显示全部", true) + px(20);
  int bx = bobo_screen_width() - margin - bw;
  bobo_draw_text(BOBO_FONT_SMALL, bx + px(10), bobo_screen_height() - px(50), "显示全部", true, false);
  zone_add(bx, bobo_screen_height() - px(62), bw, px(38), 760);
  int hy = bobo_screen_height() - px(24);
  bobo_draw_text(BOBO_FONT_SMALL, bobo_screen_width() / 2 - bobo_text_width(BOBO_FONT_SMALL, hint, false) / 2, hy,
                 hint, true, false);
}

static void draw_study(void) {
  bobo_clear();
  zone_clear();
  int w = bobo_screen_width();
  int h = bobo_screen_height();
  int top = bobo_draw_header(g.decks[deck_index_by_id(g_deck)].name);
  int margin = px(16);
  if (g_qi >= g_qn) {
    draw_done();
    return;
  }
  card_t* c = &g.cards[g_queue[g_qi]];
  int y = top + px(8);
  /* progress line */
  if (g_undo.valid) {
    bobo_draw_text(BOBO_FONT_SMALL, margin, y, "撤销上一步", true, false);
    zone_add(margin, y - px(4), px(90), bobo_line_height(BOBO_FONT_SMALL) + px(8), 350);
  }
  char sub[64];
  snprintf(sub, sizeof(sub), "%d / %d", g_qi + 1, g_qn);
  bobo_draw_text(BOBO_FONT_SMALL, w - margin - bobo_text_width(BOBO_FONT_SMALL, sub, false), y, sub, true, false);
  y += bobo_line_height(BOBO_FONT_SMALL) + px(8);
  int rating_h = g_show_back ? px(72) : 0;
  int bottom = h - px(10) - rating_h - (g_show_back ? px(16) : 0);
  int text_bottom = bottom - px(26);
  bobo_draw_line(margin, y, w - margin, y, 1, false);
  y += px(10);
  /* one side at a time, using the whole page; swipe scrolls long text */
  if (!g_show_back && c->nlines > 0) {
    draw_line_card(c, margin, y, w, text_bottom);
    bobo_refresh(g_refresh);
    return;
  }
  const char* body = g_show_back ? (c->back ? c->back : "") : (c->front ? c->front : "");
  int used = draw_wrapped_skip(body, margin, y, w - margin * 2 - px(8), text_bottom, BOBO_FONT_TITLE, false,
                               g_scroll, NULL);
  const char* hint = NULL;
  if (g_scroll > 0) hint = "上下滑动翻页";
  else if (used >= text_bottom - px(4)) hint = "上下滑动看完整内容";
  else if (!g_show_back) hint = "点屏幕看答案";
  if (hint) {
    int hy = h - rating_h - px(24);
    bobo_draw_text(BOBO_FONT_SMALL, w / 2 - bobo_text_width(BOBO_FONT_SMALL, hint, false) / 2, hy, hint, true,
                   false);
  }
  if (g_show_back) {
    const char* labels[4] = {"忘记", "困难", "良好", "简单"};
    int gap = px(8);
    int bw = (w - margin * 2 - gap * 3) / 4;
    int by = h - px(10) - rating_h;
    for (int i = 0; i < 4; i++) {
      int bx = margin + i * (bw + gap);
      if (i == 2) {
        bobo_fill_round_rect(bx, by, bw, rating_h, px(10), true);
        int tw = bobo_text_width(BOBO_FONT_TITLE, labels[i], true);
        bobo_draw_text(BOBO_FONT_TITLE, bx + (bw - tw) / 2, by + (rating_h - bobo_line_height(BOBO_FONT_TITLE)) / 2,
                       labels[i], false, true);
      } else {
        bobo_draw_round_rect(bx, by, bw, rating_h, 1, px(10), true);
        int tw = bobo_text_width(BOBO_FONT_TITLE, labels[i], true);
        bobo_draw_text(BOBO_FONT_TITLE, bx + (bw - tw) / 2, by + (rating_h - bobo_line_height(BOBO_FONT_TITLE)) / 2,
                       labels[i], true, true);
      }
      zone_add(bx, by, bw, rating_h, 400 + i + 1);
    }
  } else {
    /* the whole page reveals the answer */
    zone_add(margin, top + px(8), w - margin * 2, h - top - px(8), 300);
  }
  bobo_refresh(g_refresh);
}

static uint32_t now_local(void) {
  int64_t t = bobo_time();
  if (t == 0) return g.today * 86400u;
  return (uint32_t)(t + bobo_tz_offset());
}

static void answer(int rating) {
  card_t* c = &g.cards[g_queue[g_qi]];
  deck_t* d = NULL;
  for (int i = 0; i < g.ndecks; i++)
    if (g.decks[i].id == c->did) d = &g.decks[i];
  g_undo.valid = 1;
  g_undo.qi = g_qi;
  g_undo.type = c->type;
  g_undo.queue = c->queue;
  g_undo.left = c->left;
  g_undo.due = c->due;
  g_undo.ivl = c->ivl;
  g_undo.factor = c->factor;
  g_undo.reps = c->reps;
  g_undo.lapses = c->lapses;
  if (d) {
    g_undo.new_done = d->new_done;
    g_undo.rev_done = d->rev_done;
    g_undo.ans_today = d->ans_today;
    g_undo.again_today = d->again_today;
  }
  g.now = now_local();
  anki_answer(&g, c, rating);
  g_dirty++;
  if (g_dirty >= 8) {
    anki_progress_save(&g);
    g_dirty = 0;
  }
  g_qi++;
  g_show_back = 0;
  g_scroll = 0;
  g_reveal = 0;
  if (g_qi >= g_qn) g_page = 3;
}

/* ---------- done ---------- */

static void undo_last(void) {
  if (!g_undo.valid) return;
  card_t* c = &g.cards[g_queue[g_undo.qi]];
  deck_t* d = NULL;
  for (int i = 0; i < g.ndecks; i++)
    if (g.decks[i].id == c->did) d = &g.decks[i];
  c->type = g_undo.type;
  c->queue = g_undo.queue;
  c->left = g_undo.left;
  c->due = g_undo.due;
  c->ivl = g_undo.ivl;
  c->factor = g_undo.factor;
  c->reps = g_undo.reps;
  c->lapses = g_undo.lapses;
  if (d) {
    d->new_done = g_undo.new_done;
    d->rev_done = g_undo.rev_done;
    d->ans_today = g_undo.ans_today;
    d->again_today = g_undo.again_today;
  }
  g_qi = g_undo.qi;
  g_show_back = 0;
  g_scroll = 0;
  g_reveal = 0;
  g_page = 2;
  g_undo.valid = 0;
  g_dirty = 0;
  anki_progress_save(&g);
}

static void draw_done(void) {
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header("完成");
  int w = bobo_screen_width();
  int h = bobo_screen_height();
  int margin = px(16);
  int y = top + px(28);
  bobo_draw_text(BOBO_FONT_TITLE, margin, y, "这一轮复习完了", true, true);
  y += bobo_line_height(BOBO_FONT_TITLE) + px(16);
  deck_t* d = NULL;
  for (int i = 0; i < g.ndecks; i++)
    if (g.decks[i].id == g_deck) d = &g.decks[i];
  char line[128];
  if (d) {
    snprintf(line, sizeof(line), "今日新卡 %d / %d 张", d->new_done, g.new_per_day + d->extra_new);
    bobo_draw_text(BOBO_FONT_BODY, margin, y, line, true, false);
    y += bobo_line_height(BOBO_FONT_BODY) + px(8);
    snprintf(line, sizeof(line), "今日复习完成 %d / %d 张", d->rev_done, g.rev_per_day);
    bobo_draw_text(BOBO_FONT_BODY, margin, y, line, true, false);
    y += px(16);
  }
  int nn, ln, dn, tt;
  anki_counts(&g, g_deck, &nn, &ln, &dn, &tt);
  snprintf(line, sizeof(line), "本牌组还剩：新 %d · 学习中 %d · 待复习 %d", nn, ln, dn);
  y = draw_wrapped(line, margin, y, w - margin * 2, h - px(160), BOBO_FONT_SMALL, false) + px(10);
  int btn_y = h - px(150);
  if (nn > 0) {
    bobo_fill_round_rect(margin, btn_y, w - margin * 2, px(56), px(10), true);
    int tw = bobo_text_width(BOBO_FONT_BODY, "继续学习 20 张", true);
    bobo_draw_text(BOBO_FONT_BODY, w / 2 - tw / 2, btn_y + px(14), "继续学习 20 张", false, true);
    zone_add(margin, btn_y, w - margin * 2, px(56), 501);
    btn_y += px(70);
  }
  bobo_draw_round_rect(margin, btn_y, w - margin * 2, px(56), 1, px(10), true);
  int tw = bobo_text_width(BOBO_FONT_BODY, "返回牌组", true);
  bobo_draw_text(BOBO_FONT_BODY, w / 2 - tw / 2, btn_y + px(14), "返回牌组", true, true);
  zone_add(margin, btn_y, w - margin * 2, px(56), 500);
  bobo_refresh(g_refresh);
}

static void draw_stats(void) {
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header("统计");
  int w = bobo_screen_width();
  int h = bobo_screen_height();
  int margin = px(16);
  int y = top + px(10);
  char line[128];
  bobo_draw_text(BOBO_FONT_SMALL, margin, y, g_title, true, false);
  y += bobo_line_height(BOBO_FONT_SMALL) + px(8);
  for (int i = 0; i < g.ndecks && y + px(96) < h - px(70); i++) {
    deck_t* d = &g.decks[i];
    int nn, ln, dn, tt;
    anki_counts(&g, d->id, &nn, &ln, &dn, &tt);
    bobo_draw_text(BOBO_FONT_BODY, margin, y, d->name, true, true);
    y += bobo_line_height(BOBO_FONT_BODY) + px(2);
    snprintf(line, sizeof(line), "共 %d 张：新 %d · 学习中 %d · 待复习 %d", tt, nn, ln, dn);
    bobo_draw_text(BOBO_FONT_SMALL, margin, y, line, true, false);
    y += bobo_line_height(BOBO_FONT_SMALL) + px(2);
    int acc = d->ans_today > 0 ? (d->ans_today - d->again_today) * 100 / d->ans_today : 0;
    snprintf(line, sizeof(line), "今日 新卡 %d/%d · 复习完成 %d / %d · 答题 %d，记得 %d%%", d->new_done,
             g.new_per_day + d->extra_new, d->rev_done, g.rev_per_day, d->ans_today, acc);
    bobo_draw_text(BOBO_FONT_SMALL, margin, y, line, true, false);
    y += bobo_line_height(BOBO_FONT_SMALL) + px(8);
    if (i + 1 < g.ndecks) bobo_draw_line(margin, y - px(4), w - margin, y - px(4), 1, false);
  }
  bobo_draw_line(margin, h - px(64), w - margin, h - px(64), 1, false);
  bobo_draw_text(BOBO_FONT_SMALL, w / 2 - bobo_text_width(BOBO_FONT_SMALL, "返回牌组", false) / 2, h - px(40),
                 "返回牌组", true, false);
  bobo_refresh(g_refresh);
}

static void draw(int mode) {
  g_refresh = mode;
  if (g_page == 0) draw_files();
  else if (g_page == 1) draw_decks();
  else if (g_page == 2) draw_study();
  else if (g_page == 4) draw_stats();
  else draw_done();
}

/* ---------- actions ---------- */

static void open_file(int idx) {
  if (idx < 0 || idx >= g_nfiles) return;
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header(g_files[idx]);
  draw_wrapped("正在读取卡组…", px(16), top + px(24), bobo_screen_width() - px(32), bobo_screen_height() - px(40),
               BOBO_FONT_BODY, false);
  bobo_refresh(BOBO_REFRESH_FAST);
  char path[300];
  snprintf(path, sizeof(path), "/Anki/%s", g_files[idx]);
  if (g.cards) {
    anki_progress_save(&g);
    anki_free(&g);
  }
  memset(&g, 0, sizeof(g));
  int r = anki_load(&g, path, g_err, sizeof(g_err));
  if (r != 0) {
    g_page = 0;
    draw(BOBO_REFRESH_FAST);
    return;
  }
  g.today = anki_today();
  g.now = now_local();
  anki_progress_load(&g);
  g_err[0] = 0;
  snprintf(g_title, sizeof(g_title), "%.60s", g_files[idx]);
  if (g.ndecks == 1) {
    g_page = 1;
    start_deck(g.decks[0].id);
    draw(BOBO_REFRESH_FAST);
    return;
  }
  g_page = 1;
  draw(BOBO_REFRESH_FAST);
}

int app_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  bobo_clear();
  zone_clear();
  int top = bobo_draw_header("Anki 卡片");
  draw_wrapped("正在读取…", px(16), top + px(24), bobo_screen_width() - px(32), bobo_screen_height(), BOBO_FONT_BODY,
               false);
  bobo_refresh(BOBO_REFRESH_HALF);
  scan_files();
  g_page = 0;
  draw(BOBO_REFRESH_HALF);   /* 首屏 */
  for (;;) {
    bobo_event_t ev;
    if (!bobo_wait_event(&ev, 30000)) continue;
    if (ev.type == BOBO_EV_EXIT) {
      if (g.cards) anki_progress_save(&g);
      anki_free(&g);
      return 0;
    }
    if (ev.type == BOBO_EV_REDRAW) {
      draw(BOBO_REFRESH_HALF);
      continue;
    }
    if (ev.type == BOBO_EV_BUTTON) {
      if (ev.button == BOBO_BTN_BACK) {
        if (g_page == 4) {
          g_page = 1;
          draw(BOBO_REFRESH_FAST);
        } else if (g_page == 2 || g_page == 1) {
          anki_progress_save(&g);
          g_page = g_page == 2 ? 1 : 0;
          draw(BOBO_REFRESH_FAST);
        } else if (g_page == 3) {
          g_page = 1;
          draw(BOBO_REFRESH_FAST);
        } else {
          if (g.cards) anki_progress_save(&g);
          anki_free(&g);
          return 0;
        }
      } else if (ev.button == BOBO_BTN_UP && g_page == 2 && !g_show_back) {
        g_show_back = 1;
        g_scroll = 0;
        draw(BOBO_REFRESH_FAST);
      } else if (ev.button == BOBO_BTN_DOWN && g_page == 2 && g_show_back) {
        answer(3);
        draw(BOBO_REFRESH_FAST);
      }
      continue;
    }
    if (ev.type == BOBO_EV_SWIPE && g_page == 2) {
      if (ev.dy < 0) {
        g_scroll += 2;
        if (g_scroll > 240) g_scroll = 240;
        draw(BOBO_REFRESH_FAST);
      } else if (ev.dy > 0 && g_scroll > 0) {
        g_scroll -= 2;
        if (g_scroll < 0) g_scroll = 0;
        draw(BOBO_REFRESH_FAST);
      }
      continue;
    }
    if (ev.type == BOBO_EV_TAP || ev.type == BOBO_EV_LONG_TAP) {
      int id = zone_hit(ev.x, ev.y);
      if (g_page == 0 && id >= 100 && id < 124) {
        open_file(id - 100);
      } else if (g_page == 1 && id >= 600 && id < 600 + ANKI_MAX_DECKS) {
        int di = id - 600;
        if (di < g.ndecks) {
          start_deck(g.decks[di].id);
          draw(BOBO_REFRESH_FAST);
        }
      } else if (g_page == 2) {
        if (id == 760) {
          card_t* c2 = &g.cards[g_queue[g_qi]];
          g_reveal = c2->nlines >= 31 ? 0xffffffffu : ((1u << c2->nlines) - 1);
          g_show_back = 1;
          draw(BOBO_REFRESH_FAST);
        } else if (id >= 700 && id < 740) {
          card_t* c2 = &g.cards[g_queue[g_qi]];
          int li = id - 700;
          if (li < c2->nlines) {
            g_reveal |= (1u << li);
            uint32_t all = c2->nlines >= 31 ? 0xffffffffu : ((1u << c2->nlines) - 1);
            if (g_reveal == all) g_show_back = 1;
            draw(BOBO_REFRESH_FAST);
          }
        } else if (id == 350) {
          undo_last();
          draw(BOBO_REFRESH_FAST);
        } else if (id == 300) {
          g_show_back = 1;
          g_scroll = 0;
          draw(BOBO_REFRESH_FAST);
        } else if (id >= 401 && id <= 404) {
          answer(id - 400);
          draw(BOBO_REFRESH_FAST);
        }
      } else if (g_page == 1 && id >= 220 && id <= 222) {
        if (id == 220) {
          g_page = 4;
          draw(BOBO_REFRESH_FAST);
        } else if (id == 221) {
          anki_progress_save(&g);
          snprintf(g_msg, sizeof(g_msg), anki_backup(&g) == 0 ? "已备份到 user/srs-backup.bin" : "备份失败");
          draw(BOBO_REFRESH_FAST);
        } else {
          snprintf(g_msg, sizeof(g_msg), anki_restore(&g) == 0 ? "已从备份恢复，重新进入卡组生效" : "没有可用的备份");
          draw(BOBO_REFRESH_FAST);
        }
      } else if (g_page == 3 && id == 500) {
        g_page = 1;
        draw(BOBO_REFRESH_FAST);
      } else if (g_page == 3 && id == 501) {
        anki_more_new(&g, g_deck, 20);
        start_deck(g_deck);
        draw(BOBO_REFRESH_FAST);
      } else if (g_page == 4) {
        g_page = 1;
        draw(BOBO_REFRESH_FAST);
      }
    }
  }
}
