/*
 * FPC Disum (10a5:9201) security channel: GET_TLS_KEY envelope authentication.
 *
 * Reference: docs/CLEANROOM_SPEC.md §8 and appendix B.3-B.4.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_tls.h"

#include <string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/ssl.h>

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

/*
 * TLS-PSK channel (SPEC §8).  The host is the TLS server; only TLS 1.2 with
 * PSK-AES128-CBC-SHA256 is accepted and the identity is the reviewed one.
 */
struct _FpcDisumTls
{
  SSL_CTX *ctx;
  SSL *ssl;
  /* Non-NULL only while the BIO has not yet been handed to the SSL object. */
  BIO *rbio;
  BIO *wbio;
  guint8 psk[FPC_DISUM_PSK_LEN];
  gboolean identity_checked;
  gboolean identity_ok;
  gboolean established;
  gboolean failed;
  FpcDisumTlsStatus status;
  gchar *error_text;
};

static const gchar psk_identity[] = "Disum PSK";
static const gchar session_context[] = "fpc-disum";

static void
record_error (FpcDisumTls *self)
{
  unsigned long code = ERR_peek_last_error ();
  gchar text[256];

  if (code == 0)
    return;

  ERR_error_string_n (code, text, sizeof (text));
  g_clear_pointer (&self->error_text, g_free);
  self->error_text = g_strdup (text);
}

static unsigned int
psk_server_callback (SSL *ssl, const char *identity, unsigned char *psk, unsigned int max_psk_len)
{
  FpcDisumTls *self = SSL_get_app_data (ssl);

  self->identity_checked = TRUE;
  self->identity_ok = identity != NULL && g_strcmp0 (identity, psk_identity) == 0;

  if (!self->identity_ok || max_psk_len < FPC_DISUM_PSK_LEN)
    return 0;

  memcpy (psk, self->psk, FPC_DISUM_PSK_LEN);
  return FPC_DISUM_PSK_LEN;
}

FpcDisumTls *
fpc_disum_tls_new (const FpcDisumPsk *psk)
{
  FpcDisumTls *self;
  SSL_CTX *ctx;

  g_return_val_if_fail (psk != NULL, NULL);

  if (psk->psk_len != FPC_DISUM_PSK_LEN)
    return NULL;

  ctx = SSL_CTX_new (TLS_server_method ());
  if (ctx == NULL)
    return NULL;

  if (SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION) != 1
      || SSL_CTX_set_max_proto_version (ctx, TLS1_2_VERSION) != 1
      || SSL_CTX_set_cipher_list (ctx, "PSK-AES128-CBC-SHA256") != 1
      || SSL_CTX_set_session_id_context (
             ctx, (const unsigned char *)session_context, sizeof (session_context) - 1)
             != 1)
    {
      SSL_CTX_free (ctx);
      return NULL;
    }

  /*
   * Pin the server-side session behaviour: the device only answers a ServerHello
   * that carries a non-empty session id (SPEC §8).  The handshake test asserts the
   * id the synthetic client observes.
   */
  SSL_CTX_set_session_cache_mode (ctx, SSL_SESS_CACHE_SERVER);
  SSL_CTX_set_psk_server_callback (ctx, psk_server_callback);

  self = g_new0 (FpcDisumTls, 1);
  memcpy (self->psk, psk->psk, FPC_DISUM_PSK_LEN);
  self->status = FPC_DISUM_TLS_WANT_READ;
  self->ctx = ctx;
  self->ssl = SSL_new (ctx);
  self->rbio = BIO_new (BIO_s_mem ());
  self->wbio = BIO_new (BIO_s_mem ());

  if (self->ssl == NULL || self->rbio == NULL || self->wbio == NULL)
    {
      fpc_disum_tls_free (self);
      return NULL;
    }

  BIO_set_mem_eof_return (self->rbio, -1);
  SSL_set_accept_state (self->ssl);
  SSL_set_app_data (self->ssl, self);
  /* The SSL object owns both BIOs from here on; I/O goes through SSL_get_*bio(). */
  SSL_set_bio (self->ssl, self->rbio, self->wbio);
  self->rbio = NULL;
  self->wbio = NULL;

  return self;
}

void
fpc_disum_tls_free (FpcDisumTls *self)
{
  if (self == NULL)
    return;

  SSL_free (self->ssl);
  BIO_free (self->rbio);
  BIO_free (self->wbio);
  SSL_CTX_free (self->ctx);

  OPENSSL_cleanse (self->psk, sizeof (self->psk));
  g_clear_pointer (&self->error_text, g_free);
  g_free (self);
}

gboolean
fpc_disum_tls_feed (FpcDisumTls *self, const guint8 *data, gsize len)
{
  BIO *rbio;

  g_return_val_if_fail (self != NULL, FALSE);

  if (self->failed)
    return FALSE;

  if (len == 0)
    return TRUE;

  if (len > G_MAXINT)
    return FALSE;

  rbio = SSL_get_rbio (self->ssl);
  if (rbio == NULL || BIO_write (rbio, data, (int)len) != (int)len)
    {
      self->failed = TRUE;
      self->status = FPC_DISUM_TLS_ERROR_IO;
      record_error (self);
      return FALSE;
    }

  return TRUE;
}

FpcDisumTlsStatus
fpc_disum_tls_step (FpcDisumTls *self)
{
  int ret;
  int error;

  g_return_val_if_fail (self != NULL, FPC_DISUM_TLS_ERROR_IO);

  if (self->failed || self->established)
    return self->status;

  ERR_clear_error ();
  ret = SSL_do_handshake (self->ssl);

  if (ret == 1)
    {
      self->established = TRUE;
      self->status = FPC_DISUM_TLS_OK;
      return self->status;
    }

  error = SSL_get_error (self->ssl, ret);
  if (error == SSL_ERROR_WANT_READ)
    {
      self->status = FPC_DISUM_TLS_WANT_READ;
      return self->status;
    }

  if (error == SSL_ERROR_WANT_WRITE)
    {
      /* The memory BIO always accepts the write, so this is progress. */
      self->status = FPC_DISUM_TLS_OK;
      return self->status;
    }

  self->failed = TRUE;
  self->status = (self->identity_checked && !self->identity_ok) ? FPC_DISUM_TLS_ERROR_IDENTITY
                                                                : FPC_DISUM_TLS_ERROR_HANDSHAKE;
  record_error (self);
  return self->status;
}

gboolean
fpc_disum_tls_is_established (const FpcDisumTls *self)
{
  g_return_val_if_fail (self != NULL, FALSE);

  return self->established && !self->failed;
}

FpcDisumTlsStatus
fpc_disum_tls_status (const FpcDisumTls *self)
{
  g_return_val_if_fail (self != NULL, FPC_DISUM_TLS_ERROR_IO);

  return self->status;
}

const gchar *
fpc_disum_tls_error_text (const FpcDisumTls *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return self->error_text;
}

gsize
fpc_disum_tls_take_output (FpcDisumTls *self, guint8 *buffer, gsize max_len)
{
  BIO *wbio;
  int pending;
  int ret;

  g_return_val_if_fail (self != NULL, 0);
  g_return_val_if_fail (buffer != NULL || max_len == 0, 0);

  if (max_len == 0)
    return 0;

  wbio = SSL_get_wbio (self->ssl);
  if (wbio == NULL)
    return 0;

  pending = BIO_pending (wbio);
  if (pending <= 0)
    return 0;

  if ((gsize)pending < max_len)
    max_len = (gsize)pending;
  if (max_len > G_MAXINT)
    max_len = G_MAXINT;

  ret = BIO_read (wbio, buffer, (int)max_len);

  return ret > 0 ? (gsize)ret : 0;
}

gssize
fpc_disum_tls_read (FpcDisumTls *self, guint8 *buffer, gsize max_len)
{
  int ret;

  g_return_val_if_fail (self != NULL, -1);

  if (!self->established || self->failed)
    return -1;

  if (max_len == 0)
    return 0;

  if (max_len > G_MAXINT)
    return -1;

  ERR_clear_error ();
  ret = SSL_read (self->ssl, buffer, (int)max_len);

  if (ret > 0)
    return ret;

  {
    int error = SSL_get_error (self->ssl, ret);

    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_ZERO_RETURN)
      return 0;
  }

  self->failed = TRUE;
  self->status = FPC_DISUM_TLS_ERROR_HANDSHAKE;
  record_error (self);
  return -1;
}

gssize
fpc_disum_tls_write (FpcDisumTls *self, const guint8 *data, gsize len)
{
  int ret;

  g_return_val_if_fail (self != NULL, -1);

  if (!self->established || self->failed)
    return -1;

  if (len == 0)
    return 0;

  if (len > G_MAXINT)
    return -1;

  ERR_clear_error ();
  ret = SSL_write (self->ssl, data, (int)len);

  if (ret > 0)
    return ret;

  self->failed = TRUE;
  self->status = FPC_DISUM_TLS_ERROR_HANDSHAKE;
  record_error (self);
  return -1;
}
