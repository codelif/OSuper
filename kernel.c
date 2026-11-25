/* kernel.c - Module 3: processes + CPU scheduling, Module 4: synchronization + deadlock */
#define _GNU_SOURCE
#include "toyos.h"
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

pcb jobs[MAXJOBS], *cur;
int cpu_algo = RR, quantum = 50;
sem forks[5];
const char *cpu_names[] = {"fcfs", "sjf", "prio", "rr", "mlfq"};
static const char *stname[] = {"FREE", "NEW", "READY", "RUNNING", "BLOCKED", "DISKWAIT", "SLEEPING", "DONE"};

static ucontext_t kctx;              /* the kernel's own context */
static char ram[RAMSIZE];            /* "physical memory": job stacks live here */
static sigset_t intr, none;
static int reason, nextpid = 1, lastpid, nswitch;
static long seqno, tbase, lastboost, lastage, busy;
static double alpha = 0.5;           /* SJF exponential average */
static sem *sems[128];                /* all semaphores, for the deadlock detector */
static int nsems, ng, ndone, nhist;
static pipe_t pipes[NPIPES];
static sem bankq;                            /* jobs waiting for the banker to say "safe" */
static int total[NRES] = {10, 5, 7}, avail[NRES] = {10, 5, 7}, denied;
static struct { int pid; long t0, t1; } gantt[512];      /* bars of the gantt chart */
typedef struct { int pid, killed; char name[16]; long arrive, cpu, wait, tat, resp; } rec;
static rec done[64];                  /* finished jobs, for stats */
static struct { char name[16]; double tau; } hist[32];   /* SJF guess per command */

static void bank_free(pcb *p);

long ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

pcb *find(int pid) {
    for (int i = 0; i < MAXJOBS; i++) if (jobs[i].state != FREE && jobs[i].pid == pid) return &jobs[i];
    return 0;
}

/* ================= MODULE 3: PROCESSES AND CPU SCHEDULING ================= */

/* "disable interrupts" = block timer and disk signals while a job is in the kernel */
void intr_off(void) { if (cur && cur->idepth++ == 0) sigprocmask(SIG_BLOCK, &intr, 0); }
void intr_on(void) { if (cur && --cur->idepth == 0) sigprocmask(SIG_UNBLOCK, &intr, 0); }
void to_kernel(int why) { reason = why; swapcontext(&cur->ctx, &kctx); }
static void timer_set(int msec) {
    struct itimerval t = {{0, 0}, {msec / 1000, (msec % 1000) * 1000}};
    setitimer(ITIMER_REAL, &t, 0);
}
/* timer interrupt: quantum is over, go back to the scheduler */
static void on_timer(int sig) { if (cur) to_kernel(R_PREEMPT); }
/* disk interrupt: a job became ready, priority schedulers decide again */
static void on_disk(int sig) { if (cur && (cpu_algo == PRIO || cpu_algo == MLFQ)) to_kernel(R_YIELD); }

void kernel_init(void) {
    struct sigaction sa = {0};
    char n[16];
    sigemptyset(&none); sigemptyset(&intr);
    sigaddset(&intr, SIGALRM); sigaddset(&intr, SIGUSR1);
    sigprocmask(SIG_BLOCK, &intr, 0);        /* the kernel always runs with interrupts off */
    sa.sa_handler = on_timer; sigaction(SIGALRM, &sa, 0);
    sa.sa_handler = on_disk;  sigaction(SIGUSR1, &sa, 0);
    for (int i = 0; i < 5; i++) { sprintf(n, "fork%d", i); sem_init(&forks[i], 1, 1, n); }
    for (int i = 0; i < NPIPES; i++) {
        sprintf(n, "pipe%d.empty", i); sem_init(&pipes[i].empty, 0, 0, n);
        sprintf(n, "pipe%d.full", i);  sem_init(&pipes[i].full, 0, 0, n);
        sprintf(n, "pipe%d.mutex", i); sem_init(&pipes[i].mutex, 1, 1, n);
    }
    sem_init(&bankq, 0, 0, "banker");
    tbase = lastboost = ms();
}

static double *tau_of(const char *name) {
    for (int i = 0; i < nhist; i++)
        if (!strcmp(hist[i].name, name)) return &hist[i].tau;
    if (nhist == 32) nhist = 31;
    strcpy(hist[nhist].name, name); hist[nhist].tau = 100;   /* first guess: 100 ms */
    return &hist[nhist++].tau;
}

/* create a job (PCB) in the NEW state */
pcb *spawn(const char *name, void (*fn)(int, char **), int kb, int argc, char **argv,
           int prio, pipe_t *in, pipe_t *out, int outfile) {
    pcb *p = 0;
    for (int i = 0; i < MAXJOBS && !p; i++)
        if (jobs[i].state == FREE) p = &jobs[i];
    if (!p) { printf("toyos: process table full\n"); return 0; }
    memset(p, 0, sizeof *p);
    p->pid = nextpid++; p->state = NEW; p->prio = prio; p->fn = fn; p->msize = kb * 1024;
    p->argc = argc > 8 ? 8 : argc;
    for (int i = 0; i < p->argc; i++) { strncpy(p->args[i], argv[i], 31); p->argv[i] = p->args[i]; }
    snprintf(p->name, 16, "%s%s%s", name, argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
    p->in = in; p->out = out; p->outfile = outfile; p->infile = -1;
    p->tau = *tau_of(p->name); p->arrive = ms();
    p->idepth = 1;                           /* a new job starts with interrupts off */
    return p;
}

static void job_entry(void) {
    intr_on();
    if (cur->outfile >= 0) {                 /* "> file": we are a writer of that file */
        wr_lock(cur->outfile);
        if (!cur->append) fs_trunc(cur->outfile);
    }
    if (cur->infile >= 0) rd_lock(cur->infile);  /* "< file": we are a reader */
    cur->fn(cur->argc, cur->argv);
    kexit();
}

void make_ready(pcb *p) { p->state = READY; p->tready = ms(); p->seq = ++seqno; }

/* long term scheduler: give memory to NEW jobs and make them READY */
static void admit(void) {
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (p->state != NEW || (p->mbase = mem_alloc(p->msize, p->pid)) < 0) continue;
        getcontext(&p->ctx);
        p->ctx.uc_stack.ss_sp = ram + p->mbase;  /* the job's stack is its memory block */
        p->ctx.uc_stack.ss_size = p->msize; p->ctx.uc_link = 0;
        makecontext(&p->ctx, job_entry, 0);
        make_ready(p);
    }
}

/* lower key runs first, ties broken by queue order (FCFS / RR use only queue order) */
static double key(pcb *p) {
    return cpu_algo == SJF ? p->tau : cpu_algo == PRIO ? p->prio : cpu_algo == MLFQ ? p->level : 0;
}

/* short term scheduler */
static pcb *pick(void) {
    pcb *b = 0;
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (p->state == READY && (!b || key(p) < key(b) || (key(p) == key(b) && p->seq < b->seq))) b = p;
    }
    return b;
}

static void dispatch(pcb *p) {
    int q = (cpu_algo == FCFS || cpu_algo == SJF) ? 0 : cpu_algo == MLFQ ? quantum << p->level : quantum;
    struct timespec zero = {0, 0};
    p->trun = ms(); p->wait += p->trun - p->tready;
    p->state = RUNNING; cur = p;
    if (p->pid != lastpid) { nswitch++; lastpid = p->pid; }
    if (!p->tfirst) p->tfirst = p->trun;     /* first time on the CPU: response time */
    if (q) timer_set(q);
    swapcontext(&kctx, &p->ctx);             /* context switch: run the job */
    timer_set(0);
    while (sigtimedwait(&intr, 0, &zero) > 0);   /* throw away late interrupts */
    cur = 0;
    long now = ms(), ran = now - p->trun;
    p->cpu += ran; p->burst += ran; busy += ran;
    if (ng && gantt[ng - 1].pid == p->pid) gantt[ng - 1].t1 = now;
    else if (ng < 512) { gantt[ng].pid = p->pid; gantt[ng].t0 = p->trun; gantt[ng++].t1 = now; }
    if (reason == R_PREEMPT || reason == R_YIELD) {
        if (reason == R_PREEMPT && cpu_algo == MLFQ && p->level < 2) p->level++;   /* used full quantum */
        make_ready(p); return;
    }
    p->tau = alpha * p->burst + (1 - alpha) * p->tau;    /* burst over: tau = a*t + (1-a)*tau */
    *tau_of(p->name) = p->tau; p->burst = 0;
    if (p->state == DONE) finish(p);
}

/* clean up a finished or killed job */
void finish(pcb *p) {
    p->state = DONE;
    if (ndone < 64) {
        rec *r = &done[ndone++];
        r->pid = p->pid; r->killed = p->killed; strcpy(r->name, p->name);
        r->arrive = p->arrive - tbase; r->cpu = p->cpu; r->wait = p->wait; r->tat = ms() - p->arrive;
        r->resp = p->tfirst ? p->tfirst - p->arrive : r->tat;
    }
    if (p->killed)                           /* recovery: give back the semaphores it held */
        for (int i = 0; i < nsems; i++)
            if (sems[i]->binary && sems[i]->holder == p->pid && sems[i]->val == 0) sem_signal(sems[i]);
    if (p->out) { p->out->closed = 1; sem_signal(&p->out->full); p->out->used--; }
    if (p->in) p->in->used--;
    bank_free(p);
    cache_release(p->pid);
    mem_free(p->pid);
    p->waiting = 0; p->state = FREE;
}

void kill_job(int pid) {
    pcb *p = find(pid);
    if (!p) { printf("kill: no such process %d\n", pid); return; }
    p->killed = 1;
    if (p->state == DISKWAIT) { printf("kill: P%d will be killed when its disk I/O finishes\n", pid); return; }
    finish(p);
    printf("killed P%d (%s)\n", pid, p->name);
}

/* run the scheduler once; returns 0 if nothing can make progress */
int kernel_step(int use_stdin) {
    struct pollfd pf = {0, POLLIN, 0}; struct timespec ts = {0, 5000000};
    disk_poll(); admit();
    for (int i = 0; i < MAXJOBS; i++)        /* wake up sleeping jobs whose time has come */
        if (jobs[i].state == SLEEPING && ms() >= jobs[i].wake) make_ready(&jobs[i]);
    if (cpu_algo == PRIO && ms() - lastage > 500) {      /* aging: waiting jobs slowly gain priority */
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].state == READY && ms() - jobs[i].tready > 500 && jobs[i].prio > 0) jobs[i].prio--;
        lastage = ms();
    }
    if (cpu_algo == MLFQ && ms() - lastboost > 1000) {   /* priority boost against starvation */
        for (int i = 0; i < MAXJOBS; i++) jobs[i].level = 0;
        lastboost = ms();
    }
    pcb *p = pick();
    if (p) { dispatch(p); return 1; }
    ppoll(&pf, use_stdin, &ts, &none);       /* CPU idle: wait for keyboard or disk */
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == DISKWAIT || jobs[i].state == SLEEPING) return 1;
    return 0;
}

void ps(void) {
    printf("PID  NAME           STATE     PRI LVL   TAU   CPU  WAIT  MEMORY       INFO\n");
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (p->state == FREE) continue;
        printf("%-4d %-14s %-9s %-3d %-3d %5.0f %5ld %5ld  ", p->pid, p->name, stname[p->state], p->prio,
               p->level, p->tau, p->cpu, p->wait + (p->state == READY ? ms() - p->tready : 0));
        if (p->state == NEW) { printf("-            needs %dK\n", p->msize / 1024); continue; }
        printf("%4dK-%4dK  ", p->mbase / 1024, (p->mbase + p->msize) / 1024);
        if (p->state == BLOCKED) printf("waits on %s", p->waiting->name);
        if (p->state == BLOCKED && p->waiting->binary && p->waiting->holder > 0) printf(" (held by P%d)", p->waiting->holder);
        if (p->state == DISKWAIT) printf("disk block %d (cyl %d)", p->io->blk, p->io->blk / BPC);
        if (p->state == SLEEPING) printf("wakes in %ld ms", p->wake - ms());
        printf("\n");
    }
}

void stats_show(void) {
    long w = 0, t = 0, rs = 0, el = ms() - tbase;
    printf("PID  NAME           ARRIVAL  CPU-TIME  WAITING  RESPONSE  TURNAROUND   (ms)\n");
    for (int i = 0; i < ndone; i++) {
        rec *r = &done[i];
        printf("%-4d %-14s %7ld %9ld %8ld %9ld %11ld%s\n", r->pid, r->name, r->arrive, r->cpu, r->wait, r->resp,
               r->tat, r->killed ? "  (killed)" : "");
        w += r->wait; t += r->tat; rs += r->resp;
    }
    if (ndone) printf("Average waiting %.1f ms, response %.1f ms, turnaround %.1f ms (%d jobs)\n",
                      (double)w / ndone, (double)rs / ndone, (double)t / ndone, ndone);
    printf("CPU: %s, quantum %d ms, %d context switches, utilization %.1f%%, throughput %.2f jobs/s\n",
           cpu_names[cpu_algo], quantum, nswitch, el ? 100.0 * busy / el : 0, el ? 1000.0 * ndone / el : 0);
    printf("Page cache (%s): %ld hits, %ld page faults, hit ratio %.1f%%\n", cache_names[cache_algo],
           hits, misses, hits + misses ? 100.0 * hits / (hits + misses) : 0);
    printf("Disk (%s): %ld requests, total head movement %ld cylinders\n", disk_names[disk_algo], served, moved);
}

void stats_reset(void) {
    ng = ndone = nswitch = lastpid = 0; hits = misses = moved = served = busy = 0; tbase = ms();
}

void gantt_show(void) {
    int s = ng > 40 ? ng - 40 : 0;
    if (!ng) { printf("Gantt chart is empty\n"); return; }
    printf("Gantt chart (ms since reset)%s:\n", s ? ", last 40 slices" : "");
    for (int r = s; r < ng; r += 10) {
        int e = r + 10 < ng ? r + 10 : ng;
        for (int i = r; i < e; i++) printf("| P%-4d", gantt[i].pid);
        printf("|\n");
        for (int i = r; i < e; i++) printf("%-7ld", gantt[i].t0 - tbase);
        printf("%ld\n", gantt[e - 1].t1 - tbase);
    }
}

/* ================= MODULE 4: SYNCHRONIZATION AND DEADLOCK ================= */

void sem_init(sem *s, int v, int binary, const char *name) {
    s->val = v; s->holder = -1; s->binary = binary;
    snprintf(s->name, sizeof s->name, "%s", name);
    sems[nsems++] = s;
}

/* sem_wait / sem_signal are called with interrupts off */
void sem_wait(sem *s) {
    if (s->val > 0) { s->val--; s->holder = cur->pid; return; }
    cur->state = BLOCKED; cur->waiting = s; cur->seq = ++seqno;
    to_kernel(R_BLOCK);                      /* we come back here after someone signals */
}

void sem_signal(sem *s) {
    pcb *w = 0;
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == BLOCKED && jobs[i].waiting == s && (!w || jobs[i].seq < w->seq)) w = &jobs[i];
    if (w) { w->waiting = 0; s->holder = w->pid; make_ready(w); }   /* hand it to the oldest waiter */
    else { s->val++; s->holder = -1; }
}

void P(sem *s) { intr_off(); sem_wait(s); intr_on(); }
void V(sem *s) { intr_off(); sem_signal(s); intr_on(); }
void kyield(void) { intr_off(); to_kernel(R_YIELD); intr_on(); }

/* sleep for msec milliseconds: the job leaves the CPU until the kernel wakes it */
void ksleep(long msec) {
    intr_off();
    cur->wake = ms() + msec; cur->state = SLEEPING;
    to_kernel(R_BLOCK);
    intr_on();
}

void kexit(void) {
    intr_off();
    if (cur->outfile >= 0) wr_unlock(cur->outfile);
    if (cur->infile >= 0) rd_unlock(cur->infile);
    cur->state = DONE; to_kernel(R_EXIT);
}

/* pipes: bounded buffer (producer - consumer) */
pipe_t *pipe_new(void) {
    for (int i = 0; i < NPIPES; i++) {
        pipe_t *p = &pipes[i];
        if (p->used) continue;
        p->in = p->out = p->n = p->closed = 0; p->used = 2;
        p->empty.val = PIPESZ; p->full.val = 0; p->mutex.val = 1;
        p->empty.holder = p->full.holder = p->mutex.holder = -1;
        return p;
    }
    return 0;
}

void pipe_put(pipe_t *p, int c) {
    sem_wait(&p->empty); sem_wait(&p->mutex);
    p->buf[p->in] = c; p->in = (p->in + 1) % PIPESZ; p->n++;
    sem_signal(&p->mutex); sem_signal(&p->full);
}

int pipe_get(pipe_t *p) {                    /* returns -1 at end of file */
    int c = -1;
    sem_wait(&p->full); sem_wait(&p->mutex);
    if (p->n) { c = p->buf[p->out]; p->out = (p->out + 1) % PIPESZ; p->n--; sem_signal(&p->empty); }
    else sem_signal(&p->full);             /* writer closed the pipe: keep EOF visible */
    sem_signal(&p->mutex);
    return c;
}

/* wait-for graph: P -> holder of the binary semaphore that P is waiting on */
void deadlock_check(void) {
    printf("Wait-for graph (edges):\n");
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (p->state != BLOCKED || !p->waiting->binary || p->waiting->holder < 0) continue;
        printf("  P%d (%s) --%s--> P%d\n", p->pid, p->name, p->waiting->name, p->waiting->holder);
    }
    for (int i = 0; i < MAXJOBS; i++) {      /* follow the chain from every job */
        pcb *p = &jobs[i], *q = p;
        for (int n = 0; n < MAXJOBS && q && q->state == BLOCKED && q->waiting->binary; n++) {
            if ((q = find(q->waiting->holder)) != p) continue;
            printf("DEADLOCK: cycle P%d", p->pid);
            for (q = find(p->waiting->holder); q != p; q = find(q->waiting->holder)) printf(" -> P%d", q->pid);
            printf(" -> P%d\nRecovery: kill one process in the cycle, e.g. 'kill %d'\n", p->pid, p->pid);
            return;
        }
    }
    printf("No deadlock found.\n");
}

/* ---------- deadlock avoidance: banker's algorithm ---------- */

/* safety algorithm: can every job that has a claim still finish? fills seq with a safe order */
static int safe(int *seq) {
    int work[NRES], fin[MAXJOBS] = {0}, n = 0, found = 1;
    memcpy(work, avail, sizeof work);
    while (found) {
        found = 0;
        for (int i = 0; i < MAXJOBS; i++) {
            pcb *p = &jobs[i];
            int ok = p->banker && !fin[i];
            for (int r = 0; ok && r < NRES; r++) if (p->max[r] - p->alloc[r] > work[r]) ok = 0;
            if (!ok) continue;
            for (int r = 0; r < NRES; r++) work[r] += p->alloc[r];   /* it finishes and gives back */
            fin[i] = found = 1;
            if (seq) seq[n] = p->pid;
            n++;
        }
    }
    for (int i = 0; i < MAXJOBS; i++) if (jobs[i].banker && !fin[i]) return -1;
    return n;
}

void bank_claim(int *max) {                  /* a job declares its maximum need */
    intr_off();
    for (int r = 0; r < NRES; r++) { cur->max[r] = max[r] > total[r] ? total[r] : max[r]; cur->alloc[r] = 0; }
    cur->banker = 1;
    intr_on();
}

/* resource-request algorithm: grant only if the new state is safe, otherwise wait */
int bank_request(int *req) {
    intr_off();
    for (int r = 0; r < NRES; r++)
        if (req[r] > cur->max[r] - cur->alloc[r]) { intr_on(); return -1; }   /* more than claimed */
    for (;;) {
        int ok = 1;
        for (int r = 0; r < NRES; r++) if (req[r] > avail[r]) ok = 0;
        if (ok) {
            for (int r = 0; r < NRES; r++) { avail[r] -= req[r]; cur->alloc[r] += req[r]; }  /* pretend */
            if (safe(0) >= 0) break;
            for (int r = 0; r < NRES; r++) { avail[r] += req[r]; cur->alloc[r] -= req[r]; }  /* undo */
            denied++;
        }
        cur->state = BLOCKED; cur->waiting = &bankq; cur->seq = ++seqno;
        to_kernel(R_BLOCK);                  /* try again after someone releases */
    }
    intr_on();
    return 0;
}

static void bank_free(pcb *p) {              /* job finished: give everything back, wake waiters */
    if (!p->banker) return;
    for (int r = 0; r < NRES; r++) { avail[r] += p->alloc[r]; p->alloc[r] = 0; }
    p->banker = 0;
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == BLOCKED && jobs[i].waiting == &bankq) { jobs[i].waiting = 0; make_ready(&jobs[i]); }
}

void banker_show(void) {
    int seq[MAXJOBS], n = safe(seq);
    printf("Banker's algorithm: total %d %d %d, available %d %d %d, unsafe requests delayed: %d\n",
           total[0], total[1], total[2], avail[0], avail[1], avail[2], denied);
    printf("PID  NAME            ALLOC       MAX        NEED\n");
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (!p->banker) continue;
        printf("%-4d %-14s %2d %2d %2d   %2d %2d %2d   %2d %2d %2d%s\n", p->pid, p->name,
               p->alloc[0], p->alloc[1], p->alloc[2], p->max[0], p->max[1], p->max[2],
               p->max[0] - p->alloc[0], p->max[1] - p->alloc[1], p->max[2] - p->alloc[2],
               p->state == BLOCKED && p->waiting == &bankq ? "  (waiting)" : "");
    }
    if (n < 0) { printf("State is UNSAFE\n"); return; }
    printf("State is SAFE, safe sequence:");
    for (int i = 0; i < n; i++) printf(" P%d", seq[i]);
    printf("\n");
}
