// Martin Cracauer's version of bonnie

#ifdef __linux
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#endif

/*
 * This is a file system benchmark which attempts to study bottlenecks -
 * it is named 'bonnie' for semi-obvious reasons.
 *
 * Specifically, these are the types of filesystem activity that have been
 * observed to be bottlenecks in I/O-intensive applications, in particular
 * the text database work done in connection with the New Oxford English
 * Dictionary Project at the University of Waterloo.
 * 
 * It performs a series of tests on a file of known size.  By default, that
 * size is 100 Mb (but that's not enough - see below).  For each test, bonnie 
 * reports the bytes processed per elapsed second, per CPU second, and the 
 * % CPU usage (user and system).
 * 
 * In each case, an attempt is made to keep optimizers from noticing it's 
 * all bogus.  The idea is to make sure that these are real transfers to/from
 * user space to the physical disk.  The tests are:
 * 
 * 1. Sequential Output
 * 
 * 1.1 Per-Character.  The file is written using the putc() stdio macro.
 * The loop that does the writing should be small enough to fit into any
 * reasonable I-cache.  The CPU overhead here is that required to do the
 * stdio code plus the OS file space allocation.
 * 
 * 1.2 Block.  The file is created using write(2).  The CPU overhead
 * should be just the OS file space allocation.
 * 
 * 1.3 Rewrite.  Each BUFSIZ of the file is read with read(2), dirtied, and
 * rewritten with write(2), requiring an lseek(2).  Since no space
 * allocation is done, and the I/O is well-localized, this should test the
 * effectiveness of the filesystem cache and the speed of data transfer.
 * 
 * 2. Sequential Input
 * 
 * 2.1 Per-Character.  The file is read using the getc() stdio macro.  Once
 * again, the inner loop is small.  This should exercise only stdio and
 * sequential input.
 * 
 * 2.2 Block.  The file is read using read(2).  This should be a very pure
 * test of sequential input performance.
 * 
 * 3. Random Seeks
 * 
 * This test runs SeekProcCount processes in parallel, doing a total of
 * 4000 lseek()s to locations in the file specified by random() in bsd systems,
 * drand48() on sysV systems.  In each case, the block is read with read(2).  
 * In 10% of cases, it is dirtied and written back with write(2).
 *
 * The idea behind the SeekProcCount processes is to make sure there's always 
 * a seek queued up.
 * 
 * AXIOM: For any unix filesystem, the effective number of lseek(2) calls
 * per second declines asymptotically to near 30, once the effect of
 * caching is defeated.
 * 
 * The size of the file has a strong nonlinear effect on the results of
 * this test.  Many Unix systems that have the memory available will make
 * aggressive efforts to cache the whole thing, and report random I/O rates
 * in the thousands per second, which is ridiculous.  As an extreme
 * example, an IBM RISC 6000 with 64 Mb of memory reported 3,722 per second
 * on a 50 Mb file.  Some have argued that bypassing the cache is artificial
 * since the cache is just doing what it's designed to.  True, but in any 
 * application that requires rapid random access to file(s) significantly
 * larger than main memory which is running on a system which is doing
 * significant other work, the caches will inevitably max out.  There is
 * a hard limit hiding behind the cache which has been observed by the
 * author to be of significant import in many situations - what we are trying
 * to do here is measure that number.
 *
 * COPYRIGHT NOTICE: 
 * Copyright (c) Tim Bray, 1990.
 * Everybody is hereby granted rights to use, copy, and modify this program, 
 *  provided only that this copyright notice and the disclaimer below
 *  are preserved without change.
 * DISCLAIMER:
 * This program is provided AS IS with no warranty of any kind, and
 * The author makes no representation with respect to the adequacy of this
 *  program for any particular purpose or with respect to its adequacy to 
 *  produce any particular result, and
 * The author shall not be liable for loss or damage arising out of
 *  the use of this program regardless of how sustained, and
 * In no event shall the author be liable for special, direct, indirect
 *  or consequential damage, loss, costs or fees or expenses of any
 *  nature or kind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#if defined(SysV) || defined(__svr4__)
#include <limits.h>
#include <time.h>
#include <sys/times.h>
#else
#include <sys/resource.h>
#endif

#define IntSize (4)

/*
 * N.B. in seeker_reports, CPU appears and Start/End time, but not Elapsed,
 *  so position 1 is re-used; icky data coupling.
 */
#define CPU (0)
#define Elapsed (1)
#define StartTime (1)
#define EndTime (2)
#define Seeks (100000)
#define Seeks2 (100000)
#define UpdateSeek (10)
#define SeekProcCount (3)
#define Chunk (8192)

static double cpu_so_far();
static void  doseek(long long where, int fd, int update, int do_touch);
static void   get_delta_t();
static void   io_error();
static void   newfile();
#if 0
#if defined(SysV) || defined(__svr4__)
static long   random();
static void   srandom(int seed);
#endif
#endif
static void   report(void);
static double time_so_far();
static void   timestamp();
static void   usage();

typedef enum
{
  Putc,
  ReWrite,
  FastWrite,
  Getc,
  FastRead,
  Lseek,
  Lseek2,
  TestCount
} tests_t;

static int    basetime;
static double delta[(int) TestCount][2];
static char * machine = "";
static double last_cpustamp = 0.0;
static double last_timestamp = 0.0;

long long file_size = 24;
char filename[8192];

int do_cleanup = 1;

static void cleanup(void)
{
  if (do_cleanup) {
    unlink(filename);
    report();
  }
}

static void cleanup_sig(int whichsignal)
{
  cleanup();
  signal(whichsignal, SIG_DFL);
  kill(getpid(), whichsignal);
}

void do_seekstuff(int do_write, int whereto)
{
  int    seek_control[2];
  int    seek_feedback[2];
  char   seek_tickets[Seeks + SeekProcCount];
  double seeker_report[3];
  long long    next;
  pid_t    child;
  int status;
  int fd;
  FILE *stream;
  int    lseek_count = 0;
  double first_start = 0.0;
  double last_stop = 0.0;

  system("dropthedamncaches");
  /*
   * Now test random seeks; first, set up for communicating with children.
   * The object of the game is to do "Seeks" lseek() calls as quickly
   *  as possible.  So we'll farm them out among SeekProcCount processes.
   *  We'll control them by writing 1-byte tickets down a pipe which
   *  the children all read.  We write "Seeks" bytes with val 1, whichever
   *  child happens to get them does it and the right number of seeks get
   *  done.
   * The idea is that since the write() of the tickets is probably
   *  atomic, the parent process likely won't get scheduled while the
   *  children are seeking away.  If you draw a picture of the likely
   *  timelines for three children, it seems likely that the seeks will
   *  overlap very nicely with the process scheduling with the effect
   *  that there will *always* be a seek() outstanding on the file.
   * Question: should the file be opened *before* the fork, so that
   *  all the children are lseeking on the same underlying file object?
   */
  if (pipe(seek_feedback) == -1 || pipe(seek_control) == -1)
    io_error("pipe");
  for (next = 0; next < Seeks; next++)
    seek_tickets[next] = 1;
  for ( ; next < (Seeks + SeekProcCount); next++)
    seek_tickets[next] = 0;

  /* launch some parallel seek processes */
  for (next = 0; next < SeekProcCount; next++)
  { /* for each seek proc */
    if ((child = fork()) == -1)
      io_error("fork");
    else if (child == 0)
    { /* child process */

      do_cleanup = 0;

      /* set up and wait for the go-ahead */
      close(seek_feedback[0]);
      close(seek_control[1]);
      newfile(filename, &fd, &stream, 0);
      srandom(getpid());
      fprintf(stderr, "Seeker %lld...", next + 1);

      /* wait for the go-ahead */
      if (read(seek_control[0], seek_tickets, 1) != 1)
	io_error("read ticket");
      timestamp();
      seeker_report[StartTime] = time_so_far();

      /* loop until we read a 0 ticket back from our parent */
      while(seek_tickets[0])
      { /* until Mom says stop */
        doseek((long long) ((random() * 0xFFFFFFFF) % file_size), fd,
	  ((lseek_count++ % UpdateSeek) == 0), do_write);
	if (read(seek_control[0], seek_tickets, 1) != 1)
	  io_error("read ticket");
      } /* until Mom says stop */
      if (fsync(fd) == -1)
	perror("fsync after seek");
      if (close(fd) == -1)
        io_error("close after seek");

      /* report to parent */
      get_delta_t(whereto);
      seeker_report[EndTime] = time_so_far();
      seeker_report[CPU] = delta[(int) whereto][CPU];
      if (write(seek_feedback[1], seeker_report, sizeof(seeker_report))
          != sizeof(seeker_report))
        io_error("pipe write");
      exit(0);
    } /* child process */
  } /* for each seek proc */

  /*
   * Back in the parent; in an effort to ensure the children get an even
   *  start, wait a few seconds for them to get scheduled, open their
   *  files & so on.
   */
  close(seek_feedback[1]);
  close(seek_control[0]);
  sleep(1);
  fprintf(stderr, "start 'em...");
  if (write(seek_control[1], seek_tickets, sizeof(seek_tickets)) 
      != sizeof(seek_tickets))
    io_error("write tickets");
  
  /* read back from children */
  for (next = 0; next < SeekProcCount; next++)
  { /* for each child */
    if (read(seek_feedback[0], (char *) seeker_report, sizeof(seeker_report))
        != sizeof(seeker_report))
      io_error("pipe read");

    /*
     * each child writes back its CPU, start & end times.  The elapsed time 
     *  to do all the seeks is the time the first child started until the 
     *  time the last child stopped
     */
    delta[whereto][CPU] += seeker_report[CPU];
    if (next == 0)
    { /* first time */
      first_start = seeker_report[StartTime];
      last_stop = seeker_report[EndTime];
    } /* first time */
    else
    { /* not first time */
      first_start = (first_start < seeker_report[StartTime]) ?
	first_start : seeker_report[StartTime]; 
      last_stop = (last_stop > seeker_report[EndTime]) ?
	last_stop : seeker_report[EndTime]; 
    } /* not first time */
    if (wait(&status) == -1)
      io_error("wait");
    fprintf(stderr, "done...");
  } /* for each child */
  fprintf(stderr, "\n");
  delta[(int) whereto][Elapsed] = last_stop - first_start;
}

void randomize_buffer(void *ptr, int size)
{
  int *optr = ptr;
  int i;
  for (i = 0; i < size  / sizeof(int); i++)
    optr[i] = rand();
}


int main(argc, argv)
  int    argc;
  char * argv[];
{
  int    buf[Chunk / IntSize];
  int    bufindex;
  int    chars[256];
  char * dir;
  int    fd;
  FILE * stream;
  int    words;
  long long next;
  int do_random = 0;

  if (atexit(cleanup) == -1) {
    perror("atexit(3)");
    exit(1);
  }
  if (signal(SIGINT, cleanup_sig)) {
    perror("signal");
    exit(1);
  }
  if (signal(SIGTERM, cleanup_sig)) {
    perror("signal");
    exit(1);
  }
  if (signal(SIGQUIT, cleanup_sig)) {
    perror("signal");
    exit(1);
  }

  fd = -1;
  basetime = (int) time((time_t *) NULL);
  dir = ".";

  for (next = 1; next < argc - 1; next++)
    if (argv[next][0] == '-')
    { /* option? */
      if (strcmp(argv[next] + 1, "d") == 0)
        dir = argv[next + 1];
      else if (strcmp(argv[next] + 1, "s") == 0)
        file_size = atoll(argv[next + 1]);
      else if (strcmp(argv[next] + 1, "m") == 0)
        machine = argv[next + 1];
      else if (strcmp(argv[next] + 1, "r") == 0) {
        do_random = 1;
      } else
        usage();
      next++;
    } /* option? */
    else
      usage();

  if (file_size < 1)
    usage();
  file_size *= (1024 * 1024 * 1024);
  snprintf(filename, sizeof(filename), "%s/bonnie.%d", dir, (int)getpid());
  fprintf(stderr, "File '%s', size: %.2f GB\n", filename, file_size / 1024.0 / 1024.0 / 1024.0);

#if 0
  /* Fill up a file, writing it a char at a time with the stdio putc() call */
  fprintf(stderr, "Writing with putc()...");
  newfile(name, &fd, &stream, 1);
  timestamp();
  for (words = 0; words < size; words++)
    if (putc(words & 0x7f, stream) == EOF)
      io_error("putc");
  
  /*
   * note that we always close the file before measuring time, in an
   *  effort to force as much of the I/O out as we can
   */
  if (fclose(stream) == -1)
    io_error("fclose after putc");
  get_delta_t(Putc);
  fprintf(stderr, "done\n");
#endif

  /* Write the whole file from scratch, again, with block I/O */
  newfile(filename, &fd, &stream, 1);
  fprintf(stderr, "Writing intelligently...");
  for (words = 0; words < Chunk / IntSize; words++)
    buf[words] = 42;
  timestamp();
  for (words = bufindex = 0; words < (file_size / (long long)Chunk); words++)
  { /* for each word */
    if (bufindex == (Chunk / IntSize))
      bufindex = 0;
    buf[bufindex++]++;
    if (do_random)
      randomize_buffer(buf, Chunk);
    if (write(fd, (char *) buf, Chunk) == -1)
      io_error("write(2)");
  } /* for each word */
  if (fsync(fd) == -1)
    perror("fsync after fast write");
  if (close(fd) == -1)
    io_error("close after fast write");
  get_delta_t(FastWrite);
  fprintf(stderr, "done\n");

  /* Now read & rewrite it using block I/O.  Dirty one word in each block */
  newfile(filename, &fd, &stream, 0);
  if (lseek(fd, (off_t) 0, 0) == (off_t) -1)
    io_error("lseek(2) before rewrite");
  fprintf(stderr, "Rewriting...");
  timestamp();
  bufindex = 0;
  if ((words = read(fd, (char *) buf, Chunk)) == -1)
    io_error("rewrite read");
  while (words == Chunk)
  { /* while we can read a block */
    if (bufindex == Chunk / IntSize)
      bufindex = 0;
    buf[bufindex++]++;
    if (lseek(fd, (off_t) -words, 1) == -1)
      io_error("relative lseek(2)");
    if (do_random)
      randomize_buffer(buf, Chunk);
    if (write(fd, (char *) buf, words) == -1)
      io_error("re write(2)");
    if (write(fd, (char *) buf, words) == -1)
      io_error("re write(2)");
#if 0
      /* Too hard and kinda useless */
    if (fsync(fd) == -1)
      io_error("fsync(2) in rewrite(2)");
#endif
    if ((words = read(fd, (char *) buf, Chunk)) == -1)
      io_error("rwrite read");
  } /* while we can read a block */
  if (fsync(fd) == -1)
    perror("fsync after fast rewrite");
  if (close(fd) == -1)
    io_error("close after rewrite");
  get_delta_t(ReWrite);
  fprintf(stderr, "done\n");

#if 0
  /* read them all back with getc() */
  newfile(filename, &fd, &stream, 0);
  for (words = 0; words < 256; words++)
    chars[words] = 0;
  fprintf(stderr, "Reading with getc()...");
  timestamp();
  for (words = 0; words < file_size; words++)
  { /* for each byte */
    if ((next = getc(stream)) == EOF)
      io_error("getc(3)");

    /* just to fool optimizers */
    chars[next]++;
  } /* for each byte */
  if (fclose(stream) == -1)
    io_error("fclose after getc");
  get_delta_t(Getc);
  fprintf(stderr, "done\n");
#endif

  system("dropthedamncaches");

  /* use the frequency count */
  for (words = 0; words < 256; words++)
    sprintf((char *) buf, "%d", chars[words]);

  /* Now suck it in, Chunk at a time, as fast as we can */
  newfile(filename, &fd, &stream, 0);
  if (lseek(fd, (off_t) 0, 0) == -1)
    io_error("lseek before read");
  fprintf(stderr, "Reading intelligently...");
  timestamp();
  do
  {
    if ((words = read(fd, (char *) buf, Chunk)) == -1)
      io_error("read(2)");
    chars[buf[abs(buf[0]) % (Chunk / IntSize)] & 0x7f]++;
  } while (words);

  if (close(fd) == -1)
    io_error("close after read");
  get_delta_t(FastRead);
  fprintf(stderr, "done\n");

  /* use the frequency count */
  /* Stupid hack */
  for (words = 0; words < 256; words++)
    sprintf((char *)buf, "%d", chars[words]);

  do_seekstuff(0, Lseek);
  do_seekstuff(1, Lseek2);

  return 0;
}

static void
report(void)
{
  long long size = file_size;
  printf("         ");
  printf(
    "---Sequential Output----- ---Input---- ------Random-- -----Random----\n");
  printf("         ");
  printf(
    "---Block---- ---Rewrite-- ---Block---- ---ro Seeks--- -Seeks rewrite-\n");
  printf("    MB ");
  printf("   M/sec %%CPU    M/sec %%CPU   M/sec ");
  printf("%%CPU      /sec  %%CPU     /sec  %%CPU\n");

  printf("%6lld ", size / (1024 * 1024));
  printf("%7.1f %5.1f %7.1f %5.1f ",
    (((double) size) / (delta[(int) FastWrite][Elapsed] * 1024.0 * 1024.0)),
    delta[(int) FastWrite][CPU] / delta[(int) FastWrite][Elapsed] * 100.0,
    (((double) size) / (delta[(int) ReWrite][Elapsed] * 1024.0 * 1024.0)),
    delta[(int) ReWrite][CPU] / delta[(int) ReWrite][Elapsed] * 100.0);
  printf("%8.1f %5.1f ",
    (((double) size) / (delta[(int) FastRead][Elapsed] * 1024.0 * 1024.0)),
    delta[(int) FastRead][CPU] / delta[(int) FastRead][Elapsed] * 100.0);
  printf("%8.1f %5.1f ",
    ((double) Seeks) / delta[(int) Lseek][Elapsed],
    delta[(int) Lseek][CPU] / delta[(int) Lseek][Elapsed] * 100.0);
  printf("%7.1f %5.1f\n",
    ((double) Seeks2) / delta[(int) Lseek2][Elapsed],
    delta[(int) Lseek2][CPU] / delta[(int) Lseek2][Elapsed] * 100.0);
}

static void
newfile(name, fd, stream, create)
  char *   name;
  int *    fd;
  FILE * * stream;
  int      create;
{
  if (create)
  { /* create from scratch */
    if (unlink(name) == -1 && *fd != -1)
      io_error("unlink");
    *fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0777);
  } /* create from scratch */
  else
    *fd = open(name, O_RDWR, 0777);

  if (*fd == -1)
    io_error(name);
  *stream = fdopen(*fd, "r+");
  if (*stream == NULL)
    io_error("fdopen");
}

static void
usage()
{
  fprintf(stderr,
    "usage: bonnie [-d scratch-dir] [-s size-in-Mb] [-m machine-label]\n");
  exit(1);
}

static void
timestamp()
{
  last_timestamp = time_so_far();
  last_cpustamp = cpu_so_far();
}

static void 
get_delta_t(test)
  tests_t test;
{
  int which = (int) test;

  delta[which][Elapsed] = time_so_far() - last_timestamp;
  delta[which][CPU] = cpu_so_far() - last_cpustamp;
}

static double 
cpu_so_far()
{
#if defined(SysV) || defined(__svr4__)
  struct tms tms;

  if (times(&tms) == -1)
    io_error("times");
  return ((double) tms.tms_utime) / ((double) CLK_TCK) +
    ((double) tms.tms_stime) / ((double) CLK_TCK);

#else
  struct rusage rusage;

  getrusage(RUSAGE_SELF, &rusage);
  return
    ((double) rusage.ru_utime.tv_sec) +
      (((double) rusage.ru_utime.tv_usec) / 1000000.0) +
        ((double) rusage.ru_stime.tv_sec) +
          (((double) rusage.ru_stime.tv_usec) / 1000000.0);
#endif
}

static double
time_so_far()
{
#if defined(SysV) || defined(__svr4__)
  int        val;
  struct tms tms;

  if ((val = times(&tms)) == -1)
    io_error("times");

  return ((double) val) / ((double) CLK_TCK);

#else
  struct timeval tp;

  if (gettimeofday(&tp, (struct timezone *) NULL) == -1)
    io_error("gettimeofday");
  return ((double) (tp.tv_sec - basetime)) +
    (((double) tp.tv_usec) / 1000000.0);
#endif
}

static void
io_error(message)
  char * message;
{
  char buf[Chunk];

  sprintf(buf, "bonnie: drastic I/O error (%s)", message);
  perror(buf);
  exit(1);
}

/*
 * Do a typical-of-something random I/O.  Any serious application that
 *  has a random I/O bottleneck is going to be smart enough to operate
 *  in a page mode, and not stupidly pull individual words out at
 *  odd offsets.  To keep the cache from getting too clever, some
 *  pages must be updated.  However an application that updated each of
 *  many random pages that it looked at is hard to imagine.  
 * However, it would be wrong to put the update percentage in as a
 *  parameter - the effect is too nonlinear.  Need a profile
 *  of what Oracle or Ingres or some such actually does.
 * Be warned - there is a *sharp* elbow in this curve - on a 1-Mb file,
 *  most substantial unix systems show >2000 random I/Os per second -
 *  obviously they've cached the whole thing and are just doing buffer
 *  copies.  
 */
static void 
doseek(long long where, int fd, int update, int do_touch)
{
  int   buf[Chunk / IntSize];
  off_t probe;
  int   size; // this is not the file size

  probe = (where / Chunk) * Chunk;
  if (lseek(fd, probe, 0) != probe)
    io_error("lseek in doseek");
  if ((size = read(fd, (char *) buf, Chunk)) == -1)
    io_error("read in doseek");

  /* every so often, update a block */
  if (update && do_touch)
  { /* update this block */

    /* touch a word */
    buf[((long long) random() % (size/IntSize - 2)) + 1]--;
    if (lseek(fd, probe, 0) != probe)
      io_error("lseek in doseek update");
    if (write(fd, (char *) buf, size) == -1)
      io_error("write in doseek");
    if (fsync(fd) == -1)
      io_error("fsync(2) in seek w/write(2)");
  } /* update this block */
}
  
#if 0
#if defined(SysV) || defined(__svr4__)
static char randseed[32];

static void
srandom(seed)
  int seed;
{
  sprintf(randseed, "%06d", seed);
}

static long
random()
{
  return nrand48(randseed);
}
#endif
#endif

#ifdef bla
/*---Clemens Dinges' FS scrambler code--*/
void main(int argc, char * argv[])
{
   static char name[] = "/mnt/h.";
   int    fd_lst [10];
   int    buf[7<<10];
   int    size = 50;
   int    lv,lvv,lvvv;

   for (lv = 1; lv < argc; lv++)
      if (argv[lv][0] == '-')
         { /* option? */
         if ((strcmp(argv[lv] + 1, "s") == 0) && (lv < argc-1))
            size = atoi(argv[++lv]);
         else
            usage();
         } /* option? */
      else
         usage();
   if (size < 1)
      usage();
   size <<=20;

   name[6] = '0';
   for (lv=0; lv < 10; lv++)  {
      fd_lst[lv] = open(name,O_RDWR|O_CREAT,0777);
      name[6]++;
   }
   lvv = 0;
   while (lvv < size) { 
      for (lv=0; lv < 10; lv++) {
         write(fd_lst[lv],buf,(lvvv = (random() % (7<<10))));
         lvv += lvvv;
      }
   }
   name[6] = '0';
   for (lv=0; lv < 10; lv++) 
      close(fd_lst[lv]);
      unlink(name);
      name[6]++;
   }

   /* then umount ... */
}
#endif
