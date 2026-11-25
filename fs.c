/* fs.c - Module 5: disk scheduling, Module 6: page cache + file system */
#include "toyos.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#define MAGIC 0x746f7954                  /* changed when the disk layout changes */

int disk_algo = D_FCFS, cache_algo = C_LRU;
long hits, misses, moved, served;
const char *disk_names[] = {"fcfs", "sstf", "scan", "cscan", "look", "clook"};
const char *cache_names[] = {"fifo", "lru", "clock"};

/* ================= MODULE 5: DISK SCHEDULING (disk controller thread) ================= */

static int dfd, head, dir = 1, quit, path[16], npath;
static dreq *pend[MAXJOBS]; static int npend;   /* disk queue */
static pthread_t dthread, mainth;
static pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dcond = PTHREAD_COND_INITIALIZER;

static int cyl(int i) { return pend[i]->blk / BPC; }

/* closest pending request in direction d (+1 up, -1 down, 0 either) */
static int nearest(int d) {
    int b = -1;
    for (int i = 0; i < npend; i++)
        if ((cyl(i) - head) * d >= 0 && (b < 0 || abs(cyl(i) - head) < abs(cyl(b) - head))) b = i;
    return b;
}

/* pick the next request to serve */
static int choose(void) {
    int b = 0;                               /* FCFS: oldest request */
    if (disk_algo == D_SSTF) b = nearest(0);
    else if (disk_algo == D_SCAN) {
        if ((b = nearest(dir)) < 0) {        /* go to the end of the disk, then turn around */
            int edge = dir > 0 ? NCYL - 1 : 0;
            moved += abs(edge - head); head = edge; dir = -dir;
            b = nearest(dir);
        }
    } else if (disk_algo == D_CSCAN) {
        if ((b = nearest(1)) < 0) {          /* go to the last cylinder, jump back to 0 */
            moved += (NCYL - 1 - head) + (NCYL - 1);
            head = 0;
            b = nearest(1);
        }
    } else if (disk_algo == D_LOOK) {
        if ((b = nearest(dir)) < 0) {        /* turn around at the last request, not the edge */
            dir = -dir;
            b = nearest(dir);
        }
    } else if (disk_algo == D_CLOOK) {
        dir = 1;
        if ((b = nearest(1)) < 0)            /* nothing above: jump back to the lowest request */
            for (int i = b = 0; i < npend; i++)
                if (cyl(i) < cyl(b)) b = i;
    }
    return b;
}

static dreq *take(int b) {                   /* remove request b from the queue */
    dreq *r = pend[b];
    memmove(&pend[b], &pend[b + 1], (--npend - b) * sizeof(dreq *));
    return r;
}

static void *disk_thread(void *arg) {
    pthread_mutex_lock(&dlock);
    while (!quit) {
        if (!npend) { pthread_cond_wait(&dcond, &dlock); continue; }
        dreq *r = take(choose());
        int c = r->blk / BPC, dist = abs(c - head);
        moved += dist; served++; head = c;
        if (npath == 16) { memmove(path, path + 1, 15 * sizeof(int)); npath--; }
        path[npath++] = c;
        pthread_mutex_unlock(&dlock);
        usleep(1000 + 40 * dist);            /* rotation + seek time */
        if (r->write) pwrite(dfd, r->buf, BSIZE, (off_t)r->blk * BSIZE);
        else pread(dfd, r->buf, BSIZE, (off_t)r->blk * BSIZE);
        pthread_mutex_lock(&dlock); r->done = 1;
        pthread_kill(mainth, SIGUSR1);       /* "I/O complete" interrupt */
    }
    pthread_mutex_unlock(&dlock);
    return 0;
}

/* called by a job with interrupts off: queue a request and block until it is done */
static void disk_io(int blk, char *buf, int write) {
    dreq r = {blk, write, 0, buf};
    pthread_mutex_lock(&dlock); pend[npend++] = &r; pthread_cond_signal(&dcond); pthread_mutex_unlock(&dlock);
    cur->io = &r; cur->state = DISKWAIT;
    to_kernel(R_BLOCK);                      /* sleep until the disk interrupt */
    cur->io = 0;
}

/* kernel: wake up jobs whose disk request is complete */
void disk_poll(void) {
    pthread_mutex_lock(&dlock);
    for (int i = 0; i < MAXJOBS; i++) {
        pcb *p = &jobs[i];
        if (p->state != DISKWAIT || !p->io->done) continue;
        if (p->killed) finish(p); else make_ready(p);
    }
    pthread_mutex_unlock(&dlock);
}

void disk_show(void) {
    pthread_mutex_lock(&dlock);
    printf("Disk: %d cylinders, head at %d, algorithm %s", NCYL, head, disk_names[disk_algo]);
    if (disk_algo == D_SCAN || disk_algo == D_LOOK) printf(" (moving %s)", dir > 0 ? "up" : "down");
    printf("\nPending requests (cylinders):");
    for (int i = 0; i < npend; i++) printf(" %d", cyl(i));
    printf("\nRecent head path:");
    for (int i = 0; i < npath; i++) printf(" %s%d", i ? "-> " : "", path[i]);
    printf("\nServed %ld requests, total head movement %ld cylinders (%.1f per request)\n",
           served, moved, served ? (double)moved / served : 0);
    pthread_mutex_unlock(&dlock);
}

/* textbook exercise: run every algorithm on a given request queue, using the same choose() */
void disksim(int h, int *req, int n) {
    dreq fake[MAXJOBS], *save[MAXJOBS];
    pthread_mutex_lock(&dlock);              /* the disk thread must not run meanwhile */
    int snp = npend, sh = head, sd = dir, salgo = disk_algo;
    long sm = moved;
    memcpy(save, pend, sizeof pend);
    printf("Request queue:");
    for (int i = 0; i < n; i++) printf(" %d", req[i]);
    printf(", head at %d, cylinders 0-%d\n", h, NCYL - 1);
    for (disk_algo = 0; disk_algo < 6; disk_algo++) {
        head = h; dir = 1; moved = 0; npend = n;
        for (int i = 0; i < n; i++) { fake[i].blk = req[i] * BPC; pend[i] = &fake[i]; }
        printf("  %-6s %3d", disk_names[disk_algo], h);
        while (npend) {
            int c = take(choose())->blk / BPC;
            moved += abs(c - head); head = c;
            printf(" -> %d", c);
        }
        printf("   total %ld cylinders\n", moved);
    }
    npend = snp; head = sh; dir = sd; disk_algo = salgo; moved = sm;
    memcpy(pend, save, sizeof pend);
    pthread_mutex_unlock(&dlock);
}

/* ================= MODULE 6: PAGE CACHE AND FILE SYSTEM ================= */

typedef struct { int blk, busy, owner, ref; long tload, tuse; char data[BSIZE]; } frame;
static frame fr[NFRAMES];
static int hand; static long tick;

static long age(int i) { return cache_algo == C_FIFO ? fr[i].tload : fr[i].tuse; }

/* page replacement: choose a frame to throw out */
static int victim(void) {
    int b = -1;
    for (int i = 0; i < NFRAMES; i++) if (fr[i].blk < 0 && !fr[i].busy) return i;   /* free frame */
    if (cache_algo == C_CLOCK) {
        for (int n = 0; n < 2 * NFRAMES; n++, hand = (hand + 1) % NFRAMES) {
            if (fr[hand].busy) continue;
            if (fr[hand].ref) { fr[hand].ref = 0; continue; }   /* second chance */
            b = hand; hand = (hand + 1) % NFRAMES; return b;
        }
        return -1;
    }
    for (int i = 0; i < NFRAMES; i++)                       /* FIFO / LRU: oldest */
        if (!fr[i].busy && (b < 0 || age(i) < age(b))) b = i;
    return b;
}

/* read a block through the cache (interrupts off) */
static void bread(int blk, char *buf) {
    for (;;) {
        int f = -1;
        for (int i = 0; i < NFRAMES; i++)
            if (fr[i].blk == blk) f = i;
        if (f >= 0 && !fr[f].busy) {                        /* hit */
            hits++; fr[f].ref = 1; fr[f].tuse = ++tick;
            memcpy(buf, fr[f].data, BSIZE); return;
        }
        if (f < 0) f = victim();
        if (f < 0 || fr[f].busy) { to_kernel(R_YIELD); continue; }   /* someone else is loading */
        misses++;                                           /* page fault */
        fr[f].blk = blk; fr[f].busy = 1; fr[f].owner = cur->pid; fr[f].ref = 1;
        fr[f].tload = fr[f].tuse = ++tick;
        disk_io(blk, fr[f].data, 0);
        fr[f].busy = 0; memcpy(buf, fr[f].data, BSIZE); return;
    }
}

/* write through: update the cached copy and write to disk */
static void bwrite(int blk, char *buf) {
    for (int i = 0; i < NFRAMES; i++) {
        if (fr[i].blk != blk) continue;
        if (fr[i].busy) fr[i].blk = -1;                     /* being loaded: just drop it */
        else { memcpy(fr[i].data, buf, BSIZE); fr[i].tuse = ++tick; }
    }
    disk_io(blk, buf, 1);
}

void blk_read(int blk, char *buf) { intr_off(); bread(blk, buf); intr_on(); }

void cache_release(int pid) {
    for (int i = 0; i < NFRAMES; i++)
        if (fr[i].busy && fr[i].owner == pid) { fr[i].busy = 0; fr[i].blk = -1; }
}

void cache_show(void) {
    printf("Page cache: %d frames, replacement %s\n", NFRAMES, cache_names[cache_algo]);
    for (int i = 0; i < NFRAMES; i++) {
        printf("  frame %d: ", i);
        if (fr[i].blk < 0) { printf("empty\n"); continue; }
        printf("block %4d  loaded@%-5ld used@%-5ld ref=%d%s%s\n", fr[i].blk, fr[i].tload, fr[i].tuse,
               fr[i].ref, fr[i].busy ? " (loading)" : "", cache_algo == C_CLOCK && hand == i ? "  <- hand" : "");
    }
    printf("Hits %ld, page faults %ld, hit ratio %.1f%%\n", hits, misses,
           hits + misses ? 100.0 * hits / (hits + misses) : 0);
}

/* disk layout: 0 superblock | 1 bitmap | 2-5 inodes | 6-7 directory | 8.. data */
typedef struct { int magic, nblocks, ninodes, data0; } super_t;
typedef struct { int used, size; short blk[NDIRECT], ind; } inode;   /* 64 bytes, ind = indirect block */
typedef struct { char name[28]; int ino; } dirent;                 /* 32 bytes */
static super_t sb; static inode itab[NINODES]; static dirent dirs[NINODES];
static unsigned char bmap[NBLOCKS / 8];      /* free block bitmap */
static struct { sem mutex, wrt; int rc; } rw[NINODES];             /* readers-writers lock per file */
static unsigned seed = 2024;

unsigned rnd(void) { seed = seed * 1103515245 + 12345; return seed >> 8; }
static int bit(int b) { return bmap[b / 8] >> (b % 8) & 1; }
static void setbit(int b, int v) { if (v) bmap[b / 8] |= 1 << (b % 8); else bmap[b / 8] &= ~(1 << (b % 8)); }

void fs_sync(void) {                         /* write metadata back to the disk */
    pwrite(dfd, &sb, sizeof sb, 0);
    pwrite(dfd, bmap, sizeof bmap, BSIZE);
    pwrite(dfd, itab, sizeof itab, 2 * BSIZE);
    pwrite(dfd, dirs, sizeof dirs, 6 * BSIZE);
}

void disk_init(void) {
    char n[16];
    dfd = open("disk.img", O_RDWR | O_CREAT, 0644);
    pread(dfd, &sb, sizeof sb, 0);
    if (sb.magic != MAGIC) {                 /* new disk: format it */
        printf("toyos: formatting disk.img (%d blocks of %d bytes)\n", NBLOCKS, BSIZE);
        ftruncate(dfd, NBLOCKS * BSIZE);
        sb = (super_t){MAGIC, NBLOCKS, NINODES, DATA0};
        for (int b = 0; b < DATA0; b++) setbit(b, 1);
        fs_sync();
    }
    pread(dfd, bmap, sizeof bmap, BSIZE); pread(dfd, itab, sizeof itab, 2 * BSIZE); pread(dfd, dirs, sizeof dirs, 6 * BSIZE);
    for (int i = 0; i < NINODES; i++) {
        sprintf(n, "file%d.mutex", i); sem_init(&rw[i].mutex, 1, 1, n);
        sprintf(n, "file%d.wrt", i);   sem_init(&rw[i].wrt, 1, 1, n);
    }
    for (int i = 0; i < NFRAMES; i++) fr[i].blk = -1;
    mainth = pthread_self(); pthread_create(&dthread, 0, disk_thread, 0);
}

void disk_stop(void) {
    pthread_mutex_lock(&dlock); quit = 1; pthread_cond_signal(&dcond); pthread_mutex_unlock(&dlock);
    pthread_join(dthread, 0);
    fs_sync(); close(dfd);
}

int fs_lookup(const char *name) {
    for (int i = 0; i < NINODES; i++) if (dirs[i].name[0] && !strcmp(dirs[i].name, name)) return dirs[i].ino;
    return -1;
}

int fs_create(const char *name) {
    int i, d;
    intr_off();
    if ((i = fs_lookup(name)) < 0) {
        for (i = 0; i < NINODES && itab[i].used; i++);
        for (d = 0; d < NINODES && dirs[d].name[0]; d++);
        if (i < NINODES && d < NINODES) {
            memset(&itab[i], 0, sizeof(inode)); itab[i].used = 1;
            strncpy(dirs[d].name, name, 27); dirs[d].ino = i;
        } else i = -1;
    }
    intr_on();
    return i;
}

void fs_trunc(int ino) {
    short ind[BSIZE / 2];
    inode *ip = &itab[ino];
    intr_off();
    for (int k = 0; k < NDIRECT; k++)
        if (ip->blk[k]) { setbit(ip->blk[k], 0); ip->blk[k] = 0; }
    if (ip->ind) {                           /* free the blocks listed in the indirect block too */
        bread(ip->ind, (char *)ind);
        for (int k = 0; k < BSIZE / 2; k++) if (ind[k]) setbit(ind[k], 0);
        setbit(ip->ind, 0);
        ip->ind = 0;
    }
    ip->size = 0;
    intr_on();
}

int fs_remove(const char *name) {
    intr_off();
    int ino = fs_lookup(name);
    if (ino >= 0) {
        fs_trunc(ino); itab[ino].used = 0;
        for (int d = 0; d < NINODES; d++)
            if (dirs[d].name[0] && dirs[d].ino == ino) dirs[d].name[0] = 0;
    }
    intr_on();
    return ino;
}

static int balloc(int hint) {                /* first free block at or after hint */
    for (int k = 0; k < NBLOCKS - DATA0; k++) {
        int b = DATA0 + (hint - DATA0 + k) % (NBLOCKS - DATA0);
        if (!bit(b)) { setbit(b, 1); return b; }
    }
    return 0;                                /* disk full */
}

/* file block number -> disk block number (0 = none); alloc = 1 allocates missing blocks */
static int fblock(inode *ip, int bi, int alloc) {
    short ind[BSIZE / 2];
    if (bi < NDIRECT) {
        if (!ip->blk[bi] && alloc)           /* a new file starts somewhere random on the disk */
            ip->blk[bi] = balloc(bi ? ip->blk[bi - 1] + 1 : DATA0 + rnd() % (NBLOCKS - DATA0));
        return ip->blk[bi];
    }
    if ((bi -= NDIRECT) >= BSIZE / 2) return 0;
    if (!ip->ind) {
        if (!alloc || !(ip->ind = balloc(ip->blk[NDIRECT - 1] + 1))) return 0;
        memset(ind, 0, BSIZE);
        bwrite(ip->ind, (char *)ind);
    }
    bread(ip->ind, (char *)ind);
    if (!ind[bi] && alloc) {
        ind[bi] = balloc((bi ? ind[bi - 1] : ip->ind) + 1);
        bwrite(ip->ind, (char *)ind);
    }
    return ind[bi];
}

int fs_read(int ino, int off, char *buf, int n) {
    char b[BSIZE];
    int got = 0;
    intr_off();
    if (off + n > itab[ino].size) n = itab[ino].size - off;
    while (got < n) {
        int pos = off + got, c = BSIZE - pos % BSIZE;
        if (c > n - got) c = n - got;
        bread(fblock(&itab[ino], pos / BSIZE, 0), b);
        memcpy(buf + got, b + pos % BSIZE, c); got += c;
    }
    intr_on();
    return got;
}

int fs_write(int ino, const char *buf, int n) {   /* append n bytes to the file */
    char b[BSIZE];
    int done = 0;
    inode *ip = &itab[ino];
    intr_off();
    while (done < n && ip->size < MAXFILE) {
        int bo = ip->size % BSIZE, c = BSIZE - bo, blk = fblock(ip, ip->size / BSIZE, 1);
        if (!blk) break;                     /* disk full */
        if (c > n - done) c = n - done;
        if (bo) bread(blk, b);               /* add to a half-filled block */
        else memset(b, 0, BSIZE);            /* start a new block */
        memcpy(b + bo, buf + done, c);
        bwrite(blk, b);
        ip->size += c; done += c;
    }
    intr_on();
    return done;
}

/* readers-writers problem (readers preference) using our semaphores */
void rd_lock(int i) {
    intr_off(); sem_wait(&rw[i].mutex);
    if (++rw[i].rc == 1) sem_wait(&rw[i].wrt);      /* first reader locks out writers */
    sem_signal(&rw[i].mutex); intr_on();
}

void rd_unlock(int i) {
    intr_off(); sem_wait(&rw[i].mutex);
    if (--rw[i].rc == 0) sem_signal(&rw[i].wrt);    /* last reader lets writers in */
    sem_signal(&rw[i].mutex); intr_on();
}

void wr_lock(int i) { P(&rw[i].wrt); }
void wr_unlock(int i) { V(&rw[i].wrt); }

/* the "ls" program */
void fs_ls(int argc, char **argv) {
    int used = 0;
    kprintf("NAME                 INODE   SIZE  BLOCKS (cylinder)\n");
    for (int d = 0; d < NINODES; d++) {
        if (!dirs[d].name[0]) continue;
        inode *ip = &itab[dirs[d].ino];
        int nb = (ip->size + BSIZE - 1) / BSIZE;
        kprintf("%-20s %5d %6d ", dirs[d].name, dirs[d].ino, ip->size);
        for (int k = 0; k < nb && k < 4; k++) kprintf(" %d(c%d)", ip->blk[k], ip->blk[k] / BPC);
        kprintf(nb > 4 ? " ... %d blocks\n" : "\n", nb);
    }
    for (int b = 0; b < NBLOCKS; b++) used += bit(b);
    kprintf("%d of %d blocks used\n", used, NBLOCKS);
}

/* the "df" program: disk usage */
void fs_df(int argc, char **argv) {
    int used = 0, files = 0;
    for (int b = 0; b < NBLOCKS; b++) used += bit(b);
    for (int i = 0; i < NINODES; i++) files += itab[i].used;
    kprintf("disk.img: %d blocks of %d bytes, %d used, %d free (%d%% full)\n", NBLOCKS, BSIZE, used,
            NBLOCKS - used, used * 100 / NBLOCKS);
    kprintf("inodes: %d of %d used, max file size %d KB\n", files, NINODES, MAXFILE / 1024);
}

/* the "stat" program: inode details of a file */
void fs_stat(int argc, char **argv) {
    for (int a = 1; a < argc; a++) {
        int ino = fs_lookup(argv[a]);
        if (ino < 0) { kprintf("stat: %s: no such file\n", argv[a]); continue; }
        inode *ip = &itab[ino];
        int nb = (ip->size + BSIZE - 1) / BSIZE;
        kprintf("%s: inode %d, %d bytes, %d blocks%s\n  blocks:", argv[a], ino, ip->size, nb,
                ip->ind ? ", uses indirect block" : "");
        intr_off();
        for (int k = 0; k < nb; k++) kprintf(" %d", fblock(ip, k, 0));
        intr_on();
        kprintf("\n");
    }
}

/* the "fsck" program: check that the bitmap matches the blocks used by the inodes, and repair it */
void fs_fsck(int argc, char **argv) {
    unsigned char use[NBLOCKS] = {0};
    short ind[BSIZE / 2];
    int files = 0, dup = 0, lost = 0, leak = 0, orphan = 0;
    intr_off();
    for (int b = 0; b < DATA0; b++) use[b] = 1;
    for (int i = 0; i < NINODES; i++) {
        inode *ip = &itab[i];
        if (!ip->used) continue;
        files++;
        for (int k = 0; k < NDIRECT; k++) if (ip->blk[k]) dup += use[ip->blk[k]]++ > 0;
        if (!ip->ind) continue;
        dup += use[ip->ind]++ > 0;
        bread(ip->ind, (char *)ind);
        for (int k = 0; k < BSIZE / 2; k++) if (ind[k]) dup += use[ind[k]]++ > 0;
    }
    for (int b = 0; b < NBLOCKS; b++) {
        if (use[b] && !bit(b)) { lost++; setbit(b, 1); }       /* used but marked free */
        if (!use[b] && bit(b)) { leak++; setbit(b, 0); }       /* marked used but nobody owns it */
    }
    for (int d = 0; d < NINODES; d++)        /* directory entries pointing to free inodes */
        if (dirs[d].name[0] && !itab[dirs[d].ino].used) { dirs[d].name[0] = 0; orphan++; }
    intr_on();
    int used = 0;
    for (int b = 0; b < NBLOCKS; b++) used += bit(b);
    kprintf("fsck: %d files checked, %d blocks in use\n", files, used);
    kprintf("  fixed: %d used blocks marked free, %d leaked blocks freed, %d bad directory entries\n",
            lost, leak, orphan);
    kprintf("  %d blocks shared by two files%s\n", dup, lost + leak + orphan + dup ? "" : " - file system is clean");
}
