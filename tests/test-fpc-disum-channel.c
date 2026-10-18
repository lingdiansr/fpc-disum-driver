/*
 * FPC Disum TLS-PSK channel tests against a synthetic in-process client.
 *
 * The PSK is the synthetic one from the reviewed envelope vector; no device,
 * key or captured traffic is involved.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "fpc_disum_tls.h"
#include "fpc-disum-corpus.h"

typedef struct
{
  FpcDisumTls *server;
  SSL_CTX *client_ctx;
  SSL *client;
  BIO *client_rbio;
  BIO *client_wbio;
  gchar *identity;
  guint8 psk[FPC_DISUM_PSK_LEN];
  guint8 server_out[FPC_DISUM_TLS_DATA_MAX];
  guint output_calls;
} Channel;

static const FpcDisumCorpusEnvelopeVector *
accepted_envelope (void)
{
  for (gsize i = 0; i < fpc_disum_corpus_n_envelope_vectors; i++)
    {
      if (fpc_disum_corpus_envelope_vectors[i].accepted)
        return &fpc_disum_corpus_envelope_vectors[i];
    }

  g_assert_not_reached ();
  return NULL;
}

static unsigned int
client_psk_callback (SSL *ssl, const char *hint, char *identity, unsigned int max_identity_len,
    unsigned char *psk, unsigned int max_psk_len)
{
  Channel *channel = SSL_get_app_data (ssl);
  gsize identity_len = strlen (channel->identity) + 1;

  if (max_identity_len < identity_len || max_psk_len < FPC_DISUM_PSK_LEN)
    return 0;

  memcpy (identity, channel->identity, identity_len);
  memcpy (psk, channel->psk, FPC_DISUM_PSK_LEN);

  return FPC_DISUM_PSK_LEN;
}

static Channel *
channel_new (const gchar *identity, const guint8 *psk, gsize psk_len)
{
  Channel *channel = g_new0 (Channel, 1);
  FpcDisumPsk secret = { { 0 }, 0 };

  channel->identity = g_strdup (identity);
  memcpy (channel->psk, psk, MIN (psk_len, sizeof channel->psk));

  secret.psk_len = psk_len;
  memcpy (secret.psk, psk, MIN (psk_len, sizeof secret.psk));
  channel->server = fpc_disum_tls_new (&secret);
  fpc_disum_psk_clear (&secret);
  g_assert_nonnull (channel->server);

  channel->client_ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (channel->client_ctx);
  g_assert_cmpint (SSL_CTX_set_min_proto_version (channel->client_ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_max_proto_version (channel->client_ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_cipher_list (channel->client_ctx, "PSK-AES128-CBC-SHA256"), ==, 1);
  SSL_CTX_set_psk_client_callback (channel->client_ctx, client_psk_callback);

  channel->client = SSL_new (channel->client_ctx);
  g_assert_nonnull (channel->client);
  channel->client_rbio = BIO_new (BIO_s_mem ());
  channel->client_wbio = BIO_new (BIO_s_mem ());
  g_assert_nonnull (channel->client_rbio);
  g_assert_nonnull (channel->client_wbio);
  BIO_set_mem_eof_return (channel->client_rbio, -1);

  /* The SSL object owns the BIOs from here on. */
  SSL_set_bio (channel->client, channel->client_rbio, channel->client_wbio);
  SSL_set_connect_state (channel->client);
  SSL_set_app_data (channel->client, channel);

  return channel;
}

static void
channel_free (Channel *channel)
{
  if (channel == NULL)
    return;

  fpc_disum_tls_free (channel->server);
  SSL_free (channel->client);
  SSL_CTX_free (channel->client_ctx);
  g_free (channel->identity);
  g_free (channel);
}

/* Move queued host->device records to the client, one bounded chunk at a time. */
static void
move_server_to_client (Channel *channel, gsize fragment)
{
  gsize n;

  fragment = MIN (fragment, sizeof channel->server_out);
  while ((n = fpc_disum_tls_take_output (channel->server, channel->server_out, fragment)) > 0)
    {
      g_assert_cmpint (BIO_write (channel->client_rbio, channel->server_out, (int)n), ==, (int)n);
      channel->output_calls++;
    }
}

/* Move client records to the server, split at an arbitrary boundary. */
static void
move_client_to_server (Channel *channel, gsize fragment)
{
  guint8 buffer[4096];
  int got = BIO_pending (channel->client_wbio);

  if (got <= 0)
    return;

  got = BIO_read (channel->client_wbio, buffer, MIN ((gsize)got, sizeof buffer));
  g_assert_cmpint (got, >, 0);

  for (gsize offset = 0; offset < (gsize)got; offset += fragment)
    {
      if (!fpc_disum_tls_feed (
              channel->server, buffer + offset, MIN (fragment, (gsize)got - offset)))
        g_error ("feed refused: %s", fpc_disum_tls_error_text (channel->server));
    }
}

static void
drive (Channel *channel, gsize fragment)
{
  for (guint round = 0; round < 500; round++)
    {
      FpcDisumTlsStatus status;

      move_server_to_client (channel, fragment);
      move_client_to_server (channel, fragment);

      if (!SSL_is_init_finished (channel->client))
        {
          ERR_clear_error ();
          (void)SSL_do_handshake (channel->client);
        }

      status = fpc_disum_tls_step (channel->server);
      if (status >= FPC_DISUM_TLS_ERROR_IDENTITY)
        return;

      if (fpc_disum_tls_is_established (channel->server) && SSL_is_init_finished (channel->client))
        return;
    }

  g_assert_not_reached ();
}

static void
test_handshake (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  SSL_SESSION *session;
  const unsigned char *session_id;
  unsigned int session_id_len;

  drive (channel, FPC_DISUM_TLS_DATA_MAX);

  g_assert_true (fpc_disum_tls_is_established (channel->server));
  g_assert_cmpint (fpc_disum_tls_status (channel->server), ==, FPC_DISUM_TLS_OK);
  g_assert_null (fpc_disum_tls_error_text (channel->server));

  /* The device requires a non-empty session id in ServerHello (SPEC §8). */
  session = SSL_get_session (channel->client);
  g_assert_nonnull (session);
  session_id = SSL_SESSION_get_id (session, &session_id_len);
  g_assert_cmpuint (session_id_len, >, 0);
  g_assert_nonnull (session_id);

  /* The server flight spans more than one TLS_DATA fragment. */
  g_assert_cmpuint (channel->output_calls, >, 1);

  channel_free (channel);
}

static void
test_handshake_with_one_byte_fragments (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);

  drive (channel, 1);

  g_assert_true (fpc_disum_tls_is_established (channel->server));

  channel_free (channel);
}

static void
test_rejects_wrong_identity (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Not Disum PSK", vector->psk, vector->psk_len);

  drive (channel, FPC_DISUM_TLS_DATA_MAX);

  g_assert_false (fpc_disum_tls_is_established (channel->server));
  g_assert_cmpint (fpc_disum_tls_status (channel->server), ==, FPC_DISUM_TLS_ERROR_IDENTITY);

  channel_free (channel);
}

static void
test_rejects_wrong_psk (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  guint8 wrong[FPC_DISUM_PSK_LEN];
  Channel *channel;

  /* Same length and identity, different key: the Finished MAC must fail. */
  for (gsize i = 0; i < sizeof wrong; i++)
    wrong[i] = vector->psk[i] ^ 0xff;

  channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  /* Keep the server's key; only the synthetic client switches to another one. */
  memcpy (channel->psk, wrong, sizeof wrong);
  drive (channel, FPC_DISUM_TLS_DATA_MAX);

  g_assert_false (fpc_disum_tls_is_established (channel->server));
  g_assert_cmpint (fpc_disum_tls_status (channel->server), ==, FPC_DISUM_TLS_ERROR_HANDSHAKE);
  g_assert_nonnull (fpc_disum_tls_error_text (channel->server));

  channel_free (channel);
}

static void
test_rejects_alert (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  /* TLS 1.2 fatal alert: handshake_failure. */
  const guint8 alert[] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28 };
  guint8 client_hello[4096];
  int got;

  /* Start a real handshake so the alert arrives at a defined point. */
  ERR_clear_error ();
  (void)SSL_do_handshake (channel->client);
  got = BIO_read (channel->client_wbio, client_hello, sizeof client_hello);
  g_assert_cmpint (got, >, 0);
  g_assert_true (fpc_disum_tls_feed (channel->server, client_hello, got));
  /* The server answers ServerHello and then waits for the next client flight. */
  g_assert_cmpint (fpc_disum_tls_step (channel->server), <, FPC_DISUM_TLS_ERROR_IDENTITY);

  g_assert_true (fpc_disum_tls_feed (channel->server, alert, sizeof alert));
  g_assert_cmpint (fpc_disum_tls_step (channel->server), ==, FPC_DISUM_TLS_ERROR_HANDSHAKE);
  /* The diagnostic must name the alert, not an internal failure. */
  g_assert_nonnull (fpc_disum_tls_error_text (channel->server));
  g_assert_cmpstr (strstr (fpc_disum_tls_error_text (channel->server), "alert"), !=, NULL);

  channel_free (channel);
}

static void
test_partial_record_then_complete (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  guint8 first_flight[4096];
  guint8 output[FPC_DISUM_TLS_DATA_MAX];
  int got;

  /* Capture the ClientHello and deliver only its first bytes. */
  ERR_clear_error ();
  (void)SSL_do_handshake (channel->client);
  got = BIO_read (channel->client_wbio, first_flight, sizeof first_flight);
  g_assert_cmpint (got, >, 10);

  g_assert_true (fpc_disum_tls_feed (channel->server, first_flight, 10));

  /* A stalled channel keeps asking for input and produces nothing. */
  g_assert_cmpint (fpc_disum_tls_step (channel->server), ==, FPC_DISUM_TLS_WANT_READ);
  g_assert_cmpint (fpc_disum_tls_step (channel->server), ==, FPC_DISUM_TLS_WANT_READ);
  g_assert_cmpuint (fpc_disum_tls_take_output (channel->server, output, sizeof output), ==, 0);
  g_assert_false (fpc_disum_tls_is_established (channel->server));
  g_assert_null (fpc_disum_tls_error_text (channel->server));

  /* The remainder of the same record still completes the handshake. */
  g_assert_true (fpc_disum_tls_feed (channel->server, first_flight + 10, got - 10));
  drive (channel, FPC_DISUM_TLS_DATA_MAX);
  g_assert_true (fpc_disum_tls_is_established (channel->server));

  channel_free (channel);
}

static void
test_cancel_mid_handshake (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  guint8 first_flight[4096];
  Channel *cancelled = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  Channel *fresh;
  int got;

  ERR_clear_error ();
  (void)SSL_do_handshake (cancelled->client);
  got = BIO_read (cancelled->client_wbio, first_flight, sizeof first_flight);
  g_assert_cmpint (got, >, 0);
  g_assert_true (fpc_disum_tls_feed (cancelled->server, first_flight, 20));
  g_assert_cmpint (fpc_disum_tls_step (cancelled->server), ==, FPC_DISUM_TLS_WANT_READ);

  /* Cancelling mid-handshake must leave nothing behind (ASan checks the rest). */
  channel_free (cancelled);

  fresh = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  drive (fresh, FPC_DISUM_TLS_DATA_MAX);
  g_assert_true (fpc_disum_tls_is_established (fresh->server));

  channel_free (fresh);
}

static void
test_rejects_short_psk (void)
{
  FpcDisumPsk secret = { { 0 }, 0 };

  secret.psk_len = FPC_DISUM_PSK_LEN - 1;
  g_assert_null (fpc_disum_tls_new (&secret));

  secret.psk_len = FPC_DISUM_PSK_LEN;
  fpc_disum_tls_free (fpc_disum_tls_new (&secret));
}

static void
test_failed_channel_refuses_input (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Not Disum PSK", vector->psk, vector->psk_len);
  guint8 buffer[16] = { 0 };

  drive (channel, FPC_DISUM_TLS_DATA_MAX);
  g_assert_cmpint (fpc_disum_tls_status (channel->server), ==, FPC_DISUM_TLS_ERROR_IDENTITY);

  g_assert_false (fpc_disum_tls_feed (channel->server, buffer, sizeof buffer));
  g_assert_cmpint (fpc_disum_tls_step (channel->server), ==, FPC_DISUM_TLS_ERROR_IDENTITY);
  g_assert_cmpint (fpc_disum_tls_read (channel->server, buffer, sizeof buffer), ==, -1);
  g_assert_cmpint (fpc_disum_tls_write (channel->server, buffer, 1), ==, -1);

  channel_free (channel);
}

static void
test_application_round_trip (void)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  Channel *channel = channel_new ("Disum PSK", vector->psk, vector->psk_len);
  const guint8 uplink[] = "device-to-host payload";
  const guint8 downlink[] = "host-to-device payload";
  guint8 buffer[128];
  gssize n;

  drive (channel, FPC_DISUM_TLS_DATA_MAX);
  g_assert_true (fpc_disum_tls_is_established (channel->server));

  /* Device -> host application data. */
  g_assert_cmpint (SSL_write (channel->client, uplink, sizeof uplink), ==, (int)sizeof uplink);
  move_client_to_server (channel, FPC_DISUM_TLS_DATA_MAX);
  n = fpc_disum_tls_read (channel->server, buffer, sizeof buffer);
  g_assert_cmpint (n, ==, (gssize)sizeof uplink);
  g_assert_cmpmem (buffer, sizeof uplink, uplink, sizeof uplink);

  /* Host -> device application data. */
  g_assert_cmpint (fpc_disum_tls_write (channel->server, downlink, sizeof downlink), ==,
      (gssize)sizeof downlink);
  move_server_to_client (channel, FPC_DISUM_TLS_DATA_MAX);
  n = SSL_read (channel->client, buffer, sizeof buffer);
  g_assert_cmpint (n, ==, (int)sizeof downlink);
  g_assert_cmpmem (buffer, sizeof downlink, downlink, sizeof downlink);

  channel_free (channel);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/channel/handshake", test_handshake);
  g_test_add_func ("/channel/handshake-one-byte-fragments", test_handshake_with_one_byte_fragments);
  g_test_add_func ("/channel/rejects-wrong-identity", test_rejects_wrong_identity);
  g_test_add_func ("/channel/rejects-wrong-psk", test_rejects_wrong_psk);
  g_test_add_func ("/channel/rejects-alert", test_rejects_alert);
  g_test_add_func ("/channel/partial-record-then-complete", test_partial_record_then_complete);
  g_test_add_func ("/channel/cancel-mid-handshake", test_cancel_mid_handshake);
  g_test_add_func ("/channel/rejects-short-psk", test_rejects_short_psk);
  g_test_add_func ("/channel/failed-channel-refuses-input", test_failed_channel_refuses_input);
  g_test_add_func ("/channel/application-round-trip", test_application_round_trip);

  return g_test_run ();
}
