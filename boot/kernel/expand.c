#include <config.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/types.h>
#include "system.h"
#include "die.h"
#include "xstrndup.h"
#include "expand-common.h"
static char const shortopts[] = "it:0::1::2::3::4::5::6::7::8::9::";
static struct option const longopts[] =
{
  {"tabs", required_argument, NULL, 't'},
  {"initial", no_argument, NULL, 'i'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION]... [FILE]...\n\
"),
              program_name);
      fputs (_("\
Convert tabs in each FILE to spaces, writing to standard output.\n\
"), stdout);

      emit_stdin_note ();
      emit_mandatory_arg_note ();

      fputs (_("\
  -i, --initial    do not convert tabs after non blanks\n\
  -t, --tabs=N     have tabs N characters apart, not 8\n\
"), stdout);
      emit_tab_list_info ();
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

static void
expand (void)
{
  FILE *fp = next_file (NULL);

  if (!fp)
    return;

  while (true)
    {
      int c;
      bool convert = true;
      uintmax_t column = 0;
      size_t tab_index = 0;

      do
        {
          while ((c = getc (fp)) < 0 && (fp = next_file (fp)))
            continue;

          if (convert)
            {
              if (c == '\t')
                {
                  /* Column the next input tab stop is on.  */
                  uintmax_t next_tab_column;
                  bool last_tab IF_LINT (=0);

                  next_tab_column = get_next_tab_column (column, &tab_index,
                                                         &last_tab);

                  if (last_tab)
                    next_tab_column = column + 1;

                  if (next_tab_column < column)
                    die (EXIT_FAILURE, 0, _("input line is too long"));

                  while (++column < next_tab_column)
                    if (putchar (' ') < 0)
                      die (EXIT_FAILURE, errno, _("write error"));

                  c = ' ';
                }
              else if (c == '\b')
                {
                  column -= !!column;
                  tab_index -= !!tab_index;
                }
              else
                {
                  column++;
                  if (!column)
                    die (EXIT_FAILURE, 0, _("input line is too long"));
                }

              convert &= convert_entire_line || !! isblank (c);
            }

          if (c < 0)
            return;

          if (putchar (c) < 0)
            die (EXIT_FAILURE, errno, _("write error"));
        }
      while (c != '\n');
    }
}

int
main (int argc, char **argv)
{
  int c;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);
  convert_entire_line = true;

  while ((c = getopt_long (argc, argv, shortopts, longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'i':
          convert_entire_line = false;
          break;

        case 't':
          parse_tab_stops (optarg);
          break;

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
          if (optarg)
            parse_tab_stops (optarg - 1);
          else
            {
              char tab_stop[2];
              tab_stop[0] = c;
              tab_stop[1] = '\0';
              parse_tab_stops (tab_stop);
            }
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  finalize_tab_stops ();

  set_file_list ( (optind < argc) ? &argv[optind] : NULL);

  expand ();

  cleanup_file_list_stdin ();

  return exit_status;
}
