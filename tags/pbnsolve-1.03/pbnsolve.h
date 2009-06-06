/* Copyright 2007 Jan Wolter
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "bitstring.h"
#include "config.h"


/* Puzzle solution - Representing a partial solution of a puzzle.
 *
 *  For each cell in the puzzle, we have a Cell structure  This contains
 *  a bitstring with one bit for each color in the puzzle.  Bit i is 1 if
 *  it is possible that the puzzle could be color i.  The cell structure also
 *  keeps a count of the number of bits set in the bitstring.
 *
 *  The solution structure contains arrays of pointers to Cells.  For grid
 *  puzzles we have sol->line[D_ROW] and sol->line[D_COL].  For triddlers
 *  we have sol->line[D_HORIZ], sol->line[D_UP] and sol->line[D_DOWN].
 *  Each of these will point to an array of pointers to arrays of pointers
 *  to cells.  So sol->line[D_ROW][3] is an array representing row 3 of the
 *  grid, and sol->line[D_COL][0] is an array representing the first column
 *  of the puzzle.   Each of these line arrays is NULL terminated (because
 *  for triddlers they will be all different lengths.
 *
 *  sol->line[D_ROW][3][0] and sol->line[D_COL][0][3] will point to the same
 *  Cell object, the cell for column 0, row 3.  In that cell object,
 *  cell->line[D_ROW] is 3, and cell->line[D_COL] is 0.
 *
 *  This somewhat redundant array structure is meant to generalize to things
 *  like triddlers more easily, and simplify a lot of the solver coding by
 *  making rows and columns work exactly alike.
 */

typedef struct {
    short line[3];	/* coordinates of this cell */
    short n;		/* Number of bits set in the bit string */
    bit_decl(bit,1);	/* bit string with 1 for each possible color */

    /* Do not define any fields after 'bit'.  When we allocate memory for this
     * data structure, we will actually be allocating more if we need longer
     * bitstrings.  Though actually that would only happen if we had a puzzle
     * with more than 32 colors, which isn't too likely.
     */
} Cell;

/* Macro to test if a cell may be a given color */
#define may_be(cell,color)  bit_test(cell->bit,color)

typedef struct {
    int nset;		/* Number of directions 2 for grids, 3 for triddlers */
    Cell ***line[3];	/* 2 or three roots for the cell array */
    int n[3];		/* Length of the line[] arrays */
} Solution;


/* Solution List - A list of solutions, loaded from the XML file */

#define STYPE_GOAL 0
#define STYPE_SOLUTION 1
#define STYPE_SAVED 2

typedef struct solution_list {
    char *id;		/* ID string for solution (or NULL) */
    int type;		/* Solution type (STYPE_* values) */
    char *note;		/* Any text describing the solution */
    Solution s;		/* The solution */
    struct solution_list *next;
} SolutionList;


/* Clue Structure - describes a row or column clue. */

typedef struct {
    int n,s;		/* Number of clues and size of array */
    int *length;	/* Array of clue lengths */
    int *color;		/* Array of clue colors (indexes into puz->color) */
    int jobindex;	/* Where is this clue on the job list? -1 if not. */
    int slack;		/* Amount of slack in this clue */
} Clue;


/* Color Definition - definition of one color. */

typedef struct {
    char *name;		/* Color name */
    char *rgb;		/* RGB color value */
    char ch;		/* A character to represent color */
} ColorDef;


/* Linked list of lines that need working on */

typedef struct {
    int priority;	/* High number for more promissing jobs */
    int dir;		/* Direction of line that needs work (D_ROW/D_COL) */
    int n;		/* Index of line that needs work */
} Job;

/* History of things set, used for backtracking */

typedef struct hist_list {
    struct hist_list *prev;
    char branch;	/* Was this a branch point? */
    Cell *cell;		/* The cell that was set */
    short n;		/* Old n value of cell */
    bit_decl(bit,1);	/* Old bit string of cell */
    /* Do not define any fields after 'bit'.  When we allocate memory for this
     * data structure, we will actually be allocating more if we need longer
     * bitstrings.
     */
} Hist;

/* Probe Merge List - settings that have been made for all probes on the
 * current cell.
 */

typedef struct merge_elem {
    struct merge_elem *next;	/* Link to next cell in list */
    Cell *cell;			/* The cell that was set */
    short maxc;			/* Maximum guess number */
    bit_decl(bit,1);	 	/* each eliminated color has a 1 bit */
    /* Do not define any fields after 'bit'.  When we allocate memory for this
     * data structure, we will actually be allocating more if we need longer
     * bitstrings.
     */
} MergeElem;

/* Puzzle definition - Describes a puzzle (not it's solution).
 *
 * Color table.  puz->color is an array of color definitions used in the
 *  puz.  puz->color[0] is always the background color.  All others are
 *  clue colors.  ncolor gives the number of colors, including the background
 *  color.
 *
 * Clue sets.  The number of clue sets is given by nset, and will be either
 *  2 for a grid or 3 for a griddler.  So, for a grid puzzle, the side clues
 *  are given by puz->clue[D_ROW] and the top clues are given by
 *  puz->clue[D_TOP].  Each of these will point to an array of Clue objects,
 *  as defined above which give the clues for the corresponding rows or
 *  columns.
 */

/* Puzzle types stored in puz->type */

#define PT_GRID 0	/* Standard Grid puzzle */
#define PT_TRID 1	/* "triddler" */

/* Indices into puz->clue[] array used in grid puzzles */
#define D_ROW 0		/* Clues on side of puzzle */
#define D_COL 1		/* Clues on top of puzzle */

/* Indices into puz->clue[] array used in triddler puzzles */
#define D_HORIZ 0	/* Clues for - horizontal rows */
#define D_UP    1	/* Clues for / lines that climb from left to right */
#define D_DOWN  2	/* Clues for \ lines that descend from left to right */

typedef struct {
    int type;		/* Puzzle type.  Some PT_ value */
    int ncolor,scolor;	/* Number of colors used, size of color array */
    int nset;		/* Number of directions 2 for grids, 3 for triddlers */
    Clue *clue[3];	/* Arrays of clues (nset of which are used) */
    int n[3];		/* Length of the clue[] arrays */
    ColorDef *color;	/* Array of color definitions */
    char *source;
    char *id;
    char *title;
    char *seriestitle;
    char *author;
    char *copyright;
    char *description;
    SolutionList *sol;	/* List of solutions loaded from the file */
    int ncells;		/* Number of cells in the puzzle */
    int nsolved;	/* Number of cells with only one possible color */
    Job *job;		/* Pointer to priority queue of jobs */
    int sjob, njob;	/* Allocated and current size of job array */
    Hist *history;	/* Undo history, if any */
    char *found;	/* A stringified solution we have found, if any */
} Puzzle;

/* Various file formats that we can read */

#define FF_UNKNOWN	0	/* File type unknown */
#define FF_XML		1	/* Our own XML file format */
#define FF_MK		2	/* The Olsak's MK file format */
#define FF_NIN		3	/* Wilk's NIN file format */
#define FF_NON		4	/* Simpson's NON file format */
#define FF_PBM		5	/* netpbm PBM image file */
#define FF_LP		6	/* Bosch's format for LP solver */



/* Debug Flags - You can disable any of these completely by just defining them
 * to zero.  Then the optimizer will throw out those error messages and things
 * will run a tiny bit faster.
 */

#define VA verb[0]	/* Top Level Messages */
#define VB verb[1]	/* Backtracking */
#define VE verb[2]	/* Try Everything */
#define VG verb[3]	/* Guessing */
#define VJ verb[4]	/* Job Management */
#define VL verb[5]	/* Line Solver Details */
#define VM verb[6]	/* Merging */
#define VP verb[7]	/* Probing */
#define VU verb[8]	/* Undo Information from Job Management */
#define VS verb[9]	/* Cell State Changes */
#define VV verb[10]	/* Report with extra verbosity */
#define VCHAR "ABEGJLMPUSV"
#define NVERB 11

extern int verb[];

/* Macros */

#define safedup(x) (x ? strdup(x) : NULL)
#define safefree(x) if (x) free(x)


/* Global variables */

extern int maylinesolve;
extern int maybacktrack;
extern int mayprobe;
extern int mergeprobe;
extern int checkunique;
extern int checksolution;
extern int mayexhaust;

/* pbnsolve.c functions */

void fail(const char *fmt, ...);

/* read.c functions */

Puzzle *load_puzzle_file(char *filename, int fmt, int index);
Puzzle *load_puzzle_mem(char *image, int fmt, int index);
Puzzle *load_puzzle_stdin(int fmt, int index);

/* puzz.c functions */

Puzzle *new_puzzle(void);
void free_puzzle(Puzzle *puz);
int find_color(Puzzle *puz, char *name);
int find_color_char(Puzzle *puz, char ch);
int find_or_add_color(Puzzle *puz, char *name);
void add_color(Puzzle *puz, char *name, char *rgb, char ch);

/* dump.c functions */
char *cluename(int type, int k);
char *CLUENAME(int type, int k);
void dump_bits(FILE *fp, Puzzle *puz, bit_type *bits);
void print_solution(FILE *fp, Puzzle *puz, Solution *sol);
void dump_line(FILE *fp, Puzzle *puz, Solution *sol, int k, int i);
void dump_solution(FILE *fp, Puzzle *puz, Solution *sol, int max);
void dump_puzzle(FILE *fp, Puzzle *puz);
void dump_jobs(FILE *fp, Puzzle *puz);
void dump_history(FILE *fp, Puzzle *puz, int full);

/* grid.c functions */
Cell *new_cell(int ncolor);
Solution *new_solution(Puzzle *puz);
void init_solution(Puzzle *puz, Solution *sol, int set);
void free_solution(Solution *sol);
void free_solution_list(SolutionList *sl);
int count_cells(Puzzle *puz, Solution *sol, int k, int i);
int count_slack(Puzzle *puz, Solution *sol, int k, int i);
int count_paint(Puzzle *puz, Solution *sol, int k, int i);
void count_cell(Puzzle *puz, Cell *cell);
char *solution_string(Puzzle *puz, Solution *sol);

/* line_lro.c functions */
void dump_lro_solve(Puzzle *puz, Solution *sol, int k, int i, bit_type *col);
int *left_solve(Puzzle *puz, Solution *sol, int k, int i);
int *right_solve(Puzzle *puz, Solution *sol, int k, int i, int ncell);
bit_type *lro_solve(Puzzle *puz, Solution *sol, int k, int i, int ncell);

/* job.c functions */
void flush_jobs(Puzzle *puz);
void init_jobs(Puzzle *puz, Solution *sol);
int next_job(Puzzle *puz, int *k, int *i);
void add_job(Puzzle *puz, int k, int i);
void add_jobs(Puzzle *puz, Cell *cell);
void add_hist(Puzzle *puz, Cell *cell, int branch);
void add_hist2(Puzzle *puz, Cell *cell, int oldn, bit_type *oldbit, int branch);
int backtrack(Puzzle *puz, Solution *sol);

/* solve.c functions */
extern int nlines, guesses, backtracks, probes, merges;
int solve(Puzzle *puz, Solution *sol);

/* exhaust.c functions */
extern int exh_runs, exh_cells;
int try_everything(Puzzle *puz, Solution *sol, int check);

/* http.c functions */
char *get_query(void);
char *query_lookup(char *query, char *var);

/* clue.c functions */
void make_clues(Puzzle *puz, Solution *sol);

/* merge.c functions */
void merge_guess(void);
void merge_set(Puzzle *puz, Cell *cell, bit_type *bit);
int merge_check(Puzzle *puz);
