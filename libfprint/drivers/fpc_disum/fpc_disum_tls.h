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

G_END_DECLS
