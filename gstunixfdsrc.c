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
 * SECTION:element-unixfdsrc
 * @title: unixfdsrc
 *
 * Receive file-descriptor backed buffers (e.g. memfd, dmabuf) over unix socket
 * from matching unixfdsink.
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
#include <dlfcn.h>

GST_DEBUG_CATEGORY (unixfdsrc_debug);
#define GST_CAT_DEFAULT (unixfdsrc_debug)

#ifdef HAVE_IPC_TARGET_NV
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
#define DEFAULT_BUFFER_TIMESTAMP_COPY FALSE
#define DEFAULT_CONNECTION_ATTEMPTS -1
#define DEFAULT_CONNECTION_INTERVAL 1000000
#endif

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#ifdef HAVE_IPC_TARGET_NV
typedef enum
{
  NVBUF_MAPPING_ONCE,
  NVBUF_MAPPING_ALWAYS,
} NvUnixFdSrcBufferMapping;
#endif

struct _GstUnixFdSrc
{
  GstPushSrc parent;

  gchar *socket_path;
  GUnixSocketAddressType socket_type;
  GSocket *socket;
  GCancellable *cancellable;

  GstAllocator *allocators[MEMORY_TYPE_LAST];
  GHashTable *memories;
  gboolean uses_monotonic_clock;

#ifdef HAVE_IPC_TARGET_NV
  gboolean nvmm_memory;
  gboolean buffer_timestamp_copy;
  GHashTable *nvbufsurfaces;
  gint connection_attempts;
  guint64 connection_interval;
  gboolean stopped;
  gint connected;
  GCond stopped_cond;
  GCond connected_cond;
  GMutex stopped_lock;
  GMutex connected_lock;
  GThread *connection_thread;
  GSocketAddress *addr;
  NvUnixFdSrcBufferMapping buf_mapping;
  gchar *meta_deserialization_lib_name;
  void *lib_handle;
  void (*deserialize_meta_func)(GstBuffer *buf, guint8*, guint);
#endif
};


G_DEFINE_TYPE (GstUnixFdSrc, gst_unix_fd_src, GST_TYPE_PUSH_SRC);

#define DEFAULT_SOCKET_TYPE G_UNIX_SOCKET_ADDRESS_PATH

enum
{
  PROP_0,
  PROP_SOCKET_PATH,
  PROP_SOCKET_TYPE,
#ifdef HAVE_IPC_TARGET_NV
  PROP_BUFFER_TIMESTAMP_COPY,
  PROP_CONNECTION_ATTEMPTS,
  PROP_CONNECTION_INTERVAL,
  PROP_META_DESERIALIZATION_LIB_NAME,
#endif
};

typedef struct
{
  guint64 id;
  guint n_memory;
#ifdef HAVE_IPC_TARGET_NV
  NvBufSurface *nvbuf_surf;
#endif
} BufferContext;

#ifdef HAVE_IPC_TARGET_NV
static void
nvbufsurface_free (NvBufSurface * nvbuf_surf)
{
  NvBufSurfaceDestroy(nvbuf_surf);
}
#endif

static void
memory_weak_ref_cb (GstUnixFdSrc * self, GstMemory * mem)
{
  GST_OBJECT_LOCK (self);

  BufferContext *ctx = g_hash_table_lookup (self->memories, mem);
  if (ctx == NULL)
    goto out;

  if (--ctx->n_memory == 0) {
    /* Notify that we are not using this buffer anymore */
    ReleaseBufferPayload payload = { ctx->id };
    GError *error = NULL;
    if (!gst_unix_fd_send_command (self->socket, COMMAND_TYPE_RELEASE_BUFFER,
            NULL, (guint8 *) & payload, sizeof (payload), &error)) {
      GST_WARNING_OBJECT (self, "Failed to send release-buffer command: %s",
          error->message);
      g_clear_error (&error);
    }

#ifdef HAVE_IPC_TARGET_NV
    if (self->nvmm_memory) {
      if (ctx->nvbuf_surf) {
        NvBufSurfaceDestroy(ctx->nvbuf_surf);
        ctx->nvbuf_surf = NULL;
      }
    }
#endif

    g_free (ctx);
  }

  g_hash_table_remove (self->memories, mem);

out:
  GST_OBJECT_UNLOCK (self);
}

static void
gst_unix_fd_src_init (GstUnixFdSrc * self)
{
  g_return_if_fail (GST_IS_UNIX_FD_SRC (self));

  self->cancellable = g_cancellable_new ();
  self->memories = g_hash_table_new (NULL, NULL);
  self->allocators[0] = gst_fd_allocator_new ();
  self->allocators[1] = gst_dmabuf_allocator_new ();
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
#ifdef HAVE_IPC_TARGET_NV
  self->nvbufsurfaces = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) nvbufsurface_free);
  self->connection_attempts = DEFAULT_CONNECTION_ATTEMPTS;
  self->connection_interval = DEFAULT_CONNECTION_INTERVAL;
  self->stopped = FALSE;
  self->connected = 0;
  self->buf_mapping = NVBUF_MAPPING_ONCE;
  g_mutex_init (&self->stopped_lock);
  g_cond_init (&self->stopped_cond);
  g_mutex_init (&self->connected_lock);
  g_cond_init (&self->connected_cond);
#endif
}

static void
gst_unix_fd_src_finalize (GObject * object)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (object);

  g_free (self->socket_path);
  g_object_unref (self->cancellable);
  g_hash_table_unref (self->memories);
#ifdef HAVE_IPC_TARGET_NV
  g_hash_table_unref (self->nvbufsurfaces);
  g_cond_clear (&self->stopped_cond);
  g_mutex_clear (&self->stopped_lock);
  g_cond_clear (&self->connected_cond);
  g_mutex_clear (&self->connected_lock);
#endif
  for (int i = 0; i < MEMORY_TYPE_LAST; i++)
    gst_object_unref (self->allocators[i]);

#ifdef HAVE_IPC_TARGET_NV
  if (self->meta_deserialization_lib_name) {
    g_free (self->meta_deserialization_lib_name);
    self->meta_deserialization_lib_name = NULL;
  }
#endif
  G_OBJECT_CLASS (gst_unix_fd_src_parent_class)->finalize (object);
}

static void
gst_unix_fd_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (object);

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
    case PROP_BUFFER_TIMESTAMP_COPY:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change buffer timestamp copy in NULL or READY state");
        break;
      }
      self->buffer_timestamp_copy = g_value_get_boolean (value);
      break;
    case PROP_CONNECTION_ATTEMPTS:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change connection attempts in NULL or READY state");
        break;
      }
      self->connection_attempts = g_value_get_int (value);
      break;
    case PROP_CONNECTION_INTERVAL:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change connection interval in NULL or READY state");
        break;
      }
      self->connection_interval = g_value_get_uint64 (value);
      break;
    case PROP_META_DESERIALIZATION_LIB_NAME:
      if (self->socket) {
        GST_WARNING_OBJECT (self,
            "Can only change meta deserialization lib name in NULL or READY state");
        break;
      }
      g_free (self->meta_deserialization_lib_name);
      self->meta_deserialization_lib_name = (gchar *)g_value_dup_string (value);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

static void
gst_unix_fd_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_SOCKET_PATH:
      g_value_set_string (value, self->socket_path);
      break;
    case PROP_SOCKET_TYPE:
      g_value_set_enum (value, self->socket_type);
      break;
#ifdef HAVE_IPC_TARGET_NV
    case PROP_BUFFER_TIMESTAMP_COPY:
      g_value_set_boolean (value, self->buffer_timestamp_copy);
      break;
    case PROP_CONNECTION_ATTEMPTS:
      g_value_set_int (value, self->connection_attempts);
      break;
    case PROP_CONNECTION_INTERVAL:
      g_value_set_uint64 (value, self->connection_interval);
      break;
    case PROP_META_DESERIALIZATION_LIB_NAME:
      g_value_set_string (value, self->meta_deserialization_lib_name);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

#ifdef HAVE_IPC_TARGET_NV
static gpointer
connection_thread_cb (gpointer user_data)
{
  GstUnixFdSrc *self = (GstUnixFdSrc *) user_data;
  gint max_retries = self->connection_attempts;
  gint retry_count = 0;
  GError *error = NULL;
  gboolean connected = FALSE;

  g_mutex_lock(&self->stopped_lock);
  while (self->stopped != TRUE)
  {
      if (!g_cond_wait_until(&self->stopped_cond, &self->stopped_lock, g_get_monotonic_time () + self->connection_interval)) {
        if(!g_socket_connect (self->socket, self->addr, NULL, &error)) {
          retry_count++;
          if (max_retries != -1 && retry_count >= max_retries) {
            GST_ERROR_OBJECT (self, "Failed to connect socket: %s retry_count: %d max_retries: %d",
                              error->message, retry_count, max_retries);
            g_clear_error (&error);
            break;
          }
          GST_WARNING_OBJECT (self, "Failed to connect socket: %s retry_count: %d max_retries: %d",
                              error->message, retry_count, max_retries);
          g_clear_error (&error);
        } else {
          connected = TRUE;
          GST_DEBUG_OBJECT (self, "Socket connected successfully retry_count: %d max_retries: %d",
                            retry_count, max_retries);
          g_clear_error (&error);
          break;
        }
      } else {
        GST_DEBUG_OBJECT(self, "stopped condition is true");
        break;
      }
  }
  g_mutex_unlock(&self->stopped_lock);

  g_mutex_lock (&self->connected_lock);
  self->connected = (connected == TRUE) ? 1 : -1;
  g_cond_signal (&self->connected_cond);
  g_mutex_unlock (&self->connected_lock);

  return NULL;
}
#endif

static gboolean
gst_unix_fd_src_start (GstBaseSrc * bsrc)
{
  GstUnixFdSrc *self = (GstUnixFdSrc *) bsrc;
#ifndef HAVE_IPC_TARGET_NV
  GSocketAddress *addr = NULL;
#endif
  GError *error = NULL;
  gboolean ret = TRUE;
#ifdef HAVE_IPC_TARGET_NV
  gchar* dl_error = NULL;
#endif

  gst_base_src_set_format (bsrc, GST_FORMAT_TIME);

  GST_OBJECT_LOCK (self);

#ifdef HAVE_IPC_TARGET_NV
  self->socket =
      gst_unix_fd_socket_new (self->socket_path, self->socket_type, &self->addr,
      &error);
#else
  self->socket =
      gst_unix_fd_socket_new (self->socket_path, self->socket_type, &addr,
      &error);
#endif
  if (self->socket == NULL) {
    GST_ERROR_OBJECT (self, "Failed to create UNIX socket: %s", error->message);
    ret = FALSE;
    goto out;
  }

#ifdef HAVE_IPC_TARGET_NV
  self->connection_thread = g_thread_new ("connection_thread", connection_thread_cb, self);
#else
  if (!g_socket_connect (self->socket, addr, NULL, &error)) {
    GST_ERROR_OBJECT (self, "Failed to connect socket: %s", error->message);
    g_clear_object (&self->socket);
    ret = FALSE;
    goto out;
  }
#endif

#ifdef HAVE_IPC_TARGET_NV
  if (self->meta_deserialization_lib_name) {
    self->lib_handle = dlopen (self->meta_deserialization_lib_name, RTLD_NOW);
    if (self->lib_handle == NULL) {
      GST_ERROR_OBJECT(self, "Could not open deserialiaztion library %s", dlerror());
      g_clear_object (&self->socket);
      ret = FALSE;
      goto out;
    }

    dlerror();
    self->deserialize_meta_func = (void (*)(GstBuffer*, guint8*, guint))dlsym (self->lib_handle, "deserialize_meta");
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

out:
  GST_OBJECT_UNLOCK (self);
  g_clear_error (&error);
#ifndef HAVE_IPC_TARGET_NV
  g_clear_object (&addr);
#endif

  return ret;
}

static gboolean
gst_unix_fd_src_stop (GstBaseSrc * bsrc)
{
  GstUnixFdSrc *self = (GstUnixFdSrc *) bsrc;

  GST_OBJECT_LOCK (self);

#ifdef HAVE_IPC_TARGET_NV
  g_thread_join (self->connection_thread);
#endif

  /* Remove all weak refs we could still have to not be called back later.
   * Service side will cleanup pending buffers when socket gets closed. */
  GstMemory *mem;
  BufferContext *ctx;
  GHashTableIter iter;
  g_hash_table_iter_init (&iter, self->memories);
  while (g_hash_table_iter_next (&iter, (gpointer *) & mem, (gpointer *) & ctx)) {
    gst_mini_object_weak_unref (GST_MINI_OBJECT_CAST (mem),
        (GstMiniObjectNotify) memory_weak_ref_cb, self);
    if (--ctx->n_memory == 0) {
#ifdef HAVE_IPC_TARGET_NV
      if (self->nvmm_memory) {
        if (ctx->nvbuf_surf) {
          NvBufSurfaceDestroy(ctx->nvbuf_surf);
          ctx->nvbuf_surf = NULL;
        }
      }
#endif
      g_free (ctx);
    }
  }
  g_hash_table_remove_all (self->memories);
#ifdef HAVE_IPC_TARGET_NV
  g_hash_table_remove_all (self->nvbufsurfaces);
  g_clear_object (&self->addr);
#endif
  g_clear_object (&self->socket);

#ifdef HAVE_IPC_TARGET_NV
  if (self->lib_handle) {
    dlclose (self->lib_handle);
    self->lib_handle = NULL;
  }
#endif

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_unix_fd_src_unlock (GstBaseSrc * bsrc)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (bsrc);
  g_cancellable_cancel (self->cancellable);

#ifdef HAVE_IPC_TARGET_NV
  g_mutex_lock (&self->stopped_lock);
  self->stopped = TRUE;
  g_cond_signal (&self->stopped_cond);
  g_mutex_unlock (&self->stopped_lock);
#endif

  return TRUE;
}

static gboolean
gst_unix_fd_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (bsrc);
  g_cancellable_reset (self->cancellable);
  return TRUE;
}

static GstClockTime
calculate_timestamp (GstClockTime timestamp, GstClockTime base_time,
    GstClockTimeDiff clock_diff)
{
  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    /* Convert from system monotonic clock time to pipeline clock time */
    if (clock_diff > 0 && ((unsigned long int)clock_diff > timestamp))
      return 0;
    timestamp -= clock_diff;
    /* Convert to running time */
    if (base_time > timestamp)
      return 0;
    timestamp -= base_time;
  }
  return timestamp;
}

static GstFlowReturn
gst_unix_fd_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstUnixFdSrc *self = GST_UNIX_FD_SRC (psrc);
  CommandType command;
  GUnixFDList *fds = NULL;
  guint8 *payload = NULL;
  gsize payload_size;
  GError *error = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

#ifdef HAVE_IPC_TARGET_NV
  /* Block until client is connected */
  g_mutex_lock(&self->connected_lock);
  while (self->connected == 0) {
    if (!g_cond_wait_until(&self->connected_cond, &self->connected_lock, g_get_monotonic_time () + self->connection_interval)) {
      GST_DEBUG_OBJECT(self, "waiting for client connection");
    } else {
      if (self->connected == 1) {
        GST_DEBUG_OBJECT(self, "client is connected");
      } else {
        GST_ERROR_OBJECT(self, "client is not connected and connection thread stopped");
        g_mutex_unlock(&self->connected_lock);
        ret = GST_FLOW_ERROR;
        return ret;
      }
    }
  }
  g_mutex_unlock(&self->connected_lock);
#endif

again:
  /* Block until we receive a command */
  if (!gst_unix_fd_receive_command (self->socket, self->cancellable, &command,
          &fds, &payload, &payload_size, &error)) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      ret = GST_FLOW_FLUSHING;
      goto on_error;
    }
    GST_ERROR_OBJECT (self, "Failed to read from sink element: %s",
        error != NULL ? error->message : "Connection closed by peer");
    ret = GST_FLOW_ERROR;
    goto on_error;
  }

  switch (command) {
    case COMMAND_TYPE_RELEASE_BUFFER:
      GST_ERROR_OBJECT (self, "Received wrong command %d", command);
      ret = GST_FLOW_ERROR;
      goto on_error;
    case COMMAND_TYPE_NEW_BUFFER:{
      NewBufferPayload *new_buffer;
      guint32 payload_off = 0;
#ifdef HAVE_IPC_TARGET_NV
      NvBufSurface *nvbuf_surf = 0;
#endif

      if (!gst_unix_fd_parse_new_buffer (payload, payload_size, &new_buffer,
              &payload_off)) {
        GST_ERROR_OBJECT (self, "Received new-buffer with wrong payload size");
        ret = GST_FLOW_ERROR;
        goto on_error;
      }

      if (fds == NULL) {
        GST_ERROR_OBJECT (self,
            "Received new buffer command without file descriptors");
        return GST_FLOW_ERROR;
      }

      if (g_unix_fd_list_get_length (fds) != new_buffer->n_memory) {
        GST_ERROR_OBJECT (self,
            "Received new buffer command with %d file descriptors instead of "
            "%d", g_unix_fd_list_get_length (fds), new_buffer->n_memory);
        ret = GST_FLOW_ERROR;
        goto on_error;
      }

      if (new_buffer->type >= MEMORY_TYPE_LAST) {
        GST_ERROR_OBJECT (self, "Unknown buffer type %d", new_buffer->type);
        ret = GST_FLOW_ERROR;
        goto on_error;
      }
      GstAllocator *allocator = self->allocators[new_buffer->type];

      gint *fds_arr = g_unix_fd_list_steal_fds (fds, NULL);

      BufferContext *ctx = g_new0 (BufferContext, 1);
      ctx->id = new_buffer->id;
      ctx->n_memory = new_buffer->n_memory;

      *outbuf = gst_buffer_new ();

      GstClockTime base_time =
          gst_element_get_base_time (GST_ELEMENT_CAST (self));
      GstClockTimeDiff clock_diff = 0;
      if (!self->uses_monotonic_clock) {
        clock_diff = GST_CLOCK_DIFF (g_get_monotonic_time () * GST_USECOND,
            gst_clock_get_time (GST_ELEMENT_CLOCK (self)));
      }

#ifdef HAVE_IPC_TARGET_NV
      if (self->buffer_timestamp_copy) {
        GST_BUFFER_PTS (*outbuf) = new_buffer->pts;
        GST_BUFFER_DTS (*outbuf) = new_buffer->dts;
      } else {
#endif
        GST_BUFFER_PTS (*outbuf) =
          calculate_timestamp (new_buffer->pts, base_time, clock_diff);
        GST_BUFFER_DTS (*outbuf) =
          calculate_timestamp (new_buffer->dts, base_time, clock_diff);
#ifdef HAVE_IPC_TARGET_NV
      }
#endif
      GST_BUFFER_DURATION (*outbuf) = new_buffer->duration;
      GST_BUFFER_OFFSET (*outbuf) = new_buffer->offset;
      GST_BUFFER_OFFSET_END (*outbuf) = new_buffer->offset_end;
      GST_BUFFER_FLAGS (*outbuf) = new_buffer->flags;

#ifndef HAVE_IPC_TARGET_NV
      for (int i = 0; i < new_buffer->n_meta; i++) {
        guint32 consumed = 0;
        gst_meta_deserialize (*outbuf, (guint8 *) payload + payload_off,
            payload_size - payload_off, &consumed);
        if (consumed == 0) {
          GST_ERROR_OBJECT (self, "Malformed meta serialization");
          ret = GST_FLOW_ERROR;
          g_free (ctx);
          goto on_error;
        }
        payload_off += consumed;
      }
#endif

#ifdef HAVE_IPC_TARGET_NV
      if (self->nvmm_memory) {
        /* Map and Import the buffer only once when it receive first time and destroy it on stop */
        if (self->buf_mapping == NVBUF_MAPPING_ONCE) {
          NvBufSurfaceMapParams *map_params = (NvBufSurfaceMapParams *) (payload + payload_off);
          nvbuf_surf = g_hash_table_lookup (self->nvbufsurfaces, GINT_TO_POINTER(map_params->fd));
          if (nvbuf_surf == NULL) {
            GST_DEBUG_OBJECT (self, "NvBufSurface is not found for buffer so adding it into hashtable");
            int sender_fd = map_params->fd;
            /* Update map params with receiver side fd */
            map_params->fd = fds_arr[0];
            if (NvBufSurfaceImport(&nvbuf_surf, map_params) < 0) {
              GST_ERROR_OBJECT (self, "NvBufSurfaceImport failed");
              ret = GST_FLOW_ERROR;
              g_free (ctx);
              goto on_error;
            }
            nvbuf_surf->numFilled = 1;
            g_hash_table_insert (self->nvbufsurfaces, GINT_TO_POINTER (sender_fd), nvbuf_surf);
          } else {
            /* close receiver side fd */
            close (fds_arr[0]);
          }
        } else {
        /* Map and Import the buffer always and destroy it everytime */
          NvBufSurfaceMapParams *map_params = (NvBufSurfaceMapParams *) (payload + payload_off);
          map_params->fd = fds_arr[0];
          if (NvBufSurfaceImport(&nvbuf_surf, map_params) < 0) {
            GST_ERROR_OBJECT (self, "NvBufSurfaceImport failed");
            ret = GST_FLOW_ERROR;
            g_free (ctx);
            goto on_error;
          }
          nvbuf_surf->numFilled = 1;
          ctx->nvbuf_surf = nvbuf_surf;
        }
        payload_off += sizeof (NvBufSurfaceMapParams);
        guint *length = (guint *)(payload + payload_off);
        payload_off += sizeof(guint);
        if (*length > 0) {
          guint8 *data = payload + payload_off;
          if (self->deserialize_meta_func) {
            self->deserialize_meta_func(*outbuf, data, *length);
          }
          payload_off = payload_off + *length;
        }
      }
#endif

      GST_OBJECT_LOCK (self);
      for (int i = 0; i < new_buffer->n_memory; i++) {
        GstMemory *mem;
#ifdef HAVE_IPC_TARGET_NV
        if (self->nvmm_memory) {
          GstMapInfo map = GST_MAP_INFO_INIT;
          mem = gst_allocator_alloc (NULL, sizeof(NvBufSurface), NULL);
          gst_memory_map (mem, &map, GST_MAP_WRITE);
          memcpy(map.data, nvbuf_surf, sizeof(NvBufSurface));
          gst_memory_unmap (mem, &map);
        } else {
#endif
          mem = gst_fd_allocator_alloc (allocator, fds_arr[i],
              new_buffer->memories[i].size, GST_FD_MEMORY_FLAG_NONE);
          gst_memory_resize (mem, new_buffer->memories[i].offset,
              new_buffer->memories[i].size);
          GST_MINI_OBJECT_FLAG_SET (mem, GST_MEMORY_FLAG_READONLY);
#ifdef HAVE_IPC_TARGET_NV
        }
#endif
        g_hash_table_insert (self->memories, mem, ctx);
        gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (mem),
            (GstMiniObjectNotify) memory_weak_ref_cb, self);

        gst_buffer_append_memory (*outbuf, mem);
      }
      GST_OBJECT_UNLOCK (self);

      g_free (fds_arr);

      break;
    }
    case COMMAND_TYPE_CAPS:{
      gchar *caps_str;
      if (!gst_unix_fd_parse_caps (payload, payload_size, &caps_str)) {
        GST_ERROR_OBJECT (self, "Received caps string is not nul-terminated");
        ret = GST_FLOW_ERROR;
        goto on_error;
      }
      GstCaps *caps = gst_caps_from_string (caps_str);
      GST_DEBUG_OBJECT (self, "Received caps %" GST_PTR_FORMAT, caps);
      gst_base_src_set_caps (GST_BASE_SRC_CAST (self), caps);
#ifdef HAVE_IPC_TARGET_NV
      GstCapsFeatures *ft = gst_caps_get_features (caps, 0);
      if (gst_caps_features_contains (ft, GST_CAPS_FEATURE_MEMORY_NVMM)) {
        self->nvmm_memory = TRUE;
      } else {
        self->nvmm_memory = FALSE;
      }
#endif
      gst_caps_unref (caps);
      break;
    }
    case COMMAND_TYPE_EOS:{
      GST_DEBUG_OBJECT (self, "Received EOS");
      ret = GST_FLOW_EOS;
      break;
    }
    default:
      /* Protocol could have been extended with new command */
      GST_DEBUG_OBJECT (self, "Ignoring unknown command %d", command);
      break;
  }

  if (*outbuf == NULL && ret == GST_FLOW_OK) {
    g_clear_object (&fds);
    g_clear_pointer (&payload, g_free);
    goto again;
  }

on_error:
  g_clear_error (&error);
  g_clear_object (&fds);
  g_free (payload);
  return ret;
}

static gboolean
gst_unix_fd_src_set_clock (GstElement * element, GstClock * clock)
{
  GstUnixFdSrc *self = (GstUnixFdSrc *) element;

  self->uses_monotonic_clock = FALSE;
  if (clock != NULL && G_OBJECT_TYPE (clock) == GST_TYPE_SYSTEM_CLOCK) {
    GstClockType clock_type;
    g_object_get (clock, "clock-type", &clock_type, NULL);
    self->uses_monotonic_clock = clock_type == GST_CLOCK_TYPE_MONOTONIC;
  }

  return GST_ELEMENT_CLASS (gst_unix_fd_src_parent_class)->set_clock (element,
      clock);
}

static void
gst_unix_fd_src_class_init (GstUnixFdSrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseSrcClass *gstbasesrc_class = (GstBaseSrcClass *) klass;
  GstPushSrcClass *gstpushsrc_class = (GstPushSrcClass *) klass;

#ifdef HAVE_IPC_TARGET_NV
  GST_DEBUG_CATEGORY_INIT (unixfdsrc_debug, "nvunixfdsrc", 0,
      "Unix file descriptor source");
#else
  GST_DEBUG_CATEGORY_INIT (unixfdsrc_debug, "unixfdsrc", 0,
      "Unix file descriptor source");
#endif
  gst_element_class_set_static_metadata (gstelement_class,
      "Unix file descriptor source", "Src", "Unix file descriptor source",
      "Xavier Claessens <xavier.claessens@collabora.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);

  gobject_class->finalize = gst_unix_fd_src_finalize;
  gobject_class->set_property = gst_unix_fd_src_set_property;
  gobject_class->get_property = gst_unix_fd_src_get_property;

  gstelement_class->set_clock = GST_DEBUG_FUNCPTR (gst_unix_fd_src_set_clock);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_unix_fd_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_unix_fd_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_unix_fd_src_unlock);
  gstbasesrc_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_unix_fd_src_unlock_stop);

  gstpushsrc_class->create = gst_unix_fd_src_create;

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
  g_object_class_install_property (gobject_class, PROP_BUFFER_TIMESTAMP_COPY,
    g_param_spec_boolean ("buffer-timestamp-copy", "Buffer timestamp copy",
        "Buffer timestamp copy", DEFAULT_BUFFER_TIMESTAMP_COPY,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CONNECTION_ATTEMPTS,
    g_param_spec_int ("connection-attempts", "connection-attempts",
        "Max number of attempts for connection (-1 = unlimited)",
        -1, G_MAXINT, DEFAULT_CONNECTION_ATTEMPTS, G_PARAM_READWRITE |
        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CONNECTION_INTERVAL,
    g_param_spec_uint64 ("connection-interval", "connection-interval",
        "connection interval between connection attempts in micro seconds",
        0, G_MAXUINT64, DEFAULT_CONNECTION_INTERVAL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_META_DESERIALIZATION_LIB_NAME,
    g_param_spec_string ("meta-deserialization-lib", "Meta deserialization library name",
        "Set meta deserialization library name to be used", NULL,
         (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif
}
