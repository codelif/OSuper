/* main.c - Module 1: shell + user programs, Module 2: memory manager */
#include "toyos.h"
#include <stdarg.h>
#include <poll.h>
#include <unistd.h>

int mem_algo = FIRST;
const char *mem_names[] = {"first", "next", "best", "worst"};
static char history[50][256];               /* shell history */
static int nhistory;

/* ================= MODULE 1: SHELL AND USER PROGRAMS ================= */

/* system calls used by the programs: output goes to console, a pipe or a file */
void kwrite(const char *s, int n) {
    intr_off();
    if (cur->out) for (int i = 0; i < n; i++) pipe_put(cur->out, (unsigned char)s[i]);
    else if (cur->outfile >= 0) fs_write(cur->outfile, s, n);
    else write(1, s, n);
    intr_on();
}

void kprintf(const char *fmt, ...) {
    char b[256]; va_list ap;
    intr_off();
    va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    kwrite(b, n > 255 ? 255 : n);
    intr_on();
}

int kgetc(void) {                            /* read one character of input: "< file" or a pipe */
    unsigned char ch;
    if (cur->infile >= 0) return fs_read(cur->infile, cur->inoff++, (char *)&ch, 1) == 1 ? ch : -1;
    if (!cur->in) return -1;
    intr_off(); int c = pipe_get(cur->in); intr_on();
    return c;
}

/* input for programs that take "[file]": read the file, or stdin (pipe / "< file") if none */
typedef struct { int ino, off, n, k; char b[BSIZE]; } input;

static int in_open(input *in, char *name) {
    memset(in, 0, sizeof *in);
    in->ino = name ? fs_lookup(name) : -1;
    if (name && in->ino < 0) { kprintf("%s: no such file\n", name); return -1; }
    if (in->ino >= 0) rd_lock(in->ino);
    return 0;
}

static void in_close(input *in) { if (in->ino >= 0) rd_unlock(in->ino); }

static int in_getc(input *in) {
    if (in->ino < 0) return kgetc();
    if (in->k == in->n) { in->n = fs_read(in->ino, in->off, in->b, BSIZE); in->off += in->n; in->k = 0; }
    return in->n > 0 ? (unsigned char)in->b[in->k++] : -1;
}

static int in_line(input *in, char *line, int max) {    /* returns length, -1 at end of input */
    int c, n = 0;
    while ((c = in_getc(in)) >= 0 && c != '\n') if (n < max - 1) line[n++] = c;
    line[n] = 0;
    return c < 0 && !n ? -1 : n;
}

/* ---------- user programs: each one runs as a toyOS job ---------- */

static void spin(long msec) {                /* use the CPU for msec milliseconds */
    long until = cur->cpu + msec;
    while (cur->cpu + (ms() - cur->trun) < until)
        ;
}

static void c_burn(int argc, char **argv) {
    spin((argc > 1 ? atoi(argv[1]) : 1) * 100);
    kprintf("[P%d] burn done\n", cur->pid);
}

static void c_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void c_gen(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 10;
    for (int i = 1; i <= n; i++) kprintf("%d: the quick brown fox jumps over the lazy dog\n", i);
}

static void c_cat(int argc, char **argv) {
    char b[BSIZE];
    int n, c;
    if (argc < 2)
        while ((c = kgetc()) >= 0) { b[0] = c; kwrite(b, 1); }
    for (int i = 1; i < argc; i++) {
        int ino = fs_lookup(argv[i]), off = 0;
        if (ino < 0) { kprintf("cat: %s: no such file\n", argv[i]); continue; }
        rd_lock(ino);
        while ((n = fs_read(ino, off, b, BSIZE)) > 0) { kwrite(b, n); off += n; }
        rd_unlock(ino);
    }
}

static void c_wc(int argc, char **argv) {
    input in;
    long l = 0, w = 0, ch = 0;
    int word = 0, c;
    if (in_open(&in, argc > 1 ? argv[1] : 0) < 0) return;
    while ((c = in_getc(&in)) >= 0) {
        ch++; if (c == '\n') l++;
        if (c == ' ' || c == '\n' || c == '\t') word = 0;
        else if (!word) { word = 1; w++; }
    }
    in_close(&in);
    kprintf("%ld lines, %ld words, %ld chars\n", l, w, ch);
}

static void c_head(int argc, char **argv) {  /* head N [file]: first N lines */
    input in;
    char line[256];
    int n = argc > 1 ? atoi(argv[1]) : 10;
    if (in_open(&in, argc > 2 ? argv[2] : 0) < 0) return;
    while (n-- > 0 && in_line(&in, line, sizeof line) >= 0) kprintf("%s\n", line);
    while (in.ino < 0 && kgetc() >= 0);      /* drain the pipe so the writer can finish */
    in_close(&in);
}

static void c_grep(int argc, char **argv) {  /* grep word [file]: lines containing word */
    input in;
    char line[256];
    int found = 0;
    if (argc < 2) { kprintf("usage: grep word [file]\n"); return; }
    if (in_open(&in, argc > 2 ? argv[2] : 0) < 0) return;
    while (in_line(&in, line, sizeof line) >= 0)
        if (strstr(line, argv[1])) { kprintf("%s\n", line); found++; }
    in_close(&in);
    if (!found) kprintf("grep: no match for '%s'\n", argv[1]);
}

static void c_sort(int argc, char **argv) {  /* sort [file]: insertion sort of up to 64 lines */
    input in;
    char lines[64][80], tmp[80];
    int n = 0;
    if (in_open(&in, argc > 1 ? argv[1] : 0) < 0) return;
    while (n < 64 && in_line(&in, lines[n], 80) >= 0) {
        int j = n++;
        strcpy(tmp, lines[j]);
        while (j > 0 && strcmp(lines[j - 1], tmp) > 0) { strcpy(lines[j], lines[j - 1]); j--; }
        strcpy(lines[j], tmp);
    }
    in_close(&in);
    for (int i = 0; i < n; i++) kprintf("%s\n", lines[i]);
}

static void c_touch(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (fs_create(argv[i]) < 0) kprintf("touch: disk full\n");
}

static void c_nap(int argc, char **argv) {   /* nap MS: sleep without using the CPU */
    ksleep(argc > 1 ? atoi(argv[1]) : 1000);
    kprintf("[P%d] woke up\n", cur->pid);
}

/* res A B C: declare a maximum claim of A, B, C resources, then ask for it in two halves */
static void c_res(int argc, char **argv) {
    int max[NRES], half[NRES], rest[NRES];
    for (int r = 0; r < NRES; r++) {
        max[r] = argc > r + 1 ? atoi(argv[r + 1]) : 1;
        half[r] = (max[r] + 1) / 2;
        rest[r] = max[r] - half[r];
    }
    bank_claim(max);
    bank_request(half);
    kprintf("[P%d] got %d %d %d\n", cur->pid, half[0], half[1], half[2]);
    spin(100);
    bank_request(rest);
    kprintf("[P%d] got its full claim %d %d %d, working\n", cur->pid, max[0], max[1], max[2]);
    spin(100);                               /* resources are given back when the job ends */
}

static void c_cp(int argc, char **argv) {
    char b[BSIZE];
    int n, off = 0, s, d;
    if (argc < 3) { kprintf("usage: cp src dst\n"); return; }
    if ((s = fs_lookup(argv[1])) < 0) { kprintf("cp: %s: no such file\n", argv[1]); return; }
    if ((d = fs_create(argv[2])) < 0) { kprintf("cp: disk full\n"); return; }
    rd_lock(s); wr_lock(d); fs_trunc(d);
    while ((n = fs_read(s, off, b, BSIZE)) > 0) { fs_write(d, b, n); off += n; }
    wr_unlock(d); rd_unlock(s);
}

static void c_rm(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int ino = fs_lookup(argv[i]);
        if (ino < 0) { kprintf("rm: %s: no such file\n", argv[i]); continue; }
        wr_lock(ino); fs_remove(argv[i]); wr_unlock(ino);
    }
}

static void c_io(int argc, char **argv) {    /* I/O bound job: random disk reads */
    char b[BSIZE];
    int n = argc > 1 ? atoi(argv[1]) : 10;
    for (int i = 0; i < n; i++) { blk_read(DATA0 + rnd() % (NBLOCKS - DATA0), b); spin(2); }
    kprintf("[P%d] io: %d random reads done\n", cur->pid, n);
}

/* dining philosopher i. "safe": last one picks forks in the other order */
static void c_phil(int argc, char **argv) {
    int i = atoi(argv[1]), l = i, r = (i + 1) % 5;
    if (argc > 2 && i == 4) { l = 0; r = 4; }    /* deadlock prevention: no circular wait */
    for (int k = 0; k < 3; k++) {
        P(&forks[l]);
        kprintf("philosopher %d took fork %d\n", i, l);
        kyield();                                  /* let the others run: invites deadlock */
        P(&forks[r]);
        kprintf("philosopher %d took fork %d and is eating\n", i, r);
        spin(30);
        V(&forks[r]); V(&forks[l]);
        kprintf("philosopher %d is thinking\n", i);
        kyield();
    }
}

/* name, function, memory needed in KB */
static struct { const char *name; void (*fn)(int, char **); int kb; } cmds[] = {
    {"ls", fs_ls, 16},    {"cat", c_cat, 24}, {"cp", c_cp, 32},   {"rm", c_rm, 16},
    {"echo", c_echo, 16}, {"wc", c_wc, 20},   {"gen", c_gen, 20}, {"burn", c_burn, 16},
    {"io", c_io, 24},     {"phil", c_phil, 16}, {"head", c_head, 24}, {"grep", c_grep, 24},
    {"sort", c_sort, 32}, {"touch", c_touch, 16}, {"nap", c_nap, 16},  {"res", c_res, 16},
    {"df", fs_df, 16},    {"stat", fs_stat, 16},  {"fsck", fs_fsck, 24},
};
#define NCMDS (int)(sizeof cmds / sizeof cmds[0])

/* ---------- the shell ---------- */

static const char *help =
    "Programs (run as jobs; & = background, | = pipe, < file, > file, >> file):\n"
    "  ls  cat [f..]  cp a b  rm f..  touch f..  echo text  wc [f]  head N [f]  grep w [f]  sort [f]\n"
    "  df  stat f..  fsck  gen N (text)  burn N (N*100ms CPU)  io N (disk reads)  nap MS (sleep)\n"
    "  res A B C (banker's algorithm claim)    nice P cmd ... (priority P, 0 = highest)\n"
    "Kernel commands:\n"
    "  ps | jobs | fg PID | kill PID | wait | sleep MS | stats | gantt | reset | sync | history | !N | exit\n"
    "  policy [fcfs|sjf|prio|rr|mlfq] [quantum]       CPU scheduling\n"
    "  mem    [first|next|best|worst]                 memory map / allocation\n"
    "  pagesim FRAMES page page ...                   FIFO / LRU / OPT on a reference string\n"
    "  cache  [fifo|lru|clock]                        page cache / replacement\n"
    "  disk   [fcfs|sstf|scan|cscan|look|clook]       disk queue / scheduling\n"
    "  disksim HEAD cyl cyl ...                       all disk algorithms on a request queue\n"
    "  philo [safe] | deadlock | banker               deadlock prevention, detection, avoidance\n";

static int lookup(const char **names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}

/* is any of these jobs (or any job at all if n == 0) still alive? */
static int alive(int *pids, int n) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].state == FREE) continue;
        if (!n) return 1;
        for (int k = 0; k < n; k++) if (jobs[i].pid == pids[k]) return 1;
    }
    return 0;
}

static void wait_jobs(int *pids, int n) {
    while (alive(pids, n))
        if (!kernel_step(0)) { printf("toyos: jobs are stuck (all blocked) - try 'deadlock'\n"); return; }
}

static int builtin(int n, char **t) {
    int k;
    if (!strcmp(t[0], "help")) printf("%s", help);
    else if (!strcmp(t[0], "ps")) ps();
    else if (!strcmp(t[0], "stats")) stats_show();
    else if (!strcmp(t[0], "gantt")) gantt_show();
    else if (!strcmp(t[0], "deadlock")) deadlock_check();
    else if (!strcmp(t[0], "sync")) fs_sync();
    else if (!strcmp(t[0], "wait")) wait_jobs(0, 0);
    else if (!strcmp(t[0], "kill")) { if (n > 1) kill_job(atoi(t[1])); else printf("usage: kill PID\n"); }
    else if (!strcmp(t[0], "banker")) banker_show();
    else if (!strcmp(t[0], "fg") && n > 1) { k = atoi(t[1]); wait_jobs(&k, 1); }
    else if (!strcmp(t[0], "jobs")) {
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].state != FREE) printf("[%d] %-9s %s\n", jobs[i].pid, jobs[i].state == NEW ? "NEW" :
                   jobs[i].state == READY || jobs[i].state == RUNNING ? "Running" : "Waiting", jobs[i].name);
    } else if (!strcmp(t[0], "history")) {
        for (int i = nhistory > 50 ? nhistory - 50 : 0; i < nhistory; i++) printf("%4d  %s", i + 1, history[i % 50]);
    } else if (!strcmp(t[0], "pagesim") || !strcmp(t[0], "disksim")) {
        int v[32], m = 0;
        for (int i = 2; i < n && m < 30; i++) v[m++] = atoi(t[i]);
        if (n < 3) printf("usage: %s FRAMES|HEAD n n n ...\n", t[0]);
        else if (t[0][0] == 'p') pagesim(atoi(t[1]), v, m);
        else disksim(atoi(t[1]), v, m > MAXJOBS ? MAXJOBS : m);
    }
    else if (!strcmp(t[0], "reset")) { stats_reset(); printf("statistics cleared\n"); }
    else if (!strcmp(t[0], "policy")) {
        if (n > 1 && (k = lookup(cpu_names, 5, t[1])) >= 0) cpu_algo = k;
        if (n > 2) quantum = atoi(t[2]);
        printf("CPU scheduling: %s, quantum %d ms\n", cpu_names[cpu_algo], quantum);
    } else if (!strcmp(t[0], "disk")) {
        if (n > 1 && (k = lookup(disk_names, 6, t[1])) >= 0) disk_algo = k;
        disk_show();
    } else if (!strcmp(t[0], "mem")) {
        if (n > 1 && (k = lookup(mem_names, 4, t[1])) >= 0) mem_algo = k;
        mem_show();
    } else if (!strcmp(t[0], "cache")) {
        if (n > 1 && (k = lookup(cache_names, 3, t[1])) >= 0) cache_algo = k;
        cache_show();
    } else if (!strcmp(t[0], "sleep")) {
        long until = ms() + (n > 1 ? atoi(t[1]) : 1000);
        while (ms() < until) kernel_step(0);
    } else if (!strcmp(t[0], "philo")) {
        for (int i = 0; i < 5; i++) {
            char num[4], *a[3] = {"phil", num, "safe"};
            sprintf(num, "%d", i);
            pcb *p = spawn("phil", c_phil, 16, n > 1 ? 3 : 2, a, 5, 0, 0, -1);
            if (p) printf("[%d] phil %d\n", p->pid, i);
        }
    } else return 0;
    return 1;
}

static void run(char *line) {
    char *t[64];
    int n = 0, bg = 0, prio = 5, s = 0, outfile = -1, infile = -1, append = 0, pids[8], np = 0;
    pipe_t *prev = 0;
    for (char *w = strtok(line, " \t\n"); w && n < 63; w = strtok(0, " \t\n")) t[n++] = w;
    if (!n || t[0][0] == '#') return;       /* empty line or comment */
    if (!strcmp(t[n - 1], "&")) { bg = 1; if (!--n) return; }
    if (builtin(n, t)) return;
    if (!strcmp(t[0], "nice") && n > 2) { prio = atoi(t[1]); s = 2; }
    for (int i = s; i < n - 1;) {            /* redirections: < file, > file, >> file */
        if (!strcmp(t[i], "<") && (infile = fs_lookup(t[i + 1])) < 0) { printf("toyos: %s: no such file\n", t[i + 1]); return; }
        else if (!strcmp(t[i], ">") || !strcmp(t[i], ">>")) { outfile = fs_create(t[i + 1]); append = t[i][1] == '>'; }
        else if (strcmp(t[i], "<")) { i++; continue; }
        memmove(&t[i], &t[i + 2], (n - i - 2) * sizeof(char *));
        n -= 2;
    }
    for (int i = s; i <= n; i++) {           /* split "a | b | c" into jobs joined by pipes */
        if (i < n && strcmp(t[i], "|")) continue;
        int c = 0;
        while (c < NCMDS && strcmp(cmds[c].name, t[s])) c++;
        if (c == NCMDS) { printf("toyos: %s: command not found\n", t[s]); break; }
        pipe_t *next = i < n ? pipe_new() : 0;
        pcb *p = spawn(t[s], cmds[c].fn, cmds[c].kb, i - s, t + s, prio, prev, next, i == n ? outfile : -1);
        if (p && !np) p->infile = infile;    /* "< file" goes to the first job of the pipeline */
        if (p && i == n) p->append = append;
        if (p && np < 8) pids[np++] = p->pid;
        prev = next; s = i + 1;
    }
    if (bg) {
        for (int k = 0; k < np; k++) printf("[%d] ", pids[k]);
        printf("\n");
    } else if (np) wait_jobs(pids, np);      /* foreground: wait until the jobs finish */
}

int main(void) {
    char line[256];
    struct pollfd pf = {0, POLLIN, 0};
    setvbuf(stdout, 0, _IONBF, 0); setvbuf(stdin, 0, _IONBF, 0);
    kernel_init(); disk_init();
    printf("toyOS booted: %dK RAM, %d KB disk, %d cache frames. Type 'help'.\n",
           RAMSIZE / 1024, NBLOCKS * BSIZE / 1024, NFRAMES);
    for (;;) {
        printf("toyos> ");
        while (poll(&pf, 1, 0) <= 0) kernel_step(1);    /* run jobs while the user types */
        if (!fgets(line, sizeof line, stdin)) break;
        if (!isatty(0)) printf("%s", line);              /* echo commands from a script */
        if (line[0] == '!') {                            /* !N or !! repeats a command */
            int k = line[1] == '!' ? nhistory : atoi(line + 1);
            if (k < 1 || k > nhistory || k <= nhistory - 50) { printf("toyos: no such command in history\n"); continue; }
            strcpy(line, history[(k - 1) % 50]);
            printf("%s", line);
        }
        if (line[0] != '\n' && line[0] != '#') strcpy(history[nhistory++ % 50], line);
        if (!strncmp(line, "exit", 4)) break;
        run(line);
        fs_sync();
    }
    disk_stop(); printf("bye\n");
    return 0;
}

/* ================= MODULE 2: MEMORY MANAGER (contiguous allocation) ================= */

typedef struct { int start, size, pid; } seg;      /* memory segment, pid 0 = hole */
static seg segs[64] = {{0, RAMSIZE, 0}};
static int nsegs = 1, nextpos, nalloc, peak;

int mem_alloc(int size, int pid) {
    int b = -1, k0 = 0;
    if (mem_algo == NEXT)                    /* next fit starts where the last search stopped */
        while (k0 < nsegs && segs[k0].start + segs[k0].size <= nextpos) k0++;
    for (int j = 0; j < nsegs; j++) {
        int i = (k0 + j) % nsegs;
        if (segs[i].pid || segs[i].size < size) continue;
        if (b < 0 || (mem_algo == BEST && segs[i].size < segs[b].size)
                  || (mem_algo == WORST && segs[i].size > segs[b].size)) b = i;
        if (mem_algo == FIRST || mem_algo == NEXT) break;
    }
    if (b < 0) return -1;
    nextpos = segs[b].start + size;
    nalloc++;
    if (segs[b].size > size) {               /* split the hole */
        memmove(&segs[b + 2], &segs[b + 1], (nsegs - b - 1) * sizeof(seg));
        segs[b + 1] = (seg){segs[b].start + size, segs[b].size - size, 0};
        segs[b].size = size; nsegs++;
    }
    segs[b].pid = pid;
    int used = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].pid) used += segs[i].size;
    if (used > peak) peak = used;
    return segs[b].start;
}

void mem_free(int pid) {
    for (int i = 0; i < nsegs; i++)
        if (segs[i].pid == pid) segs[i].pid = 0;
    for (int i = 0; i + 1 < nsegs;)          /* merge neighbouring holes */
        if (!segs[i].pid && !segs[i + 1].pid) {
            segs[i].size += segs[i + 1].size;
            memmove(&segs[i + 1], &segs[i + 2], (nsegs - i - 2) * sizeof(seg));
            nsegs--;
        } else i++;
}

void mem_show(void) {
    int fr = 0, big = 0, holes = 0, k = 0;
    char tag[64];                            /* letter used for each segment in the memory bar */
    printf("Memory: %dK, %s-fit\n", RAMSIZE / 1024, mem_names[mem_algo]);
    for (int i = 0; i < nsegs; i++) {
        seg *s = &segs[i];
        tag[i] = s->pid ? 'A' + k++ % 26 : '.';
        printf("  %c [%4dK - %4dK] %4dK  ", tag[i], s->start / 1024, (s->start + s->size) / 1024, s->size / 1024);
        if (s->pid) { printf("P%d %s\n", s->pid, find(s->pid)->name); continue; }
        printf("free\n"); fr += s->size; holes++;
        if (s->size > big) big = s->size;
    }
    printf("Free %dK in %d hole(s), largest hole %dK, external fragmentation %d%%\n",
           fr / 1024, holes, big / 1024, fr ? 100 - big * 100 / fr : 0);
    printf("  |");                           /* memory bar: one character = 2.5K, '.' = free */
    for (int c = 0; c < 64; c++) {
        int a = c * RAMSIZE / 64;
        for (int i = 0; i < nsegs; i++) if (a >= segs[i].start && a < segs[i].start + segs[i].size) putchar(tag[i]);
    }
    printf("|\n  %d allocations so far, peak use %dK\n", nalloc, peak / 1024);
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == NEW) printf("  P%d %s is waiting for %dK\n", jobs[i].pid, jobs[i].name, jobs[i].msize / 1024);
}

/* virtual memory: page replacement on a reference string (textbook table) */
void pagesim(int nf, int *ref, int n) {
    const char *names[] = {"FIFO", "LRU", "OPT"};
    if (nf < 1 || nf > 8) { printf("pagesim: frames must be 1-8\n"); return; }
    for (int a = 0; a < 3; a++) {
        int fr[8], when[8], used = 0, faults = 0, grid[8][32], fault[32];
        for (int i = 0; i < n; i++) {
            int hit = -1, v = used;
            for (int f = 0; f < used; f++) if (fr[f] == ref[i]) hit = f;
            fault[i] = hit < 0;
            if (hit >= 0 && a == 1) when[hit] = i;               /* LRU: remember last use */
            if (hit < 0 && used == nf) {                         /* choose a victim */
                v = 0;
                for (int f = 1; f < nf; f++) {
                    if (a < 2) { if (when[f] < when[v]) v = f; continue; }   /* oldest load / use */
                    int nf_ = n, nv = n;                         /* OPT: used farthest in the future */
                    for (int k = i + 1; k < n; k++) if (ref[k] == fr[f]) { nf_ = k; break; }
                    for (int k = i + 1; k < n; k++) if (ref[k] == fr[v]) { nv = k; break; }
                    if (nf_ > nv) v = f;
                }
            }
            if (hit < 0) { fr[v] = ref[i]; when[v] = i; faults++; if (v == used) used++; }
            for (int f = 0; f < nf; f++) grid[f][i] = f < used ? fr[f] : -1;
        }
        printf("%s with %d frames:\n  ref  |", names[a], nf);
        for (int i = 0; i < n; i++) printf("%3d", ref[i]);
        for (int f = 0; f < nf; f++) {
            printf("\n  fr%d  |", f);
            for (int i = 0; i < n; i++)
                if (grid[f][i] < 0) printf("   ");
                else printf("%3d", grid[f][i]);
        }
        printf("\n  fault|");
        for (int i = 0; i < n; i++) printf("%3s", fault[i] ? "*" : "");
        printf("\n  page faults: %d of %d references (hit ratio %.0f%%)\n\n", faults, n, 100.0 * (n - faults) / n);
    }
}
