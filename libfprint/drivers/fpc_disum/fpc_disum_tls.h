/*
 * FPC Disum (10a5:9201) security channel: GET_TLS_KEY envelope authentication.
 *
 * Reference: docs/CLEANROOM_SPEC.md §8 and appendix B.3-B.4.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* GET_TLS_KEY returns exactly this many bytes on the target firmware. */
#define FPC_DISUM_ENVELOPE_LEN 121
#define FPC_DISUM_ENVELOPE_MAGIC 0x0DEC0DED

/*
 * The first 28 B are seven little-endian u32:
 * magic, key_off, key_len, aad_off, aad_len, sig_off, sig_len.
 * Fields must always be sliced from the header, never from the offsets observed
 * for one sample (SPEC §8).  Bytes 60..76 are unnamed and must not be interpreted.
 */
#define FPC_DISUM_ENVELOPE_KEY_ALIGN 16
#define FPC_DISUM_ENVELOPE_SIGNATURE_LEN 32

/* The target firmware decrypts a 32 B PSK.  The envelope itself only guarantees a
 * block-aligned key range, so callers must check psk_len (SPEC §8). */
#define FPC_DISUM_PSK_LEN 32
#define FPC_DISUM_PSK_MAX_LEN FPC_DISUM_ENVELOPE_LEN

/*
 * First failing check, in the order fixed by SPEC §8:
 * length/magic -> field bounds -> AES -> HMAC.
 */
typedef enum
{
  FPC_DISUM_ENVELOPE_OK = 0,
  /* Programming error (NULL argument); never produced by wire data. */
  FPC_DISUM_ENVELOPE_ERROR_INVALID_ARGUMENT,
  /* Length is not 121 B, or the magic does not match. */
  FPC_DISUM_ENVELOPE_ERROR_TOTAL_LENGTH,
  /* A declared range leaves the envelope, or a declared length rule fails. */
  FPC_DISUM_ENVELOPE_ERROR_FIELD_BOUNDS,
  /* AES-256-CBC (NoPadding) decryption failed. */
  FPC_DISUM_ENVELOPE_ERROR_AES,
  /* The full 32 B HMAC over AAD || encrypted key did not match. */
  FPC_DISUM_ENVELOPE_ERROR_HMAC,
} FpcDisumEnvelopeStatus;

typedef struct
{
  /* Cleared on every failure; psk_len is 0 then. */
  guint8 psk[FPC_DISUM_PSK_MAX_LEN];
  gsize psk_len;
} FpcDisumPsk;

/*
 * Authenticate and decrypt a GET_TLS_KEY envelope.
 *
 * On FPC_DISUM_ENVELOPE_OK the PSK is available in @psk and must be wiped with
 * fpc_disum_psk_clear() once it is no longer needed.  Any other result leaves @psk
 * zeroed.  The PSK is never logged by this module.
 */
FpcDisumEnvelopeStatus fpc_disum_envelope_unseal (
    const guint8 *envelope, gsize len, FpcDisumPsk *psk);

/* Wipe key material.  Safe on a zeroed struct and safe to call twice. */
void fpc_disum_psk_clear (FpcDisumPsk *psk);

/*
 * TLS-PSK channel towards the sensor (SPEC §8).
 *
 * The host is the TLS server; the device is the client.  Only TLS 1.2 with
 * PSK-AES128-CBC-SHA256 is used, the PSK identity must be the reviewed one, and
 * the ServerHello carries a non-empty session id.  The channel is passive: it
 * never blocks and never times out on its own, so the caller decides the
 * no-progress timeout and may simply free the channel to cancel.
 *
 * Byte streams: fpc_disum_tls_feed() consumes the payload of EVT_TLS frames and
 * fpc_disum_tls_take_output() returns the bytes for TLS_DATA fragments.  Neither
 * boundary carries meaning here; records may be split across any number of calls.
 */
#define FPC_DISUM_TLS_DATA_MAX 64

typedef enum
{
  /* Progress was made; call again once more input is available. */
  FPC_DISUM_TLS_OK = 0,
  /* The handshake needs more device bytes. */
  FPC_DISUM_TLS_WANT_READ,
  /* The device did not present the reviewed PSK identity. */
  FPC_DISUM_TLS_ERROR_IDENTITY,
  /* Alert, Finished/MAC mismatch or any other TLS protocol failure. */
  FPC_DISUM_TLS_ERROR_HANDSHAKE,
  /* Memory BIO failure; no TLS state was advanced. */
  FPC_DISUM_TLS_ERROR_IO,
} FpcDisumTlsStatus;

typedef struct _FpcDisumTls FpcDisumTls;

/* Start a server-side channel.  Requires a 32 B PSK; NULL otherwise. */
FpcDisumTls *fpc_disum_tls_new (const FpcDisumPsk *psk);
void fpc_disum_tls_free (FpcDisumTls *tls);

/* Feed device bytes (EVT_TLS payload).  Refused once the channel has failed. */
gboolean fpc_disum_tls_feed (FpcDisumTls *tls, const guint8 *data, gsize len);

/* Drive the handshake with the bytes fed so far. */
FpcDisumTlsStatus fpc_disum_tls_step (FpcDisumTls *tls);
gboolean fpc_disum_tls_is_established (const FpcDisumTls *tls);
FpcDisumTlsStatus fpc_disum_tls_status (const FpcDisumTls *tls);

/* Reason for the last failure, for diagnostics; NULL before a failure.
 * Contains no key material. */
const gchar *fpc_disum_tls_error_text (const FpcDisumTls *tls);

/* Drain host->device bytes; call with FPC_DISUM_TLS_DATA_MAX per TLS_DATA transfer. */
gsize fpc_disum_tls_take_output (FpcDisumTls *tls, guint8 *buffer, gsize max_len);

/* Decrypted application data.  Returns the byte count, 0 if none is ready yet,
 * or -1 on failure. */
gssize fpc_disum_tls_read (FpcDisumTls *tls, guint8 *buffer, gsize max_len);

/* Queue application data towards the device.  Returns the byte count or -1. */
gssize fpc_disum_tls_write (FpcDisumTls *tls, const guint8 *data, gsize len);

G_END_DECLS
