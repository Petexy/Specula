/* discovery.c - device discovery.
 *
 * Two cooperating strategies, started together:
 *   1. mDNS / DNS-SD via Avahi (PM_HAVE_AVAHI): browse the service types that
 *      Android "wireless debugging" advertises (`_adb-tls-connect._tcp` and
 *      legacy `_adb._tcp`), resolve the first IPv4 hit, and report it. This
 *      runs asynchronously on the GTK main loop (avahi-glib poll).
 *   2. ADB-backed fallback: a worker thread asks the local adb server for
 *      already-known wireless devices, validates saved/manual host:port
 *      endpoints with `adb connect`, and - when pairing supplied only the host
 *      IP - scans that host for Android's dynamic wireless-debugging port.
 *      Covers paired devices when Avahi is not available and avoids mistaking
 *      Android's temporary pairing port for the real connect port.
 *
 * Whichever fires first wins; the found-callback is delivered on the main
 * thread. PmSession's handler is idempotent, so a rare double-report is benign.
 */
#define _POSIX_C_SOURCE 200112L

#include "discovery.h"
#include "adb.h"
#include "pm-config.h"
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
#define PM_PORT_SCAN_TIMEOUT_MS 20
#define PM_PORT_SCAN_LOW 30000
#define PM_PORT_SCAN_HIGH 49999
/* A single mDNS hit is one known endpoint (not a 20k-port sweep), so it can
 * afford a longer confirm than the port scan: long enough for a LAN round trip,
 * short enough that an unreachable/cached record bails the searching phase
 * quickly. */
#define PM_MDNS_VERIFY_TIMEOUT_MS 300

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

static void
set_sockaddr_port (struct sockaddr *sa, guint16 port)
{
  if (sa->sa_family == AF_INET)
    ((struct sockaddr_in *) sa)->sin_port = htons (port);
  else if (sa->sa_family == AF_INET6)
    ((struct sockaddr_in6 *) sa)->sin6_port = htons (port);
}

static gboolean
tcp_port_open (const struct addrinfo *addrs, guint16 port, int timeout_ms)
{
  for (const struct addrinfo *ai = addrs; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_socktype != SOCK_STREAM)
      continue;

    struct sockaddr_storage ss;
    if (ai->ai_addrlen > sizeof ss)
      continue;
    memcpy (&ss, ai->ai_addr, ai->ai_addrlen);
    set_sockaddr_port ((struct sockaddr *) &ss, port);

    int fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;

    int fd_flags = fcntl (fd, F_GETFD, 0);
    if (fd_flags >= 0)
      fcntl (fd, F_SETFD, fd_flags | FD_CLOEXEC);

    int flags = fcntl (fd, F_GETFL, 0);
    if (flags >= 0)
      fcntl (fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect (fd, (struct sockaddr *) &ss, ai->ai_addrlen);
    if (rc == 0) {
      close (fd);
      return TRUE;
    }

    if (errno == EINPROGRESS) {
      struct pollfd pfd = { .fd = fd, .events = POLLOUT };
      if (poll (&pfd, 1, timeout_ms) > 0) {
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
          close (fd);
          return TRUE;
        }
      }
    }

    close (fd);
  }

  return FALSE;
}

static gboolean
try_report_target_port (PmDiscovery *self,
                        guint16      port)
{
  if (g_atomic_int_get (&self->stop))
    return FALSE;
  if (self->target.host == NULL || port == 0)
    return FALSE;

  if (!pm_adb_connect (self->target.host, port, NULL))
    return FALSE;

  PmDeviceInfo info = {
    .name   = self->target.name,
    .host   = self->target.host,
    .port   = port,
  };
  return report_found_from_thread (self, &info);
}

static gboolean
scan_pairing_host (PmDiscovery *self)
{
  if (self->target.host == NULL)
    return FALSE;

  g_message ("discovery: scanning %s for wireless debugging port", self->target.host);

  struct addrinfo hints = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *addrs = NULL;
  if (getaddrinfo (self->target.host, NULL, &hints, &addrs) != 0)
    return FALSE;

  if (tcp_port_open (addrs, 5555, PM_PORT_SCAN_TIMEOUT_MS) &&
      try_report_target_port (self, 5555)) {
    freeaddrinfo (addrs);
    return TRUE;
  }

  for (guint port = PM_PORT_SCAN_LOW;
       port <= PM_PORT_SCAN_HIGH && !g_atomic_int_get (&self->stop);
       port++) {
    if (!tcp_port_open (addrs, (guint16) port, PM_PORT_SCAN_TIMEOUT_MS))
      continue;
    if (try_report_target_port (self, (guint16) port)) {
      freeaddrinfo (addrs);
      return TRUE;
    }
  }

  freeaddrinfo (addrs);
  return FALSE;
}

static gpointer
probe_thread (gpointer data)
{
  PmDiscovery *self = data;
  gboolean reported_first_failure = FALSE;
  int attempts = 0;

  while (!g_atomic_int_get (&self->stop)) {
    /* A saved/manual endpoint is the user's strongest signal. Try it before
     * accepting whatever adb may already know from previous sessions. */
    if (self->target.host != NULL && self->target.port != 0) {
      if (try_report_target_port (self, self->target.port))
        break;
    }

    PmDeviceInfo adb_info = { 0 };
    if (pm_adb_find_wireless_device (&adb_info, NULL)) {
      /* Confirm it connects before committing: `adb mdns services` can list a
       * device that no longer routes. An already-attached device (the `adb
       * devices` path) returns "already connected" and so passes cheaply. */
      gboolean reachable = adb_info.host != NULL &&
                           pm_adb_connect (adb_info.host, adb_info.port, NULL);
      gboolean reported = reachable && report_found_from_thread (self, &adb_info);
      pm_device_info_clear (&adb_info);
      if (reported)
        break;
    } else
      pm_device_info_clear (&adb_info);

    if (self->target.host != NULL) {
      if (scan_pairing_host (self))
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
/* TRUE if a TCP connection to host:port can be opened within timeout_ms. Gates
 * mDNS hits: Avahi can serve a *cached* A-record from a previous network state,
 * so a resolve may hand back an address that no longer routes - committing to it
 * makes the real `adb connect` fail with "No route to host", with no fallback
 * because reporting it tears the verified probe path down. A quick connect probe
 * here keeps the "reported == reachable" contract the probe path already holds,
 * so a stale record is skipped and the probe path (or a fresh mDNS hit) wins. */
static gboolean
endpoint_reachable (const char *host, guint16 port, int timeout_ms)
{
  struct addrinfo hints = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *addrs = NULL;
  if (host == NULL || getaddrinfo (host, NULL, &hints, &addrs) != 0)
    return FALSE;
  gboolean ok = tcp_port_open (addrs, port, timeout_ms);
  freeaddrinfo (addrs);
  return ok;
}

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

    /* Only commit to a resolved endpoint that actually answers - a cached/stale
     * mDNS record (old IP, or a connect port closed since it was advertised)
     * would otherwise be adopted and fail the real connect intermittently. */
    if (endpoint_reachable (addr, port, PM_MDNS_VERIFY_TIMEOUT_MS)) {
      g_debug ("discovery: mDNS resolved %s -> %s:%u (reachable)", name, addr, port);
      emit_found_main (self, addr, port, name);
    } else {
      g_debug ("discovery: mDNS %s -> %s:%u unreachable; skipping stale record",
               name, addr, port);
    }
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
  self->target.name   = g_strdup (target->name);
  self->target.host   = g_strdup (target->host);
  self->target.port   = (target->host == NULL && target->port == 0) ? 5555 : target->port;
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
