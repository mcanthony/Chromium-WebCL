/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "./vpx_config.h"
#include "vpx_scale/yv12config.h"
#include "vpx_mem/vpx_mem.h"
#include "vp9/common/inter_ocl/vp9_inter_ocl_init.h"
#include "vp9/common/inter_ocl/vp9_inter_ocl_calcu.h"
#include "vp9/common/inter_ocl/vp9_inter_ocl_param.h"

/****************************************************************************
*  Exports
****************************************************************************/

/****************************************************************************
 *
 ****************************************************************************/

#define yv12_align_addr(addr, align) \
  (void*)(((size_t)(addr) + ((align) - 1)) & (size_t)-(align))

int vp9_free_frame_buffer_ocl(int fb_index);
int vp8_yv12_de_alloc_frame_buffer(YV12_BUFFER_CONFIG *ybf) {
  if (ybf) {
    // If libvpx is using external frame buffers then buffer_alloc_sz must
    // not be set.
    if (ybf->buffer_alloc_sz > 0) {
      vpx_free(ybf->buffer_alloc);
    }

    /* buffer_alloc isn't accessed by most functions.  Rather y_buffer,
      u_buffer and v_buffer point to buffer_alloc and are used.  Clear out
      all of this so that a freed pointer isn't inadvertently used */
    vpx_memset(ybf, 0, sizeof(YV12_BUFFER_CONFIG));
  } else {
    return -1;
  }

  return 0;
}

int vp8_yv12_realloc_frame_buffer(YV12_BUFFER_CONFIG *ybf,
                                  int width, int height, int border) {
  if (ybf) {
    int aligned_width = (width + 15) & ~15;
    int aligned_height = (height + 15) & ~15;
    int y_stride = ((aligned_width + 2 * border) + 31) & ~31;
    int yplane_size = (aligned_height + 2 * border) * y_stride;
    int uv_width = aligned_width >> 1;
    int uv_height = aligned_height >> 1;
    /** There is currently a bunch of code which assumes
      *  uv_stride == y_stride/2, so enforce this here. */
    int uv_stride = y_stride >> 1;
    int uvplane_size = (uv_height + border) * uv_stride;
    const int frame_size = yplane_size + 2 * uvplane_size;

    if (!ybf->buffer_alloc) {
      ybf->buffer_alloc = vpx_memalign(32, frame_size);
      ybf->buffer_alloc_sz = frame_size;
    }

    if (!ybf->buffer_alloc || ybf->buffer_alloc_sz < frame_size)
      return -1;

    /* Only support allocating buffers that have a border that's a multiple
     * of 32. The border restriction is required to get 16-byte alignment of
     * the start of the chroma rows without introducing an arbitrary gap
     * between planes, which would break the semantics of things like
     * vpx_img_set_rect(). */
    if (border & 0x1f)
      return -3;

    ybf->y_crop_width = width;
    ybf->y_crop_height = height;
    ybf->y_width  = aligned_width;
    ybf->y_height = aligned_height;
    ybf->y_stride = y_stride;

    ybf->uv_width = uv_width;
    ybf->uv_height = uv_height;
    ybf->uv_stride = uv_stride;

    ybf->alpha_width = 0;
    ybf->alpha_height = 0;
    ybf->alpha_stride = 0;

    ybf->border = border;
    ybf->frame_size = frame_size;

    ybf->y_buffer = ybf->buffer_alloc + (border * y_stride) + border;
    ybf->u_buffer = ybf->buffer_alloc + yplane_size +
                    (border / 2  * uv_stride) + border / 2;
    ybf->v_buffer = ybf->buffer_alloc + yplane_size + uvplane_size
                    + (border / 2  * uv_stride) + border / 2;
    ybf->alpha_buffer = NULL;

    ybf->corrupted = 0; /* assume not currupted by errors */
    return 0;
  }
  return -2;
}

int vp8_yv12_alloc_frame_buffer(YV12_BUFFER_CONFIG *ybf,
                                int width, int height, int border) {
  if (ybf) {
    vp8_yv12_de_alloc_frame_buffer(ybf);
    return vp8_yv12_realloc_frame_buffer(ybf, width, height, border);
  }
  return -2;
}

#if CONFIG_VP9
// TODO(jkoleszar): Maybe replace this with struct vpx_image

int vp9_free_frame_buffer(YV12_BUFFER_CONFIG *ybf) {
  if (ybf) {
    if (ybf->buffer_alloc_sz > 0) {
#if USE_INTER_PREDICT_OCL
      //vp9_free_frame_buffer_ocl(ybf);
#else
      vpx_free(ybf->buffer_alloc);
#endif // USE_INTER_PREDICT_OCL
    }

    /* buffer_alloc isn't accessed by most functions.  Rather y_buffer,
      u_buffer and v_buffer point to buffer_alloc and are used.  Clear out
      all of this so that a freed pointer isn't inadvertently used */
    vpx_memset(ybf, 0, sizeof(YV12_BUFFER_CONFIG));
  } else {
    return -1;
  }

  return 0;
}

int vp9_realloc_frame_buffer(YV12_BUFFER_CONFIG *ybf,
                             int width, int height,
                             int ss_x, int ss_y, int border,
                             vpx_codec_frame_buffer_t *ext_fb,
                             vpx_realloc_frame_buffer_cb_fn_t cb,
                             void *user_priv) {
  if (ybf) {
    const int aligned_width = (width + 7) & ~7;
    const int aligned_height = (height + 7) & ~7;
    const int y_stride = ((aligned_width + 2 * border) + 31) & ~31;
    const int yplane_size = (aligned_height + 2 * border) * y_stride;
    const int uv_width = aligned_width >> ss_x;
    const int uv_height = aligned_height >> ss_y;
    const int uv_stride = y_stride >> ss_x;
    const int uv_border_w = border >> ss_x;
    const int uv_border_h = border >> ss_y;
    const int uvplane_size = (uv_height + 2 * uv_border_h) * uv_stride;
#if CONFIG_ALPHA
    const int alpha_width = aligned_width;
    const int alpha_height = aligned_height;
    const int alpha_stride = y_stride;
    const int alpha_border_w = border;
    const int alpha_border_h = border;
    const int alpha_plane_size = (alpha_height + 2 * alpha_border_h) *
                                 alpha_stride;
    const int frame_size = yplane_size + 2 * uvplane_size +
                           alpha_plane_size;
#else
    const int frame_size = yplane_size + 2 * uvplane_size;
#endif

    if (ext_fb != NULL) {
      const int align_addr_extra_size = 31;
      const size_t external_frame_size = frame_size + align_addr_extra_size;
      if (external_frame_size > ext_fb->size) {
        // Allocation to hold larger frame, or first allocation.
        if (cb(user_priv, external_frame_size, ext_fb) < 0) {
          return -1;
        }

        if (ext_fb->data == NULL || ext_fb->size < external_frame_size) {
          return -1;
        }

        // This memset is needed for fixing valgrind error from C loop filter
        // due to access uninitialized memory in frame border. It could be
        // removed if border is totally removed.
        vpx_memset(ext_fb->data, 0, ext_fb->size);

        ybf->buffer_alloc = yv12_align_addr(ext_fb->data, 32);
      }
    } else {
      if (frame_size > ybf->buffer_alloc_sz) {
        // Allocation to hold larger frame, or first allocation.
        if (ybf->buffer_alloc)
          vpx_free(ybf->buffer_alloc);
        ybf->buffer_alloc = vpx_memalign(32, frame_size);
        if (!ybf->buffer_alloc)
          return -1;

        ybf->buffer_alloc_sz = frame_size;

        // This memset is needed for fixing valgrind error from C loop filter
        // due to access uninitialized memory in frame boarder. It could be
        // removed if border is totally removed.
        vpx_memset(ybf->buffer_alloc, 0, ybf->buffer_alloc_sz);
      }

      if (ybf->buffer_alloc_sz < frame_size)
        return -1;
    }

    /* Only support allocating buffers that have a border that's a multiple
     * of 32. The border restriction is required to get 16-byte alignment of
     * the start of the chroma rows without introducing an arbitrary gap
     * between planes, which would break the semantics of things like
     * vpx_img_set_rect(). */
    if (border & 0x1f)
      return -3;

    ybf->y_crop_width = width;
    ybf->y_crop_height = height;
    ybf->y_width  = aligned_width;
    ybf->y_height = aligned_height;
    ybf->y_stride = y_stride;

    ybf->uv_crop_width = (width + ss_x) >> ss_x;
    ybf->uv_crop_height = (height + ss_y) >> ss_y;
    ybf->uv_width = uv_width;
    ybf->uv_height = uv_height;
    ybf->uv_stride = uv_stride;

    ybf->border = border;
    ybf->frame_size = frame_size;

    ybf->y_buffer = ybf->buffer_alloc + (border * y_stride) + border;
    ybf->u_buffer = ybf->buffer_alloc + yplane_size +
                    (uv_border_h * uv_stride) + uv_border_w;
    ybf->v_buffer = ybf->buffer_alloc + yplane_size + uvplane_size +
                    (uv_border_h * uv_stride) + uv_border_w;

#if CONFIG_ALPHA
    ybf->alpha_width = alpha_width;
    ybf->alpha_height = alpha_height;
    ybf->alpha_stride = alpha_stride;
    ybf->alpha_buffer = ybf->buffer_alloc + yplane_size + 2 * uvplane_size +
                        (alpha_border_h * alpha_stride) + alpha_border_w;
#endif
    ybf->corrupted = 0; /* assume not corrupted by errors */
    return 0;
  }
  return -2;
}

int vp9_alloc_frame_buffer(YV12_BUFFER_CONFIG *ybf,
                           int width, int height,
                           int ss_x, int ss_y, int border) {
  if (ybf) {
    vp9_free_frame_buffer(ybf);
    return vp9_realloc_frame_buffer(ybf, width, height, ss_x, ss_y, border,
                                    NULL, NULL, NULL);
  }
  return -2;
}

#if USE_INTER_PREDICT_OCL
int vp9_free_frame_buffer_ocl(int fb_index) {
  vpx_memset(inter_ocl_obj.buffer_pool_map_ptr + fb_index *
             (inter_ocl_obj.buffer_size ), 0, (inter_ocl_obj.buffer_size ));
  return 0;
}


void *vpx_memalign_ocl(size_t align, size_t size, YV12_BUFFER_CONFIG *ybf) {
  uint8_t * addr = NULL;
  int status;
  if(inter_ocl_obj.buffer_pool_flag == 0) {
    inter_ocl_obj.buffer_pool_kernel = clCreateBuffer(
                                         ocl_context.context,
                                         CL_MEM_ALLOC_HOST_PTR |
                                         CL_MEM_WRITE_ONLY,
                                         size * FRAME_BUFFERS,
                                         NULL, &status);
    if (status != CL_SUCCESS) {
      printf("Failed to clCreateBuffe buffer_pool_kernel, error: %d \n", status);
      return NULL;
    }

    inter_ocl_obj.buffer_pool_read_only_kernel = clCreateBuffer(
                                                     ocl_context.context,
                                                     CL_MEM_READ_ONLY,
                                                     size * FRAME_BUFFERS,
                                                     NULL, &status);
    if (status != CL_SUCCESS) {
      printf("Failed to clCreateBuffe buffer_pool_read_only_kernel, error: %d \n",
             status);
      return NULL;
    }

    //map buffer pool ptr
    inter_ocl_obj.buffer_pool_map_ptr= (uint8_t *) clEnqueueMapBuffer(
                                                ocl_context.command_queue,
                                                inter_ocl_obj.buffer_pool_kernel,
                                                CL_TRUE, CL_MAP_WRITE, 0,
                                                size * FRAME_BUFFERS,
                                                0, NULL, NULL, &status);
    if (status != CL_SUCCESS) {
      printf("Failed to clEnqueueMapBuffer buffer_pool_kernel, error: %d \n", status);
      return NULL;
    }

   inter_ocl_obj.buffer_pool_flag = 1;
 }

  assert(inter_ocl_obj.buffer_pool_map_ptr != NULL);
  addr = inter_ocl_obj.buffer_pool_map_ptr +
         (ybf->nFrameNum *  size );

  return (void*)addr;
}

int vp9_realloc_frame_buffer_ocl(YV12_BUFFER_CONFIG *ybf,
                             int width, int height,
                             int ss_x, int ss_y, int border,
                             vpx_codec_frame_buffer_t *ext_fb,
                             vpx_realloc_frame_buffer_cb_fn_t cb,
                             void *user_priv, int new_fb_idx) {
  if (ybf) {
    const int aligned_width = (width + 7) & ~7;
    const int aligned_height = (height + 7) & ~7;
    const int y_stride = ((aligned_width + 2 * border) + 31) & ~31;
    const int yplane_size = (aligned_height + 2 * border) * y_stride;
    const int uv_width = aligned_width >> ss_x;
    const int uv_height = aligned_height >> ss_y;
    const int uv_stride = y_stride >> ss_x;
    const int uv_border_w = border >> ss_x;
    const int uv_border_h = border >> ss_y;
    const int uvplane_size = (uv_height + 2 * uv_border_h) * uv_stride;
#if CONFIG_ALPHA
    const int alpha_width = aligned_width;
    const int alpha_height = aligned_height;
    const int alpha_stride = y_stride;
    const int alpha_border_w = border;
    const int alpha_border_h = border;
    const int alpha_plane_size = (alpha_height + 2 * alpha_border_h) *
                                 alpha_stride;
    const int frame_size = yplane_size + 2 * uvplane_size +
                           alpha_plane_size;
#else
    const int frame_size = yplane_size + 2 * uvplane_size;
#endif

    if (ext_fb != NULL) {
      const int align_addr_extra_size = 31;
      const size_t external_frame_size = frame_size + align_addr_extra_size;
      if (external_frame_size > ext_fb->size) {
        // Allocation to hold larger frame, or first allocation.
        if (cb(user_priv, external_frame_size, ext_fb) < 0) {
          return -1;
        }

        if (ext_fb->data == NULL || ext_fb->size < external_frame_size) {
          return -1;
        }

        // This memset is needed for fixing valgrind error from C loop filter
        // due to access uninitialized memory in frame border. It could be
        // removed if border is totally removed.
        vpx_memset(ext_fb->data, 0, ext_fb->size);

        ybf->buffer_alloc = yv12_align_addr(ext_fb->data, 32);
      }
    } else {
      if (frame_size > ybf->buffer_alloc_sz) {
        // Allocation to hold larger frame, or first allocation.
        if (ybf->buffer_alloc) {
#if USE_INTER_PREDICT_OCL
          vp9_free_frame_buffer_ocl(new_fb_idx);
#endif
        }
        ybf->nFrameNum =  new_fb_idx;
        ybf->buffer_alloc = (uint8_t *)vpx_memalign_ocl(32, frame_size, ybf);
        if (!ybf->buffer_alloc)
          return -1;

        ybf->buffer_alloc_sz = frame_size;

        // This memset is needed for fixing valgrind error from C loop filter
        // due to access uninitialized memory in frame boarder. It could be
        // removed if border is totally removed.
        vpx_memset(ybf->buffer_alloc, 0, ybf->buffer_alloc_sz);
      }

      if (ybf->buffer_alloc_sz < frame_size)
        return -1;
    }

    /* Only support allocating buffers that have a border that's a multiple
     * of 32. The border restriction is required to get 16-byte alignment of
     * the start of the chroma rows without introducing an arbitrary gap
     * between planes, which would break the semantics of things like
     * vpx_img_set_rect(). */
    if (border & 0x1f)
      return -3;

    ybf->y_crop_width = width;
    ybf->y_crop_height = height;
    ybf->y_width  = aligned_width;
    ybf->y_height = aligned_height;
    ybf->y_stride = y_stride;

    ybf->uv_crop_width = (width + ss_x) >> ss_x;
    ybf->uv_crop_height = (height + ss_y) >> ss_y;
    ybf->uv_width = uv_width;
    ybf->uv_height = uv_height;
    ybf->uv_stride = uv_stride;

    ybf->border = border;
    ybf->frame_size = frame_size;

    ybf->y_buffer = ybf->buffer_alloc + (border * y_stride) + border;
    ybf->u_buffer = ybf->buffer_alloc + yplane_size +
                    (uv_border_h * uv_stride) + uv_border_w;
    ybf->v_buffer = ybf->buffer_alloc + yplane_size + uvplane_size +
                    (uv_border_h * uv_stride) + uv_border_w;

#if CONFIG_ALPHA
    ybf->alpha_width = alpha_width;
    ybf->alpha_height = alpha_height;
    ybf->alpha_stride = alpha_stride;
    ybf->alpha_buffer = ybf->buffer_alloc + yplane_size + 2 * uvplane_size +
                        (alpha_border_h * alpha_stride) + alpha_border_w;
#endif
    ybf->corrupted = 0; /* assume not corrupted by errors */
    return 0;
  }
  return -2;
}
#endif // USE_INTER_PREDICT_OCL

#endif
