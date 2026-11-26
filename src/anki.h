#pragma once
#include <stdint.h>

#define ANKI_MAX_CARDS 12000
#define ANKI_MAX_DECKS 64
#define ANKI_TEXT_MAX 8192

typedef struct {
  int64_t id;
  char name[48];
  int new_done;   /* today */
  int rev_done;   /* today */
  int extra_new;  /* today's extra allowance from "continue" */
  int ans_today;
  int again_today;
} deck_t;

typedef struct {
  uint16_t blanks; /* width hint for the blank line */
  char* text;
} card_line_t;

typedef struct {
  uint32_t id;
  int64_t nid;
  int64_t did;
  uint8_t type;  /* 0 new, 1 learning, 2 review, 3 relearning */
  uint8_t queue;
  int32_t due;   /* learning: local epoch seconds; review: local day number */
  int32_t ivl;   /* days */
  uint16_t factor; /* 2500 = 250% */
  uint16_t reps;
  uint16_t lapses;
  uint16_t left; /* remaining learning step index */
  char* front;
  char* back;
  card_line_t* lines; /* per-line recall cards only */
  uint8_t nlines;
} card_t;

typedef struct {
  deck_t decks[ANKI_MAX_DECKS];
  int ndecks;
  card_t* cards;
  int ncards;
  int new_per_day;
  int rev_per_day;
  int step1;
  int step2;
  int graduating_ivl;
  int easy_ivl;
  int relearn_step;
  uint32_t today;
  uint32_t now;
  int session_new; /* whole file, informational */
  int session_rev;
  char path[300];
} anki_t;

/* Load an apkg into memory. Returns 0, or -1 with a message in err. */
int anki_load(anki_t* a, const char* path, char* err, int errcap);
void anki_free(anki_t* a);

/* Progress: user/srs.bin inside the app folder. */
int anki_progress_load(anki_t* a);
int anki_progress_save(anki_t* a);

/* Raise today's new-card allowance for one deck ("continue learning"). */
void anki_more_new(anki_t* a, int64_t did, int extra);

/* Backup / restore progress inside the app folder. */
int anki_backup(anki_t* a);
int anki_restore(anki_t* a);

/* Fill queue with card indexes to study in order; returns count. */
int anki_queue(anki_t* a, int64_t did, uint32_t* queue, int cap);

/* Apply a rating (1 again, 2 hard, 3 good, 4 easy) and persist later. */
void anki_answer(anki_t* a, card_t* c, int rating);

int anki_counts(anki_t* a, int64_t did, int* new_n, int* learn_n, int* due_n, int* total);

/* local day number from bobo time */
uint32_t anki_today(void);
