#include <config.h>
#include <sys/types.h>
#include <signal.h>
#include <getopt.h>
#include "system.h"
#include "argmatch.h"
#include "die.h"
#include "error.h"
#include "fadvise.h"
#include "stdio--.h"
#include "xbinary-io.h"
static bool tee_files (int nfiles, char **files);
static bool append;
static bool ignore_interrupts;

enum output_error
  {
    output_error_sigpipe,
    output_error_warn,
    output_error_warn_nopipe,  
    output_error_exit,       
    output_error_exit_nopipe   
  };

static enum output_error output_error;

static struct option const long_options[] =
{
  {"append", no_argument, NULL, 'a'},
  {"ignore-interrupts", no_argument, NULL, 'i'},
  {"output-error", optional_argument, NULL, 'p'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

static char const *const output_error_args[] =
{
  "warn", "warn-nopipe", "exit", "exit-nopipe", NULL
};
static enum output_error const output_error_types[] =
{
  output_error_warn, output_error_warn_nopipe,
  output_error_exit, output_error_exit_nopipe
};
ARGMATCH_VERIFY (output_error_args, output_error_types);

void usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("Usage: %s [OPTION]... [FILE]...\n"), program_name);
      fputs (_("\
Copy standard input to each FILE, and also to standard output.\n\
\n\
  -a, --append              append to the given FILEs, do not overwrite\n\
  -i, --ignore-interrupts   ignore interrupt signals\n\
"), stdout);
      fputs (_("\
  -p                        diagnose errors writing to non pipes\n\
      --output-error[=MODE]   set behavior on write error.  See MODE below\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      fputs (_("\
\n\
MODE determines behavior with write errors on the outputs:\n\
  'warn'         diagnose errors writing to any output\n\
  'warn-nopipe'  diagnose errors writing to any output not a pipe\n\
  'exit'         exit on error writing to any output\n\
  'exit-nopipe'  exit on error writing to any output not a pipe\n\
The default MODE for the -p option is 'warn-nopipe'.\n\
The default operation when --output-error is not specified, is to\n\
exit immediately on error writing to a pipe, and diagnose errors\n\
writing to non pipe outputs.\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  bool ok;
  int optc;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  append = false;
  ignore_interrupts = false;

  while ((optc = getopt_long (argc, argv, "aip", long_options, NULL)) != -1)
    {
      switch (optc)
        {
        case 'a':
          append = true;
          break;

        case 'i':
          ignore_interrupts = true;
          break;

        case 'p':
          if (optarg)
            output_error = XARGMATCH ("--output-error", optarg,
                                      output_error_args, output_error_types);
          else
            output_error = output_error_warn_nopipe;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (ignore_interrupts)
    signal (SIGINT, SIG_IGN);

  if (output_error != output_error_sigpipe)
    signal (SIGPIPE, SIG_IGN);

  ok = tee_files (argc - optind, &argv[optind]);
  if (close (STDIN_FILENO) != 0)
    die (EXIT_FAILURE, errno, "%s", _("standard input"));

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}


static bool
tee_files (int nfiles, char **files)
{
  size_t n_outputs = 0;
  FILE **descriptors;
  char buffer[BUFSIZ];
  ssize_t bytes_read = 0;
  int i;
  bool ok = true;
  char const *mode_string =
    (O_BINARY
     ? (append ? "ab" : "wb")
     : (append ? "a" : "w"));

  xset_binary_mode (STDIN_FILENO, O_BINARY);
  xset_binary_mode (STDOUT_FILENO, O_BINARY);
  fadvise (stdin, FADVISE_SEQUENTIAL);

  /* Set up FILES[0 .. NFILES] and DESCRIPTORS[0 .. NFILES].
     In both arrays, entry 0 corresponds to standard output.  */

  descriptors = xnmalloc (nfiles + 1, sizeof *descriptors);
  files--;
  descriptors[0] = stdout;
  files[0] = bad_cast (_("standard output"));
  setvbuf (stdout, NULL, _IONBF, 0);
  n_outputs++;

  for (i = 1; i <= nfiles; i++)
    {
      /* Do not treat "-" specially - as mandated by POSIX.  */
      descriptors[i] = fopen (files[i], mode_string);
      if (descriptors[i] == NULL)
        {
          error (output_error == output_error_exit
                 || output_error == output_error_exit_nopipe,
                 errno, "%s", quotef (files[i]));
          ok = false;
        }
      else
        {
          setvbuf (descriptors[i], NULL, _IONBF, 0);
          n_outputs++;
        }
    }

  while (n_outputs)
    {
      bytes_read = read (STDIN_FILENO, buffer, sizeof buffer);
      if (bytes_read < 0 && errno == EINTR)
        continue;
      if (bytes_read <= 0)
        break;

      /* Write to all NFILES + 1 descriptors.
         Standard output is the first one.  */
      for (i = 0; i <= nfiles; i++)
        if (descriptors[i]
            && fwrite (buffer, bytes_read, 1, descriptors[i]) != 1)
          {
            int w_errno = errno;
            bool fail = errno != EPIPE || (output_error == output_error_exit
                                          || output_error == output_error_warn);
            if (descriptors[i] == stdout)
              clearerr (stdout); /* Avoid redundant close_stdout diagnostic.  */
            if (fail)
              {
                error (output_error == output_error_exit
                       || output_error == output_error_exit_nopipe,
                       w_errno, "%s", quotef (files[i]));
              }
            descriptors[i] = NULL;
            if (fail)
              ok = false;
            n_outputs--;
          }
    }

  if (bytes_read == -1)
    {
      error (0, errno, _("read error"));
      ok = false;
    }

  /* Close the files, but not standard output.  */
  for (i = 1; i <= nfiles; i++)
    if (descriptors[i] && fclose (descriptors[i]) != 0)
      {
        error (0, errno, "%s", quotef (files[i]));
        ok = false;
      }

  free (descriptors);

  return ok;
}
