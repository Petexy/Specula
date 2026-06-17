/* net.c - blocking TCP client implementation. */
#include "net.h"
#include <netinet/in.h>
#include <netinet/tcp.h>

struct _PmNet {
  GSocketClient     *client;
  GSocketConnection *conn;
  GInputStream      *in;
  GOutputStream     *out;
  GCancellable      *cancellable;  /* tripped by pm_net_close to wake blocking I/O */
};

PmNet *
pm_net_new (void)
{
  PmNet *self = g_new0 (PmNet, 1);
  self->client = g_socket_client_new ();
  /* Low latency over throughput: this is interactive screen control. */
  g_socket_client_set_tls (self->client, FALSE);
  /* All reads/writes pass this cancellable so another thread can abort a
   * blocking call (e.g. when the phone's Wi-Fi drops and the socket goes
   * half-open) by calling pm_net_close. Without it, a blocked read never
   * returns and a subsequent g_thread_join on the main thread hangs the UI. */
  self->cancellable = g_cancellable_new ();
  return self;
}

gboolean
pm_net_connect (PmNet *self, const char *host, guint16 port, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);

  self->conn = g_socket_client_connect_to_host (self->client, host, port,
                                                self->cancellable, error);
  if (self->conn == NULL)
    return FALSE;

  /* Disable Nagle to send input events immediately. */
  GSocket *sock = g_socket_connection_get_socket (self->conn);
  g_socket_set_option (sock, IPPROTO_TCP, TCP_NODELAY, 1, NULL);

  self->in  = g_io_stream_get_input_stream  (G_IO_STREAM (self->conn));
  self->out = g_io_stream_get_output_stream (G_IO_STREAM (self->conn));
  return TRUE;
}

gssize
pm_net_read (PmNet *self, guint8 *buf, gsize len, GError **error)
{
  g_return_val_if_fail (self != NULL && self->in != NULL, -1);
  return g_input_stream_read (self->in, buf, len, self->cancellable, error);
}

gboolean
pm_net_read_exact (PmNet *self, guint8 *buf, gsize len, GError **error)
{
  g_return_val_if_fail (self != NULL && self->in != NULL, FALSE);

  gsize got = 0;
  if (!g_input_stream_read_all (self->in, buf, len, &got, self->cancellable, error))
    return FALSE;

  if (got != len) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                 "connection closed while reading stream data");
    return FALSE;
  }

  return TRUE;
}

gboolean
pm_net_write_all (PmNet *self, const guint8 *buf, gsize len, GError **error)
{
  g_return_val_if_fail (self != NULL && self->out != NULL, FALSE);

  gsize wrote = 0;
  return g_output_stream_write_all (self->out, buf, len, &wrote, self->cancellable, error);
}

int
pm_net_get_fd (PmNet *self)
{
  if (self == NULL || self->conn == NULL)
    return -1;
  GSocket *sock = g_socket_connection_get_socket (self->conn);
  return sock ? g_socket_get_fd (sock) : -1;
}

void
pm_net_close (PmNet *self)
{
  if (self == NULL)
    return;

  /* Wake any blocking read/write in progress on the worker thread first, so a
   * caller (e.g. the main thread in pm_session_stop) can safely join the
   * worker afterwards instead of deadlocking on a half-open socket. */
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->conn == NULL)
    return;
  g_io_stream_close (G_IO_STREAM (self->conn), NULL, NULL);
  g_clear_object (&self->conn);
  self->in = NULL;
  self->out = NULL;
}

void
pm_net_free (PmNet *self)
{
  if (self == NULL)
    return;
  pm_net_close (self);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->client);
  g_free (self);
}
