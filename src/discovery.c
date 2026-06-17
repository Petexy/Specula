/* discovery.c - device discovery.
 *
 * Two cooperating strategies, started together:
 *   1. mDNS / DNS-SD via Avahi (PM_HAVE_AVAHI): browse the service types that
 *      Android "wireless debugging" advertises (`_adb-tls-connect._tcp` and
 *      legacy `_adb._tcp`), resolve the first IPv4 hit, and report it. This
 *      runs asynchronously on the GTK main loop (avahi-glib poll).
 *   2. ADB-backed fallback: a worker thread asks the local adb server for
 *      already-known wireless devices and validates saved/manual host:port
 *      endpoints with `adb connect`. Covers paired devices when Avahi is not
 *      available and avoids mistaking Android's temporary pairing port for the
 *      real connect port.
 *
 * Whichever fires first wins; the found-callback is delivered on the main
 * thread. PmSession's handler is idempotent, so a rare double-report is benign.
 */
#include "discovery.h"
#include "adb.h"
#include "pm-config.h"
#include <gio/gio.h>

#if PM_HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/address.h>
#include <avahi-glib/glib-watch.h>
#endif

#define PM_PROBE_INTERVAL_MS 1500  /* wait between attempts        */
#define PM_PROBE_MAX_ATTEMPTS 5    /* total reachability checks before giving up.
                                    * The first failure reveals the UI (see
                                    * probe_failed_cb); the remaining attempts are
                                    * quiet retries that can still upgrade to a
                                    * live mirror if the phone comes online.    */

struct _PmDiscovery {
  PmDeviceInfo        target;       /* owned copy */
  PmDiscoveryFoundCb  cb;
  gpointer            user_data;
  PmDiscoveryProbeFailedCb probe_failed_cb;
  gpointer            probe_failed_user_data;

  /* ADB-backed fallback */
  GThread            *thread;
  gint                stop;         /* atomic */

  /* mDNS (main thread) */
#if PM_HAVE_AVAHI
  AvahiGLibPoll      *glib_poll;
  AvahiClient        *client;
  AvahiServiceBrowser *browsers[2];
#endif
  gint                found;        /* atomic guard for the mDNS path */
};

/* --- main-thread delivery ------------------------------------------------- */

/* Called on the main thread. Reports a reachable device exactly once for the
 * mDNS path (the probe path is naturally one-shot per thread). */
static void
emit_found_main (PmDiscovery *self, const char *host, guint16 port, const char *name)
{
  if (!g_atomic_int_compare_and_exchange (&self->found, 0, 1))
    return;

  PmDeviceInfo info = {
    .serial = self->target.serial,
    .name   = (char *) (name ? name : self->target.name),
    .host   = (char *) host,
    .port   = port,
  };
  self->cb (&info, self->user_data);
}

/* --- ADB fallback (worker thread) ----------------------------------------- */

typedef struct {
  PmDiscoveryFoundCb cb;
  gpointer           user_data;
  PmDeviceInfo       info;
} FoundPayload;

static gboolean
deliver_found (gpointer data)
{
  FoundPayload *p = data;
  p->cb (&p->info, p->user_data);
  return G_SOURCE_REMOVE;
}

static void
found_payload_free (gpointer data)
{
  FoundPayload *p = data;
  /* Balances the ref taken in report_found_from_thread; runs exactly once
   * whether the idle was dispatched or discarded. */
  g_clear_object (&p->user_data);
  pm_device_info_clear (&p->info);
  g_free (p);
}

static gboolean
report_found_from_thread (PmDiscovery        *self,
                          const PmDeviceInfo *info)
{
  if (!g_atomic_int_compare_and_exchange (&self->found, 0, 1))
    return FALSE;

  FoundPayload *p = g_new0 (FoundPayload, 1);
  p->cb = self->cb;
  /* The callback's user_data is the PmSession (a GObject); hold a ref so a
   * late-dispatched idle can't land on a finalized session (use-after-free).
   * The session's handler is a no-op once it has left SEARCHING. */
  p->user_data = self->user_data ? g_object_ref (self->user_data) : NULL;
  p->info.serial = g_strdup (info->serial);
  p->info.name   = g_strdup (info->name);
  p->info.host   = g_strdup (info->host);
  p->info.port   = info->port;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              deliver_found, p, found_payload_free);
  return TRUE;
}

typedef struct {
  PmDiscoveryProbeFailedCb cb;
  gpointer                 user_data;
} ProbeFailedPayload;

static gboolean
deliver_probe_failed (gpointer data)
{
  ProbeFailedPayload *p = data;
  p->cb (p->user_data);
  return G_SOURCE_REMOVE;
}

static void
probe_failed_payload_free (gpointer data)
{
  ProbeFailedPayload *p = data;
  /* Balances the ref taken when the payload is posted (see probe_thread). */
  g_clear_object (&p->user_data);
  g_free (p);
}

static gpointer
probe_thread (gpointer data)
{
  PmDiscovery *self = data;
  gboolean reported_first_failure = FALSE;
  int attempts = 0;

  while (!g_atomic_int_get (&self->stop)) {
    PmDeviceInfo adb_info = { 0 };
    if (pm_adb_find_wireless_device (&adb_info, NULL)) {
      gboolean reported = report_found_from_thread (self, &adb_info);
      pm_device_info_clear (&adb_info);
      if (reported)
        break;
    } else
      pm_device_info_clear (&adb_info);

    if (self->target.host != NULL &&
        pm_adb_connect (self->target.host, self->target.port, NULL)) {
      if (report_found_from_thread (self, &self->target))
        break;
    }

    if (g_atomic_int_get (&self->found))
      break;

    /* First failure: surface the UI while background retries continue. */
    if (self->target.host != NULL &&
        !reported_first_failure && self->probe_failed_cb != NULL) {
      reported_first_failure = TRUE;
      ProbeFailedPayload *p = g_new0 (ProbeFailedPayload, 1);
      p->cb = self->probe_failed_cb;
      /* user_data is the PmSession (a GObject); ref it so a late idle can't
       * fire on a finalized session. The handler no-ops outside SEARCHING. */
      p->user_data = self->probe_failed_user_data
                       ? g_object_ref (self->probe_failed_user_data) : NULL;
      g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                  deliver_probe_failed, p, probe_failed_payload_free);
    }

    /* Stop after a bounded number of reachability checks instead of probing
     * until the discovery timeout. */
    if (++attempts >= PM_PROBE_MAX_ATTEMPTS)
      break;

    for (int i = 0; i < PM_PROBE_INTERVAL_MS / 50 && !g_atomic_int_get (&self->stop); i++)
      g_usleep (50 * 1000);
  }
  return NULL;
}

/* --- mDNS via Avahi (main thread) ----------------------------------------- */

#if PM_HAVE_AVAHI
static void
resolve_cb (AvahiServiceResolver *r,
            AvahiIfIndex interface, AvahiProtocol protocol,
            AvahiResolverEvent event,
            const char *name, const char *type, const char *domain,
            const char *host_name, const AvahiAddress *address,
            uint16_t port, AvahiStringList *txt,
            AvahiLookupResultFlags flags, void *userdata)
{
  PmDiscovery *self = userdata;

  if (event == AVAHI_RESOLVER_FOUND && address->proto == AVAHI_PROTO_INET) {
    char addr[AVAHI_ADDRESS_STR_MAX];
    avahi_address_snprint (addr, sizeof addr, address);
    g_debug ("discovery: mDNS resolved %s -> %s:%u", name, addr, port);
    emit_found_main (self, addr, port, name);
  }

  avahi_service_resolver_free (r);   /* one-shot */
}

static void
browse_cb (AvahiServiceBrowser *b,
           AvahiIfIndex interface, AvahiProtocol protocol,
           AvahiBrowserEvent event,
           const char *name, const char *type, const char *domain,
           AvahiLookupResultFlags flags, void *userdata)
{
  PmDiscovery *self = userdata;

  if (event != AVAHI_BROWSER_NEW)
    return;

  /* Kick off resolution; the resolver frees itself in resolve_cb. */
  avahi_service_resolver_new (self->client, interface, protocol,
                              name, type, domain,
                              AVAHI_PROTO_INET, 0, resolve_cb, self);
}

static void
client_cb (AvahiClient *client, AvahiClientState state, void *userdata)
{
  PmDiscovery *self = userdata;

  if (state != AVAHI_CLIENT_S_RUNNING)
    return;
  if (self->browsers[0] != NULL)
    return;   /* already browsing */

  static const char *types[2] = { "_adb-tls-connect._tcp", "_adb._tcp" };
  for (int i = 0; i < 2; i++) {
    self->browsers[i] =
      avahi_service_browser_new (client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET,
                                 types[i], NULL, 0, browse_cb, self);
  }
}

static void
avahi_start (PmDiscovery *self)
{
  self->glib_poll = avahi_glib_poll_new (NULL, G_PRIORITY_DEFAULT);

  int err = 0;
  self->client = avahi_client_new (avahi_glib_poll_get (self->glib_poll),
                                   0, client_cb, self, &err);
  if (self->client == NULL) {
    g_message ("discovery: mDNS unavailable (%s); using adb fallback only",
               avahi_strerror (err));
    g_clear_pointer (&self->glib_poll, avahi_glib_poll_free);
  }
}

static void
avahi_stop (PmDiscovery *self)
{
  for (int i = 0; i < 2; i++)
    g_clear_pointer (&self->browsers[i], avahi_service_browser_free);
  g_clear_pointer (&self->client, avahi_client_free);
  g_clear_pointer (&self->glib_poll, avahi_glib_poll_free);
}
#endif /* PM_HAVE_AVAHI */

/* --- public API ----------------------------------------------------------- */

PmDiscovery *
pm_discovery_new (const PmDeviceInfo *target,
                  PmDiscoveryFoundCb  cb,
                  gpointer            user_data)
{
  g_return_val_if_fail (target != NULL, NULL);

  PmDiscovery *self = g_new0 (PmDiscovery, 1);
  self->target.serial = g_strdup (target->serial);
  self->target.name   = g_strdup (target->name);
  self->target.host   = g_strdup (target->host);
  self->target.port   = target->port ? target->port : 5555;
  self->cb = cb;
  self->user_data = user_data;
  return self;
}

void
pm_discovery_set_probe_failed_cb (PmDiscovery              *self,
                                  PmDiscoveryProbeFailedCb  cb,
                                  gpointer                  user_data)
{
  g_return_if_fail (self != NULL);

  self->probe_failed_cb = cb;
  self->probe_failed_user_data = user_data;
}

void
pm_discovery_start (PmDiscovery *self)
{
  g_return_if_fail (self != NULL);
  g_atomic_int_set (&self->stop, 0);
  g_atomic_int_set (&self->found, 0);

#if PM_HAVE_AVAHI
  avahi_start (self);
#endif

  /* Ask adb for already-paired wireless devices in parallel with the direct
   * probe. This matters when Android has been paired with this PC but the app
   * doesn't have a saved host:port yet, or when Avahi isn't available. */
  if (self->thread == NULL)
    self->thread = g_thread_new ("pm-discovery", probe_thread, self);
}

void
pm_discovery_stop (PmDiscovery *self)
{
  if (self == NULL)
    return;

#if PM_HAVE_AVAHI
  avahi_stop (self);
#endif

  if (self->thread != NULL) {
    g_atomic_int_set (&self->stop, 1);
    g_thread_join (self->thread);
    self->thread = NULL;
  }
}

void
pm_discovery_free (PmDiscovery *self)
{
  if (self == NULL)
    return;
  pm_discovery_stop (self);
  pm_device_info_clear (&self->target);
  g_free (self);
}
