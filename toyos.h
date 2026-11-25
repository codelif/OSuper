/* toyos.h - common definitions for toyOS */
#ifndef TOYOS_H
#define TOYOS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <pthread.h>

#define MAXJOBS  16
#define RAMSIZE  (160 * 1024)   /* "physical memory" used for job stacks */
#define BSIZE    512            /* disk block size */
#define NBLOCKS  2048           /* 1 MB disk */
#define BPC      8              /* blocks per cylinder */
#define NCYL     (NBLOCKS / BPC)
#define NINODES  32
#define NDIRECT  27             /* direct blocks per inode, plus one indirect block */
#define MAXFILE  ((NDIRECT + BSIZE / 2) * BSIZE)   /* 283 blocks = 141.5 KB */
#define DATA0    8              /* first data block */
#define NFRAMES  8              /* page cache frames */
#define PIPESZ   64
#define NPIPES   8
#define NRES     3              /* resource types for the banker's algorithm */

enum { FREE, NEW, READY, RUNNING, BLOCKED, DISKWAIT, SLEEPING, DONE };   /* process states */
enum { R_EXIT, R_PREEMPT, R_YIELD, R_BLOCK };    /* why a job came back to the kernel */
enum { FCFS, SJF, PRIO, RR, MLFQ };
enum { D_FCFS, D_SSTF, D_SCAN, D_CSCAN, D_LOOK, D_CLOOK };
enum { FIRST, NEXT, BEST, WORST };
enum { C_FIFO, C_LRU, C_CLOCK };

typedef struct sem { int val, holder, binary; char name[16]; } sem;   /* holder: for deadlock graph */
typedef struct pipe_t { int buf[PIPESZ], in, out, n, used, closed; sem empty, full, mutex; } pipe_t;
typedef struct dreq { int blk, write, done; char *buf; } dreq;

typedef struct pcb {
    int pid, state, prio, level, killed, idepth, argc;
    char name[16], args[8][32], *argv[8];
    void (*fn)(int, char **);
    ucontext_t ctx;
    int mbase, msize;           /* memory given to this job */
    pipe_t *in, *out;
    int outfile, append;        /* inode for "> file" (-1 if none), ">>" appends */
    int infile, inoff;          /* inode and offset for "< file", -1 if none */
    sem *waiting;               /* semaphore it is blocked on */
    dreq *io;                   /* disk request it is waiting for */
    long seq;                   /* position in ready / wait queue */
    double tau;                 /* SJF predicted burst */
    long burst, arrive, tready, trun, wait, cpu, tfirst, wake;
    int banker, max[NRES], alloc[NRES];   /* banker's algorithm: claim and allocation */
} pcb;

/* main.c */
extern int mem_algo;
int mem_alloc(int size, int pid);
void mem_free(int pid), mem_show(void), pagesim(int nf, int *ref, int n);
void kwrite(const char *s, int n), kprintf(const char *fmt, ...);   /* system calls for programs */
int kgetc(void);

/* kernel.c */
extern pcb jobs[MAXJOBS], *cur;
extern int cpu_algo, quantum;
extern sem forks[5];
extern const char *cpu_names[], *disk_names[], *mem_names[], *cache_names[];
long ms(void);
pcb *find(int pid);
void kernel_init(void);
int kernel_step(int use_stdin);
pcb *spawn(const char *name, void (*fn)(int, char **), int kb, int argc, char **argv,
           int prio, pipe_t *in, pipe_t *out, int outfile);
void finish(pcb *p), kill_job(int pid), make_ready(pcb *p);
void intr_off(void), intr_on(void), to_kernel(int why), kyield(void), kexit(void), ksleep(long msec);
void sem_init(sem *s, int v, int binary, const char *name);
void sem_wait(sem *s), sem_signal(sem *s), P(sem *s), V(sem *s);
pipe_t *pipe_new(void);
void pipe_put(pipe_t *p, int c);
int pipe_get(pipe_t *p);
void ps(void), stats_show(void), stats_reset(void), gantt_show(void), deadlock_check(void);
void bank_claim(int *max), banker_show(void);
int bank_request(int *req);

/* fs.c */
extern int disk_algo, cache_algo;
extern long hits, misses, moved, served;
void disk_init(void), disk_stop(void), disk_poll(void), disk_show(void), disksim(int h, int *req, int n);
void cache_show(void), cache_release(int pid), blk_read(int blk, char *buf);
void fs_sync(void), fs_trunc(int ino);
void fs_ls(int argc, char **argv), fs_df(int argc, char **argv), fs_stat(int argc, char **argv), fs_fsck(int argc, char **argv);
int fs_lookup(const char *name);
int fs_create(const char *name);
int fs_remove(const char *name);
int fs_read(int ino, int off, char *buf, int n);
int fs_write(int ino, const char *buf, int n);
void rd_lock(int i), rd_unlock(int i), wr_lock(int i), wr_unlock(int i);
unsigned rnd(void);
#endif
