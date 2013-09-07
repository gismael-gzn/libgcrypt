/* t-ed25519.c - Check the Ed25519 crypto
 * Copyright (C) 2013 g10 Code GmbH
 *
 * This file is part of Libgcrypt.
 *
 * Libgcrypt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * Libgcrypt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../src/gcrypt-int.h"

#include "stopwatch.h"

#define PGM "t-ed25519"

#define my_isascii(c) (!((c) & 0x80))
#define digitp(p)   (*(p) >= '0' && *(p) <= '9')
#define hexdigitp(a) (digitp (a)                     \
                      || (*(a) >= 'A' && *(a) <= 'F')  \
                      || (*(a) >= 'a' && *(a) <= 'f'))
#define xtoi_1(p)   (*(p) <= '9'? (*(p)- '0'): \
                     *(p) <= 'F'? (*(p)-'A'+10):(*(p)-'a'+10))
#define xtoi_2(p)   ((xtoi_1(p) * 16) + xtoi_1((p)+1))
#define xmalloc(a)    gcry_xmalloc ((a))
#define xcalloc(a,b)  gcry_xcalloc ((a),(b))
#define xstrdup(a)    gcry_xstrdup ((a))
#define xfree(a)      gcry_free ((a))
#define pass()        do { ; } while (0)

static int verbose;
static int debug;
static int error_count;
static int sign_with_pk;
static int no_verify;

static void
die (const char *format, ...)
{
  va_list arg_ptr ;

  fflush (stdout);
  fprintf (stderr, "%s: ", PGM);
  va_start( arg_ptr, format ) ;
  vfprintf (stderr, format, arg_ptr );
  va_end(arg_ptr);
  if (*format && format[strlen(format)-1] != '\n')
    putc ('\n', stderr);
  exit (1);
}

static void
fail (const char *format, ...)
{
  va_list arg_ptr;

  fflush (stdout);
  fprintf (stderr, "%s: ", PGM);
  /* if (wherestr) */
  /*   fprintf (stderr, "%s: ", wherestr); */
  va_start (arg_ptr, format);
  vfprintf (stderr, format, arg_ptr);
  va_end (arg_ptr);
  if (*format && format[strlen(format)-1] != '\n')
    putc ('\n', stderr);
  error_count++;
  if (error_count)
    die ("stopped after 50 errors.");
}

static void
show (const char *format, ...)
{
  va_list arg_ptr;

  if (!verbose)
    return;
  fprintf (stderr, "%s: ", PGM);
  va_start (arg_ptr, format);
  vfprintf (stderr, format, arg_ptr);
  if (*format && format[strlen(format)-1] != '\n')
    putc ('\n', stderr);
  va_end (arg_ptr);
}


static void
show_sexp (const char *prefix, gcry_sexp_t a)
{
  char *buf;
  size_t size;

  fprintf (stderr, "%s: ", PGM);
  if (prefix)
    fputs (prefix, stderr);
  size = gcry_sexp_sprint (a, GCRYSEXP_FMT_ADVANCED, NULL, 0);
  buf = xmalloc (size);

  gcry_sexp_sprint (a, GCRYSEXP_FMT_ADVANCED, buf, size);
  fprintf (stderr, "%.*s", (int)size, buf);
  gcry_free (buf);
}


/* Prepend FNAME with the srcdir environment variable's value and
   retrun an allocated filename. */
char *
prepend_srcdir (const char *fname)
{
  static const char *srcdir;
  char *result;

  if (!srcdir && !(srcdir = getenv ("srcdir")))
    srcdir = ".";

  result = xmalloc (strlen (srcdir) + 1 + strlen (fname) + 1);
  strcpy (result, srcdir);
  strcat (result, "/");
  strcat (result, fname);
  return result;
}


/* Read next line but skip over empty and comment lines.  Caller must
   xfree the result.  */
static char *
read_textline (FILE *fp, int *lineno)
{
  char line[4096];
  char *p;

  do
    {
      if (!fgets (line, sizeof line, fp))
        {
          if (feof (fp))
            return NULL;
          die ("error reading input line: %s\n", strerror (errno));
        }
      ++*lineno;
      p = strchr (line, '\n');
      if (!p)
        die ("input line %d not terminated or too long\n", *lineno);
      *p = 0;
      for (p--;p > line && my_isascii (*p) && isspace (*p); p--)
        *p = 0;
    }
  while (!*line || *line == '#');
  /* if (debug) */
  /*   show ("read line: '%s'\n", line); */
  return xstrdup (line);
}


/* Copy the data after the tag to BUFFER.  BUFFER will be allocated as
   needed.  */
static void
copy_data (char **buffer, const char *line, int lineno)
{
  const char *s;

  xfree (*buffer);
  *buffer = NULL;

  s = strchr (line, ':');
  if (!s)
    {
      fail ("syntax error at input line %d", lineno);
      return;
    }
  for (s++; my_isascii (*s) && isspace (*s); s++)
    ;
  *buffer = xstrdup (s);
}


/* Convert STRING consisting of hex characters into its binary
   representation and return it as an allocated buffer. The valid
   length of the buffer is returned at R_LENGTH.  The string is
   delimited by end of string.  The function returns NULL on
   error.  */
static void *
hex2buffer (const char *string, size_t *r_length)
{
  const char *s;
  unsigned char *buffer;
  size_t length;

  buffer = xmalloc (strlen(string)/2+1);
  length = 0;
  for (s=string; *s; s +=2 )
    {
      if (!hexdigitp (s) || !hexdigitp (s+1))
        return NULL;           /* Invalid hex digits. */
      ((unsigned char*)buffer)[length++] = xtoi_2 (s);
    }
  *r_length = length;
  return buffer;
}


static void
hexdowncase (char *string)
{
  char *p;

  for (p=string; *p; p++)
    if (my_isascii (*p))
      *p = tolower (*p);
}


static void
one_test (int testno, const char *sk, const char *pk,
          const char *msg, const char *sig)
{
  gpg_error_t err;
  int i;
  char *p;
  void *buffer = NULL;
  void *buffer2 = NULL;
  size_t buflen, buflen2;
  gcry_sexp_t s_tmp, s_tmp2;
  gcry_sexp_t s_sk = NULL;
  gcry_sexp_t s_pk = NULL;
  gcry_sexp_t s_msg= NULL;
  gcry_sexp_t s_sig= NULL;
  unsigned char *sig_r = NULL;
  unsigned char *sig_s = NULL;
  char *sig_rs_string = NULL;
  size_t sig_r_len, sig_s_len;

  if (verbose > 1)
    show ("Running test %d\n", testno);

  if (!(buffer = hex2buffer (sk, &buflen)))
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "sk", "invalid hex string");
      goto leave;
    }
  if (!(buffer2 = hex2buffer (pk, &buflen2)))
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "pk", "invalid hex string");
      goto leave;
    }
  if (sign_with_pk)
    err = gcry_sexp_build (&s_sk, NULL,
                           "(private-key"
                           " (ecc"
                           "  (curve \"Ed25519\")"
                           "  (q %b)"
                           "  (d %b)))",
                           (int)buflen2, buffer2,
                           (int)buflen, buffer);
  else
    err = gcry_sexp_build (&s_sk, NULL,
                           "(private-key"
                           " (ecc"
                           "  (curve \"Ed25519\")"
                           "  (d %b)))",
                           (int)buflen, buffer);
  if (err)
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "sk", gpg_strerror (err));
      goto leave;
    }

  if ((err = gcry_sexp_build (&s_pk, NULL,
                              "(public-key"
                              " (ecc"
                              "  (curve \"Ed25519\")"
                              "  (q %b)))",  (int)buflen2, buffer2)))
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "pk", gpg_strerror (err));
      goto leave;
    }

  xfree (buffer);
  if (!(buffer = hex2buffer (msg, &buflen)))
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "msg", "invalid hex string");
      goto leave;
    }
  if ((err = gcry_sexp_build (&s_msg, NULL,
                              "(data"
                              " (flags eddsa)"
                              " (hash-algo sha512)"
                              " (value %b))",  (int)buflen, buffer)))
    {
      fail ("error building s-exp for test %d, %s: %s",
            testno, "msg", gpg_strerror (err));
      goto leave;
    }

  if ((err = gcry_pk_sign (&s_sig, s_msg, s_sk)))
    fail ("gcry_pk_sign failed for test %d: %s", testno, gpg_strerror (err));
  if (debug)
    show_sexp ("sig=", s_sig);

  s_tmp2 = NULL;
  s_tmp = gcry_sexp_find_token (s_sig, "sig-val", 0);
  if (s_tmp)
    {
      s_tmp2 = s_tmp;
      s_tmp = gcry_sexp_find_token (s_tmp2, "eddsa", 0);
      if (s_tmp)
        {
          gcry_sexp_release (s_tmp2);
          s_tmp2 = s_tmp;
          s_tmp = gcry_sexp_find_token (s_tmp2, "r", 0);
          if (s_tmp)
            {
              sig_r = gcry_sexp_nth_buffer (s_tmp, 1, &sig_r_len);
              gcry_sexp_release (s_tmp);
            }
          s_tmp = gcry_sexp_find_token (s_tmp2, "s", 0);
          if (s_tmp)
            {
              sig_s = gcry_sexp_nth_buffer (s_tmp, 1, &sig_s_len);
              gcry_sexp_release (s_tmp);
            }
        }
    }
  gcry_sexp_release (s_tmp2); s_tmp2 = NULL;

  if (!sig_r || !sig_s)
    fail ("gcry_pk_sign failed for test %d: %s", testno, "r or s missing");
  else
    {
      sig_rs_string = xmalloc (2*(sig_r_len + sig_s_len)+1);
      p = sig_rs_string;
      *p = 0;
      for (i=0; i < sig_r_len; i++, p += 2)
        snprintf (p, 3, "%02x", sig_r[i]);
      for (i=0; i < sig_s_len; i++, p += 2)
        snprintf (p, 3, "%02x", sig_s[i]);
      if (strcmp (sig_rs_string, sig))
        {
          fail ("gcry_pk_sign failed for test %d: %s",
                testno, "wrong value returned");
          show ("  expected: '%s'", sig);
          show ("       got: '%s'", sig_rs_string);
        }
    }

  if (!no_verify)
    if ((err = gcry_pk_verify (s_sig, s_msg, s_pk)))
      fail ("gcry_pk_verify failed for test %d: %s",
            testno, gpg_strerror (err));


 leave:
  gcry_sexp_release (s_sig);
  gcry_sexp_release (s_sk);
  gcry_sexp_release (s_pk);
  gcry_sexp_release (s_msg);
  xfree (buffer);
  xfree (buffer2);
  xfree (sig_r);
  xfree (sig_s);
  xfree (sig_rs_string);
}


static void
check_ed25519 (void)
{
  char *fname;
  FILE *fp;
  int lineno, ntests;
  char *line;
  int testno;
  char *sk, *pk, *msg, *sig;

  show ("Checking Ed25519.\n");

  fname = prepend_srcdir ("t-ed25519.inp");
  fp = fopen (fname, "r");
  if (!fp)
    die ("error opening '%s': %s\n", fname, strerror (errno));

  testno = 0;
  sk = pk = msg = sig = NULL;
  lineno = ntests = 0;
  while ((line = read_textline (fp, &lineno)))
    {
      if (!strncmp (line, "TST:", 4))
        testno = atoi (line+4);
      else if (!strncmp (line, "SK:", 3))
        copy_data (&sk, line, lineno);
      else if (!strncmp (line, "PK:", 3))
        copy_data (&pk, line, lineno);
      else if (!strncmp (line, "MSG:", 4))
        copy_data (&msg, line, lineno);
      else if (!strncmp (line, "SIG:", 4))
        copy_data (&sig, line, lineno);
      else
        fail ("unknown tag at input line %d", lineno);

      xfree (line);
      if (testno && sk && pk && msg && sig)
        {
          hexdowncase (sig);
          one_test (testno, sk, pk, msg, sig);
          ntests++;
          xfree (pk);  pk = NULL;
          xfree (sk);  sk = NULL;
          xfree (msg); msg = NULL;
          xfree (sig); sig = NULL;
        }

    }
  xfree (pk);
  xfree (sk);
  xfree (msg);
  xfree (sig);

  if (ntests != 1024)
    fail ("did %d tests but expected 1024", ntests);

  fclose (fp);
  xfree (fname);
}


int
main (int argc, char **argv)
{
  int last_argc = -1;

  if (argc)
    { argc--; argv++; }

  while (argc && last_argc != argc )
    {
      last_argc = argc;
      if (!strcmp (*argv, "--"))
        {
          argc--; argv++;
          break;
        }
      else if (!strcmp (*argv, "--help"))
        {
          fputs ("usage: " PGM " [options]\n"
                 "Options:\n"
                 "  --verbose       print timings etc.\n"
                 "  --debug         flyswatter\n"
                 "  --sign-with-pk  also use the public key for signing\n"
                 "  --no-verify     skip the verify test\n",
                 stdout);
          exit (0);
        }
      else if (!strcmp (*argv, "--verbose"))
        {
          verbose++;
          argc--; argv++;
        }
      else if (!strcmp (*argv, "--debug"))
        {
          verbose += 2;
          debug++;
          argc--; argv++;
        }
      else if (!strcmp (*argv, "--sign-with-pk"))
        {
          sign_with_pk = 1;
          argc--; argv++;
        }
      else if (!strcmp (*argv, "--no-verify"))
        {
          no_verify = 1;
          argc--; argv++;
        }
      else if (!strncmp (*argv, "--", 2))
        die ("unknown option '%s'", *argv);

    }


  gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
  if (!gcry_check_version (GCRYPT_VERSION))
    die ("version mismatch\n");
  if (debug)
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 1u , 0);
  gcry_control (GCRYCTL_ENABLE_QUICK_RANDOM, 0);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

  start_timer ();
  check_ed25519 ();
  stop_timer ();

  show ("All tests completed in %s.  Errors: %d\n",
        elapsed_time (), error_count);
  return !!error_count;
}