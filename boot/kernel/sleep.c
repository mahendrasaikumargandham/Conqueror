#include <config.h>
#include <stdio.h>
#include <sys/types.h>
#include "system.h"
#include "cl-strtod.h"
#include "die.h"
#include "error.h"
#include "long-options.h"
#include "quote.h"
#include "xnanosleep.h"
#include "xstrtod.h"
void usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s NUMBER[SUFFIX]...\n\
  or:  %s OPTION\n\
Pause for NUMBER seconds.  SUFFIX may be 's' for seconds (the default),\n\
'm' for minutes, 'h' for hours or 'd' for days.  NUMBER need not be an\n\
integer.  Given two or more arguments, pause for the amount of time\n\
specified by the sum of their values.\n\
\n\
"),
              program_name, program_name);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

static bool apply_suffix (double *x, char suffix_char)
{
  int multiplier;

  switch (suffix_char)
    {
    case 0:
    case 's':
      multiplier = 1;
      break;
    case 'm':
      multiplier = 60;
      break;
    case 'h':
      multiplier = 60 * 60;
      break;
    case 'd':
      multiplier = 60 * 60 * 24;
      break;
    default:
      return false;
    }

  *x *= multiplier;

  return true;
}

int main (int argc, char **argv)
{
  double seconds = 0.0;
  bool ok = true;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  atexit (close_stdout);
  parse_gnu_standard_options_only (argc, argv, Version, true, usage, (char const *) NULL);
  if (argc == 1)
    {
      error (0, 0, _("missing operand"));
      usage (EXIT_FAILURE);
    }

  for (int i = optind; i < argc; i++)
    {
      double s;
      char const *p;
      if (! (xstrtod (argv[i], &p, &s, cl_strtod) || errno == ERANGE) || ! (0 <= s) || (*p && *(p+1)) || ! apply_suffix (&s, *p))
        {
          error (0, 0, _("invalid time interval %s"), quote (argv[i]));
          ok = false;
        }

      seconds += s;
    }

  if (!ok)
    usage (EXIT_FAILURE);
  if (xnanosleep (seconds))
    die (EXIT_FAILURE, errno, _("cannot read realtime clock"));
  return EXIT_SUCCESS;
}
