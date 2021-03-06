#include <config.h>
#define PROGRAM_NAME "cksum"
#define AUTHORS proper_name ("Q. Frank Xia")
#include <getopt.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include "system.h"
#include "fadvise.h"
#include "xbinary-io.h"
#include <byteswap.h>
#ifdef WORDS_BIGENDIAN
# define SWAP(n) (n)
#else
# define SWAP(n) bswap_32 (n)
#endif

#ifdef CRCTAB

# define BIT(x)	((uint_fast32_t) 1 << (x))
# define SBIT	BIT (31)

# define GEN	(BIT (26) | BIT (23) | BIT (22) | BIT (16) | BIT (12) \ | BIT (11) | BIT (10) | BIT (8) | BIT (7) | BIT (5) \ | BIT (4) | BIT (2) | BIT (1) | BIT (0))
static uint_fast32_t r[8];
static void
fill_r (void)
{
  r[0] = GEN;
  for (int i = 1; i < 8; i++)
    r[i] = (r[i - 1] << 1) ^ ((r[i - 1] & SBIT) ? GEN : 0);
}
static uint_fast32_t
crc_remainder (int m)
{
  uint_fast32_t rem = 0;
  for (int i = 0; i < 8; i++)
    if (BIT (i) & m)
      rem ^= r[i];
  return rem & 0xFFFFFFFF;	/* Make it run on 64-bit machine.  */
}

int main (void)
{
  int i;
  static uint_fast32_t crctab[8][256];

  fill_r ();

  for (i = 0; i < 256; i++)
    {
      crctab[0][i] = crc_remainder (i);
    }
  for (i = 0; i < 256; i++)
    {
      uint32_t crc = 0;

      crc = (crc << 8) ^ crctab[0][((crc >> 24) ^ (i & 0xFF)) & 0xFF];
      for (unsigned int offset = 1; offset < 8; offset++)
        {
          crc = (crc << 8) ^ crctab[0][((crc >> 24) ^ 0x00) & 0xFF];
          crctab[offset][i] = crc;
        }
    }

  printf ("uint_fast32_t const crctab[8][256] = {\n");
  for (int y = 0; y < 8; y++)
    {
      printf ("{\n  0x%08x", crctab[y][0]);
      for (i = 0; i < 51; i++)
        {
          printf (",\n  0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%08x",
                  crctab[y][i * 5 + 1], crctab[y][i * 5 + 2],
                  crctab[y][i * 5 + 3], crctab[y][i * 5 + 4],
                  crctab[y][i * 5 + 5]);
        }
        printf ("\n},\n");
    }
  printf ("};\n");
  return EXIT_SUCCESS;
}

#else 

# include "die.h"
# include "error.h"

# include "cksum.h"
# if USE_PCLMUL_CRC32
#  include "cpuid.h"
# endif 
# define BUFLEN (1 << 16)

static bool debug;

enum
{
  DEBUG_PROGRAM_OPTION = CHAR_MAX + 1,
};

static struct option const longopts[] =
{
  {"debug", no_argument, NULL, DEBUG_PROGRAM_OPTION},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0},
};

static bool have_read_stdin;

static bool
cksum_slice8 (FILE *fp, char const *file, uint_fast32_t *crc_out, uintmax_t *length_out);
static bool
  (*cksum_fp)(FILE *, char const *, uint_fast32_t *, uintmax_t *) = cksum_slice8;

# if USE_PCLMUL_CRC32
static bool
pclmul_supported (void)
{
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  if (! __get_cpuid (1, &eax, &ebx, &ecx, &edx))
    {
      if (debug)
        error (0, 0, "%s", _("failed to get cpuid"));
      return false;
    }

  if (! (ecx & bit_PCLMUL) || ! (ecx & bit_AVX))
    {
      if (debug)
        error (0, 0, "%s", _("pclmul support not detected"));
      return false;
    }

  if (debug)
    error (0, 0, "%s", _("using pclmul hardware support"));

  return true;
}
# endif 

static bool
cksum_slice8 (FILE *fp, char const *file, uint_fast32_t *crc_out, uintmax_t *length_out)
{
  uint32_t buf[BUFLEN/sizeof (uint32_t)];
  uint_fast32_t crc = 0;
  uintmax_t length = 0;
  size_t bytes_read;

  if (!fp || !file || !crc_out || !length_out)
    return false;

  while ((bytes_read = fread (buf, 1, BUFLEN, fp)) > 0)
    {
      uint32_t *datap;

      if (length + bytes_read < length)
        {
          error (0, EOVERFLOW, _("%s: file too long"), quotef (file));
          return false;
        }
      length += bytes_read;
      datap = (uint32_t *)buf;
      while (bytes_read >= 8)
        {
          uint32_t first = *datap++, second = *datap++;
          crc ^= SWAP (first);
          second = SWAP (second);
          crc = (crctab[7][(crc >> 24) & 0xFF]
                 ^ crctab[6][(crc >> 16) & 0xFF]
                 ^ crctab[5][(crc >> 8) & 0xFF]
                 ^ crctab[4][(crc) & 0xFF]
                 ^ crctab[3][(second >> 24) & 0xFF]
                 ^ crctab[2][(second >> 16) & 0xFF]
                 ^ crctab[1][(second >> 8) & 0xFF]
                 ^ crctab[0][(second) & 0xFF]);
          bytes_read -= 8;
        }
      unsigned char *cp = (unsigned char *)datap;
      while (bytes_read--)
        crc = (crc << 8) ^ crctab[0][((crc >> 24) ^ *cp++) & 0xFF];
      if (feof (fp))
        break;
    }

  *crc_out = crc;
  *length_out = length;

  return true;
}
static bool cksum (char const *file, bool print_name)
{
  uint_fast32_t crc = 0;
  uintmax_t length = 0;
  FILE *fp;
  char length_buf[INT_BUFSIZE_BOUND (uintmax_t)];
  char const *hp;

  if (STREQ (file, "-"))
    {
      fp = stdin;
      have_read_stdin = true;
      xset_binary_mode (STDIN_FILENO, O_BINARY);
    }
  else
    {
      fp = fopen (file, (O_BINARY ? "rb" : "r"));
      if (fp == NULL)
        {
          error (0, errno, "%s", quotef (file));
          return false;
        }
    }

  fadvise (fp, FADVISE_SEQUENTIAL);

  if (! cksum_fp (fp, file, &crc, &length))
    return false;

  if (ferror (fp))
    {
      error (0, errno, "%s", quotef (file));
      if (!STREQ (file, "-"))
        fclose (fp);
      return false;
    }

  if (!STREQ (file, "-") && fclose (fp) == EOF)
    {
      error (0, errno, "%s", quotef (file));
      return false;
    }

  hp = umaxtostr (length, length_buf);

  for (; length; length >>= 8)
    crc = (crc << 8) ^ crctab[0][((crc >> 24) ^ length) & 0xFF];

  crc = ~crc & 0xFFFFFFFF;

  if (print_name)
    printf ("%u %s %s\n", (unsigned int) crc, hp, file);
  else
    printf ("%u %s\n", (unsigned int) crc, hp);

  if (ferror (stdout))
    die (EXIT_FAILURE, errno, "-: %s", _("write error"));

  return true;
}

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [FILE]...\n\
  or:  %s [OPTION]\n\
"),
              program_name, program_name);
      fputs (_("\
Print CRC checksum and byte counts of each FILE.\n\
\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  int i, c;
  bool ok;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);
  setvbuf (stdout, NULL, _IOLBF, 0);

  while ((c = getopt_long (argc, argv, "", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case DEBUG_PROGRAM_OPTION:
          debug = true;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  have_read_stdin = false;

# if USE_PCLMUL_CRC32
  if (pclmul_supported ())
     cksum_fp = cksum_pclmul;
# endif 

  if (optind == argc)
    ok = cksum ("-", false);
  else
    {
      ok = true;
      for (i = optind; i < argc; i++)
        ok &= cksum (argv[i], true);
    }

  if (have_read_stdin && fclose (stdin) == EOF)
    die (EXIT_FAILURE, errno, "-");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
