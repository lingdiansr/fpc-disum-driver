/*
 * FPC Disum (10a5:9201) security channel: GET_TLS_KEY envelope authentication.
 *
 * Reference: docs/CLEANROOM_SPEC.md §8 and appendix B.3-B.4.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_tls.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

/*
 * NUL-terminated ASCII constants fixed by the reviewed specification (§8).  The
 * derived AES-256 and HMAC-SHA256 keys are SHA-256 of these seeds including the
 * terminating NUL; sizeof() includes it, so the lengths are 16 and 13 bytes.
 */
static const gchar sealing_key_seed[] = "FPC_SEALING_KEY";
static const gchar hmac_key_seed[] = "FPC_HMAC_KEY";

/* The envelope is little-endian on the wire; do not use host-endian macros. */
static guint32
read_le32 (const guint8 *data)
{
  return (guint32)data[0] | ((guint32)data[1] << 8) | ((guint32)data[2] << 16)
         | ((guint32)data[3] << 24);
}

/* Whether [offset, offset + length) stays inside a buffer of @total bytes. */
static gboolean
range_within (gsize total, guint32 offset, guint32 length)
{
  return (gsize)offset <= total && (gsize)length <= total - (gsize)offset;
}

static gboolean
sha256 (const guint8 *data, gsize len, guint8 out[32])
{
  unsigned int out_len = 0;

  return EVP_Digest (data, len, out, &out_len, EVP_sha256 (), NULL) == 1 && out_len == 32;
}

static gboolean
aes_256_cbc_decrypt_nopad (const guint8 *key, const guint8 *input, gsize len, guint8 *output)
{
  static const guint8 zero_iv[16] = { 0 };
  EVP_CIPHER_CTX *ctx;
  int out_len = 0;
  int final_len = 0;
  gboolean ok = FALSE;

  if (len > G_MAXINT)
    return FALSE;

  ctx = EVP_CIPHER_CTX_new ();
  if (ctx == NULL)
    return FALSE;

  if (EVP_DecryptInit_ex (ctx, EVP_aes_256_cbc (), NULL, key, zero_iv) != 1
      || EVP_CIPHER_CTX_set_padding (ctx, 0) != 1
      || EVP_DecryptUpdate (ctx, output, &out_len, input, (int)len) != 1
      || EVP_DecryptFinal_ex (ctx, output + out_len, &final_len) != 1)
    goto out;

  ok = (gsize)(out_len + final_len) == len;

out:
  EVP_CIPHER_CTX_free (ctx);
  return ok;
}

/* HMAC-SHA256 over part1 || part2, without copying the parts together. */
static gboolean
hmac_sha256 (const guint8 *key, gsize key_len, const guint8 *part1, gsize part1_len,
    const guint8 *part2, gsize part2_len, guint8 out[32])
{
  OSSL_PARAM params[] = {
    OSSL_PARAM_construct_utf8_string (OSSL_MAC_PARAM_DIGEST, (char *)"SHA256", 0),
    OSSL_PARAM_construct_end (),
  };
  EVP_MAC *mac = EVP_MAC_fetch (NULL, "HMAC", NULL);
  EVP_MAC_CTX *ctx = NULL;
  gsize out_len = 0;
  gboolean ok = FALSE;

  if (mac == NULL)
    return FALSE;

  ctx = EVP_MAC_CTX_new (mac);
  if (ctx == NULL)
    goto out;

  if (EVP_MAC_init (ctx, key, key_len, params) != 1)
    goto out;
  if (part1_len > 0 && EVP_MAC_update (ctx, part1, part1_len) != 1)
    goto out;
  if (part2_len > 0 && EVP_MAC_update (ctx, part2, part2_len) != 1)
    goto out;

  ok = EVP_MAC_final (ctx, out, &out_len, 32) == 1 && out_len == 32;

out:
  EVP_MAC_CTX_free (ctx);
  EVP_MAC_free (mac);
  return ok;
}

void
fpc_disum_psk_clear (FpcDisumPsk *psk)
{
  if (psk == NULL)
    return;

  OPENSSL_cleanse (psk->psk, sizeof (psk->psk));
  psk->psk_len = 0;
}

FpcDisumEnvelopeStatus
fpc_disum_envelope_unseal (const guint8 *envelope, gsize len, FpcDisumPsk *psk)
{
  guint8 sealing_key[32];
  guint8 hmac_key[32];
  guint8 digest[32];
  guint32 key_offset;
  guint32 key_length;
  guint32 aad_offset;
  guint32 aad_length;
  guint32 signature_offset;
  guint32 signature_length;
  FpcDisumEnvelopeStatus status = FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH;

  if (psk == NULL)
    return FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT;

  /* Never leave stale key material behind, whatever the outcome is. */
  fpc_disum_psk_clear (psk);

  if (envelope == NULL)
    return FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT;

  if (len != FPC_DISUM_ENVELOPE_LEN || read_le32 (envelope) != FPC_DISUM_ENVELOPE_MAGIC)
    return FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH;

  key_offset = read_le32 (envelope + 4);
  key_length = read_le32 (envelope + 8);
  aad_offset = read_le32 (envelope + 12);
  aad_length = read_le32 (envelope + 16);
  signature_offset = read_le32 (envelope + 20);
  signature_length = read_le32 (envelope + 24);

  if (key_length % FPC_DISUM_ENVELOPE_KEY_ALIGN != 0
      || signature_length != FPC_DISUM_ENVELOPE_SIGNATURE_LEN
      || !range_within (len, key_offset, key_length) || !range_within (len, aad_offset, aad_length)
      || !range_within (len, signature_offset, signature_length))
    return FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS;

  /* AES-256-CBC, NoPadding: the ciphertext is taken from the header-declared range
   * and the plaintext is used as the PSK exactly as decrypted. */
  if (!sha256 ((const guint8 *)sealing_key_seed, sizeof (sealing_key_seed), sealing_key)
      || !aes_256_cbc_decrypt_nopad (sealing_key, envelope + key_offset, key_length, psk->psk))
    {
      status = FPC_DISUM_ENVELOPE_ERROR_AES;
      goto out;
    }
  psk->psk_len = key_length;

  /* Full 32 B HMAC-SHA256 over AAD || encrypted key; the plaintext is not covered. */
  if (!sha256 ((const guint8 *)hmac_key_seed, sizeof (hmac_key_seed), hmac_key)
      || !hmac_sha256 (hmac_key, 32, envelope + aad_offset, aad_length, envelope + key_offset,
          key_length, digest)
      || CRYPTO_memcmp (digest, envelope + signature_offset, FPC_DISUM_ENVELOPE_SIGNATURE_LEN) != 0)
    {
      status = FPC_DISUM_ENVELOPE_ERROR_HMAC;
      goto out;
    }

  status = FPC_DISUM_ENVELOPE_OK;

out:
  OPENSSL_cleanse (sealing_key, sizeof (sealing_key));
  OPENSSL_cleanse (hmac_key, sizeof (hmac_key));
  OPENSSL_cleanse (digest, sizeof (digest));

  if (status != FPC_DISUM_ENVELOPE_OK)
    fpc_disum_psk_clear (psk);

  return status;
}
