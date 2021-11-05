#include <config.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/types.h>
#include "system.h"
#include "die.h"
#include "error.h"
#include "fadvise.h"
#include "xdectoint.h"
#define TAB_WIDTH 8
static bool break_spaces;
static bool count_bytes;
static bool have_read_stdin;

static char const shortopts[] = "bsw:0::1::2::3::4::5::6::7::8::9::";

static struct option const longopts[] =
{
  {"bytes", no_argument, NULL, 'b'},
  {"spaces", no_argument, NULL, 's'},
  {"width", required_argument, NULL, 'w'},
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
Wrap input lines in each FILE, writing to standard output.\n\
"), stdout);

      emit_stdin_note ();
      emit_mandatory_arg_note ();

      fputs (_("\
  -b, --bytes         count bytes rather than columns\n\
  -s, --spaces        break at spaces\n\
  -w, --width=WIDTH   use WIDTH columns instead of 80\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}


static size_t
adjust_column (size_t column, char c)
{
  if (!count_bytes)
    {
      if (c == '\b')
        {
          if (column > 0)
            column--;
        }
      else if (c == '\r')
        column = 0;
      else if (c == '\t')
        column += TAB_WIDTH - column % TAB_WIDTH;
      else /* if (isprint (c)) */
        column++;
    }
  else
    column++;
  return column;
}
static bool
fold_file (char const *filename, size_t width)
{
  FILE *istream;
  int c;
  size_t column = 0;		
  size_t offset_out = 0;
  static char *line_out = NULL;
  static size_t allocated_out = 0;
  int saved_errno;

  if (STREQ (filename, "-"))
    {
      istream = stdin;
      have_read_stdin = true;
    }
  else
    istream = fopen (filename, "r");

  if (istream == NULL)
    {
      error (0, errno, "%s", quotef (filename));
      return false;
    }

  fadvise (istream, FADVISE_SEQUENTIAL);

  while ((c = getc (istream)) != EOF)
    {
      if (offset_out + 1 >= allocated_out)
        line_out = X2REALLOC (line_out, &allocated_out);

      if (c == '\n')
        {
          line_out[offset_out++] = c;
          fwrite (line_out, sizeof (char), offset_out, stdout);
          column = offset_out = 0;
          continue;
        }

    rescan:
      column = adjust_column (column, c);

      if (column > width)
        {
          if (break_spaces)
            {
              bool found_blank = false;
              size_t logical_end = offset_out;

              /* Look for the last blank. */
              while (logical_end)
                {
                  --logical_end;
                  if (isblank (to_uchar (line_out[logical_end])))
                    {
                      found_blank = true;
                      break;
                    }
                }

              if (found_blank)
                {
                  size_t i;
                  logical_end++;
                  fwrite (line_out, sizeof (char), (size_t) logical_end,
                          stdout);
                  putchar ('\n');
                  memmove (line_out, line_out + logical_end,
                           offset_out - logical_end);
                  offset_out -= logical_end;
                  for (column = i = 0; i < offset_out; i++)
                    column = adjust_column (column, line_out[i]);
                  goto rescan;
                }
            }

          if (offset_out == 0)
            {
              line_out[offset_out++] = c;
              continue;
            }

          line_out[offset_out++] = '\n';
          fwrite (line_out, sizeof (char), (size_t) offset_out, stdout);
          column = offset_out = 0;
          goto rescan;
        }

      line_out[offset_out++] = c;
    }

  saved_errno = errno;

  if (offset_out)
    fwrite (line_out, sizeof (char), (size_t) offset_out, stdout);

  if (ferror (istream))
    {
      error (0, saved_errno, "%s", quotef (filename));
      if (!STREQ (filename, "-"))
        fclose (istream);
      return false;
    }
  if (!STREQ (filename, "-") && fclose (istream) == EOF)
    {
      error (0, errno, "%s", quotef (filename));
      return false;
    }

  return true;
}

int
main (int argc, char **argv)
{
  size_t width = 80;
  int i;
  int optc;
  bool ok;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  break_spaces = count_bytes = have_read_stdin = false;

  while ((optc = getopt_long (argc, argv, shortopts, longopts, NULL)) != -1)
    {
      char optargbuf[2];

      switch (optc)
        {
        case 'b':		/* Count bytes rather than columns. */
          count_bytes = true;
          break;

        case 's':		/* Break at word boundaries. */
          break_spaces = true;
          break;

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
          if (optarg)
            optarg--;
          else
            {
              optargbuf[0] = optc;
              optargbuf[1] = '\0';
              optarg = optargbuf;
            }
          FALLTHROUGH;
        case 'w':		/* Line width. */
          width = xdectoumax (optarg, 1, SIZE_MAX - TAB_WIDTH - 1, "",
                              _("invalid number of columns"), 0);
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (argc == optind)
    ok = fold_file ("-", width);
  else
    {
      ok = true;
      for (i = optind; i < argc; i++)
        ok &= fold_file (argv[i], width);
    }

  if (have_read_stdin && fclose (stdin) == EOF)
    die (EXIT_FAILURE, errno, "-");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
