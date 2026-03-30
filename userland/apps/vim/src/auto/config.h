/*
 * auto/config.h - Stellux-specific Vim configuration
 *
 * Generated for cross-compilation to Stellux OS with musl libc.
 * Tiny feature set, no GUI, no shell commands, no ncurses.
 */

/* Feature level */
#define FEAT_TINY 1

/* Basic OS */
#define UNIX 1
#define HAVE_DATE_TIME 1
#define HAVE_ATTRIBUTE_UNUSED 1

/* Data type sizes */
#define VIM_SIZEOF_INT 4
#define VIM_SIZEOF_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_TIME_T 8

/* Memory */
#define USEMEMMOVE 1

/* Terminal - use builtin termcap, no ncurses/terminfo */
/* #undef TERMINFO */
/* #undef HAVE_OSPEED */
/* #undef HAVE_UP_BC_PC */
/* #undef HAVE_DEL_CURTERM */
/* #undef HAVE_TERMCAP_H */
#define TGETENT_ZERO_ERR 0

/* File operations available in Stellux */
#define HAVE_FCHDIR 1
/* #undef HAVE_FCHOWN */
/* #undef HAVE_FCHMOD */
#define HAVE_FSEEKO 1
#define HAVE_FSYNC 1
#define HAVE_FTRUNCATE 1
#define HAVE_GETCWD 1
/* #undef HAVE_GETPGID */
/* #undef HAVE_GETPWENT */
/* #undef HAVE_GETPWNAM */
/* #undef HAVE_GETPWUID */
/* #undef HAVE_GETRLIMIT */
#define HAVE_GETTIMEOFDAY 1
/* #undef HAVE_ICONV */
/* #undef HAVE_LOCALTIME_R */
/* #undef HAVE_LSTAT */
#define HAVE_MEMSET 1
/* #undef HAVE_MKDTEMP */
#define HAVE_NANOSLEEP 1
/* #undef HAVE_NL_LANGINFO_CODESET */
#define HAVE_OPENDIR 1
/* #undef HAVE_POSIX_OPENPT */
#define HAVE_PUTENV 1
#define HAVE_QSORT 1
/* #undef HAVE_READLINK */
/* #undef HAVE_RENAME */
#define HAVE_SELECT 1
/* #undef HAVE_SELINUX */
#define HAVE_SETENV 1
/* #undef HAVE_SETPGID */
/* #undef HAVE_SETSID */
#define HAVE_SIGACTION 1
/* #undef HAVE_SIGALTSTACK */
/* #undef HAVE_SIGSET */
/* #undef HAVE_SIGSTACK */
#define HAVE_SIGPROCMASK 1
#define HAVE_STRCASECMP 1
/* #undef HAVE_STRCOLL */
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRTOL 1
#define HAVE_TOWLOWER 1
#define HAVE_TOWUPPER 1
#define HAVE_ISWUPPER 1
/* #undef HAVE_TZSET */
/* #undef HAVE_UNSETENV */
#define HAVE_USLEEP 1
/* #undef HAVE_UTIME */
#define HAVE_MBLEN 1
/* #undef HAVE_TIMER_CREATE */
#define HAVE_CLOCK_GETTIME 1
/* #undef HAVE_XATTR */
/* #undef HAVE_UTIMES */

/* Headers available via musl */
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
/* #undef HAVE_ICONV_H */
#define HAVE_INTTYPES_H 1
/* #undef HAVE_LANGINFO_H */
/* #undef HAVE_LIBGEN_H */
/* #undef HAVE_LIBINTL_H */
#define HAVE_LOCALE_H 1
#define HAVE_MATH_H 1
#define HAVE_POLL_H 1
/* #undef HAVE_PWD_H */
#define HAVE_SETJMP_H 1
/* #undef HAVE_SGTTY_H */
#define HAVE_STDINT_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_IOCTL_H 1
/* #undef HAVE_SYS_PARAM_H */
#define HAVE_SYS_POLL_H 1
/* #undef HAVE_SYS_RESOURCE_H */
#define HAVE_SYS_SELECT_H 1
/* #undef HAVE_SYS_STATFS_H */
/* #undef HAVE_SYS_SYSINFO_H */
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
/* #undef HAVE_SYS_UTSNAME_H */
#define HAVE_TERMIOS_H 1
/* #undef HAVE_TERMIO_H */
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_UNISTD_H 1
/* #undef HAVE_UTIME_H */
#define HAVE_SYS_WAIT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

/* PTY - we have our own custom PTY, not SVR4/BSD */
/* #undef HAVE_SVR4_PTYS */

/* Signals */
/* #undef HAVE_SIGCONTEXT */

/* select() */
#define SYS_SELECT_WITH_SYS_TIME 1
#define SELECT_TYPE_ARG234 (fd_set *)

/* Disabled features */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_DLOPEN */
/* #undef HAVE_DLSYM */
/* #undef USE_XSMP_INTERACT */
/* #undef HAVE_FD_CLOEXEC */
/* #undef HAVE_ISINF */
/* #undef HAVE_ISNAN */
/* #undef HAVE_DIRFD */
/* #undef HAVE_FLOCK */
/* #undef HAVE_SYSCONF_SIGSTKSZ */

/* No process stuff */
/* #undef HAVE_FORK */
/* #undef HAVE_WAIT */

/* Feature flags */
/* #undef FEAT_NORMAL */
/* #undef FEAT_HUGE */
/* #undef FEAT_LUA */
/* #undef FEAT_MZSCHEME */
/* #undef FEAT_PERL */
/* #undef FEAT_PYTHON */
/* #undef FEAT_PYTHON3 */
/* #undef FEAT_RUBY */
/* #undef FEAT_TCL */
/* #undef FEAT_CSCOPE */
/* #undef FEAT_AUTOSERVERNAME */
/* #undef FEAT_XFONTSET */
/* #undef FEAT_XIM */
/* #undef FEAT_GUI_GNOME */
/* #undef FEAT_IPV6 */
/* #undef FEAT_NETBEANS_INTG */
/* #undef FEAT_JOB_CHANNEL */
/* #undef FEAT_TERMINAL */
/* #undef FEAT_SOUND */
