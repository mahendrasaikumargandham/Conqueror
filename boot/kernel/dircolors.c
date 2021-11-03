#include <config.h>
#include <sys/types.h>
#include <fnmatch.h>
#include <getopt.h>
#include "system.h"
#include "dircolors.h"
#include "c-strcase.h"
#include "die.h"
#include "error.h"
#include "obstack.h"
#include "quote.h"
#include "stdio--.h"
#include "xstrndup.h"
#define PROGRAM_NAME "dircolors"
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free

enum Shell_syntax
{
  SHELL_SYNTAX_BOURNE,
  SHELL_SYNTAX_C,
  SHELL_SYNTAX_UNKNOWN
};

#define APPEND_CHAR(C) obstack_1grow (&lsc_obstack, C)
#define APPEND_TWO_CHAR_STRING(S)					\
  do									\
    {									\
      APPEND_CHAR (S[0]);						\
      APPEND_CHAR (S[1]);						\
    }									\
  while (0)
static struct obstack lsc_obstack;

static char const *const slack_codes[] =
{
  "NORMAL", "NORM", "FILE", "RESET", "DIR", "LNK", "LINK",
  "SYMLINK", "ORPHAN", "MISSING", "FIFO", "PIPE", "SOCK", "BLK", "BLOCK",
  "CHR", "CHAR", "DOOR", "EXEC", "LEFT", "LEFTCODE", "RIGHT", "RIGHTCODE",
  "END", "ENDCODE", "SUID", "SETUID", "SGID", "SETGID", "STICKY",
  "OTHER_WRITABLE", "OWR", "STICKY_OTHER_WRITABLE", "OWT", "CAPABILITY",
  "MULTIHARDLINK", "CLRTOEOL", NULL
};

static char const *const ls_codes[] =
{
  "no", "no", "fi", "rs", "di", "ln", "ln", "ln", "or", "mi", "pi", "pi",
  "so", "bd", "bd", "cd", "cd", "do", "ex", "lc", "lc", "rc", "rc", "ec", "ec",
  "su", "su", "sg", "sg", "st", "ow", "ow", "tw", "tw", "ca", "mh", "cl", NULL
};
verify (ARRAY_CARDINALITY (slack_codes) == ARRAY_CARDINALITY (ls_codes));

static struct option const long_options[] =
  {
    {"bourne-shell", no_argument, NULL, 'b'},
    {"sh", no_argument, NULL, 'b'},
    {"csh", no_argument, NULL, 'c'},
    {"c-shell", no_argument, NULL, 'c'},
    {"print-database", no_argument, NULL, 'p'},
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
      printf (_("Usage: %s [OPTION]... [FILE]\n"), program_name);
      fputs (_("\
Output commands to set the LS_COLORS environment variable.\n\
\n\
Determine format of output:\n\
  -b, --sh, --bourne-shell    output Bourne shell code to set LS_COLORS\n\
  -c, --csh, --c-shell        output C shell code to set LS_COLORS\n\
  -p, --print-database        output defaults\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      fputs (_("\
\n\
If FILE is specified, read it to determine which colors to use for which\n\
file types and extensions.  Otherwise, a precompiled database is used.\n\
For details on the format of these files, run 'dircolors --print-database'.\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }

  exit (status);
}

static enum Shell_syntax
guess_shell_syntax (void)
{
  char *shell;

  shell = getenv ("SHELL");
  if (shell == NULL || *shell == '\0')
    return SHELL_SYNTAX_UNKNOWN;

  shell = last_component (shell);

  if (STREQ (shell, "csh") || STREQ (shell, "tcsh"))
    return SHELL_SYNTAX_C;

  return SHELL_SYNTAX_BOURNE;
}

static void
parse_line (char const *line, char **keyword, char **arg)
{
  char const *p;
  char const *keyword_start;
  char const *arg_start;

  *keyword = NULL;
  *arg = NULL;

  for (p = line; isspace (to_uchar (*p)); ++p)
    continue;
  if (*p == '\0' || *p == '#')
    return;

  keyword_start = p;

  while (!isspace (to_uchar (*p)) && *p != '\0')
    {
      ++p;
    }

  *keyword = xstrndup (keyword_start, p - keyword_start);
  if (*p  == '\0')
    return;

  do
    {
      ++p;
    }
  while (isspace (to_uchar (*p)));

  if (*p == '\0' || *p == '#')
    return;

  arg_start = p;

  while (*p != '\0' && *p != '#')
    ++p;

  for (--p; isspace (to_uchar (*p)); --p)
    continue;
  ++p;

  *arg = xstrndup (arg_start, p - arg_start);
}

static void
append_quoted (char const *str)
{
  bool need_backslash = true;

  while (*str != '\0')
    {
      switch (*str)
        {
        case '\'':
          APPEND_CHAR ('\'');
          APPEND_CHAR ('\\');
          APPEND_CHAR ('\'');
          need_backslash = true;
          break;

        case '\\':
        case '^':
          need_backslash = !need_backslash;
          break;

        case ':':
        case '=':
          if (need_backslash)
            APPEND_CHAR ('\\');
          FALLTHROUGH;

        default:
          need_backslash = true;
          break;
        }

      APPEND_CHAR (*str);
      ++str;
    }
}

static bool
dc_parse_stream (FILE *fp, char const *filename)
{
  size_t line_number = 0;
  char const *next_G_line = G_line;
  char *input_line = NULL;
  size_t input_line_size = 0;
  char const *line;
  char const *term;
  bool ok = true;
  enum { ST_TERMNO, ST_TERMYES, ST_TERMSURE, ST_GLOBAL } state = ST_GLOBAL;
  term = getenv ("TERM");
  if (term == NULL || *term == '\0')
    term = "none";

  while (1)
    {
      char *keywd, *arg;
      bool unrecognized;

      ++line_number;

      if (fp)
        {
          if (getline (&input_line, &input_line_size, fp) <= 0)
            {
              free (input_line);
              break;
            }
          line = input_line;
        }
      else
        {
          if (next_G_line == G_line + sizeof G_line)
            break;
          line = next_G_line;
          next_G_line += strlen (next_G_line) + 1;
        }

      parse_line (line, &keywd, &arg);

      if (keywd == NULL)
        continue;

      if (arg == NULL)
        {
          error (0, 0, _("%s:%lu: invalid line;  missing second token"),
                 quotef (filename), (unsigned long int) line_number);
          ok = false;
          free (keywd);
          continue;
        }

      unrecognized = false;
      if (c_strcasecmp (keywd, "TERM") == 0)
        {
          if (fnmatch (arg, term, 0) == 0)
            state = ST_TERMSURE;
          else if (state != ST_TERMSURE)
            state = ST_TERMNO;
        }
      else
        {
          if (state == ST_TERMSURE)
            state = ST_TERMYES; 

          if (state != ST_TERMNO)
            {
              if (keywd[0] == '.')
                {
                  APPEND_CHAR ('*');
                  append_quoted (keywd);
                  APPEND_CHAR ('=');
                  append_quoted (arg);
                  APPEND_CHAR (':');
                }
              else if (keywd[0] == '*')
                {
                  append_quoted (keywd);
                  APPEND_CHAR ('=');
                  append_quoted (arg);
                  APPEND_CHAR (':');
                }
              else if (c_strcasecmp (keywd, "OPTIONS") == 0
                       || c_strcasecmp (keywd, "COLOR") == 0
                       || c_strcasecmp (keywd, "EIGHTBIT") == 0)
                {
                }
              else
                {
                  int i;

                  for (i = 0; slack_codes[i] != NULL; ++i)
                    if (c_strcasecmp (keywd, slack_codes[i]) == 0)
                      break;

                  if (slack_codes[i] != NULL)
                    {
                      APPEND_TWO_CHAR_STRING (ls_codes[i]);
                      APPEND_CHAR ('=');
                      append_quoted (arg);
                      APPEND_CHAR (':');
                    }
                  else
                    {
                      unrecognized = true;
                    }
                }
            }
          else
            {
              unrecognized = true;
            }
        }

      if (unrecognized && (state == ST_TERMSURE || state == ST_TERMYES))
        {
          error (0, 0, _("%s:%lu: unrecognized keyword %s"),
                 (filename ? quotef (filename) : _("<internal>")),
                 (unsigned long int) line_number, keywd);
          ok = false;
        }

      free (keywd);
      free (arg);
    }

  return ok;
}

static bool
dc_parse_file (char const *filename)
{
  bool ok;

  if (! STREQ (filename, "-") && freopen (filename, "r", stdin) == NULL)
    {
      error (0, errno, "%s", quotef (filename));
      return false;
    }

  ok = dc_parse_stream (stdin, filename);

  if (fclose (stdin) != 0)
    {
      error (0, errno, "%s", quotef (filename));
      return false;
    }

  return ok;
}

int
main (int argc, char **argv)
{
  bool ok = true;
  int optc;
  enum Shell_syntax syntax = SHELL_SYNTAX_UNKNOWN;
  bool print_database = false;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  while ((optc = getopt_long (argc, argv, "bcp", long_options, NULL)) != -1)
    switch (optc)
      {
      case 'b':
        syntax = SHELL_SYNTAX_BOURNE;
        break;

      case 'c':
        syntax = SHELL_SYNTAX_C;
        break;

      case 'p':
        print_database = true;
        break;

      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

      default:
        usage (EXIT_FAILURE);
      }

  argc -= optind;
  argv += optind;
  if (print_database && syntax != SHELL_SYNTAX_UNKNOWN)
    {
      error (0, 0,
             _("the options to output dircolors' internal database and\n"
               "to select a shell syntax are mutually exclusive"));
      usage (EXIT_FAILURE);
    }

  if ((!print_database) < argc)
    {
      error (0, 0, _("extra operand %s"), quote (argv[!print_database]));
      if (print_database)
        fprintf (stderr, "%s\n",
                 _("file operands cannot be combined with "
                   "--print-database (-p)"));
      usage (EXIT_FAILURE);
    }

  if (print_database)
    {
      char const *p = G_line;
      while (p - G_line < sizeof G_line)
        {
          puts (p);
          p += strlen (p) + 1;
        }
    }
  else
    {
      if (syntax == SHELL_SYNTAX_UNKNOWN)
        {
          syntax = guess_shell_syntax ();
          if (syntax == SHELL_SYNTAX_UNKNOWN)
            {
              die (EXIT_FAILURE, 0,
         _("no SHELL environment variable, and no shell type option given"));
            }
        }

      obstack_init (&lsc_obstack);
      if (argc == 0)
        ok = dc_parse_stream (NULL, NULL);
      else
        ok = dc_parse_file (argv[0]);

      if (ok)
        {
          size_t len = obstack_object_size (&lsc_obstack);
          char *s = obstack_finish (&lsc_obstack);
          char const *prefix;
          char const *suffix;

          if (syntax == SHELL_SYNTAX_BOURNE)
            {
              prefix = "LS_COLORS='";
              suffix = "';\nexport LS_COLORS\n";
            }
          else
            {
              prefix = "setenv LS_COLORS '";
              suffix = "'\n";
            }
          fputs (prefix, stdout);
          fwrite (s, 1, len, stdout);
          fputs (suffix, stdout);
        }
    }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
