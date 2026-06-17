/* pinstore.c - encrypted, MAC-keyed lockscreen-PIN storage. See pinstore.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* O_CLOEXEC under -std=c11 */
#endif
#include "pinstore.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#define PM_PIN_PENDING_GROUP "pending"
#define PM_PIN_SECRET_KEY    "secret"
/* Bump if the on-disk crypto layout ever changes; old blobs then fail their MAC
 * check and are treated as absent rather than mis-decrypted. */
#define PM_PIN_ENC_INFO      "specula-pin-enc-v1"
#define PM_PIN_MAC_INFO      "specula-pin-mac-v1"
#define PM_PIN_NONCE_LEN     16
#define PM_PIN_TAG_LEN       32   /* SHA-256 */

/* ---- key derivation ------------------------------------------------------ */

/* A stable per-machine identifier. /etc/machine-id is the standard; fall back to
 * the D-Bus one and finally the hostname so key derivation never fails (a weaker
 * fallback just means a weaker-but-still-seamless key on unusual systems). */
static char *
machine_id (void)
{
  g_autofree char *contents = NULL;
  if (g_file_get_contents ("/etc/machine-id", &contents, NULL, NULL) ||
      g_file_get_contents ("/var/lib/dbus/machine-id", &contents, NULL, NULL))
    return g_strdup (g_strstrip (contents));
  return g_strdup (g_get_host_name ());
}

/* Derive a 32-byte subkey: HMAC-SHA256(master, info), where master is
 * SHA256(app_salt || machine_id || username). `out` must hold 32 bytes. */
static void
derive_subkey (const char *info, guint8 out[32])
{
  static const char app_salt[] = "io.github.petexy.Specula/pinstore";

  g_autofree char *mid = machine_id ();
  const char *user = g_get_user_name ();

  /* master = SHA256(salt || mid || user) */
  g_autoptr (GChecksum) sha = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (sha, (const guchar *) app_salt, sizeof app_salt);
  g_checksum_update (sha, (const guchar *) mid, strlen (mid));
  g_checksum_update (sha, (const guchar *) user, strlen (user));
  guint8 master[32];
  gsize master_len = sizeof master;
  g_checksum_get_digest (sha, master, &master_len);

  g_autoptr (GHmac) hmac = g_hmac_new (G_CHECKSUM_SHA256, master, master_len);
  g_hmac_update (hmac, (const guchar *) info, strlen (info));
  gsize out_len = 32;
  g_hmac_get_digest (hmac, out, &out_len);

  /* master held key material; don't leave it on the stack. */
  memset (master, 0, sizeof master);
}

/* ---- stream cipher (HMAC-SHA256 in counter mode) ------------------------- */

/* XOR `len` keystream bytes into `data`. keystream block c (32 bytes) =
 * HMAC-SHA256(enc_key, nonce || big-endian uint32 c). Symmetric: the same call
 * encrypts and decrypts. */
static void
xor_keystream (const guint8 enc_key[32],
               const guint8 nonce[PM_PIN_NONCE_LEN],
               guint8       *data,
               gsize         len)
{
  for (guint32 counter = 0, off = 0; off < len; counter++) {
    g_autoptr (GHmac) hmac = g_hmac_new (G_CHECKSUM_SHA256, enc_key, 32);
    g_hmac_update (hmac, nonce, PM_PIN_NONCE_LEN);
    guint8 be[4] = { counter >> 24, counter >> 16, counter >> 8, counter };
    g_hmac_update (hmac, be, sizeof be);

    guint8 block[32];
    gsize block_len = sizeof block;
    g_hmac_get_digest (hmac, block, &block_len);

    for (gsize i = 0; i < block_len && off < len; i++, off++)
      data[off] ^= block[i];
    memset (block, 0, sizeof block);
  }
}

/* tag = HMAC-SHA256(mac_key, nonce || ct). `out` must hold 32 bytes. */
static void
compute_tag (const guint8 mac_key[32],
             const guint8 nonce[PM_PIN_NONCE_LEN],
             const guint8 *ct,
             gsize         ct_len,
             guint8        out[PM_PIN_TAG_LEN])
{
  g_autoptr (GHmac) hmac = g_hmac_new (G_CHECKSUM_SHA256, mac_key, 32);
  g_hmac_update (hmac, nonce, PM_PIN_NONCE_LEN);
  g_hmac_update (hmac, ct, ct_len);
  gsize len = PM_PIN_TAG_LEN;
  g_hmac_get_digest (hmac, out, &len);
}

static void
fill_random (guint8 *buf, gsize len)
{
#ifdef G_OS_UNIX
  int fd = open ("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    gsize off = 0;
    while (off < len) {
      gssize n = read (fd, buf + off, len - off);
      if (n <= 0)
        break;
      off += (gsize) n;
    }
    close (fd);
    if (off == len)
      return;
  }
#endif
  /* Fallback: a unique nonce is all that correctness needs (the key is the
   * real secret), so GLib's PRNG is acceptable if /dev/urandom is unavailable. */
  for (gsize i = 0; i < len; i++)
    buf[i] = (guint8) g_random_int_range (0, 256);
}

/* Encrypt `pin` → base64(nonce || ciphertext || tag). Caller frees. */
static char *
encrypt_pin (const char *pin)
{
  guint8 enc_key[32], mac_key[32];
  derive_subkey (PM_PIN_ENC_INFO, enc_key);
  derive_subkey (PM_PIN_MAC_INFO, mac_key);

  gsize pin_len = strlen (pin);
  gsize blob_len = PM_PIN_NONCE_LEN + pin_len + PM_PIN_TAG_LEN;
  g_autofree guint8 *blob = g_malloc (blob_len);

  guint8 *nonce = blob;
  guint8 *ct    = blob + PM_PIN_NONCE_LEN;
  guint8 *tag   = blob + PM_PIN_NONCE_LEN + pin_len;

  fill_random (nonce, PM_PIN_NONCE_LEN);
  memcpy (ct, pin, pin_len);
  xor_keystream (enc_key, nonce, ct, pin_len);
  compute_tag (mac_key, nonce, ct, pin_len, tag);

  char *b64 = g_base64_encode (blob, blob_len);

  memset (blob, 0, blob_len);
  memset (enc_key, 0, sizeof enc_key);
  memset (mac_key, 0, sizeof mac_key);
  return b64;
}

/* Decrypt a base64 blob produced by encrypt_pin(). Returns plaintext or NULL on
 * a bad blob / failed MAC (tamper or wrong key). Caller frees. */
static char *
decrypt_pin (const char *b64)
{
  gsize blob_len = 0;
  g_autofree guint8 *blob = g_base64_decode (b64, &blob_len);
  if (blob == NULL || blob_len < PM_PIN_NONCE_LEN + PM_PIN_TAG_LEN)
    return NULL;

  guint8 enc_key[32], mac_key[32];
  derive_subkey (PM_PIN_ENC_INFO, enc_key);
  derive_subkey (PM_PIN_MAC_INFO, mac_key);

  gsize pin_len = blob_len - PM_PIN_NONCE_LEN - PM_PIN_TAG_LEN;
  guint8 *nonce = blob;
  guint8 *ct    = blob + PM_PIN_NONCE_LEN;
  guint8 *tag   = blob + PM_PIN_NONCE_LEN + pin_len;

  guint8 expect[PM_PIN_TAG_LEN];
  compute_tag (mac_key, nonce, ct, pin_len, expect);

  char *plain = NULL;
  /* Constant-time compare so a mismatch leaks nothing via timing. */
  guint8 diff = 0;
  for (gsize i = 0; i < PM_PIN_TAG_LEN; i++)
    diff |= (guint8) (tag[i] ^ expect[i]);
  if (diff == 0) {
    xor_keystream (enc_key, nonce, ct, pin_len);
    plain = g_strndup ((const char *) ct, pin_len);
  }

  memset (blob, 0, blob_len);
  memset (enc_key, 0, sizeof enc_key);
  memset (mac_key, 0, sizeof mac_key);
  memset (expect, 0, sizeof expect);
  return plain;
}

/* ---- file + group helpers ------------------------------------------------ */

static char *
store_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "specula", "pins.ini", NULL);
}

/* Normalise a MAC to uppercase, colon-separated form for use as a stable group
 * name (e.g. "aa-bb-..." and "AA:BB:..." map to the same key). Returns NULL for
 * an empty/NULL input. Caller frees. */
static char *
normalize_mac (const char *mac)
{
  if (mac == NULL)
    return NULL;

  GString *s = g_string_new (NULL);
  for (const char *p = mac; *p; p++) {
    if (g_ascii_isxdigit (*p))
      g_string_append_c (s, g_ascii_toupper (*p));
  }
  if (s->len == 0) {
    g_string_free (s, TRUE);
    return NULL;
  }

  /* Re-insert colons every two hex digits. */
  GString *out = g_string_new (NULL);
  for (gsize i = 0; i < s->len; i++) {
    if (i > 0 && i % 2 == 0)
      g_string_append_c (out, ':');
    g_string_append_c (out, s->str[i]);
  }
  g_string_free (s, TRUE);
  return g_string_free (out, FALSE);
}

static GKeyFile *
load_store (void)
{
  GKeyFile *kf = g_key_file_new ();
  g_autofree char *path = store_path ();
  g_key_file_load_from_file (kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return kf;   /* empty keyfile if it does not exist yet */
}

static gboolean
save_store (GKeyFile *kf, GError **error)
{
  g_autofree char *path = store_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                 "Could not create config dir: %s", dir);
    return FALSE;
  }

  if (!g_key_file_save_to_file (kf, path, error))
    return FALSE;

  /* The file holds (encrypted) secrets: keep it owner-only. g_key_file_save_to_file
   * honours the umask, so tighten it explicitly afterwards. */
  if (g_chmod (path, 0600) != 0)
    g_warning ("pinstore: could not chmod %s to 0600", path);
  return TRUE;
}

static gboolean
set_group_secret (const char *group, const char *pin, GError **error)
{
  g_autoptr (GKeyFile) kf = load_store ();

  if (pin == NULL || *pin == '\0') {
    g_key_file_remove_group (kf, group, NULL);
  } else {
    g_autofree char *enc = encrypt_pin (pin);
    g_key_file_set_string (kf, group, PM_PIN_SECRET_KEY, enc);
  }
  return save_store (kf, error);
}

static char *
get_group_secret (const char *group)
{
  g_autoptr (GKeyFile) kf = load_store ();
  g_autofree char *enc = g_key_file_get_string (kf, group, PM_PIN_SECRET_KEY, NULL);
  if (enc == NULL)
    return NULL;
  return decrypt_pin (enc);
}

/* ---- public API ---------------------------------------------------------- */

gboolean
pm_pinstore_set (const char *mac, const char *pin, GError **error)
{
  g_autofree char *group = normalize_mac (mac);
  if (group == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "invalid MAC address");
    return FALSE;
  }
  return set_group_secret (group, pin, error);
}

char *
pm_pinstore_get (const char *mac)
{
  g_autofree char *group = normalize_mac (mac);
  if (group == NULL)
    return NULL;
  return get_group_secret (group);
}

gboolean
pm_pinstore_has (const char *mac)
{
  g_autofree char *group = normalize_mac (mac);
  if (group == NULL)
    return FALSE;
  g_autoptr (GKeyFile) kf = load_store ();
  return g_key_file_has_group (kf, group) &&
         g_key_file_has_key (kf, group, PM_PIN_SECRET_KEY, NULL);
}

void
pm_pinstore_remove (const char *mac)
{
  g_autofree char *group = normalize_mac (mac);
  if (group == NULL)
    return;
  g_autoptr (GError) error = NULL;
  if (!set_group_secret (group, NULL, &error))
    g_warning ("pinstore: could not remove PIN: %s", error->message);
}

gboolean
pm_pinstore_set_pending (const char *pin, GError **error)
{
  return set_group_secret (PM_PIN_PENDING_GROUP, pin, error);
}

gboolean
pm_pinstore_commit_pending (const char *mac)
{
  g_autofree char *group = normalize_mac (mac);
  if (group == NULL)
    return FALSE;

  g_autofree char *pending = get_group_secret (PM_PIN_PENDING_GROUP);
  if (pending == NULL)
    return FALSE;

  g_autoptr (GError) error = NULL;
  gboolean ok = set_group_secret (group, pending, &error);
  if (!ok) {
    g_warning ("pinstore: could not commit pending PIN: %s", error->message);
  } else {
    /* Clear the pending slot only after the MAC group is safely written. */
    g_clear_error (&error);
    if (!set_group_secret (PM_PIN_PENDING_GROUP, NULL, &error))
      g_warning ("pinstore: committed PIN but could not clear pending: %s",
                 error->message);
  }

  memset (pending, 0, strlen (pending));
  return ok;
}
