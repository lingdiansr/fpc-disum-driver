/*
 * FPC Disum GET_TLS_KEY envelope tests.
 *
 * The envelope corpus is generated from docs/CLEANROOM_TEST_VECTORS.json; the
 * synthetic PSK is the only key material involved, and no assertion prints it.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include "fpc_disum_tls.h"
#include "fpc-disum-corpus.h"

/* Stage names are the reject_stage vocabulary of spec appendix B.3. */
static const gchar *
stage_name (FpcDisumEnvelopeStatus status)
{
  switch (status)
    {
    case FPC_DISUM_ENVELOPE_OK:
    case FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT:
      return NULL;
    case FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH:
      return "total-length";
    case FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS:
      return "field-bounds";
    case FPC_DISUM_ENVELOPE_ERROR_AES:
      return "aes";
    case FPC_DISUM_ENVELOPE_ERROR_HMAC:
      return "hmac";
    }

  return NULL;
}

static guint32
read_le32 (const guint8 *data)
{
  return (guint32)data[0] | ((guint32)data[1] << 8) | ((guint32)data[2] << 16)
         | ((guint32)data[3] << 24);
}

static void
write_le32 (guint8 *data, guint32 value)
{
  data[0] = value & 0xff;
  data[1] = (value >> 8) & 0xff;
  data[2] = (value >> 16) & 0xff;
  data[3] = (value >> 24) & 0xff;
}

static const FpcDisumCorpusEnvelopeVector *
accepted_vector (void)
{
  for (gsize i = 0; i < fpc_disum_corpus_n_envelope_vectors; i++)
    {
      if (fpc_disum_corpus_envelope_vectors[i].accepted)
        return &fpc_disum_corpus_envelope_vectors[i];
    }

  g_assert_not_reached ();
  return NULL;
}

static guint8 *
copy_accepted_envelope (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_vector ();

  return g_memdup2 (vector->envelope, vector->envelope_len);
}

static void
assert_psk_cleared (const FpcDisumPsk *psk)
{
  g_assert_cmpuint (psk->psk_len, ==, 0);

  /* Boolean assertions keep the diagnostic free of key material. */
  for (gsize i = 0; i < sizeof (psk->psk); i++)
    g_assert_true (psk->psk[i] == 0);
}

static void
test_envelope_vector (gconstpointer data)
{
  const FpcDisumCorpusEnvelopeVector *vector = data;
  FpcDisumPsk psk;
  FpcDisumEnvelopeStatus status;

  /* Anything other than success must wipe this pre-filled buffer. */
  memset (&psk, 0xA5, sizeof (psk));

  status = fpc_disum_envelope_unseal (vector->envelope, vector->envelope_len, &psk);

  if (vector->accepted)
    {
      g_assert_cmpint (status, ==, FPC_DISUM_ENVELOPE_OK);
      g_assert_cmpuint (psk.psk_len, ==, vector->psk_len);
      g_assert_true (memcmp (psk.psk, vector->psk, vector->psk_len) == 0);
    }
  else
    {
      g_assert_cmpint (status, !=, FPC_DISUM_ENVELOPE_OK);
      assert_psk_cleared (&psk);

      /* A vector without a declared stage only requires rejection. */
      if (vector->reject_stage != NULL)
        g_assert_cmpint (g_strcmp0 (stage_name (status), vector->reject_stage), ==, 0);
    }

  fpc_disum_psk_clear (&psk);
}

static void
test_rejects_wrong_length (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_vector ();
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, vector->envelope_len - 1, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH);
  assert_psk_cleared (&psk);

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, vector->envelope_len + 1, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH);
  assert_psk_cleared (&psk);
}

static void
test_rejects_wrong_magic (void)
{
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;

  write_le32 (envelope, 0);

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH);
  assert_psk_cleared (&psk);
}

static void
test_rejects_unaligned_key_length (void)
{
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;

  /* key_off, key_len, ... : the key length must be a multiple of the AES block. */
  write_le32 (envelope + 8, FPC_DISUM_ENVELOPE_KEY_ALIGN * 2 - 1);

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS);
  assert_psk_cleared (&psk);
}

static void
test_rejects_short_signature (void)
{
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;

  write_le32 (envelope + 24, FPC_DISUM_ENVELOPE_SIGNATURE_LEN - 1);

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS);
  assert_psk_cleared (&psk);
}

static void
test_rejects_out_of_range_fields (void)
{
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;

  /* aad_off = 121 with a 13 B AAD leaves the envelope. */
  write_le32 (envelope + 12, FPC_DISUM_ENVELOPE_LEN);

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS);
  assert_psk_cleared (&psk);
}

static void
test_ignores_unnamed_region (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_vector ();
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;
  guint32 key_end;
  guint32 aad_offset;

  /* The gap between the key range and the AAD is unnamed and outside the HMAC
   * range, so an implementation must not interpret it. */
  key_end = read_le32 (envelope + 4) + read_le32 (envelope + 8);
  aad_offset = read_le32 (envelope + 12);
  g_assert_cmpuint (aad_offset, >, key_end);

  for (guint32 i = key_end; i < aad_offset; i++)
    envelope[i] ^= 0xff;

  g_assert_cmpint (
      fpc_disum_envelope_unseal (envelope, vector->envelope_len, &psk), ==, FPC_DISUM_ENVELOPE_OK);
  g_assert_cmpuint (psk.psk_len, ==, vector->psk_len);
  g_assert_true (memcmp (psk.psk, vector->psk, psk.psk_len) == 0);
}

static void
test_rejects_tampered_signature (void)
{
  g_autofree guint8 *envelope = copy_accepted_envelope ();
  FpcDisumPsk psk;
  guint32 signature_end = read_le32 (envelope + 20) + read_le32 (envelope + 24);

  /* Flip only the last signature byte: the whole 32 B comparison must matter. */
  g_assert_cmpuint (signature_end, <=, FPC_DISUM_ENVELOPE_LEN);
  envelope[signature_end - 1] ^= 0x01;

  g_assert_cmpint (fpc_disum_envelope_unseal (envelope, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_HMAC);
  assert_psk_cleared (&psk);
}

static void
test_rejects_null_arguments (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_vector ();
  FpcDisumPsk psk;

  memset (&psk, 0xA5, sizeof (psk));
  g_assert_cmpint (fpc_disum_envelope_unseal (NULL, FPC_DISUM_ENVELOPE_LEN, &psk), ==,
      FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT);
  assert_psk_cleared (&psk);

  g_assert_cmpint (fpc_disum_envelope_unseal (vector->envelope, vector->envelope_len, NULL), ==,
      FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT);
}

static void
test_psk_clear (void)
{
  FpcDisumPsk psk;

  memset (&psk, 0xA5, sizeof (psk));
  psk.psk_len = 12345;

  fpc_disum_psk_clear (&psk);
  assert_psk_cleared (&psk);

  /* Idempotent, and safe on a NULL pointer. */
  fpc_disum_psk_clear (&psk);
  assert_psk_cleared (&psk);
  fpc_disum_psk_clear (NULL);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/envelope/rejects-wrong-length", test_rejects_wrong_length);
  g_test_add_func ("/envelope/rejects-wrong-magic", test_rejects_wrong_magic);
  g_test_add_func ("/envelope/rejects-unaligned-key-length", test_rejects_unaligned_key_length);
  g_test_add_func ("/envelope/rejects-short-signature", test_rejects_short_signature);
  g_test_add_func ("/envelope/rejects-out-of-range-fields", test_rejects_out_of_range_fields);
  g_test_add_func ("/envelope/rejects-tampered-signature", test_rejects_tampered_signature);
  g_test_add_func ("/envelope/ignores-unnamed-region", test_ignores_unnamed_region);
  g_test_add_func ("/envelope/rejects-null-arguments", test_rejects_null_arguments);
  g_test_add_func ("/envelope/psk-clear", test_psk_clear);

  for (gsize i = 0; i < fpc_disum_corpus_n_envelope_vectors; i++)
    {
      g_autofree gchar *path
          = g_strdup_printf ("/envelope/vector/%s", fpc_disum_corpus_envelope_vectors[i].id);
      g_test_add_data_func (path, &fpc_disum_corpus_envelope_vectors[i], test_envelope_vector);
    }

  return g_test_run ();
}
