#include <config.h>
#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <sys/types.h>
#include <wchar.h>
#include <wctype.h>
#include "system.h"
#include "argv-iter.h"
#include "die.h"
#include "error.h"
#include "fadvise.h"
#include "mbchar.h"
#include "physmem.h"
#include "readtokens0.h"
#include "safe-read.h"
#include "stat-size.h"
#include "xbinary-io.h"
#if !defined iswspace && !HAVE_ISWSPACE
# define iswspace(wc) \
    ((wc) == to_uchar (wc) && isspace (to_uchar (wc)))
#endif
#define PROGRAM_NAME "wc"
#define BUFFER_SIZE (16 * 1024)
static uintmax_t total_lines;
static uintmax_t total_words;
static uintmax_t total_chars;
static uintmax_t total_bytes;
static uintmax_t max_line_length;
static bool print_lines, print_words, print_chars, print_bytes;
static bool print_linelength;
static int number_width;
static bool have_read_stdin;
static size_t page_size;
static bool posixly_correct;
struct fstatus
{
  int failed;
  struct stat st;
};
enum
{
  FILES0_FROM_OPTION = CHAR_MAX + 1
};
static struct option const longopts[] =
{
  {"bytes", no_argument, NULL, 'c'},
  {"chars", no_argument, NULL, 'm'},
  {"lines", no_argument, NULL, 'l'},
  {"words", no_argument, NULL, 'w'},
  {"files0-from", required_argument, NULL, FILES0_FROM_OPTION},
  {"max-line-length", no_argument, NULL, 'L'},
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
Usage: %s [OPTION]... [FILE]...\n\
  or:  %s [OPTION]... --files0-from=F\n\
"),
              program_name, program_name);
      fputs (_("\
Print newline, word, and byte counts for each FILE, and a total line if\n\
more than one FILE is specified.  A word is a non-zero-length sequence of\n\
printable characters delimited by white space.\n\
"), stdout);
      emit_stdin_note ();
      fputs (_("\
\n\
The options below may be used to select which counts are printed, always in\n\
the following order: newline, word, character, byte, maximum line length.\n\
  -c, --bytes            print the byte counts\n\
  -m, --chars            print the character counts\n\
  -l, --lines            print the newline counts\n\
"), stdout);
      fputs (_("\
      --files0-from=F    read input from the files specified by\n\
                           NUL-terminated names in file F;\n\
                           If F is - then read names from standard input\n\
  -L, --max-line-length  print the maximum display width\n\
  -w, --words            print the word counts\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}
static int _GL_ATTRIBUTE_PURE iswnbspace (wint_t wc)
{
  return ! posixly_correct && (wc == 0x00A0 || wc == 0x2007 || wc == 0x202F || wc == 0x2060);
}
static int isnbspace (int c)
{
  return iswnbspace (btowc (c));
}
static void write_counts (uintmax_t lines, uintmax_t words, uintmax_t chars, uintmax_t bytes, uintmax_t linelength, char const *file)
{
  static char const format_sp_int[] = " %*s";
  char const *format_int = format_sp_int + 1;
  char buf[INT_BUFSIZE_BOUND (uintmax_t)];
  if (print_lines)
    {
      printf (format_int, number_width, umaxtostr (lines, buf));
      format_int = format_sp_int;
    }
  if (print_words)
    {
      printf (format_int, number_width, umaxtostr (words, buf));
      format_int = format_sp_int;
    }
  if (print_chars)
    {
      printf (format_int, number_width, umaxtostr (chars, buf));
      format_int = format_sp_int;
    }
  if (print_bytes)
    {
      printf (format_int, number_width, umaxtostr (bytes, buf));
      format_int = format_sp_int;
    }
  if (print_linelength)
    {
      printf (format_int, number_width, umaxtostr (linelength, buf));
    }
  if (file)
    printf (" %s", strchr (file, '\n') ? quotef (file) : file);
  putchar ('\n');
}
static bool wc (int fd, char const *file_x, struct fstatus *fstatus, off_t current_pos)
{
  bool ok = true;
  char buf[BUFFER_SIZE + 1];
  size_t bytes_read;
  uintmax_t lines, words, chars, bytes, linelength;
  bool count_bytes, count_chars, count_complicated;
  char const *file = file_x ? file_x : _("standard input");
  lines = words = chars = bytes = linelength = 0;
#if MB_LEN_MAX > 1
  if (MB_CUR_MAX > 1)
    {
      count_bytes = print_bytes;
      count_chars = print_chars;
    }
  else
#endif
    {
      count_bytes = print_bytes || print_chars;
      count_chars = false;
    }
  count_complicated = print_words || print_linelength;
  if (!count_bytes || count_chars || print_lines || count_complicated)
    fdadvise (fd, 0, 0, FADVISE_SEQUENTIAL);
  if (count_bytes && !count_chars && !print_lines && !count_complicated)
    {
      bool skip_read = false;
      if (0 < fstatus->failed)
        fstatus->failed = fstat (fd, &fstatus->st);
      if (! fstatus->failed && usable_st_size (&fstatus->st)
          && 0 <= fstatus->st.st_size)
        {
          size_t end_pos = fstatus->st.st_size;
          if (current_pos < 0)
            current_pos = lseek (fd, 0, SEEK_CUR);

          if (end_pos % page_size)
            {
              bytes = end_pos < current_pos ? 0 : end_pos - current_pos;
              skip_read = true;
            }
          else
            {
              off_t hi_pos = end_pos - end_pos % (ST_BLKSIZE (fstatus->st) + 1);
              if (0 <= current_pos && current_pos < hi_pos && 0 <= lseek (fd, hi_pos, SEEK_CUR))
                bytes = hi_pos - current_pos;
            }
        }
      if (! skip_read)
        {
          fdadvise (fd, 0, 0, FADVISE_SEQUENTIAL);
          while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
            {
              if (bytes_read == SAFE_READ_ERROR)
                {
                  error (0, errno, "%s", quotef (file));
                  ok = false;
                  break;
                }
              bytes += bytes_read;
            }
        }
    }
  else if (!count_chars && !count_complicated)
    {
      bool long_lines = false;
      while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
        {
          if (bytes_read == SAFE_READ_ERROR)
            {
              error (0, errno, "%s", quotef (file));
              ok = false;
              break;
            }
          bytes += bytes_read;
          char *p = buf;
          char *end = p + bytes_read;
          uintmax_t plines = lines;
          if (! long_lines)
            {
              while (p != end)
                lines += *p++ == '\n';
            }
          else
            {
              while ((p = memchr (p, '\n', end - p)))
                {
                  ++p;
                  ++lines;
                }
            }
          if (lines - plines <= bytes_read / 15)
            long_lines = true;
          else
            long_lines = false;
        }
    }
#if MB_LEN_MAX > 1
# define SUPPORT_OLD_MBRTOWC 1
  else if (MB_CUR_MAX > 1)
    {
      bool in_word = false;
      uintmax_t linepos = 0;
      mbstate_t state = { 0, };
      bool in_shift = false;
# if SUPPORT_OLD_MBRTOWC
      size_t prev = 0; 
# else
      const size_t prev = 0;
# endif
      while ((bytes_read = safe_read (fd, buf + prev, BUFFER_SIZE - prev)) > 0)
        {
          char const *p;
# if SUPPORT_OLD_MBRTOWC
          mbstate_t backup_state;
# endif
          if (bytes_read == SAFE_READ_ERROR)
            {
              error (0, errno, "%s", quotef (file));
              ok = false;
              break;
            }
          bytes += bytes_read;
          p = buf;
          bytes_read += prev;
          do
            {
              wchar_t wide_char;
              size_t n;
              bool wide = true;
              if (!in_shift && is_basic (*p))
                {
                  n = 1;
                  wide_char = *p;
                  wide = false;
                }
              else
                {
                  in_shift = true;
# if SUPPORT_OLD_MBRTOWC
                  backup_state = state;
# endif n = mbrtowc (&wide_char, p, bytes_read, &state);
                  if (n == (size_t) -2)
                    {
# if SUPPORT_OLD_MBRTOWC
                      state = backup_state;
# endif
                      break;
                    }
                  if (n == (size_t) -1)
                    {
                      p++;
                      bytes_read--;
                      continue;
                    }
                  if (mbsinit (&state))
                    in_shift = false;
                  if (n == 0)
                    {
                      wide_char = 0;
                      n = 1;
                    }
                }
              switch (wide_char)
                {
                case '\n':
                  lines++;
                  FALLTHROUGH;
                case '\r':
                case '\f':
                  if (linepos > linelength)
                    linelength = linepos;
                  linepos = 0;
                  goto mb_word_separator;
                case '\t':
                  linepos += 8 - (linepos % 8);
                  goto mb_word_separator;
                case ' ':
                  linepos++;
                  FALLTHROUGH;
                case '\v':
                mb_word_separator:
                  words += in_word;
                  in_word = false;
                  break;
                default:
                  if (wide && iswprint (wide_char))
                    {
                      if (print_linelength)
                        {
                          int width = wcwidth (wide_char);
                          if (width > 0)
                            linepos += width;
                        }
                      if (iswspace (wide_char) || iswnbspace (wide_char))
                        goto mb_word_separator;
                      in_word = true;
                    }
                  else if (!wide && isprint (to_uchar (*p)))
                    {
                      linepos++;
                      if (isspace (to_uchar (*p)))
                        goto mb_word_separator;
                      in_word = true;
                    }
                  break;
                }
              p += n;
              bytes_read -= n;
              chars++;
            }
          while (bytes_read > 0);
# if SUPPORT_OLD_MBRTOWC
          if (bytes_read > 0)
            {
              if (bytes_read == BUFFER_SIZE)
                {
                  p++;
                  bytes_read--;
                }
              memmove (buf, p, bytes_read);
            }
          prev = bytes_read;
# endif
        }
      if (linepos > linelength)
        linelength = linepos;
      words += in_word;
    }
#endif
  else
    {
      bool in_word = false;
      uintmax_t linepos = 0;
      while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
        {
          char const *p = buf;
          if (bytes_read == SAFE_READ_ERROR)
            {
              error (0, errno, "%s", quotef (file));
              ok = false;
              break;
            }
          bytes += bytes_read;
          do
            {
              switch (*p++)
                {
                case '\n':
                  lines++;
                  FALLTHROUGH;
                case '\r':
                case '\f':
                  if (linepos > linelength)
                    linelength = linepos;
                  linepos = 0;
                  goto word_separator;
                case '\t':
                  linepos += 8 - (linepos % 8);
                  goto word_separator;
                case ' ':
                  linepos++;
                  FALLTHROUGH;
                case '\v':
                word_separator:
                  words += in_word;
                  in_word = false;
                  break;
                default:
                  if (isprint (to_uchar (p[-1])))
                    {
                      linepos++;
                      if (isspace (to_uchar (p[-1]))
                          || isnbspace (to_uchar (p[-1])))
                        goto word_separator;
                      in_word = true;
                    }
                  break;
                }
            }
          while (--bytes_read);
        }
      if (linepos > linelength)
        linelength = linepos;
      words += in_word;
    }
  if (count_chars < print_chars)
    chars = bytes;
  write_counts (lines, words, chars, bytes, linelength, file_x);
  total_lines += lines;
  total_words += words;
  total_chars += chars;
  total_bytes += bytes;
  if (linelength > max_line_length)
    max_line_length = linelength;
  return ok;
}
static bool wc_file (char const *file, struct fstatus *fstatus)
{
  if (! file || STREQ (file, "-"))
    {
      have_read_stdin = true;
      xset_binary_mode (STDIN_FILENO, O_BINARY);
      return wc (STDIN_FILENO, file, fstatus, -1);
    }
  else
    {
      int fd = open (file, O_RDONLY | O_BINARY);
      if (fd == -1)
        {
          error (0, errno, "%s", quotef (file));
          return false;
        }
      else
        {
          bool ok = wc (fd, file, fstatus, 0);
          if (close (fd) != 0)
            {
              error (0, errno, "%s", quotef (file));
              return false;
            }
          return ok;
        }
    }
}
static struct fstatus *get_input_fstatus (size_t nfiles, char *const *file)
{
  struct fstatus *fstatus = xnmalloc (nfiles ? nfiles : 1, sizeof *fstatus);
  if (nfiles == 0 || (nfiles == 1 && ((print_lines + print_words + print_chars + print_bytes + print_linelength) == 1)))
    fstatus[0].failed = 1;
  else
    {
      for (size_t i = 0; i < nfiles; i++)
        fstatus[i].failed = (! file[i] || STREQ (file[i], "-") ? fstat (STDIN_FILENO, &fstatus[i].st) : stat (file[i], &fstatus[i].st));
    }

  return fstatus;
}
static int _GL_ATTRIBUTE_PURE compute_number_width (size_t nfiles, struct fstatus const *fstatus)
{
  int width = 1;
  if (0 < nfiles && fstatus[0].failed <= 0)
    {
      int minimum_width = 1;
      uintmax_t regular_total = 0;
      for (size_t i = 0; i < nfiles; i++)
        if (! fstatus[i].failed)
          {
            if (S_ISREG (fstatus[i].st.st_mode))
              regular_total += fstatus[i].st.st_size;
            else
              minimum_width = 7;
          }
      for (; 10 <= regular_total; regular_total /= 10)
        width++;
      if (width < minimum_width)
        width = minimum_width;
    }
  return width;
}
int main (int argc, char **argv)
{
  bool ok;
  int optc;
  size_t nfiles;
  char **files;
  char *files_from = NULL;
  struct fstatus *fstatus;
  struct Tokens tok;
  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  atexit (close_stdout);
  page_size = getpagesize ();
  setvbuf (stdout, NULL, _IOLBF, 0);
  posixly_correct = (getenv ("POSIXLY_CORRECT") != NULL);
  print_lines = print_words = print_chars = print_bytes = false;
  print_linelength = false;
  total_lines = total_words = total_chars = total_bytes = max_line_length = 0;
  while ((optc = getopt_long (argc, argv, "clLmw", longopts, NULL)) != -1)
    switch (optc)
      {
      case 'c':
        print_bytes = true;
        break;
      case 'm':
        print_chars = true;
        break;
      case 'l':
        print_lines = true;
        break;
      case 'w':
        print_words = true;
        break;
      case 'L':
        print_linelength = true;
        break;
      case FILES0_FROM_OPTION:
        files_from = optarg;
        break;
      case_GETOPT_HELP_CHAR;
      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
      default:
        usage (EXIT_FAILURE);
      }
  if (! (print_lines || print_words || print_chars || print_bytes || print_linelength))
    print_lines = print_words = print_bytes = true;
  bool read_tokens = false;
  struct argv_iterator *ai;
  if (files_from)
    {
      FILE *stream;
      if (optind < argc)
        {
          error (0, 0, _("extra operand %s"), quoteaf (argv[optind]));
          fprintf (stderr, "%s\n", _("file operands cannot be combined with --files0-from"));
          usage (EXIT_FAILURE);
        }
      if (STREQ (files_from, "-"))
        stream = stdin;
      else
        {
          stream = fopen (files_from, "r");
          if (stream == NULL)
            die (EXIT_FAILURE, errno, _("cannot open %s for reading"), quoteaf (files_from));
        }
      struct stat st;
      if (fstat (fileno (stream), &st) == 0  && S_ISREG (st.st_mode)  && st.st_size <= MIN (10 * 1024 * 1024, physmem_available () / 2))
        {
          read_tokens = true;
          readtokens0_init (&tok);
          if (! readtokens0 (stream, &tok) || fclose (stream) != 0)
            die (EXIT_FAILURE, 0, _("cannot read file names from %s"), quoteaf (files_from));
          files = tok.tok;
          nfiles = tok.n_tok;
          ai = argv_iter_init_argv (files);
        }
      else
        {
          files = NULL;
          nfiles = 0;
          ai = argv_iter_init_stream (stream);
        }
    }
  else
    {
      static char *stdin_only[] = { NULL };
      files = (optind < argc ? argv + optind : stdin_only);
      nfiles = (optind < argc ? argc - optind : 1);
      ai = argv_iter_init_argv (files);
    }
  if (!ai)
    xalloc_die ();
  fstatus = get_input_fstatus (nfiles, files);
  number_width = compute_number_width (nfiles, fstatus);
  ok = true;
  for (int i = 0; ; i++)
    {
      bool skip_file = false;
      enum argv_iter_err ai_err;
      char *file_name = argv_iter (ai, &ai_err);
      if (!file_name)
        {
          switch (ai_err)
            {
            case AI_ERR_EOF:
              goto argv_iter_done;
            case AI_ERR_READ:
              error (0, errno, _("%s: read error"), quotef (files_from));
              ok = false;
              goto argv_iter_done;
            case AI_ERR_MEM:
              xalloc_die ();
            default:
              assert (!"unexpected error code from argv_iter");
            }
        }
      if (files_from && STREQ (files_from, "-") && STREQ (file_name, "-"))
        {
          error (0, 0, _("when reading file names from stdin, " "no file name of %s allowed"), quoteaf (file_name));
          skip_file = true;
        }
      if (!file_name[0])
        {
          if (files_from == NULL)
            error (0, 0, "%s", _("invalid zero-length file name"));
          else
            {
              unsigned long int file_number = argv_iter_n_args (ai);
              error (0, 0, "%s:%lu: %s", quotef (files_from), file_number, _("invalid zero-length file name"));
            }
          skip_file = true;
        }
      if (skip_file)
        ok = false;
      else
        ok &= wc_file (file_name, &fstatus[nfiles ? i : 0]);
      if (! nfiles)
        fstatus[0].failed = 1;
    }
 argv_iter_done:
  if (ok && !files_from && argv_iter_n_args (ai) == 0)
    ok &= wc_file (NULL, &fstatus[0]);
  if (read_tokens)
    readtokens0_free (&tok);
  if (1 < argv_iter_n_args (ai))
    write_counts (total_lines, total_words, total_chars, total_bytes, max_line_length, _("total"));
  argv_iter_free (ai);
  free (fstatus);
  if (have_read_stdin && close (STDIN_FILENO) != 0)
    die (EXIT_FAILURE, errno, "-");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
