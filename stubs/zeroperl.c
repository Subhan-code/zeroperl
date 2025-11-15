#define PERL_IN_MINIPERLMAIN_C

#include "zeroperl.h" /* Must define SFS_BUILTIN_PREFIX, e.g. "builtin:" */
#include "EXTERN.h"
#include "XSUB.h"
#include "asyncify.h"
#include "perl.h"
#include "setjmp.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STRINGIZE_HELPER(x) #x
#define STRINGIZE(x) STRINGIZE_HELPER(x)
#include <wasi/api.h>

//! Export macro for public API functions - combines export_name for WASI with
//! visibility attribute
#if defined(__WASI__) || defined(__wasi__)
#define ZEROPERL_API(name)                                                     \
  __attribute__((export_name(name))) __attribute__((visibility("default")))
#else
#define ZEROPERL_API(name) __attribute__((visibility("default")))
#endif

//! Writes the given string literal directly to STDERR via __wasi_fd_write
//! Avoids calls to printf or other asyncified C library functions
#define DEBUG_LOG_INTERNAL(msg)                                                \
  do {                                                                         \
    const uint8_t *msg_start = (const uint8_t *)(msg);                         \
    const uint8_t *msg_end = msg_start;                                        \
    while (*msg_end != '\0') {                                                 \
      msg_end++;                                                               \
    }                                                                          \
    __wasi_ciovec_t iov = {.buf = msg_start,                                   \
                           .buf_len = (size_t)(msg_end - msg_start)};          \
    size_t nwritten;                                                           \
    __wasi_fd_write(STDERR_FILENO, &iov, 1, &nwritten);                        \
  } while (0)

//! Appends file name and line number, then calls DEBUG_LOG_INTERNAL
#define DEBUG_LOG(msg)                                                         \
  DEBUG_LOG_INTERNAL(__FILE__ ":" STRINGIZE(__LINE__) ": " msg "\n")

//! Forward declaration for XS init function
static void xs_init(pTHX);

//! Global Perl interpreter instance
static PerlInterpreter *zero_perl = NULL;

//! Global state flags
static bool zero_perl_system_initialized = false; // PERL_SYS_INIT3 called
static bool zero_perl_can_evaluate = false; // perl_run called, ready for eval

//! Error message buffer (stores last Perl error from $@)
static char zero_perl_error_buf[1024] = {0};

//! Environment variables
extern char **environ;

//! External declarations for the underlying ("real") syscalls (wrapped via
//! linker)
extern FILE *__real_fopen(const char *path, const char *mode);
extern int __real_fileno(FILE *stream);
extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_read(int fd, void *buf, size_t count);
extern off_t __real_lseek(int fd, off_t offset, int whence);
extern int __real_access(const char *path, int flags);
extern int __real_stat(const char *restrict path,
                       struct stat *restrict statbuf);
extern int __real_fstat(int fd, struct stat *statbuf);

//! Maximum number of file descriptors to track
#ifndef FD_MAX_TRACK
#define FD_MAX_TRACK 32
#endif

//! Maximum number of open SFS (Simple File System) files
#ifndef SFS_MAX_OPEN_FILES
#define SFS_MAX_OPEN_FILES 16
#endif

//! Tracks which file descriptors are in use
static bool g_fd_in_use[FD_MAX_TRACK] = {false};

//! Marks a file descriptor as in use (not thread-safe)
static inline void fd_mark_in_use(int fd) {
  if (fd >= 0 && fd < FD_MAX_TRACK) {
    g_fd_in_use[fd] = true;
  }
}

//! Marks a file descriptor as free
static inline void fd_mark_free(int fd) {
  if (fd >= 0 && fd < FD_MAX_TRACK) {
    g_fd_in_use[fd] = false;
  }
}

//! Checks if a file descriptor is in use
//! Out-of-range FDs are treated as in use
static inline bool fd_is_in_use(int fd) {
  return (fd >= 0 && fd < FD_MAX_TRACK) ? g_fd_in_use[fd] : true;
}

//! SFS entry structure for tracking open virtual files
//! Each slot tracks an integer FD, a FILE* (via fmemopen), file size, and a
//! "used" flag
typedef struct {
  bool used;
  int fd;
  FILE *fp;
  size_t size;
} SFS_Entry;

//! Table of open SFS files
static SFS_Entry sfs_table[SFS_MAX_OPEN_FILES];

//! Starting FD offset for SFS (skips standard FDs 0-2)
static int sfs_fd_start = 3;

//! Result codes for SFS operations
typedef enum { SFS_OK = 0, SFS_ERR = -1, SFS_NOT_OURS = -2 } SFS_Result;

//! Result codes for stat calls
typedef enum {
  SFS_STAT_ERR = -1,    // SFS path but not found/error
  SFS_STAT_OURS = 0,    // We handled it
  SFS_STAT_NOT_OURS = 1 // Not an SFS path, use fallback
} SFS_Stat_Result;

//! Removes consecutive duplicate '/' from a path for canonicalization
static void sfs_sanitize_path(char *dst, size_t dstsize, const char *src) {
  size_t j = 0, limit = (dstsize > 0) ? (dstsize - 1) : 0;
  for (size_t i = 0; src[i] != '\0' && j < limit; i++) {
    if (i > 0 && src[i] == '/' && src[i - 1] == '/') {
      continue;
    }
    dst[j++] = src[i];
  }
  if (dstsize > 0) {
    dst[j] = '\0';
  }
}

//! Checks if a path begins with SFS_BUILTIN_PREFIX
static inline bool sfs_has_prefix(const char *path) {
  size_t len = strlen(SFS_BUILTIN_PREFIX);
  return (strncmp(path, SFS_BUILTIN_PREFIX, len) == 0);
}

//! Looks up a path in the SFS and returns its data if found
//! Path is always sanitized before comparison
static bool sfs_lookup_path(const char *path, const unsigned char **found_start,
                            size_t *found_size) {
  if (!sfs_has_prefix(path)) {
    return false;
  }

  char sanitized[256];
  sfs_sanitize_path(sanitized, sizeof(sanitized), path);

  for (size_t i = 0; i < sfs_builtin_files_num; i++) {
    if (strcmp(sanitized, sfs_entries[i].abspath) == 0) {
      *found_start = sfs_entries[i].start;
      *found_size = (size_t)(sfs_entries[i].end - sfs_entries[i].start);
      return true;
    }
  }
  return false;
}

//! Finds the next free descriptor in [sfs_fd_start..FD_MAX_TRACK-1]
//! Forcibly exits if no free FDs are available
static int sfs_allocate_fd(void) {
  for (int fd = sfs_fd_start; fd < FD_MAX_TRACK; fd++) {
    if (!fd_is_in_use(fd)) {
      fd_mark_in_use(fd);
      return fd;
    }
  }
  __wasi_proc_exit(10);
  return -1;
}

//! Finds an SFS table entry by file descriptor
static SFS_Entry *sfs_find_by_fd(int fd) {
  for (int i = 0; i < SFS_MAX_OPEN_FILES; i++) {
    if (sfs_table[i].used && sfs_table[i].fd == fd) {
      return &sfs_table[i];
    }
  }
  return NULL;
}

//! Opens a path from SFS using fmemopen and allocates an FD
static int sfs_open(const char *path, FILE **outfp) {
  const unsigned char *start = NULL;
  size_t size = 0;

  if (!sfs_lookup_path(path, &start, &size)) {
    errno = ENOENT;
    if (outfp)
      *outfp = NULL;
    return -1;
  }

  FILE *fp = fmemopen((void *)start, size, "r");
  if (!fp) {
    if (outfp)
      *outfp = NULL;
    return -1;
  }

  for (int i = 0; i < SFS_MAX_OPEN_FILES; i++) {
    if (!sfs_table[i].used) {
      int newfd = sfs_allocate_fd();
      sfs_table[i].used = true;
      sfs_table[i].fd = newfd;
      sfs_table[i].fp = fp;
      sfs_table[i].size = size;
      if (outfp)
        *outfp = fp;
      return newfd;
    }
  }

  fclose(fp);
  errno = EMFILE;
  if (outfp)
    *outfp = NULL;
  return -1;
}

//! Closes an SFS file descriptor and frees its slot
static SFS_Result sfs_close(int fd) {
  SFS_Entry *e = sfs_find_by_fd(fd);
  if (!e) {
    return SFS_NOT_OURS;
  }
  if (!e->fp) {
    return SFS_ERR;
  }

  fclose(e->fp);
  e->fp = NULL;
  fd_mark_free(e->fd);
  e->used = false;
  e->fd = -1;
  e->size = 0;
  return SFS_OK;
}

//! Reads from in-memory data if FD is ours, returns -1 if not
__attribute__((noinline)) static ssize_t sfs_read(int fd, void *buf,
                                                  size_t count) {
  SFS_Entry *e = sfs_find_by_fd(fd);
  if (!e || !e->fp) {
    return -1;
  }
  return (ssize_t)fread(buf, 1, count, e->fp);
}

//! Performs fseek/ftell if FD is ours
static off_t sfs_lseek(int fd, off_t offset, int whence) {
  SFS_Entry *e = sfs_find_by_fd(fd);
  if (!e || !e->fp) {
    return (off_t)-1;
  }
  if (fseek(e->fp, (long)offset, whence) != 0) {
    return (off_t)-1;
  }
  long pos = ftell(e->fp);
  if (pos < 0) {
    return (off_t)-1;
  }
  return (off_t)pos;
}

//! Checks if a path exists in SFS - no fallback if path has our prefix
static int sfs_access(const char *path) {
  const unsigned char *start = NULL;
  size_t size = 0;
  if (sfs_lookup_path(path, &start, &size)) {
    return 0;
  }
  errno = ENOENT;
  return -1;
}

//! Path-based or FD-based stat operation
//! If path != NULL: path-based. If path == NULL: FD-based
static SFS_Stat_Result sfs_stat(const char *path, int fd, struct stat *stbuf) {
  if (path) {
    if (sfs_has_prefix(path)) {
      const unsigned char *start = NULL;
      size_t size = 0;
      if (!sfs_lookup_path(path, &start, &size)) {
        errno = ENOENT;
        return SFS_STAT_ERR;
      }
      memset(stbuf, 0, sizeof(*stbuf));
      stbuf->st_size = (off_t)size;
      stbuf->st_mode = S_IFREG;
      return SFS_STAT_OURS;
    }
    return SFS_STAT_NOT_OURS;
  } else {
    SFS_Entry *e = sfs_find_by_fd(fd);
    if (!e) {
      return SFS_STAT_NOT_OURS;
    }
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_size = (off_t)e->size;
    stbuf->st_mode = S_IFREG;
    return SFS_STAT_OURS;
  }
}

//! Wrapper for fopen: tries SFS first, then falls back to real fopen
__attribute__((noinline)) FILE *__wrap_fopen(const char *path,
                                             const char *mode) {
  if (sfs_has_prefix(path)) {
    FILE *fp = NULL;
    int sfd = sfs_open(path, &fp);
    if (sfd >= 0) {
      return fp;
    }
    return NULL;
  }

  FILE *realfp = __real_fopen(path, mode);
  if (realfp) {
    int realfd = fileno(realfp);
    if (realfd >= 0 && realfd < FD_MAX_TRACK) {
      fd_mark_in_use(realfd);
    }
  }
  return realfp;
}

//! Wrapper for open: tries SFS first, then falls back to real open
__attribute__((noinline)) int __wrap_open(const char *path, int flags, ...) {
  va_list args;
  va_start(args, flags);
  int mode = 0;
  if (flags & O_CREAT) {
    mode = va_arg(args, int);
  }
  va_end(args);

  if (sfs_has_prefix(path)) {
    int sfd = sfs_open(path, NULL);
    if (sfd >= 0) {
      return sfd;
    }
    return -1;
  }

  int realfd = __real_open(path, flags, mode);
  if (realfd >= 0 && realfd < FD_MAX_TRACK) {
    fd_mark_in_use(realfd);
  }
  return realfd;
}

//! Wrapper for close: tries SFS first, then falls back to real close
__attribute__((noinline)) int __wrap_close(int fd) {
  SFS_Result rc = sfs_close(fd);
  if (rc == SFS_OK) {
    return 0;
  }
  if (rc == SFS_NOT_OURS) {
    if (fd >= 0 && fd < FD_MAX_TRACK) {
      fd_mark_free(fd);
    }
    return __real_close(fd);
  }
  return (int)rc;
}

//! Wrapper for access: tries SFS first, then falls back to real access
__attribute__((noinline)) int __wrap_access(const char *path, int amode) {
  if (sfs_has_prefix(path)) {
    return sfs_access(path);
  }
  return __real_access(path, amode);
}

//! Wrapper for stat: tries SFS first, then falls back to real stat
__attribute__((noinline)) int __wrap_stat(const char *restrict path,
                                          struct stat *restrict stbuf) {
  SFS_Stat_Result rc = sfs_stat(path, -1, stbuf);
  if (rc == SFS_STAT_OURS) {
    return 0;
  }
  if (rc == SFS_STAT_ERR) {
    return -1;
  }
  return __real_stat(path, stbuf);
}

//! Wrapper for fstat: tries SFS first, then falls back to real fstat
__attribute__((noinline)) int __wrap_fstat(int fd, struct stat *stbuf) {
  SFS_Stat_Result rc = sfs_stat(NULL, fd, stbuf);
  if (rc == SFS_STAT_OURS) {
    return 0;
  }
  if (rc == SFS_STAT_ERR) {
    return -1;
  }
  return __real_fstat(fd, stbuf);
}

//! Wrapper for read: tries SFS first, then falls back to real read
__attribute__((noinline)) ssize_t __wrap_read(int fd, void *buf, size_t count) {
  ssize_t r = sfs_read(fd, buf, count);
  if (r >= 0) {
    return r;
  }
  return __real_read(fd, buf, count);
}

//! Wrapper for lseek: tries SFS first, then falls back to real lseek
__attribute__((noinline)) off_t __wrap_lseek(int fd, off_t offset, int whence) {
  off_t pos = sfs_lseek(fd, offset, whence);
  if (pos >= 0) {
    return pos;
  }
  return __real_lseek(fd, offset, whence);
}

//! Wrapper for fileno: checks SFS first, then falls back to real fileno
__attribute__((noinline)) int __wrap_fileno(FILE *stream) {
  for (int i = 0; i < SFS_MAX_OPEN_FILES; i++) {
    if (sfs_table[i].used && sfs_table[i].fp == stream) {
      return sfs_table[i].fd;
    }
  }

  int realfd = __real_fileno(stream);
  if (realfd >= 0 && realfd < FD_MAX_TRACK) {
    fd_mark_in_use(realfd);
  }
  return realfd;
}

//! Captures the current Perl error ($@) into the error buffer
//! Should be called after any Perl operation that might set $@
static void zeroperl_capture_error(void) {
  zero_perl_error_buf[0] = '\0';

  if (!zero_perl)
    return;

  dTHX;
  SV *errsv = get_sv("@", 0);
  if (errsv && SvTRUE(errsv)) {
    const char *err = SvPV_nolen(errsv);
    if (err) {
      strncpy(zero_perl_error_buf, err, sizeof(zero_perl_error_buf) - 1);
      zero_perl_error_buf[sizeof(zero_perl_error_buf) - 1] = '\0';
    }
  }
}

//! Clears the Perl error variable ($@)
static void zeroperl_clear_error_internal(void) {
  if (!zero_perl)
    return;

  dTHX;
  sv_setpvn(ERRSV, "", 0);
}

//! Context structure for initialization callbacks
typedef struct {
  int argc;
  char **argv;
  int result;
} zeroperl_init_context;

//! Context structure for evaluation callbacks
typedef struct {
  const char *code;
  int argc;
  char **argv;
  int result;
} zeroperl_eval_context;

//! Context structure for running files
typedef struct {
  const char *filepath;
  int argc;
  char **argv;
  int result;
} zeroperl_run_file_context;

//! Internal callback for initialization
//! Performs PERL_SYS_INIT3, perl_alloc, perl_construct, perl_parse, and
//! perl_run This follows the perlembed pattern for interactive interpreters
static int zeroperl_init_callback(int argc, char **argv) {
  (void)argc;
  zeroperl_init_context *ctx = (zeroperl_init_context *)argv;

  // One-time system initialization
  if (!zero_perl_system_initialized) {
    PERL_SYS_INIT3(&ctx->argc, &ctx->argv, &environ);
    PERL_SYS_FPU_INIT;
    zero_perl_system_initialized = true;
  }

  // Allocate and construct interpreter
  zero_perl = perl_alloc();
  if (!zero_perl) {
    ctx->result = 1;
    return 1;
  }

  perl_construct(zero_perl);

  // Set up interpreter flags
  PL_perl_destruct_level = 0;
  PL_exit_flags &= ~PERL_EXIT_DESTRUCT_END;

  // Parse the program (if provided) or minimal program for interactive use
  if (ctx->argc > 0 && ctx->argv) {
    if (perl_parse(zero_perl, xs_init, ctx->argc, ctx->argv, environ) != 0) {
      zeroperl_capture_error();
      ctx->result = 1;
      return 1;
    }
  } else {
    // No program provided - parse minimal empty program for interactive use
    // This is required before perl_run can be called
    char *minimal_argv[] = {"", "-e", "0", NULL};
    if (perl_parse(zero_perl, xs_init, 3, minimal_argv, environ) != 0) {
      zeroperl_capture_error();
      ctx->result = 1;
      return 1;
    }
  }

  // Run the interpreter - REQUIRED before eval_pv can be used (see perlembed
  // docs)
  int run_result = perl_run(zero_perl);
  if (run_result != 0) {
    zeroperl_capture_error();
    ctx->result = run_result;
    return run_result;
  }

  // Mark interpreter as ready for eval operations
  zero_perl_can_evaluate = true;
  ctx->result = 0;
  return 0;
}

//! Internal callback for evaluation
//! Uses eval_pv to evaluate a string of Perl code, optionally setting @ARGV
//! first
static int zeroperl_eval_callback(int argc, char **argv) {
  (void)argc;
  zeroperl_eval_context *ctx = (zeroperl_eval_context *)argv;

  if (!zero_perl || !zero_perl_can_evaluate) {
    ctx->result = -1;
    return -1;
  }

  zeroperl_clear_error_internal();

  dTHX;
  dSP;

  ENTER;
  SAVETMPS;

  // Set @ARGV if we have arguments
  if (ctx->argc > 0 && ctx->argv) {
    AV *argv_av = get_av("ARGV", GV_ADD);
    av_clear(argv_av);
    for (int i = 0; i < ctx->argc; i++) {
      av_push(argv_av, newSVpv(ctx->argv[i], 0));
    }
  }

  // Use eval_pv with FALSE for croak_on_error (we check ERRSV manually)
  SV *result = eval_pv(ctx->code, FALSE);

  if (SvTRUE(ERRSV)) {
    zeroperl_capture_error();
    ctx->result = -1;
  } else {
    ctx->result = 0;
  }

  FREETMPS;
  LEAVE;

  return ctx->result;
}

//! Internal callback for running a file
//! Uses Perl's do operator to load and execute the file
static int zeroperl_run_file_callback(int argc, char **argv) {
  (void)argc;
  zeroperl_run_file_context *ctx = (zeroperl_run_file_context *)argv;

  if (!zero_perl || !zero_perl_can_evaluate) {
    ctx->result = 1;
    return 1;
  }

  if (access(ctx->filepath, F_OK) != 0) {
    const char *err = "File not found";
    strncpy(zero_perl_error_buf, err, sizeof(zero_perl_error_buf) - 1);
    zero_perl_error_buf[sizeof(zero_perl_error_buf) - 1] = '\0';
    ctx->result = 1;
    return 1;
  }

  zeroperl_clear_error_internal();

  dTHX;
  dSP;

  ENTER;
  SAVETMPS;

  // Set @ARGV if we have arguments
  if (ctx->argc > 0 && ctx->argv) {
    AV *argv_av = get_av("ARGV", GV_ADD);
    av_clear(argv_av);
    for (int i = 0; i < ctx->argc; i++) {
      av_push(argv_av, newSVpv(ctx->argv[i], 0));
    }
  }

  // Build the do "file" expression
  // We use do instead of require because do re-executes every time
  char eval_code[512];
  snprintf(eval_code, sizeof(eval_code), "do '%s'", ctx->filepath);

  // Evaluate using eval_pv
  SV *result = eval_pv(eval_code, FALSE);

  if (SvTRUE(ERRSV)) {
    zeroperl_capture_error();
    ctx->result = -1;
  } else {
    ctx->result = 0;
  }

  FREETMPS;
  LEAVE;

  return ctx->result;
}

//! Internal callback for reset (destruct + construct + parse + run)
static int zeroperl_reset_callback(int argc, char **argv) {
  (void)argc;
  zeroperl_init_context *ctx = (zeroperl_init_context *)argv;

  if (!zero_perl) {
    ctx->result = -1;
    return -1;
  }

  // Destruct and reconstruct
  perl_destruct(zero_perl);
  perl_construct(zero_perl);

  PL_perl_destruct_level = 0;
  PL_exit_flags &= ~PERL_EXIT_DESTRUCT_END;
  zero_perl_can_evaluate = false;

  // Parse new program (or minimal program for interactive use)
  if (ctx->argc > 0 && ctx->argv) {
    if (perl_parse(zero_perl, xs_init, ctx->argc, ctx->argv, environ) != 0) {
      zeroperl_capture_error();
      ctx->result = 1;
      return 1;
    }
  } else {
    char *minimal_argv[] = {"", "-e", "0", NULL};
    if (perl_parse(zero_perl, xs_init, 3, minimal_argv, environ) != 0) {
      zeroperl_capture_error();
      ctx->result = 1;
      return 1;
    }
  }

  // Run the interpreter
  int run_result = perl_run(zero_perl);
  if (run_result != 0) {
    zeroperl_capture_error();
    ctx->result = run_result;
    return run_result;
  }

  zero_perl_can_evaluate = true;
  ctx->result = 0;
  return 0;
}

//! Initialize the Perl interpreter for interactive use (eval mode)
//!
//! This function performs complete Perl system initialization and creates
//! an interpreter ready for interactive evaluation of Perl code strings.
//! It follows the perlembed pattern:
//!   - PERL_SYS_INIT3 (one-time system setup)
//!   - perl_alloc
//!   - perl_construct
//!   - perl_parse (with minimal program)
//!   - perl_run (required before eval_pv can be used)
//!
//! After this function succeeds, call zeroperl_eval() to evaluate Perl code.
//!
//! @return 0 on success, non-zero on error
//!
//! Example:
//!   if (zeroperl_init() != 0) {
//!     fprintf(stderr, "Init failed: %s\n", zeroperl_last_error());
//!     return 1;
//!   }
//!   zeroperl_eval("print 'Hello, World!\\n'", 0, NULL);
//!   zeroperl_shutdown();
ZEROPERL_API("zeroperl_init")
int zeroperl_init(void) {
  if (zero_perl) {
    return 0;
  }

  zeroperl_init_context ctx = {.argc = 0, .argv = NULL, .result = 0};
  return asyncjmp_rt_start(zeroperl_init_callback, 0, (char **)&ctx);
}

//! Initialize the Perl interpreter with a program file
//!
//! This function is an alternative to zeroperl_init() for when you want to
//! run a complete Perl program from a file or with command-line arguments.
//! It performs the same system initialization but parses and runs the provided
//! program instead of a minimal interactive program.
//!
//! The program is executed during initialization, so this is typically used
//! for running scripts rather than interactive evaluation. You can still call
//! zeroperl_eval() afterwards for additional code evaluation if needed.
//!
//! @param argc Number of arguments (including program name at argv[0])
//! @param argv Argument array (argv[0] should be program name or path)
//! @return 0 on success, non-zero on error
//!
//! Example:
//!   char *args[] = { "myscript.pl", "arg1", NULL };
//!   if (zeroperl_init_with_args(2, args) != 0) {
//!     fprintf(stderr, "Failed: %s\n", zeroperl_last_error());
//!     return 1;
//!   }
//!   zeroperl_shutdown();
ZEROPERL_API("zeroperl_init_with_args")
int zeroperl_init_with_args(int argc, char **argv) {
  if (zero_perl) {
    return 0;
  }

  if (argc <= 0 || !argv) {
    return zeroperl_init();
  }

  zeroperl_init_context ctx = {.argc = argc, .argv = argv, .result = 0};
  return asyncjmp_rt_start(zeroperl_init_callback, 0, (char **)&ctx);
}

//! Evaluate a string of Perl code
//!
//! The interpreter must be initialized first via zeroperl_init() or
//! zeroperl_init_with_args(). Can be called repeatedly for multiple
//! evaluations.
//!
//! The code is evaluated using eval_pv() as documented in perlembed.
//! If the code produces an error, it will be captured in $@ and can
//! be retrieved via zeroperl_last_error().
//!
//! Optionally accepts command-line arguments that will be available in @ARGV.
//!
//! @param code Perl code to evaluate (must not be NULL)
//! @param argc Number of arguments to pass (0 for none)
//! @param argv Arguments array (can be NULL if argc is 0)
//! @return 0 on success, non-zero on error
//!
//! Example:
//!   // Simple eval
//!   zeroperl_eval("$x = 42; print $x", 0, NULL);
//!
//!   // Eval with arguments
//!   char *args[] = { "arg1", "arg2" };
//!   zeroperl_eval("print qq(Args: @ARGV\\n)", 2, args);
ZEROPERL_API("zeroperl_eval")
int zeroperl_eval(const char *code, int argc, char **argv) {
  if (!zero_perl || !zero_perl_can_evaluate) {
    return -1;
  }

  if (!code) {
    return -1;
  }

  zeroperl_eval_context ctx = {
      .code = code, .argc = argc, .argv = argv, .result = 0};
  return asyncjmp_rt_start(zeroperl_eval_callback, 0, (char **)&ctx);
}

//! Run a Perl program file
//!
//! Loads and executes a Perl script file using Perl's do operator.
//! This allows running multiple different files with the same interpreter
//! instance.
//!
//! @param filepath Path to the Perl script file
//! @param argc Number of arguments to pass to the script (for @ARGV)
//! @param argv Arguments array (can be NULL if argc is 0)
//! @return 0 on success, non-zero on error
//!
//! Example:
//!   char *args[] = { "arg1", "arg2" };
//!   zeroperl_run_file("/script.pl", 2, args);
ZEROPERL_API("zeroperl_run_file")
int zeroperl_run_file(const char *filepath, int argc, char **argv) {
  if (!zero_perl || !zero_perl_can_evaluate) {
    return 1;
  }

  if (!filepath) {
    return 1;
  }

  zeroperl_run_file_context ctx = {
      .filepath = filepath, .argc = argc, .argv = argv, .result = 0};

  return asyncjmp_rt_start(zeroperl_run_file_callback, 0, (char **)&ctx);
}

//! Free the Perl interpreter
//!
//! Destructs and frees the interpreter but leaves the Perl system initialized.
//! After this, you can call zeroperl_init() again for a fresh interpreter
//! without needing to re-initialize the entire Perl system (PERL_SYS_INIT3).
//!
//! This is faster than full shutdown+init when you need to restart the
//! interpreter multiple times.
ZEROPERL_API("zeroperl_free_interpreter")
void zeroperl_free_interpreter(void) {
  if (zero_perl) {
    perl_destruct(zero_perl);
    perl_free(zero_perl);
    zero_perl = NULL;
    zero_perl_can_evaluate = false;
  }
}

//! Complete system shutdown
//!
//! Frees the interpreter and performs full Perl system cleanup (PERL_SYS_TERM).
//! Should be called only once at program exit.
//!
//! After this, you must call zeroperl_init() to use Perl again, which will
//! perform full re-initialization including PERL_SYS_INIT3.
ZEROPERL_API("zeroperl_shutdown")
void zeroperl_shutdown(void) {
  zeroperl_free_interpreter();

  if (zero_perl_system_initialized) {
    PERL_SYS_TERM();
    zero_perl_system_initialized = false;
  }
}

//! Get a Perl scalar variable value as a string
//!
//! Returns NULL if the variable doesn't exist or interpreter not initialized.
//! The returned string is owned by Perl and should not be freed. It is valid
//! until the variable is modified or the interpreter is destroyed.
//!
//! @param name Variable name (without $, e.g., "myvar" not "$myvar")
//! @return Variable value or NULL
//!
//! Example:
//!   zeroperl_eval("$greeting = 'Hello'", 0, NULL);
//!   const char *value = zeroperl_get_sv("greeting");
//!   printf("%s\n", value);  // Prints: Hello
ZEROPERL_API("zeroperl_get_sv")
const char *zeroperl_get_sv(const char *name) {
  if (!zero_perl || !zero_perl_can_evaluate) {
    return NULL;
  }

  if (!name) {
    return NULL;
  }

  dTHX;
  SV *sv = get_sv(name, 0);
  if (!sv) {
    return NULL;
  }

  return SvPV_nolen(sv);
}

//! Set a Perl scalar variable from a string
//!
//! Creates the variable if it doesn't exist. The interpreter must be
//! initialized and ready.
//!
//! @param name Variable name (without $)
//! @param value Value to set (NULL will set to empty string)
//!
//! Example:
//!   zeroperl_set_sv("myvar", "test value");
//!   zeroperl_eval("print $myvar", 0, NULL);  // Prints: test value
ZEROPERL_API("zeroperl_set_sv")
void zeroperl_set_sv(const char *name, const char *value) {
  if (!zero_perl || !zero_perl_can_evaluate) {
    return;
  }

  if (!name) {
    return;
  }

  dTHX;
  SV *sv = get_sv(name, GV_ADD);
  if (sv) {
    sv_setpv(sv, value ? value : "");
  }
}

//! Get the last error message from Perl ($@)
//!
//! Returns an empty string if no error. The returned string is owned by
//! zeroperl and valid until the next error occurs or zeroperl_clear_error()
//! is called.
//!
//! @return Error message or empty string
//!
//! Example:
//!   if (zeroperl_eval("die 'oops'", 0, NULL) != 0) {
//!     fprintf(stderr, "Error: %s\n", zeroperl_last_error());
//!   }
ZEROPERL_API("zeroperl_last_error")
const char *zeroperl_last_error(void) { return zero_perl_error_buf; }

//! Clear the error state ($@)
//!
//! Clears both the internal error buffer and the Perl $@ variable.
//! This is useful when you want to ignore an error and continue.
ZEROPERL_API("zeroperl_clear_error")
void zeroperl_clear_error(void) {
  zero_perl_error_buf[0] = '\0';
  zeroperl_clear_error_internal();
}

//! Check if the interpreter is currently initialized
//!
//! @return True if initialized, false otherwise
ZEROPERL_API("zeroperl_is_initialized")
bool zeroperl_is_initialized(void) { return zero_perl != NULL; }

//! Check if the interpreter is ready to evaluate code
//!
//! Returns true if perl_run has been called and the interpreter
//! is ready to evaluate code strings via zeroperl_eval().
//!
//! @return True if ready for eval, false otherwise
ZEROPERL_API("zeroperl_can_evaluate")
bool zeroperl_can_evaluate(void) { return zero_perl_can_evaluate; }

//! Flush STDOUT and STDERR buffers
//!
//! Forces any buffered output to be written immediately to the underlying
//! file descriptors. This is useful when autoflush is not enabled but you
//! need to ensure output is written at specific points (e.g., before a
//! long-running operation or critical log message).
//!
//! This is equivalent to calling $fh->flush in Perl or the flush() method
//! on filehandles, but applies to both STDOUT and STDERR.
//!
//! @return 0 on success, -1 if interpreter not initialized
//!
//! Example:
//!   zeroperl_eval("print 'Processing...'", 0, NULL);
//!   zeroperl_flush();  // Ensure message is visible before long operation
//!   // ... do something that takes a long time ...
//!   zeroperl_eval("print ' done!\\n'", 0, NULL);
ZEROPERL_API("zeroperl_flush")
int zeroperl_flush(void) {
  if (!zero_perl || !zero_perl_can_evaluate) {
    return -1;
  }

  dTHX;

  // Flush STDOUT
  PerlIO *pout = PerlIO_stdout();
  if (pout) {
    if (PerlIO_flush(pout) != 0) {
      return -1;
    }
  }

  // Flush STDERR
  PerlIO *perr = PerlIO_stderr();
  if (perr) {
    if (PerlIO_flush(perr) != 0) {
      return -1;
    }
  }

  return 0;
}

//! Reset the interpreter to a clean state
//!
//! Destructs and reconstructs the interpreter, clearing all Perl state.
//! After reset, the interpreter is ready for eval() calls.
//!
//! @return 0 on success, non-zero on error
//!
//! Example:
//!   zeroperl_eval("$x = 42", 0, NULL);
//!   zeroperl_reset();
//!   zeroperl_eval("print $x", 0, NULL); // $x is undefined now
ZEROPERL_API("zeroperl_reset")
int zeroperl_reset(void) {
  if (!zero_perl) {
    const char *err = "Interpreter not initialized";
    strncpy(zero_perl_error_buf, err, sizeof(zero_perl_error_buf) - 1);
    zero_perl_error_buf[sizeof(zero_perl_error_buf) - 1] = '\0';
    return -1;
  }

  zeroperl_clear_error();

  zeroperl_init_context ctx = {.argc = 0, .argv = NULL, .result = 0};
  return asyncjmp_rt_start(zeroperl_reset_callback, 0, (char **)&ctx);
}

//! XS bootstrap declarations for statically linked modules
EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_File__DosGlob(pTHX_ CV *cv);
EXTERN_C void boot_File__Glob(pTHX_ CV *cv);
EXTERN_C void boot_Sys__Hostname(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__via(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__mmap(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__encoding(pTHX_ CV *cv);
EXTERN_C void boot_attributes(pTHX_ CV *cv);
EXTERN_C void boot_Unicode__Normalize(pTHX_ CV *cv);
EXTERN_C void boot_Unicode__Collate(pTHX_ CV *cv);
EXTERN_C void boot_re(pTHX_ CV *cv);
EXTERN_C void boot_Digest__MD5(pTHX_ CV *cv);
EXTERN_C void boot_Digest__SHA(pTHX_ CV *cv);
EXTERN_C void boot_Math__BigInt__FastCalc(pTHX_ CV *cv);
EXTERN_C void boot_Data__Dumper(pTHX_ CV *cv);
EXTERN_C void boot_I18N__Langinfo(pTHX_ CV *cv);
EXTERN_C void boot_Time__Piece(pTHX_ CV *cv);
EXTERN_C void boot_IO(pTHX_ CV *cv);
EXTERN_C void boot_Hash__Util__FieldHash(pTHX_ CV *cv);
EXTERN_C void boot_Hash__Util(pTHX_ CV *cv);
EXTERN_C void boot_Filter__Util__Call(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Unicode(pTHX_ CV *cv);
EXTERN_C void boot_Encode(pTHX_ CV *cv);
EXTERN_C void boot_Encode__JP(pTHX_ CV *cv);
EXTERN_C void boot_Encode__KR(pTHX_ CV *cv);
EXTERN_C void boot_Encode__EBCDIC(pTHX_ CV *cv);
EXTERN_C void boot_Encode__CN(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Symbol(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Byte(pTHX_ CV *cv);
EXTERN_C void boot_Encode__TW(pTHX_ CV *cv);
EXTERN_C void boot_Compress__Raw__Zlib(pTHX_ CV *cv);
EXTERN_C void boot_Compress__Raw__Bzip2(pTHX_ CV *cv);
EXTERN_C void boot_MIME__Base64(pTHX_ CV *cv);
EXTERN_C void boot_Cwd(pTHX_ CV *cv);
EXTERN_C void boot_List__Util(pTHX_ CV *cv);
EXTERN_C void boot_Fcntl(pTHX_ CV *cv);
EXTERN_C void boot_Opcode(pTHX_ CV *cv);

//! Initialize XS extensions
//! Registers all statically linked XS modules with the Perl interpreter.
//! This function is called automatically during perl_parse() via the xs_init
//! parameter.
static void xs_init(pTHX) {
  static const char file[] = __FILE__;
  dXSUB_SYS;
  PERL_UNUSED_CONTEXT;

  newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
  newXS("File::DosGlob::bootstrap", boot_File__DosGlob, file);
  newXS("File::Glob::bootstrap", boot_File__Glob, file);
  newXS("Sys::Hostname::bootstrap", boot_Sys__Hostname, file);
  newXS("PerlIO::via::bootstrap", boot_PerlIO__via, file);
  newXS("PerlIO::mmap::bootstrap", boot_PerlIO__mmap, file);
  newXS("PerlIO::encoding::bootstrap", boot_PerlIO__encoding, file);
  newXS("attributes::bootstrap", boot_attributes, file);
  newXS("Unicode::Normalize::bootstrap", boot_Unicode__Normalize, file);
  newXS("Unicode::Collate::bootstrap", boot_Unicode__Collate, file);
  newXS("re::bootstrap", boot_re, file);
  newXS("Digest::MD5::bootstrap", boot_Digest__MD5, file);
  newXS("Digest::SHA::bootstrap", boot_Digest__SHA, file);
  newXS("Math::BigInt::FastCalc::bootstrap", boot_Math__BigInt__FastCalc, file);
  newXS("Data::Dumper::bootstrap", boot_Data__Dumper, file);
  newXS("I18N::Langinfo::bootstrap", boot_I18N__Langinfo, file);
  newXS("Time::Piece::bootstrap", boot_Time__Piece, file);
  newXS("IO::bootstrap", boot_IO, file);
  newXS("Hash::Util::FieldHash::bootstrap", boot_Hash__Util__FieldHash, file);
  newXS("Hash::Util::bootstrap", boot_Hash__Util, file);
  newXS("Filter::Util::Call::bootstrap", boot_Filter__Util__Call, file);
  newXS("Encode::Unicode::bootstrap", boot_Encode__Unicode, file);
  newXS("Encode::bootstrap", boot_Encode, file);
  newXS("Encode::JP::bootstrap", boot_Encode__JP, file);
  newXS("Encode::KR::bootstrap", boot_Encode__KR, file);
  newXS("Encode::EBCDIC::bootstrap", boot_Encode__EBCDIC, file);
  newXS("Encode::CN::bootstrap", boot_Encode__CN, file);
  newXS("Encode::Symbol::bootstrap", boot_Encode__Symbol, file);
  newXS("Encode::Byte::bootstrap", boot_Encode__Byte, file);
  newXS("Encode::TW::bootstrap", boot_Encode__TW, file);
  newXS("Compress::Raw::Zlib::bootstrap", boot_Compress__Raw__Zlib, file);
  newXS("Compress::Raw::Bzip2::bootstrap", boot_Compress__Raw__Bzip2, file);
  newXS("MIME::Base64::bootstrap", boot_MIME__Base64, file);
  newXS("Cwd::bootstrap", boot_Cwd, file);
  newXS("List::Util::bootstrap", boot_List__Util, file);
  newXS("Fcntl::bootstrap", boot_Fcntl, file);
  newXS("Opcode::bootstrap", boot_Opcode, file);
}