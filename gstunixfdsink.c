/* GStreamer unix file-descriptor source/sink
 *
 * Copyright (C) 2023 Netflix Inc.
 *  Author: Xavier Claessens <xavier.claessens@collabora.com>
 *
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-unixfdsink
 * @title: unixfdsink
 *
 * Send file-descriptor backed buffers (e.g. memfd, dmabuf) over unix socket to
 * matching unixfdsrc. There can be any number of clients, if none are connected
 * buffers are dropped.
 *
 * Buffers can have any number of #GstMemory, but it is an error if any one of
 * them lacks a file-descriptor.
 *
 * #GstShmAllocator is added into the allocation proposition, which makes
 * most sources write their data into shared memory automatically.
 *
 * ## Example launch lines
 * |[
 * gst-launch-1.0 -v videotestsrc ! unixfdsink socket-path=/tmp/blah
 * gst-launch-1.0 -v unixfdsrc socket-path=/tmp/blah ! autovideosink
 * ]|
 *
 * Since: 1.24
 */

#include "gstunixfd.h"

#include <gst/base/base.h>
#include <gst/allocators/allocators.h>

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#ifdef HAVE_IPC_TARGET_NV
#include <gst/video/video.h>
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "gstnvipcbufferpool.h"
#include <dlfcn.h>
#endif

GST_DEBUG_CATEGORY (unixfdsink_debug);
#define GST_CAT_DEFAULT (unixfdsink_debug)

#ifdef HAVE_IPC_TARGET_NV
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
#define DEFAULT_BUFFER_COPY FALSE
#define DEFAULT_BUFFER_TIMESTAMP_COPY FALSE
#endif

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_TYPE_UNIX_FD_SINK gst_unix_fd_sink_get_type()
G_DECLARE_FINAL_TYPE (GstUnixFdSink, gst_unix_fd_sink, GST, UNIX_FD_SINK,
    GstBaseSink);

typedef struct
{
  GHashTable *buffers;
  GSource *source;
#ifdef HAVE_IPC_TARGET_NV
  GstBufferPool *pool;
  GByteArray *payload;
  GstBuffer *buffer;
  NvBufSurfTransformSyncObj_t syncobj;
#endif
} Client;

struct _GstUnixFdSink
{
  GstBaseSink parent;

  GThread *thread;
  GMainContext *context;
  GMainLoop *loop;

  gchar *socket_path;
  GUnixSocketAddressType socket_type;
  GSocket *socket;
  GSource *source;

  /* GSocket -> Client */
  GHashTable *clients;
  GstCaps *caps;
  gboolean uses_monotonic_clock;
  GByteArray *payload;

#ifdef HAVE_IPC_TARGET_NV
  gboolean nvmm_memory;
  gboolean buffer_copy;
  guint compute_hw;
  gboolean buffer_timestamp_copy;
  gchar *meta_serialization_lib_name;
  void *lib_handle;
  void (*serialize_meta_func)(GstBuffer *buf, guint8 **, guint *);
#endif
};

G_DEFINE_TYPE (GstUnixFdSink, gst_unix_fd_sink, GST_TYPE_BASE_SINK);
#ifdef HAVE_IPC_TARGET_NV
GST_ELEMENT_REGISTER_DEFINE (unixfdsink, "nvunixfdsink", GST_RANK_NONE,
    GST_TYPE_UNIX_FD_SINK);
#else
GST_ELEMENT_REGISTER_DEFINE (unixfdsink, "unixfdsink", GST_RANK_NONE,
    GST_TYPE_UNIX_FD_SINK);
#endif

#define DEFAULT_SOCKET_TYPE G_UNIX_SOCKET_ADDRESS_PATH

enum
{
  PROP_0,
  PROP_SOCKET_PATH,
  PROP_SOCKET_TYPE,
#ifdef HAVE_IPC_TARGET_NV
  PROP_BUFFER_COPY,
  PROP_COMPUTE_HW,
  PROP_BUFFER_TIMESTAMP_COPY,
  PROP_META_SERIALIZATION_LIB_NAME,
#endif
};

#ifdef HAVE_IPC_TARGET_NV
typedef enum {
  GST_UNIXFDSINK_COMPUTE_HW_CPU,
  GST_UNIXFDSINK_COMPUTE_HW_VIC,
} GstUnixFdSinkComputeHw;

#define GST_TYPE_UNIXFDSINK_COMPUTE_HW (gst_unixfdsink_compute_hw_get_type ())

static const GEnumValue compute_hw[] = {
  {GST_UNIXFDSINK_COMPUTE_HW_CPU, "CPU", "CPU"},
  {GST_UNIXFDSINK_COMPUTE_HW_VIC, "VIC", "VIC"},
  {0, NULL, NULL},
};

static GType
gst_unixfdsink_compute_hw_get_type (void)
{
  static GType compute_hw_type = 0;

  if(!compute_hw_type) {
    compute_hw_type = g_enum_register_static ("GstUnixFdSinkComputeHw",
       compute_hw);
  }

  return compute_hw_type;
}
#endif

static void
client_free (Client * client)
{
  g_hash_table_unref (client->buffers);
  g_source_destroy (client->source);
  g_source_unref (client->source);
#ifdef HAVE_IPC_TARGET_NV
  if (client->buffer) {
    gst_buffer_unref(client->buffer);
    client->buffer = NULL;
  }
  if (client->pool) {
    if (gst_buffer_pool_is_active (client->pool)) {
      gst_buffer_pool_set_active (client->pool, FALSE);
    }
    gst_object_unref (client->pool);
    client->pool = NULL;
  }
  if (client->payload) {
    g_clear_pointer (&client->payload, g_byte_array_unref);
  }
  NvBufSurfTransformSyncObjDestroy(&client->syncobj);
#endif
  g_free (client);
}

static void
gst_unix_fd_sink_init (GstUnixFdSink * self)
{
  g_return_if_fail (GST_IS_UNIX_FD_SINK (self));

  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);
  self->clients =
      g_hash_table_new_full (NULL, NULL, g_object_unref,
      (GDestroyNotify) client_free);
}

static void
gst_unix_fd_sink_finalize (GObject * object)
{
  GstUnixFdSink *self = GST_UNIX_FD_SINK (object);

  g_free (self->socket_path);
  g_main_context_unref (self->context);
  g_main_loop_unref (self->loop);
  g_hash_table_unref (self->clients);
#ifdef HAVE_IPC_TARGET_NV
  if (self->meta_serialization_lib_name) {
    g_free (self->meta_serialization_lib_name);
    self->meta_serialization_lib_name = NULL;
  }
#endif

  G_OBJECT_CLASS (gst_unix_fd_sink_parent_class)->finalize (object);
}

static void
gst_unix_fd_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstUnixFdSink *self = GST_UNIX_FD_SINK (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_SOCKET_PATH:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change socket path in NULL or READY state");
        break;
      }
      g_free (self->socket_path);
      self->socket_path = g_value_dup_string (value);
      break;
    case PROP_SOCKET_TYPE:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change socket type in NULL or READY state");
        break;
      }
      self->socket_type = g_value_get_enum (value);
      break;
#ifdef HAVE_IPC_TARGET_NV
    case PROP_BUFFER_COPY:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change buffer copy in NULL or READY state");
        break;
      }
      self->buffer_copy = g_value_get_boolean (value);
      break;
    case PROP_COMPUTE_HW:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change compute hw in NULL or READY state");
        break;
      }
      self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_BUFFER_TIMESTAMP_COPY:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change buffer timestamp copy in NULL or READY state");
        break;
      }
      self->buffer_timestamp_copy = g_value_get_boolean (value);
      break;
    case PROP_META_SERIALIZATION_LIB_NAME:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change meta serialization lib name in NULL or READY state");
        break;
      }
      g_free (self->meta_serialization_lib_name);
      self->meta_serialization_lib_name = (gchar *)g_value_dup_string (value);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

static void
gst_unix_fd_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstUnixFdSink *self = GST_UNIX_FD_SINK (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_SOCKET_PATH:
      g_value_set_string (value, self->socket_path);
      break;
    case PROP_SOCKET_TYPE:
      g_value_set_enum (value, self->socket_type);
      break;
#ifdef HAVE_IPC_TARGET_NV
    case PROP_BUFFER_COPY:
      g_value_set_boolean (value, self->buffer_copy);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_BUFFER_TIMESTAMP_COPY:
      g_value_set_boolean (value, self->buffer_timestamp_copy);
      break;
    case PROP_META_SERIALIZATION_LIB_NAME:
      g_value_set_string (value, self->meta_serialization_lib_name);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

static gboolean
incoming_command_cb (GSocket * socket, GIOCondition cond, gpointer user_data)
{
  GstUnixFdSink *self = user_data;
  Client *client;
  CommandType command;
  guint8 *payload = NULL;
  gsize payload_size;
  GError *error = NULL;

  GST_OBJECT_LOCK (self);

  client = g_hash_table_lookup (self->clients, socket);

  if (client == NULL) {
    GST_ERROR_OBJECT (self, "Received data from unknown client");
    goto on_error;
  }

  if (!gst_unix_fd_receive_command (socket, NULL, &command, NULL, &payload,
          &payload_size, &error)) {
    GST_DEBUG_OBJECT (self, "Failed to receive message from client %p: %s",
        client, error != NULL ? error->message : "Connection closed by peer");
    goto on_error;
  }

  switch (command) {
    case COMMAND_TYPE_NEW_BUFFER:
    case COMMAND_TYPE_CAPS:
      GST_ERROR_OBJECT (self, "Received wrong command %d from client %p",
          command, client);
      goto on_error;
    case COMMAND_TYPE_RELEASE_BUFFER:{
      ReleaseBufferPayload *release_buffer;
      if (!gst_unix_fd_parse_release_buffer (payload, payload_size,
              &release_buffer)) {
        GST_ERROR_OBJECT (self,
            "Received release-buffer with wrong payload size from client %p",
            client);
        goto on_error;
      }
      /* id is actually the GstBuffer pointer casted to guint64.
       * We can now drop its reference kept for this client. */
      if (!g_hash_table_remove (client->buffers, (gpointer) release_buffer->id)) {
        GST_ERROR_OBJECT (self,
            "Received wrong id %" G_GUINT64_FORMAT
            " in release-buffer command from client %p", release_buffer->id,
            client);
        goto on_error;
      }
      break;
    }
    default:
      /* Protocol could have been extended with new command */
      GST_DEBUG_OBJECT (self, "Ignoring unknown command %d", command);
      break;
  }

  g_free (payload);
  GST_OBJECT_UNLOCK (self);

  return G_SOURCE_CONTINUE;

on_error:
  g_hash_table_remove (self->clients, socket);
  g_clear_error (&error);
  g_free (payload);
  GST_OBJECT_UNLOCK (self);
  return G_SOURCE_REMOVE;
}

static guint8 *
caps_to_payload (GstCaps * caps, gsize * payload_size)
{
  gchar *payload = gst_caps_to_string (caps);
  *payload_size = strlen (payload) + 1;
  return (guint8 *) payload;
}

#ifdef HAVE_IPC_TARGET_NV
static int
client_buffer_pool_configuration (GstUnixFdSink * self, Client * client, GstCaps * caps)
{
  GstBufferPool *pool = NULL;
  GstStructure *config = NULL;
  gboolean is_active = false;

  if (client->pool && (TRUE == gst_buffer_pool_is_active (client->pool)))
  {
    gst_buffer_pool_set_active (client->pool, FALSE);
    gst_object_unref(client->pool);
    client->pool = NULL;
  }

  pool = gst_nvipc_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, sizeof (NvBufSurface),
               NV_GST_BUFFER_POOL_MAX_SIZE, NV_GST_BUFFER_POOL_MAX_SIZE);
  gst_structure_set (config,
          "memtype", G_TYPE_UINT, NVBUF_MEM_SURFACE_ARRAY,
          "gpu-id", G_TYPE_UINT, 0,
          "bl-output", G_TYPE_UINT, 1,
          "contiguous-alloc", G_TYPE_BOOLEAN, 1,
          "batch-size", G_TYPE_UINT, 1, NULL);
  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (self, "buffer pool set config failed for client %p", client);
    return -1;
  }

  client->pool = (GstBufferPool *) gst_object_ref (pool);
  is_active = gst_buffer_pool_set_active (client->pool, TRUE);
  if (!is_active) {
    GST_ERROR_OBJECT (self, "buffer pool activation failed for client %p", client);
    return -1;
  }

  return 0;
}
#endif

static gboolean
new_client_cb (GSocket * socket, GIOCondition cond, gpointer user_data)
{
  GstUnixFdSink *self = user_data;
  Client *client;
  GError *error = NULL;

  GSocket *client_socket = g_socket_accept (self->socket, NULL, &error);
  if (client_socket == NULL) {
    GST_ERROR_OBJECT (self, "Failed to accept connection: %s", error->message);
    return G_SOURCE_CONTINUE;
  }

  client = g_new0 (Client, 1);

#ifdef HAVE_IPC_TARGET_NV
  if (self->buffer_copy) {
    client->payload =
      g_byte_array_sized_new (sizeof (NewBufferPayload) +
      sizeof (MemoryPayload));
  }
#endif

  client->buffers =
      g_hash_table_new_full (NULL, NULL, (GDestroyNotify) gst_buffer_unref,
      NULL);
  client->source = g_socket_create_source (client_socket, G_IO_IN, NULL);
  g_source_set_callback (client->source, (GSourceFunc) incoming_command_cb,
      self, NULL);
  g_source_attach (client->source, self->context);

  GST_OBJECT_LOCK (self);
  GST_DEBUG_OBJECT (self, "New client %p", client);
  g_hash_table_insert (self->clients, client_socket, client);

  /* Start by sending our current caps. Keep the lock while doing that because
   * we don't want this client to miss a caps event or receive a buffer while we
   * send initial caps. */

  GST_DEBUG_OBJECT (self, "Send new caps to connected client: %" GST_PTR_FORMAT,
          self->caps);
  gsize payload_size;
  guint8 *payload = caps_to_payload (self->caps, &payload_size);
  if (!gst_unix_fd_send_command (client_socket, COMMAND_TYPE_CAPS, NULL,
          payload, payload_size, &error)) {
    GST_ERROR_OBJECT (self, "Failed to send caps to new client %p: %s", client,
        error->message);
    g_hash_table_remove (self->clients, client_socket);
    g_clear_error (&error);
  }
  g_free (payload);

  GST_OBJECT_UNLOCK (self);

  return G_SOURCE_CONTINUE;
}

static gpointer
thread_cb (gpointer user_data)
{
  GstUnixFdSink *self = user_data;
  g_main_loop_run (self->loop);
  return NULL;
}

static gboolean
gst_unix_fd_sink_start (GstBaseSink * bsink)
{
  GstUnixFdSink *self = (GstUnixFdSink *) bsink;
  GSocketAddress *addr = NULL;
  GError *error = NULL;
  gboolean ret = TRUE;
#ifdef HAVE_IPC_TARGET_NV
  gchar *dl_error = NULL;
#endif

  GST_OBJECT_LOCK (self);

#ifdef HAVE_IPC_TARGET_NV
  // Unlink the socket file if it exists
  g_unlink(self->socket_path);
#endif

  self->socket =
      gst_unix_fd_socket_new (self->socket_path, self->socket_type, &addr,
      &error);
  if (self->socket == NULL) {
    GST_ERROR_OBJECT (self, "Failed to create UNIX socket: %s", error->message);
    ret = FALSE;
    goto out;
  }

  if (!g_socket_bind (self->socket, addr, TRUE, &error)) {
    GST_ERROR_OBJECT (self, "Failed to bind socket: %s", error->message);
    g_clear_object (&self->socket);
    ret = FALSE;
    goto out;
  }

  if (!g_socket_listen (self->socket, &error)) {
    GST_ERROR_OBJECT (self, "Failed to listen socket: %s", error->message);
    g_clear_object (&self->socket);
    ret = FALSE;
    goto out;
  }

#ifdef HAVE_IPC_TARGET_NV
  if (self->meta_serialization_lib_name) {
    self->lib_handle = dlopen (self->meta_serialization_lib_name, RTLD_NOW);
    if (self->lib_handle == NULL) {
      GST_ERROR_OBJECT(self, "Could not open serialiaztion library %s", dlerror());
      g_clear_object (&self->socket);
      ret = FALSE;
      goto out;
    }

    dlerror();
    self->serialize_meta_func = (void (*)(GstBuffer*, guint8 **, guint *))dlsym (self->lib_handle, "serialize_meta");
    dl_error = dlerror();
    if (dl_error != NULL) {
      GST_ERROR_OBJECT(self, "%s", dl_error);
      g_clear_object (&self->socket);
      dlclose (self->lib_handle);
      ret = FALSE;
      goto out;
    }
  }
#endif

  self->source = g_socket_create_source (self->socket, G_IO_IN, NULL);
  g_source_set_callback (self->source, (GSourceFunc) new_client_cb, self, NULL);
  g_source_attach (self->source, self->context);

  self->thread = g_thread_new ("unixfdsink", thread_cb, self);

  /* Preallocate the minimum payload size for a buffer with a single memory and
   * no metas. Chances are that every buffer will require roughly the same
   * payload size, by reusing the same GByteArray we avoid reallocations. */
  self->payload =
      g_byte_array_sized_new (sizeof (NewBufferPayload) +
      sizeof (MemoryPayload));

out:
  GST_OBJECT_UNLOCK (self);
  g_clear_error (&error);
  g_clear_object (&addr);
  return ret;
}

static gboolean
gst_unix_fd_sink_stop (GstBaseSink * bsink)
{
  GstUnixFdSink *self = (GstUnixFdSink *) bsink;

  g_main_loop_quit (self->loop);
  g_thread_join (self->thread);

  g_source_destroy (self->source);
  g_clear_pointer (&self->source, g_source_unref);
  g_clear_object (&self->socket);
  gst_clear_caps (&self->caps);
  g_hash_table_remove_all (self->clients);
  g_clear_pointer (&self->payload, g_byte_array_unref);

  if (self->socket_type == G_UNIX_SOCKET_ADDRESS_PATH)
    g_unlink (self->socket_path);

#ifdef HAVE_IPC_TARGET_NV
  if (self->lib_handle) {
    dlclose (self->lib_handle);
    self->lib_handle = NULL;
  }
#endif

  return TRUE;
}

static void
send_command_to_all (GstUnixFdSink * self, CommandType type, GUnixFDList * fds,
    const guint8 * payload, gsize payload_size, GstBuffer * buffer)
{
  GHashTableIter iter;
  GSocket *socket;
  Client *client;
  GError *error = NULL;

  g_hash_table_iter_init (&iter, self->clients);
  while (g_hash_table_iter_next (&iter, (gpointer) & socket,
          (gpointer) & client)) {
    if (!gst_unix_fd_send_command (socket, type, fds, payload, payload_size,
            &error)) {
      GST_ERROR_OBJECT (self, "Failed to send command %d to client %p: %s",
          type, client, error->message);
      g_clear_error (&error);
      g_hash_table_iter_remove (&iter);
      continue;
    }
    /* Keep a ref on this buffer until all clients released it. */
    if (buffer != NULL)
      g_hash_table_add (client->buffers, gst_buffer_ref (buffer));
  }
}

static GstClockTime
calculate_timestamp (GstClockTime timestamp, GstClockTime base_time,
    GstClockTime latency, GstClockTimeDiff clock_diff)
{
  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    /* Convert running time to pipeline clock time */
    timestamp += base_time;
    if (GST_CLOCK_TIME_IS_VALID (latency))
      timestamp += latency;
    /* Convert to system monotonic clock time */
    if (clock_diff < 0 && ((unsigned long int)(-clock_diff) > timestamp))
      return 0;
    timestamp += clock_diff;
  }
  return timestamp;
}

static guint16
serialize_metas (GstBuffer * buffer, GByteArray * payload)
{
#ifdef HAVE_IPC_TARGET_NV
  return 0;
#else
  gpointer state = NULL;
  GstMeta *meta;
  guint16 n_meta = 0;

  while ((meta = gst_buffer_iterate_meta (buffer, &state)) != NULL) {
    if (gst_meta_serialize_simple (meta, payload))
      n_meta++;
  }

  return n_meta;
#endif
}

#ifdef HAVE_IPC_TARGET_NV
static GstFlowReturn
client_buffer_vic_copy (GstUnixFdSink * self, Client * client, GstBuffer * buffer)
{
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  GstMapInfo outmap = GST_MAP_INFO_INIT;
  NvBufSurface *surf, *copy_surf;
  GstFlowReturn ret = GST_FLOW_OK;
  int ret_val = 0;
  guint num_queued = 0;

  GstNvIpcBufferPool *nvpool = GST_NVIPC_BUFFER_POOL (client->pool);
  num_queued = g_atomic_int_get (&nvpool->num_queued);
  if (num_queued >= NV_GST_BUFFER_POOL_MAX_SIZE) {
    GST_DEBUG_OBJECT (self, "buffer pool is full for client %p so dropping the buffer: %" GST_PTR_FORMAT, client, buffer);
    return ret;
  }

  ret = gst_buffer_pool_acquire_buffer (client->pool, &client->buffer, NULL);
  if (ret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "buffer pool acquire buffer failed for client %p", client);
    return ret;
  }

  if (!gst_buffer_map (buffer, &inmap, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

  if (!gst_buffer_map (client->buffer, &outmap, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

  if (!gst_buffer_copy_into (client->buffer, buffer,
        (GST_BUFFER_COPY_META | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS), 0, -1)) {
     GST_ERROR_OBJECT (self, "buffer copy metadata failed for client %p", client);
     ret = GST_FLOW_ERROR;
     goto error;
   }

  surf = ((NvBufSurface *) inmap.data);
  copy_surf = ((NvBufSurface *) outmap.data);

  NvBufSurfTransformRect dest_rect, src_rect;
  NvBufSurfTransformParams transform_params;
  NvBufSurfTransformConfigParams config_params;
  config_params.compute_mode = NvBufSurfTransformCompute_VIC;
  NvBufSurfTransformSetSessionParams (&config_params);

  src_rect.top = 0;
  src_rect.left = 0;
  src_rect.width = surf->surfaceList[0].width;
  src_rect.height = surf->surfaceList[0].height;
  dest_rect.top = 0;
  dest_rect.left = 0;
  dest_rect.width = copy_surf->surfaceList[0].width;
  dest_rect.height = copy_surf->surfaceList[0].height;

  memset(&transform_params,0,sizeof(transform_params));
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_flip = NvBufSurfTransform_None;
  transform_params.transform_filter = NvBufSurfTransformInter_Nearest;
  transform_params.src_rect = &src_rect;
  transform_params.dst_rect = &dest_rect;

  memset(&client->syncobj, 0, sizeof(NvBufSurfTransformSyncObj_t));
  ret_val = NvBufSurfTransformAsync (surf, copy_surf, &transform_params, &client->syncobj);
  if (ret_val < 0) {
    GST_ERROR_OBJECT (self, "NvBufSurfTransformAsync failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }
  copy_surf->numFilled = 1;

error:
  gst_buffer_unmap (buffer, &inmap);
  gst_buffer_unmap (client->buffer, &outmap);

  return ret;
}

static GstFlowReturn
client_buffer_pool_configuration_from_buffer (GstUnixFdSink * self, Client * client, GstBuffer * buffer)
{
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  NvBufSurface *surf = NULL;
  GstVideoColorimetry cinfo;
  gchar *color = NULL;
  GstCaps *caps = NULL;
  GstCapsFeatures *feature = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!gst_buffer_map (buffer, &inmap, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

  surf = ((NvBufSurface *) inmap.data);

  switch (surf->surfaceList[0].colorFormat) {
    case NVBUF_COLOR_FORMAT_NV12:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_16_235;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT601;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
    }
    break;
    case NVBUF_COLOR_FORMAT_NV12_ER:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_0_255;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT601;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
    }
    break;
    case NVBUF_COLOR_FORMAT_NV12_709:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_16_235;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT709;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
    }
    break;
    case NVBUF_COLOR_FORMAT_NV12_709_ER:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_0_255;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT709;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
    }
    break;
    case NVBUF_COLOR_FORMAT_NV12_2020:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_16_235;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT2020_12;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
    }
    break;
    default:
    {
      cinfo.range = GST_VIDEO_COLOR_RANGE_16_235;
      cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
      cinfo.transfer = GST_VIDEO_TRANSFER_BT601;
      cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
    }
    break;
  }
  color = gst_video_colorimetry_to_string (&cinfo);
  caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, surf->surfaceList[0].width,
      "height", G_TYPE_INT, surf->surfaceList[0].height,
      "framerate", GST_TYPE_FRACTION, 30, 1,
      "colorimetry", G_TYPE_STRING, color,
      NULL);
  feature = gst_caps_features_new ("memory:NVMM", NULL);
  gst_caps_set_features (caps, 0, feature);

  ret = client_buffer_pool_configuration (self, client, caps);
  if (ret == -1) {
    GST_ERROR_OBJECT (self, "Failed to configure buffer pool for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

error:
  gst_buffer_unmap (buffer, &inmap);
  if (color)
    g_free(color);
  if (caps)
    gst_caps_unref(caps);

  return ret;
}

static GstFlowReturn
client_buffer_cpu_copy (GstUnixFdSink * self, Client * client, GstBuffer * buffer)
{
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  GstMapInfo outmap = GST_MAP_INFO_INIT;
  NvBufSurface *surf, *copy_surf;
  GstFlowReturn ret = GST_FLOW_OK;
  int ret_val = 0;
  guint num_queued = 0;

  GstNvIpcBufferPool *nvpool = GST_NVIPC_BUFFER_POOL (client->pool);
  num_queued = g_atomic_int_get (&nvpool->num_queued);
  if (num_queued >= NV_GST_BUFFER_POOL_MAX_SIZE) {
    GST_DEBUG_OBJECT (self, "buffer pool is full for client %p so dropping the buffer: %" GST_PTR_FORMAT, client, buffer);
    return ret;
  }

  ret = gst_buffer_pool_acquire_buffer (client->pool, &client->buffer, NULL);
  if (ret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "buffer pool acquire buffer failed for client %p", client);
    return ret;
  }

  if (!gst_buffer_map (buffer, &inmap, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

  if (!gst_buffer_map (client->buffer, &outmap, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }

  surf = ((NvBufSurface *) inmap.data);
  copy_surf = ((NvBufSurface *) outmap.data);

  if (!gst_buffer_copy_into (client->buffer, buffer,
        (GST_BUFFER_COPY_META | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS), 0, -1)) {
     GST_ERROR_OBJECT (self, "buffer copy metadata failed for client %p", client);
     ret = GST_FLOW_ERROR;
     goto error;
   }

  ret_val = NvBufSurfaceCopy (surf, copy_surf);
  if (ret_val < 0) {
    GST_ERROR_OBJECT (self, "NvBufSurfaceCopy failed for client %p", client);
    ret = GST_FLOW_ERROR;
    goto error;
  }
  copy_surf->numFilled = 1;

error:
  gst_buffer_unmap (buffer, &inmap);
  gst_buffer_unmap (client->buffer, &outmap);

  return ret;
}

static GstFlowReturn
client_buffer_render (GstUnixFdSink * self, Client * client, GSocket *socket,
    GstClockTime base_time, GstClockTime latency, GstClockTimeDiff clock_diff)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GError *error = NULL;
  GstMapInfo map = GST_MAP_INFO_INIT;
  NvBufSurfaceMapParams map_params = {0};
  int fd = -1;

  GstBuffer *buffer = client->buffer;
  guint n_memory = gst_buffer_n_memory (buffer);
  gsize struct_size =
      sizeof (NewBufferPayload) + sizeof (MemoryPayload) * n_memory;
  g_byte_array_set_size (client->payload, struct_size);
  guint32 n_meta = serialize_metas (buffer, client->payload);

  NewBufferPayload *new_buffer = (NewBufferPayload *) client->payload->data;
  /* Cast buffer pointer to guint64 identifier. Client will send us back that
   * id so we know which buffer to unref. */
  new_buffer->id = (guint64) buffer;

  if (self->buffer_timestamp_copy) {
    new_buffer->pts = GST_BUFFER_PTS (buffer);
    new_buffer->dts = GST_BUFFER_DTS (buffer);
  } else {
    new_buffer->pts =
      calculate_timestamp (GST_BUFFER_PTS (buffer), base_time, latency,
      clock_diff);
    new_buffer->dts =
      calculate_timestamp (GST_BUFFER_DTS (buffer), base_time, latency,
      clock_diff);
  }

  new_buffer->duration = GST_BUFFER_DURATION (buffer);
  new_buffer->offset = GST_BUFFER_OFFSET (buffer);
  new_buffer->offset_end = GST_BUFFER_OFFSET_END (buffer);
  new_buffer->flags = GST_BUFFER_FLAGS (buffer);
  new_buffer->type = MEMORY_TYPE_DEFAULT;
  new_buffer->n_memory = n_memory;
  new_buffer->n_meta = n_meta;

  guint dmabuf_count = 0;
  GUnixFDList *fds = g_unix_fd_list_new ();

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "buffer map failed for client %p: %" GST_PTR_FORMAT, client, buffer);
    ret = GST_FLOW_ERROR;
    goto out;
  }
  NvBufSurface *nvbuf_surf = (NvBufSurface *)map.data;
  fd = nvbuf_surf->surfaceList[0].bufferDesc;
  NvBufSurfaceGetMapParams(nvbuf_surf, 0, &map_params);
  g_byte_array_append(client->payload, (const guint8 *)&map_params, sizeof(map_params));

  for (guint i = 0; i < n_memory; i++) {
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    dmabuf_count++;

    if (g_unix_fd_list_append (fds, fd, &error) < 0) {
      GST_ERROR_OBJECT (self, "Failed to append FD for client %p: %s", client, error->message);
      ret = GST_FLOW_ERROR;
      goto out;
    }

    gsize offset;
    new_buffer->memories[i].size = gst_memory_get_sizes (mem, &offset, NULL);
    new_buffer->memories[i].offset = offset;
  }

  if (dmabuf_count > 0 && dmabuf_count != n_memory) {
    GST_ERROR_OBJECT (self, "Some but not all memories are DMABuf for client %p", client);
    ret = GST_FLOW_ERROR;
    goto out;
  }

  if (dmabuf_count > 0)
    new_buffer->type = MEMORY_TYPE_DMABUF;

  if (self->compute_hw == GST_UNIXFDSINK_COMPUTE_HW_VIC) {
    NvBufSurfTransformSyncObjWait(client->syncobj, -1);
  }

  if (self->serialize_meta_func) {
    guint8 *data = NULL;
    guint length = 0;
    self->serialize_meta_func(buffer, &data, &length);
    g_byte_array_append(client->payload, (const guint8 *)&length, sizeof(guint));
    if (data != NULL && length > 0) {
      g_byte_array_append(client->payload, (const guint8 *)data, length);
      g_free(data);
    }
  }

  if (!gst_unix_fd_send_command (socket, COMMAND_TYPE_NEW_BUFFER, fds, client->payload->data, client->payload->len,
          &error)) {
    GST_ERROR_OBJECT (self, "Failed to send command %d to client %p: %s",
        COMMAND_TYPE_NEW_BUFFER, client, error->message);
    g_clear_error (&error);
    ret = GST_FLOW_ERROR;
  }

out:
  gst_buffer_unmap (buffer, &map);
  g_clear_object (&fds);
  g_clear_error (&error);

  return ret;
}
#endif

static GstFlowReturn
gst_unix_fd_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstUnixFdSink *self = (GstUnixFdSink *) bsink;
  GstFlowReturn ret = GST_FLOW_OK;
  GError *error = NULL;
#ifdef HAVE_IPC_TARGET_NV
  GstMapInfo map = GST_MAP_INFO_INIT;
  NvBufSurfaceMapParams map_params = {0};
  int fd = -1;
#endif

  GstClockTime latency = gst_base_sink_get_latency (GST_BASE_SINK_CAST (self));
  GstClockTime base_time = gst_element_get_base_time (GST_ELEMENT_CAST (self));
  GstClockTimeDiff clock_diff = 0;
  if (!self->uses_monotonic_clock) {
    clock_diff = GST_CLOCK_DIFF (g_get_monotonic_time () * GST_USECOND,
        gst_clock_get_time (GST_ELEMENT_CLOCK (self)));
  }

#ifdef HAVE_IPC_TARGET_NV
  if (self->nvmm_memory && self->buffer_copy)
    goto buffer_copy;
#endif

  guint n_memory = gst_buffer_n_memory (buffer);
  gsize struct_size =
      sizeof (NewBufferPayload) + sizeof (MemoryPayload) * n_memory;
  g_byte_array_set_size (self->payload, struct_size);
  guint32 n_meta = serialize_metas (buffer, self->payload);

  NewBufferPayload *new_buffer = (NewBufferPayload *) self->payload->data;
  /* Cast buffer pointer to guint64 identifier. Client will send us back that
   * id so we know which buffer to unref. */
  new_buffer->id = (guint64) buffer;

#ifdef HAVE_IPC_TARGET_NV
  if (self->buffer_timestamp_copy) {
    new_buffer->pts = GST_BUFFER_PTS (buffer);
    new_buffer->dts = GST_BUFFER_DTS (buffer);
  } else {
#endif
    new_buffer->pts =
      calculate_timestamp (GST_BUFFER_PTS (buffer), base_time, latency,
      clock_diff);
    new_buffer->dts =
      calculate_timestamp (GST_BUFFER_DTS (buffer), base_time, latency,
      clock_diff);
#ifdef HAVE_IPC_TARGET_NV
  }
#endif

  new_buffer->duration = GST_BUFFER_DURATION (buffer);
  new_buffer->offset = GST_BUFFER_OFFSET (buffer);
  new_buffer->offset_end = GST_BUFFER_OFFSET_END (buffer);
  new_buffer->flags = GST_BUFFER_FLAGS (buffer);
  new_buffer->type = MEMORY_TYPE_DEFAULT;
  new_buffer->n_memory = n_memory;
  new_buffer->n_meta = n_meta;

  guint dmabuf_count = 0;
  GUnixFDList *fds = g_unix_fd_list_new ();

#ifdef HAVE_IPC_TARGET_NV
  if (self->nvmm_memory) {
    if (!gst_buffer_map (buffer, &map, GST_MAP_READ)) {
      GST_ERROR_OBJECT (self, "buffer map failed : %" GST_PTR_FORMAT, buffer);
      ret = GST_FLOW_ERROR;
      goto out;
    }

    NvBufSurface *nvbuf_surf = (NvBufSurface *)map.data;
    fd = nvbuf_surf->surfaceList[0].bufferDesc;
    NvBufSurfaceGetMapParams(nvbuf_surf, 0, &map_params);
    g_byte_array_append(self->payload, (const guint8 *)&map_params, sizeof(map_params));
  }
#endif

  for (guint i = 0; i < n_memory; i++) {
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
#ifdef HAVE_IPC_TARGET_NV
    if(self->nvmm_memory) {
      dmabuf_count++;

      if (g_unix_fd_list_append (fds, fd, &error) < 0) {
        GST_ERROR_OBJECT (self, "Failed to append FD: %s", error->message);
        ret = GST_FLOW_ERROR;
        goto out;
      }
    } else {
#endif
      if (!gst_is_fd_memory (mem)) {
        GST_ERROR_OBJECT (self, "Expecting buffers with FD memories");
        ret = GST_FLOW_ERROR;
        goto out;
      }

      if (gst_is_dmabuf_memory (mem))
        dmabuf_count++;

      if (g_unix_fd_list_append (fds, gst_fd_memory_get_fd (mem), &error) < 0) {
        GST_ERROR_OBJECT (self, "Failed to append FD: %s", error->message);
        ret = GST_FLOW_ERROR;
        goto out;
      }
#ifdef HAVE_IPC_TARGET_NV
    }
#endif

    gsize offset;
    new_buffer->memories[i].size = gst_memory_get_sizes (mem, &offset, NULL);
    new_buffer->memories[i].offset = offset;
  }

  if (dmabuf_count > 0 && dmabuf_count != n_memory) {
    GST_ERROR_OBJECT (self, "Some but not all memories are DMABuf");
    ret = GST_FLOW_ERROR;
    goto out;
  }

  if (dmabuf_count > 0)
    new_buffer->type = MEMORY_TYPE_DMABUF;

#ifdef HAVE_IPC_TARGET_NV
  if (self->serialize_meta_func) {
    guint8 *data = NULL;
    guint length = 0;
    self->serialize_meta_func(buffer, &data, &length);
    g_byte_array_append(self->payload, (const guint8 *)&length, sizeof(guint));
    if (data != NULL && length > 0) {
      g_byte_array_append(self->payload, (const guint8 *)data, length);
      g_free(data);
    }
  }
#endif

  GST_OBJECT_LOCK (self);
  send_command_to_all (self, COMMAND_TYPE_NEW_BUFFER, fds,
      self->payload->data, self->payload->len, buffer);
  GST_OBJECT_UNLOCK (self);

out:
#ifdef HAVE_IPC_TARGET_NV
  gst_buffer_unmap (buffer, &map);
#endif
  g_clear_object (&fds);
  g_clear_error (&error);

  return ret;

#ifdef HAVE_IPC_TARGET_NV
buffer_copy:

  GHashTableIter iter;
  GSocket *socket;
  Client *client;

  GST_OBJECT_LOCK (self);

  /* copy buffer to each client */
  g_hash_table_iter_init (&iter, self->clients);
  while (g_hash_table_iter_next (&iter, (gpointer) & socket,
          (gpointer) & client)) {

    if (client->pool == NULL) {
      ret = client_buffer_pool_configuration_from_buffer (self, client, buffer);
      if (ret != GST_FLOW_OK) {
        GST_ERROR_OBJECT (self, "Failed to configure buffer pool for client %p", client);
        g_hash_table_iter_remove (&iter);
        continue;
      }
    }

    if (self->compute_hw == GST_UNIXFDSINK_COMPUTE_HW_VIC) {
      ret = client_buffer_vic_copy (self, client, buffer);
    } else {
      ret = client_buffer_cpu_copy (self, client, buffer);
    }
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to copy buffer for client %p", client);
      g_hash_table_iter_remove (&iter);
      continue;
    }
  }

  /* render buffer for each client */
  g_hash_table_iter_init (&iter, self->clients);
  while (g_hash_table_iter_next (&iter, (gpointer) & socket,
          (gpointer) & client)) {
    if (!client->buffer) {
      GST_DEBUG_OBJECT (self, "No buffer available for render for client %p", client);
      continue;
    }

    ret = client_buffer_render (self, client, socket, base_time, latency, clock_diff);
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to render buffer for client %p", client);
      g_hash_table_iter_remove (&iter);
      continue;
    }

    /* Keep a ref on this buffer until client released it. */
    if (client->buffer != NULL) {
      g_hash_table_add (client->buffers, gst_buffer_ref(client->buffer));
      gst_buffer_unref(client->buffer);
      client->buffer = NULL;
    }
  }

  GST_OBJECT_UNLOCK (self);

  return GST_FLOW_OK;
#endif
}

static gboolean
gst_unix_fd_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  GstUnixFdSink *self = (GstUnixFdSink *) bsink;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GST_OBJECT_LOCK (self);
      gst_clear_caps (&self->caps);
      gst_event_parse_caps (event, &self->caps);
      gst_caps_ref (self->caps);
      GST_DEBUG_OBJECT (self, "Send new caps to all clients: %" GST_PTR_FORMAT,
          self->caps);
#ifdef HAVE_IPC_TARGET_NV
      GstCapsFeatures *ft = gst_caps_get_features (self->caps, 0);
      if (gst_caps_features_contains (ft, GST_CAPS_FEATURE_MEMORY_NVMM)) {
        self->nvmm_memory = TRUE;
      } else {
        self->nvmm_memory = FALSE;
      }
#endif
      gsize payload_size;
      guint8 *payload = caps_to_payload (self->caps, &payload_size);
      send_command_to_all (self, COMMAND_TYPE_CAPS, NULL, payload, payload_size,
          NULL);
      g_free (payload);
      GST_OBJECT_UNLOCK (self);
      break;
    }
    case GST_EVENT_EOS:{
      GST_OBJECT_LOCK (self);
      send_command_to_all (self, COMMAND_TYPE_EOS, NULL, NULL, 0, NULL);
      GST_OBJECT_UNLOCK (self);
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (gst_unix_fd_sink_parent_class)->event (bsink,
      event);
}

static gboolean
gst_unix_fd_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
#ifdef HAVE_IPC_TARGET_NV
  return TRUE;
#else
  GstAllocator *allocator = gst_shm_allocator_get ();
  gst_query_add_allocation_param (query, allocator, NULL);
  gst_object_unref (allocator);

  return TRUE;
#endif
}

static gboolean
gst_unix_fd_sink_set_clock (GstElement * element, GstClock * clock)
{
  GstUnixFdSink *self = (GstUnixFdSink *) element;

  self->uses_monotonic_clock = FALSE;
  if (clock != NULL && G_OBJECT_TYPE (clock) == GST_TYPE_SYSTEM_CLOCK) {
    GstClockType clock_type;
    g_object_get (clock, "clock-type", &clock_type, NULL);
    self->uses_monotonic_clock = clock_type == GST_CLOCK_TYPE_MONOTONIC;
  }

  return GST_ELEMENT_CLASS (gst_unix_fd_sink_parent_class)->set_clock (element,
      clock);
}

static void
gst_unix_fd_sink_class_init (GstUnixFdSinkClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseSinkClass *gstbasesink_class = (GstBaseSinkClass *) klass;

#ifdef HAVE_IPC_TARGET_NV
  GST_DEBUG_CATEGORY_INIT (unixfdsink_debug, "nvunixfdsink", 0,
      "Unix file descriptor sink");
#else
  GST_DEBUG_CATEGORY_INIT (unixfdsink_debug, "unixfdsink", 0,
      "Unix file descriptor sink");
#endif

  gst_element_class_set_static_metadata (gstelement_class,
      "Unix file descriptor sink", "Sink", "Unix file descriptor sink",
      "Xavier Claessens <xavier.claessens@collabora.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

#ifndef HAVE_IPC_TARGET_NV
  gst_shm_allocator_init_once ();
#endif

  gobject_class->finalize = gst_unix_fd_sink_finalize;
  gobject_class->set_property = gst_unix_fd_sink_set_property;
  gobject_class->get_property = gst_unix_fd_sink_get_property;

  gstelement_class->set_clock = GST_DEBUG_FUNCPTR (gst_unix_fd_sink_set_clock);

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_unix_fd_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_unix_fd_sink_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_unix_fd_sink_render);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_unix_fd_sink_event);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_unix_fd_sink_propose_allocation);

  g_object_class_install_property (gobject_class, PROP_SOCKET_PATH,
      g_param_spec_string ("socket-path",
          "Path to the control socket",
          "The path to the control socket used to control the shared memory "
          "transport. This may be modified during the NULL->READY transition",
          NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_SOCKET_TYPE,
      g_param_spec_enum ("socket-type", "Socket type",
          "The type of underlying socket",
          G_TYPE_UNIX_SOCKET_ADDRESS_TYPE, DEFAULT_SOCKET_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT |
          GST_PARAM_MUTABLE_READY));

#ifdef HAVE_IPC_TARGET_NV
  g_object_class_install_property (gobject_class, PROP_BUFFER_COPY,
    g_param_spec_boolean ("buffer-copy", "Buffer copy",
        "Buffer copy", DEFAULT_BUFFER_COPY,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BUFFER_TIMESTAMP_COPY,
    g_param_spec_boolean ("buffer-timestamp-copy", "Buffer timestamp copy",
        "Buffer timestamp copy", DEFAULT_BUFFER_TIMESTAMP_COPY,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
    g_param_spec_enum ("compute-hw", "compute-hw", "Compute HW for copy operation",
      GST_TYPE_UNIXFDSINK_COMPUTE_HW, GST_UNIXFDSINK_COMPUTE_HW_CPU,
      (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
      GST_PARAM_CONTROLLABLE | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_META_SERIALIZATION_LIB_NAME,
    g_param_spec_string ("meta-serialization-lib", "Meta serialization library name",
      "Set meta serialization library name to be used", NULL,
      (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif
}
