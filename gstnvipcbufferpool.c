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

#include <stdio.h>
#include <string.h>
#include <gst/video/video.h>
#include "gstnvipcbufferpool.h"

GST_DEBUG_CATEGORY_STATIC (gst_nvipc_pool_debug);
#define GST_CAT_DEFAULT gst_nvipc_pool_debug

typedef struct _GstNvIpcMemoryAllocator GstNvIpcMemoryAllocator;
typedef struct _GstNvIpcMemoryAllocatorClass GstNvIpcMemoryAllocatorClass;

typedef struct GstNvIpcMemory {
  GstMemory mem;

  NvBufSurface *data;
} GstNvIpcMemory;

struct _GstNvIpcMemoryAllocator
{
  GstAllocator parent;
};

struct _GstNvIpcMemoryAllocatorClass
{
  GstAllocatorClass parent_class;
};

struct _GstNvIpcBufferPoolPrivate
{
  GstCaps *caps;
  GstVideoInfo info;
  GstAllocator *allocator;
  GstAllocationParams params;
  guint gpuId;
  guint batchSize;
  guint colorSpace;
  guint layout;
  guint size;
  gboolean addNvIpcMeta;
  NvBufSurfaceMemType memType;
  gboolean clearChroma;
  guint planeOrder;
  guint precision;
  gboolean isContiguous;
  NvBufSurfaceTag memtag;
  gboolean disablePitchPadding;
};

GType gst_nvipc_memory_allocator_get_type (void);

#define gst_nvipc_buffer_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstNvIpcBufferPool, gst_nvipc_buffer_pool, GST_TYPE_BUFFER_POOL, G_ADD_PRIVATE (GstNvIpcBufferPool));

G_DEFINE_TYPE (GstNvIpcMemoryAllocator, gst_nvipc_memory_allocator,
    GST_TYPE_ALLOCATOR);


static NvBufSurfaceColorFormat
get_nvbuf_surface_format (GstVideoFormat format, GstVideoColorimetry *cinfo,
        gboolean planeOrder, guint precision)
{
    NvBufSurfaceColorFormat dstFormat = NVBUF_COLOR_FORMAT_INVALID;
    switch (format) {
    case GST_VIDEO_FORMAT_I420:
      dstFormat = NVBUF_COLOR_FORMAT_YUV420;
      if (cinfo->range == GST_VIDEO_COLOR_RANGE_0_255) {
          if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
              dstFormat = NVBUF_COLOR_FORMAT_YUV420_709_ER;
          else
              dstFormat = NVBUF_COLOR_FORMAT_YUV420_ER;
      }
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_YUV420_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_YUV420_2020;
      break;

    case  GST_VIDEO_FORMAT_NV12:
      dstFormat = NVBUF_COLOR_FORMAT_NV12;
      if (cinfo->range == GST_VIDEO_COLOR_RANGE_0_255) {
          if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
              dstFormat = NVBUF_COLOR_FORMAT_NV12_709_ER;
          else
              dstFormat = NVBUF_COLOR_FORMAT_NV12_ER;
      }
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_2020;
      break;
    case  GST_VIDEO_FORMAT_NV21:
      dstFormat = NVBUF_COLOR_FORMAT_NV21;
      break;
    case  GST_VIDEO_FORMAT_UYVY:
      dstFormat = NVBUF_COLOR_FORMAT_UYVY;
      break;
    case  GST_VIDEO_FORMAT_RGBx:
      dstFormat = NVBUF_COLOR_FORMAT_RGBx;
      break;
    case GST_VIDEO_FORMAT_Y444:
      dstFormat = NVBUF_COLOR_FORMAT_YUV444;
      if (cinfo->range == GST_VIDEO_COLOR_RANGE_0_255) {
          if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
              dstFormat = NVBUF_COLOR_FORMAT_YUV444_709_ER;
          else
              dstFormat = NVBUF_COLOR_FORMAT_YUV444_ER;
      }
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_2020;
      break;
    case GST_VIDEO_FORMAT_Y444_10LE:
      dstFormat = NVBUF_COLOR_FORMAT_YUV444_10LE;
      if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_10LE_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_10LE_2020;
      break;
    case GST_VIDEO_FORMAT_Y444_12LE:
      dstFormat = NVBUF_COLOR_FORMAT_YUV444_12LE;
      if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_12LE_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_YUV444_12LE_2020;
      break;
    case  GST_VIDEO_FORMAT_BGRx:
      dstFormat = NVBUF_COLOR_FORMAT_BGRx;
      break;
    case  GST_VIDEO_FORMAT_xRGB:
      dstFormat = NVBUF_COLOR_FORMAT_xRGB;
      break;
    case  GST_VIDEO_FORMAT_xBGR:
      dstFormat = NVBUF_COLOR_FORMAT_xBGR;
      break;
    case  GST_VIDEO_FORMAT_RGBA:
      dstFormat = NVBUF_COLOR_FORMAT_RGBA;
      break;
    case  GST_VIDEO_FORMAT_BGRA:
      dstFormat = NVBUF_COLOR_FORMAT_BGRA;
      break;
    case  GST_VIDEO_FORMAT_ARGB:
      dstFormat = NVBUF_COLOR_FORMAT_ARGB;
      break;
    case  GST_VIDEO_FORMAT_ABGR:
      dstFormat = NVBUF_COLOR_FORMAT_ABGR;
      break;
    case  GST_VIDEO_FORMAT_RGB:
      dstFormat = NVBUF_COLOR_FORMAT_RGB;
      break;
    case  GST_VIDEO_FORMAT_BGR:
      dstFormat = NVBUF_COLOR_FORMAT_BGR;
      break;
    case  GST_VIDEO_FORMAT_GRAY8:
      dstFormat = NVBUF_COLOR_FORMAT_GRAY8;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
      dstFormat = NVBUF_COLOR_FORMAT_NV12_10LE;
      if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_10LE_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_10LE_2020;
      break;
    case GST_VIDEO_FORMAT_I420_12LE:
      dstFormat = NVBUF_COLOR_FORMAT_NV12_12LE;
      if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT709)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_12LE_709;
      else if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_NV12_12LE_2020;
      break;
      //Use same planar format for RGB or BGR
    case GST_VIDEO_FORMAT_GBR:
      // by default 8 bit
      if (precision ==0 ){
          // Default RGB else BGR plane ordering
          if (planeOrder==0)
            dstFormat = NVBUF_COLOR_FORMAT_R8_G8_B8;
          else if (planeOrder == 1)
            dstFormat = NVBUF_COLOR_FORMAT_B8_G8_R8;
      }else if (precision == 1) {
          // Default RGB else BGR plane ordering
          if (planeOrder==0)
            dstFormat = NVBUF_COLOR_FORMAT_R32F_G32F_B32F;
          else if (planeOrder == 1)
            dstFormat = NVBUF_COLOR_FORMAT_B32F_G32F_R32F;
      }
      //TODO: add fp16 precision == 2
      break;
    case GST_VIDEO_FORMAT_Y42B:
      dstFormat =  NVBUF_COLOR_FORMAT_YUV422;
      break;
    case GST_VIDEO_FORMAT_UYVP:
      dstFormat = NVBUF_COLOR_FORMAT_UYVP;
      if (cinfo->range == GST_VIDEO_COLOR_RANGE_0_255)
          dstFormat = NVBUF_COLOR_FORMAT_UYVP_ER;
      break;
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      dstFormat = NVBUF_COLOR_FORMAT_BGRA_10_10_10_2_709;
      if (cinfo->matrix == GST_VIDEO_COLOR_MATRIX_BT2020)
          dstFormat = NVBUF_COLOR_FORMAT_BGRA_10_10_10_2_2020;
      break;
    default:
      dstFormat = NVBUF_COLOR_FORMAT_INVALID;
      break;
  }
  return dstFormat;
}

static gpointer
gst_nvipc_memory_map (GstMemory *mem, gsize maxsize, GstMapFlags flags)
{
  GstNvIpcMemory *dsmem = (GstNvIpcMemory *) mem;
  return dsmem->data;
}

static void
gst_nvipc_memory_unmap (GstMemory *mem)
{
  // Nothing to do here.
}

static GstMemory *
gst_nvipc_memory_share (GstMemory *mem, gssize offset, gssize size)
{
  /*
     Currently it won't be used because memory is non-shared.
   */
  g_assert_not_reached ();
  return NULL;
}

static void
gst_nvipc_memory_allocator_init (GstNvIpcMemoryAllocator *allocator)
{
  GstAllocator *parent = GST_ALLOCATOR_CAST (allocator);

  parent->mem_type = GST_NVIPC_MEMORY_TYPE;
  parent->mem_map = gst_nvipc_memory_map;
  parent->mem_unmap = gst_nvipc_memory_unmap;
  parent->mem_share = gst_nvipc_memory_share;

  /* We want to use default implementation of ->mem_copy which uses
     default allocator for memory allocation and then do memcpy().
     We are using this approach because we haven't implemented
     ->alloc() funciton.
     Secondly, we want to copy NvMMBuffer structure only not the deep copy.
   */

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static GstMemory *
gst_nvipc_memory_allocator_alloc (GstMemoryFlags flags,
                                 GstNvIpcBufferPoolPrivate *priv)
{
  GstNvIpcMemory *mem = NULL;
  NvBufSurface *surf = NULL;
  NvBufSurfaceAllocateParams param = {0};
  int status = -1;

  mem = g_slice_new0 (GstNvIpcMemory);

  param.params.width = priv->info.width;
  param.params.height = priv->info.height;
  param.params.memType = priv->memType;
  param.params.gpuId = priv->gpuId;
  param.params.isContiguous = priv->isContiguous;
  param.memtag = priv->memtag;
  param.disablePitchPadding = priv->disablePitchPadding;

  param.params.colorFormat = get_nvbuf_surface_format (GST_VIDEO_INFO_FORMAT (&priv->info),
      &(priv->info.colorimetry), priv->planeOrder, priv->precision);
  // Need to check if for NvBufSurface Array support present on Jetson
  if (((param.params.colorFormat == NVBUF_COLOR_FORMAT_UYVP) ||
       (param.params.colorFormat == NVBUF_COLOR_FORMAT_UYVP_ER)) &&
       (param.params.memType == NVBUF_MEM_SURFACE_ARRAY)){

    printf ("NvIpcBufferPool: Surface Array allocation for NVBUF_COLOR_FORMAT_UYVP(_ER) unsupported,"\
    " changing to cuda Device");
    param.params.memType = NVBUF_MEM_CUDA_DEVICE;
  }

  if (priv->layout)
    param.params.layout =  NVBUF_LAYOUT_BLOCK_LINEAR;
  else
    param.params.layout =   NVBUF_LAYOUT_PITCH;

  status = NvBufSurfaceAllocate(&surf, priv->batchSize, &param);

  if (status < 0) {
    printf ("Error(%d) in buffer allocation\n", status);
    goto error;
  }

  if (priv->clearChroma) {
    guint i, j;
    for (i = 0; i < surf->batchSize; i++) {
      for (j = 0; j < surf->surfaceList[i].planeParams.num_planes; j++) {
        /* Memset 0th surface to 0 and other surfaces to 128 for black color */
        status = NvBufSurfaceMemSet (surf, i, j, ((j == 0) ? 0 : 128));
        if (status < 0) {
          printf ("Error(%d) in buffer memset\n", status);
          goto error;
        }
      }
    }
  }


  flags = (GstMemoryFlags) (flags | GST_MEMORY_FLAG_NO_SHARE);

  gst_memory_init (GST_MEMORY_CAST (mem), flags, priv->allocator, NULL,
                   sizeof (NvBufSurface), 0, 0, sizeof (NvBufSurface));

  mem->data = surf;

  return GST_MEMORY_CAST (mem);

error:
  if (surf && surf->surfaceList) {
    NvBufSurfaceDestroy (surf);
  }
  g_slice_free (GstNvIpcMemory, mem);

  return NULL;
}

static GstMemory *
gst_nvipc_memory_allocator_alloc_dummy (GstAllocator *allocator,
        gsize size, GstAllocationParams *params)
{
  /*
     We are not using this function for memory allocation because
     to allocate NvBufSurface we need additional information which
     can't be passed here. We have defined custom function
     gst_nvipc_memory_allocator_alloc for this.
   */
  g_assert_not_reached ();
  return NULL;
}

static void
gst_nvipc_memory_allocator_free (GstAllocator *allocator, GstMemory *memory)
{
  GstNvIpcMemory *dsmem = (GstNvIpcMemory *) memory;
  NvBufSurface *surf = dsmem->data;


  NvBufSurfaceDestroy (surf);
  g_slice_free (GstNvIpcMemory, dsmem);
}

static void
gst_nvipc_memory_allocator_class_init (GstNvIpcMemoryAllocatorClass *klass)
{
  GstAllocatorClass *allocator_class;

  allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_nvipc_memory_allocator_alloc_dummy;
  allocator_class->free = gst_nvipc_memory_allocator_free;
}

static void
gst_nvipc_buffer_pool_finalize (GObject *object)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (object);
  GstNvIpcBufferPoolPrivate *priv = pool->priv;

  if (priv->caps)
    gst_caps_unref (priv->caps);
  priv->caps = NULL;

  if (priv->allocator)
    gst_object_unref (priv->allocator);
  priv->allocator = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static const gchar ** gst_nvipc_buffer_pool_get_options (GstBufferPool *bpool)
{
  static const gchar *pool_options[] = {
      GST_BUFFER_POOL_OPTION_NVIPC_META, NULL };

  /*
   * Currently, we are only providing NVIPC_META option by default.
   * We might need to add other options e.g. VIDEO_ALIGNMENT here.
   */
  return pool_options;
}

static gboolean
gst_nvipc_buffer_pool_set_config (GstBufferPool *bpool, GstStructure *config)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);
  GstNvIpcBufferPoolPrivate *priv = pool->priv;
  GstCaps *caps = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;
  GstVideoInfo info;
  guint size;
  gchar *capsString = NULL;

  GST_OBJECT_LOCK (pool);

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  capsString = gst_caps_to_string (caps);
  GST_DEBUG_OBJECT (pool, "caps : %s\n", capsString);
  g_free(capsString);

  if (!gst_video_info_from_caps (&info, caps))
    goto wrong_video_caps;

  //TODO: Option to allocate metadata
  /* enable metadata based on config of the pool */
  priv->addNvIpcMeta = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_NVIPC_META);

  priv->info = info;
  priv->size = size;

  if (!priv->size)
    priv->size = info.size;

  if (priv->caps)
    gst_caps_unref (priv->caps);
  priv->caps = gst_caps_ref (caps);

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, &params))
    goto wrong_config;

  if (allocator) {
    // Update user provided allocator
    if (priv->allocator)
      gst_object_unref (priv->allocator);
    if ((priv->allocator = allocator))
      gst_object_ref (allocator);

    priv->params = params;
  }

  if (!gst_structure_get_uint (config, "memtype", (guint*)(&priv->memType)))
    priv->memType = NVBUF_MEM_DEFAULT;

  if (!gst_structure_get_uint (config, "gpu-id", &priv->gpuId))
    priv->gpuId = 0;

  if (!gst_structure_get_uint (config, "memtag", (guint*)(&priv->memtag)))
    priv->memtag = NvBufSurfaceTag_NONE;

  if (!gst_structure_get_uint (config, "batch-size", &priv->batchSize) ||
      !priv->batchSize) {
    GST_DEBUG_OBJECT (pool, "batch-size not set, using 1 as default value.");
    priv->batchSize = 1;
  }

  if (!gst_structure_get_boolean (config, "clear-chroma", &priv->clearChroma))
    priv->clearChroma = FALSE;

  //Check RGB plane ordering for planar RGB format by default, RGB else BGR
  if (!gst_structure_get_uint (config,"plane-order", &priv->planeOrder))
    priv->planeOrder = 0;

  // for RGB planar format, precision=0 u8, precision=1 fp32
  if (!gst_structure_get_uint (config,"precision", &priv->precision))
    priv->precision = 0;

  if (!gst_structure_get_boolean (config,"disable-pitch-padding", &priv->disablePitchPadding))
    priv->disablePitchPadding = 0;

  if (!allocator) {
    // Allocator is not set, use private allocator.
    gst_buffer_pool_config_set_allocator (config, priv->allocator,
                                          &priv->params);
  }

  // Check block linear layout flag
  if (!gst_structure_get_uint (config,"bl-output", &priv->layout))
    priv->layout = 0;

  if (!gst_structure_get_boolean (config, "contiguous-alloc", &priv->isContiguous))
    priv->isContiguous = FALSE;

  GST_OBJECT_UNLOCK (pool);

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (bpool, config);

  /* ERRORS */
wrong_config:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_video_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool,
        "failed getting video info from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static gboolean
gst_nvipc_buffer_pool_start (GstBufferPool *bpool)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "start");


  return GST_BUFFER_POOL_CLASS (parent_class)->start (bpool);
}

static gboolean gst_nvipc_buffer_pool_stop (GstBufferPool *bpool)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "stop");

  return GST_BUFFER_POOL_CLASS (parent_class)->stop (bpool);
}

static GstFlowReturn gst_nvipc_buffer_pool_acquire_buffer (
    GstBufferPool *bpool, GstBuffer **buffer,
    GstBufferPoolAcquireParams *params)
{
  GstFlowReturn ret;
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "acquire_buffer");
  g_atomic_int_inc (&pool->num_queued);

  /*
   * Currently, we are using base class version because we are not attaching
   * anything extra to buffers. If we need to do so We will modify this function
   * accordingly.
   */
  ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (bpool, buffer, params);

  return ret;
}

static GstFlowReturn
gst_nvipc_buffer_pool_alloc_buffer (GstBufferPool *bpool,
    GstBuffer **buffer, GstBufferPoolAcquireParams *params)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);
  GstNvIpcBufferPoolPrivate *priv = pool->priv;
  GstBuffer *buf = NULL;
  GstMemory *mem = NULL;

  GST_DEBUG_OBJECT (pool, "alloc_buffer");

  if (!g_strcmp0(priv->allocator->mem_type, GST_NVIPC_MEMORY_TYPE)) {
    mem = gst_nvipc_memory_allocator_alloc ((GstMemoryFlags) 0, priv);
    g_return_val_if_fail (mem, GST_FLOW_ERROR);

    buf = gst_buffer_new ();
    gst_buffer_append_memory (buf, mem);
  } else {
    buf = gst_buffer_new_allocate (priv->allocator, priv->size,
                                   &priv->params);

    g_return_val_if_fail (buf, GST_FLOW_ERROR);
  }

  if (pool->priv->addNvIpcMeta) {
    //TODO: Add NvIpc meta to buffer.
  }

  *buffer = buf;

  return GST_FLOW_OK;
}

static void gst_nvipc_buffer_pool_release_buffer (GstBufferPool *bpool,
        GstBuffer *buffer)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);
  guint num_queued;

  GST_DEBUG_OBJECT (pool, "release_buffer");

  num_queued = g_atomic_int_get (&pool->num_queued);

  if (num_queued > 0) {
    g_atomic_int_add (&pool->num_queued, -1);
  }

  GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (bpool, buffer);
}

static void gst_nvipc_buffer_pool_free_buffer (GstBufferPool *bpool,
        GstBuffer *buffer)
{
  GstNvIpcBufferPool *pool = GST_NVIPC_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "free_buffer");

  GST_BUFFER_POOL_CLASS (parent_class)->free_buffer (bpool, buffer);
}

static void gst_nvipc_buffer_pool_init (GstNvIpcBufferPool *pool)
{
  GstNvIpcBufferPool *self = GST_NVIPC_BUFFER_POOL (pool);
  pool->priv = (GstNvIpcBufferPoolPrivate *) gst_nvipc_buffer_pool_get_instance_private (self);
  memset (pool->priv, 0, sizeof(GstNvIpcBufferPoolPrivate));

  pool->priv->allocator = (GstAllocator *) g_object_new (
            gst_nvipc_memory_allocator_get_type (), NULL);

  pool->priv->memType = NVBUF_MEM_DEFAULT;
  pool->priv->gpuId = 0;
  pool->priv->batchSize = 1;
  pool->priv->memtag = NvBufSurfaceTag_NONE;

  gst_allocation_params_init (&pool->priv->params);
}

static void
gst_nvipc_buffer_pool_class_init (GstNvIpcBufferPoolClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = gst_nvipc_buffer_pool_finalize;
  gstbufferpool_class->start = gst_nvipc_buffer_pool_start;
  gstbufferpool_class->stop = gst_nvipc_buffer_pool_stop;
  gstbufferpool_class->get_options = gst_nvipc_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_nvipc_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_nvipc_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = gst_nvipc_buffer_pool_free_buffer;
  gstbufferpool_class->acquire_buffer = gst_nvipc_buffer_pool_acquire_buffer;
  gstbufferpool_class->release_buffer = gst_nvipc_buffer_pool_release_buffer;

  GST_DEBUG_CATEGORY_INIT (gst_nvipc_pool_debug, "nvipcpool", 0,
        "nvipc buffer pool object");
}

GstBufferPool * gst_nvipc_buffer_pool_new (void)
{
  GstNvIpcBufferPool *pool;
  pool = (GstNvIpcBufferPool *) g_object_new (GST_TYPE_NVIPC_BUFFER_POOL, NULL);

  return GST_BUFFER_POOL (pool);
}
