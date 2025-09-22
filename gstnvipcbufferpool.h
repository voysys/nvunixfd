/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#ifndef GSTNVIPCBUFFERPOOL_H_
#define GSTNVIPCBUFFERPOOL_H_

#include <gst/gst.h>
#include "nvbufsurface.h"

G_BEGIN_DECLS

typedef struct _GstNvIpcBufferPool GstNvIpcBufferPool;
typedef struct _GstNvIpcBufferPoolClass GstNvIpcBufferPoolClass;
typedef struct _GstNvIpcBufferPoolPrivate GstNvIpcBufferPoolPrivate;

#define GST_TYPE_NVIPC_BUFFER_POOL      (gst_nvipc_buffer_pool_get_type())
#define GST_IS_NVIPC_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_NVIPC_BUFFER_POOL))
#define GST_NVIPC_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_NVIPC_BUFFER_POOL, GstNvIpcBufferPool))
#define GST_NVIPC_BUFFER_POOL_CAST(obj) ((GstNvIpcBufferPool*)(obj))

#define GST_NVIPC_MEMORY_TYPE "nvipc"
#define GST_BUFFER_POOL_OPTION_NVIPC_META "GstBufferPoolOptionNvIpcMeta"
#define NV_GST_BUFFER_POOL_MAX_SIZE 8

struct _GstNvIpcBufferPool
{
  GstBufferPool bufferpool;

  GstNvIpcBufferPoolPrivate *priv;
  guint num_queued;
};

struct _GstNvIpcBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_nvipc_buffer_pool_get_type (void);

GstBufferPool* gst_nvipc_buffer_pool_new (void);

G_END_DECLS

#endif /* GSTNVIPCBUFFERPOOL_H_ */
