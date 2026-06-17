/* pinstore.h - at-rest storage for lockscreen PINs, keyed by device MAC.
 *
 * The PIN must be *recoverable* (it is replayed into the phone via
 * `adb shell input text`), so it cannot be a one-way hash. Instead each PIN is
 * encrypted with a stream cipher built from HMAC-SHA256 (encrypt-then-MAC) under
 * a key derived from this machine's id + the local username + a fixed app salt.
 * The ciphertext lives in a separate file under $XDG_CONFIG_HOME/specula/
 * (mode 0600), grouped by the device's Wi-Fi MAC address so it survives the
 * device's IP changing on a DHCP lease.
 *
 * Threat model: this is seamless and never prompts, which means anything running
 * as the same Unix user can derive the same key and recover the PIN. It is NOT
 * meant to resist a local attacker with your account. What it buys is that the
 * PIN is never on disk in cleartext, so it survives backup archives, synced
 * dotfiles, and casual snooping without leaking. An OS keyring would be stronger
 * but is not prompt-free in the general case.
 *
 * Keyed by MAC: the dialog collects the PIN before the device's MAC is known
 * (the MAC is read over adb at connect time), so the PIN is first stashed as a
 * single "pending" entry and committed to its MAC group on the first connect.
 *
 * All functions are blocking file I/O and safe to call from any thread (no
 * shared state beyond the on-disk file).
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Encrypt and persist `pin` for `mac` (any case / separator-normalised form).
 * A NULL or empty `pin` removes any stored entry for that MAC instead. */
gboolean pm_pinstore_set (const char *mac, const char *pin, GError **error);

/* Return the decrypted PIN stored for `mac`, or NULL if none / on tamper.
 * Caller owns the string; it is plaintext, so free it promptly (the store zeroes
 * its own working buffers). */
char *pm_pinstore_get (const char *mac);

/* TRUE if a PIN is stored for `mac`. */
gboolean pm_pinstore_has (const char *mac);

/* Forget the PIN stored for `mac` (no-op if none). */
void pm_pinstore_remove (const char *mac);

/* Stash a PIN entered before the MAC is known. A NULL/empty `pin` clears any
 * pending entry. Overwrites any previous pending PIN. */
gboolean pm_pinstore_set_pending (const char *pin, GError **error);

/* If a pending PIN exists, move it to `mac`'s group and clear the pending slot.
 * Returns TRUE if a pending PIN was committed, FALSE if there was none. */
gboolean pm_pinstore_commit_pending (const char *mac);

G_END_DECLS
