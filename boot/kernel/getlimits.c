#include <config.h>        
#include <stdio.h>
#include <sys/types.h>
#include <float.h>

#include "ftoastr.h"
#include "system.h"
#include "long-options.h"

#define PROGRAM_NAME "getlimits"

#define AUTHORS proper_name ("Padraig Brady")

#ifndef TIME_T_MAX
# define TIME_T_MAX TYPE_MAXIMUM (time_t)
#endif

#ifndef TIME_T_MIN
# define TIME_T_MIN TYPE_MINIMUM (time_t)
#endif

#ifndef SSIZE_MIN
# define SSIZE_MIN TYPE_MINIMUM (ssize_t)
#endif

#ifndef PID_T_MIN
# define PID_T_MIN TYPE_MINIMUM (pid_t)
#endif
#define SIZE_MIN 0
#define UCHAR_MIN 0
#define UINT_MIN 0
#define ULONG_MIN 0
#define UINTMAX_MIN 0
#define UID_T_MIN 0
#define GID_T_MIN 0

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s\n\
"), program_name);

      fputs (_("\
Output platform dependent limits in a format useful for shell scripts.\n\
\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

static char const *
decimal_absval_add_one (char *buf)
{
  bool negative = (buf[1] == '-');
  char *absnum = buf + 1 + negative;
  char *p = absnum + strlen (absnum);
  absnum[-1] = '0';
  while (*--p == '9')
    *p = '0';
  ++*p;
  char *result = MIN (absnum, p);
  if (negative)
    *--result = '-';
  return result;
}

#define PRINT_FLOATTYPE(N, T, FTOASTR, BUFSIZE)                         \
static void                                                             \
N (T x)                                                                 \
{                                                                       \
  char buf[BUFSIZE];                                                    \
  FTOASTR (buf, sizeof buf, FTOASTR_LEFT_JUSTIFY, 0, x);                \
  puts (buf);                                                           \
}

PRINT_FLOATTYPE (print_FLT, float, ftoastr, FLT_BUFSIZE_BOUND)
PRINT_FLOATTYPE (print_DBL, double, dtoastr, DBL_BUFSIZE_BOUND)
PRINT_FLOATTYPE (print_LDBL, long double, ldtoastr, LDBL_BUFSIZE_BOUND)

int
main (int argc, char **argv)
{
  char limit[1 + MAX (INT_BUFSIZE_BOUND (intmax_t),
                      INT_BUFSIZE_BOUND (uintmax_t))];

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  initialize_exit_failure (EXIT_FAILURE);
  atexit (close_stdout);

  parse_long_options (argc, argv, PROGRAM_NAME, PACKAGE_NAME, VERSION,
                      usage, AUTHORS, (char const *) NULL);

#define print_int(TYPE)                                                  \
  sprintf (limit + 1, "%"PRIuMAX, (uintmax_t) TYPE##_MAX);               \
  printf (#TYPE"_MAX=%s\n", limit + 1);                                  \
  printf (#TYPE"_OFLOW=%s\n", decimal_absval_add_one (limit));           \
  if (TYPE##_MIN)                                                        \
    {                                                                    \
      sprintf (limit + 1, "%"PRIdMAX, (intmax_t) TYPE##_MIN);            \
      printf (#TYPE"_MIN=%s\n", limit + 1);                              \
      printf (#TYPE"_UFLOW=%s\n", decimal_absval_add_one (limit));       \
    }

#define print_float(TYPE)                                                \
  printf (#TYPE"_MIN="); print_##TYPE (TYPE##_MIN);                      \
  printf (#TYPE"_MAX="); print_##TYPE (TYPE##_MAX);

  /* Variable sized ints */
  print_int (CHAR);
  print_int (SCHAR);
  print_int (UCHAR);
  print_int (SHRT);
  print_int (INT);
  print_int (UINT);
  print_int (LONG);
  print_int (ULONG);
  print_int (SIZE);
  print_int (SSIZE);
  print_int (TIME_T);
  print_int (UID_T);
  print_int (GID_T);
  print_int (PID_T);
  print_int (OFF_T);
  print_int (INTMAX);
  print_int (UINTMAX);
  print_float (FLT);
  print_float (DBL);
  print_float (LDBL);

  return EXIT_SUCCESS;
}
