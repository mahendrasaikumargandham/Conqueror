#include <config.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/types.h>
#include "system.h"
#include "error.h"
#include "quote.h"

static struct option const longopts[] =
{
  {"multiple", no_argument, NULL, 'a'},
  {"suffix", required_argument, NULL, 's'},
  {"zero", no_argument, NULL, 'z'},
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
      printf (_("\
Usage: %s NAME [SUFFIX]\n\
  or:  %s OPTION... NAME...\n\
"),
              program_name, program_name);
      fputs (_("\
Print NAME with any leading directory components removed.\n\
If specified, also remove a trailing SUFFIX.\n\
"), stdout);
      emit_mandatory_arg_note ();
      fputs (_("\
  -a, --multiple       support multiple arguments and treat each as a NAME\n\
  -s, --suffix=SUFFIX  remove a trailing SUFFIX; implies -a\n\
  -z, --zero           end each output line with NUL, not newline\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      printf (_("\
\n\
Examples:\n\
  %s /usr/bin/sort          -> \"sort\"\n\
  %s include/stdio.h .h     -> \"stdio\"\n\
  %s -s .h include/stdio.h  -> \"stdio\"\n\
  %s -a any/str1 any/str2   -> \"str1\" followed by \"str2\"\n\
"),
              program_name, program_name, program_name, program_name);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}
static void remove_suffix (char *name, char const *suffix)
{
  char *np;
  char const *sp;
  np = name + strlen (name);
  sp = suffix + strlen (suffix);
  while (np > name && sp > suffix)
    if (*--np != *--sp)
      return;
  if (np > name)
    *np = '\0';
}
static void perform_basename (char const *string, char const *suffix, bool use_nuls)
{
  char *name = base_name (string);
  strip_trailing_slashes (name);
  if (suffix && IS_RELATIVE_FILE_NAME (name) && ! FILE_SYSTEM_PREFIX_LEN (name))
    remove_suffix (name, suffix);
  fputs (name, stdout);
  putchar (use_nuls ? '\0' : '\n');
  free (name);
}

int main (int argc, char **argv)
{
  bool multiple_names = false;
  bool use_nuls = false;
  char const *suffix = NULL;
  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  atexit (close_stdout);
  while (true)
    {
      int c = getopt_long (argc, argv, "+as:z", longopts, NULL);
      if (c == -1)
        break;
      switch (c)
        {
        case 's':
          suffix = optarg;
          FALLTHROUGH;
        case 'a':
          multiple_names = true;
          break;
        case 'z':
          use_nuls = true;
          break;
        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
        default:
          usage (EXIT_FAILURE);
        }
    }
  if (argc < optind + 1)
    {
      error (0, 0, _("missing operand"));
      usage (EXIT_FAILURE);
    }
  if (!multiple_names && optind + 2 < argc)
    {
      error (0, 0, _("extra operand %s"), quote (argv[optind + 2]));
      usage (EXIT_FAILURE);
    }
  if (multiple_names)
    {
      for (; optind < argc; optind++)
        perform_basename (argv[optind], suffix, use_nuls);
    }
  else
    perform_basename (argv[optind], optind + 2 == argc ? argv[optind + 1] : NULL, use_nuls);
  return EXIT_SUCCESS;
}