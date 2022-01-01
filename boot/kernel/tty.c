#include <config.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/types.h>
#include "system.h"
#include "error.h"
#include "quote.h"
enum
  {
    TTY_STDIN_NOTTY = 1,
    TTY_FAILURE = 2,
    TTY_WRITE_ERROR = 3
  };
static bool silent;
static struct option const longopts[] =
{
  {"silent", no_argument, NULL, 's'},
  {"quiet", no_argument, NULL, 's'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

void usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("Usage: %s [OPTION]...\n"), program_name);
      fputs (_("\
Print the file name of the terminal connected to standard input.\n\
\n\
  -s, --silent, --quiet   print nothing, only return an exit status\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int main (int argc, char **argv)
{
  int optc;
  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  initialize_exit_failure (TTY_WRITE_ERROR);
  atexit (close_stdout);
  silent = false;
  while ((optc = getopt_long (argc, argv, "s", longopts, NULL)) != -1)
    {
      switch (optc)
        {
        case 's':
          silent = true;
          break;
        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
        default:
          usage (TTY_FAILURE);
        }
    }

  if (optind < argc)
    {
      error (0, 0, _("extra operand %s"), quote (argv[optind]));
      usage (TTY_FAILURE);
    }

  errno = ENOENT;

  if (silent)
    return isatty (STDIN_FILENO) ? EXIT_SUCCESS : TTY_STDIN_NOTTY;
  int status = EXIT_SUCCESS;
  char const *tty = ttyname (STDIN_FILENO);

  if (! tty)
    {
      tty = _("not a tty");
      status = TTY_STDIN_NOTTY;
    }
  puts (tty);
  return status;
}
