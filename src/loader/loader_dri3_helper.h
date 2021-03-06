/*
 * Copyright © 2013 Keith Packard
 * Copyright © 2015 Boyan Ding
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef LOADER_DRI3_HEADER_H
#define LOADER_DRI3_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include <GL/gl.h>
#include <GL/internal/dri_interface.h>

enum loader_dri3_buffer_type {
   loader_dri3_buffer_back = 0,
   loader_dri3_buffer_front = 1
};

struct loader_dri3_buffer {
   __DRIimage   *image;
   __DRIimage   *linear_buffer;
   uint32_t     pixmap;

   /* Synchronization between the client and X server is done using an
    * xshmfence that is mapped into an X server SyncFence. This lets the
    * client check whether the X server is done using a buffer with a simple
    * xshmfence call, rather than going to read X events from the wire.
    *
    * However, we can only wait for one xshmfence to be triggered at a time,
    * so we need to know *which* buffer is going to be idle next. We do that
    * by waiting for a PresentIdleNotify event. When that event arrives, the
    * 'busy' flag gets cleared and the client knows that the fence has been
    * triggered, and that the wait call will not block.
    */

   uint32_t     sync_fence;     /* XID of X SyncFence object */
   struct xshmfence *shm_fence; /* pointer to xshmfence object */
   bool         busy;           /* Set on swap, cleared on IdleNotify */
   bool         own_pixmap;     /* We allocated the pixmap ID, free on destroy */

   uint32_t     size;
   uint32_t     pitch;
   uint32_t     cpp;
   uint32_t     flags;
   uint32_t     width, height;
   uint64_t     last_swap;

   enum loader_dri3_buffer_type        buffer_type;
};


#define LOADER_DRI3_MAX_BACK   4
#define LOADER_DRI3_BACK_ID(i) (i)
#define LOADER_DRI3_FRONT_ID   (LOADER_DRI3_MAX_BACK)

static inline int
loader_dri3_pixmap_buf_id(enum loader_dri3_buffer_type buffer_type)
{
   if (buffer_type == loader_dri3_buffer_back)
      return LOADER_DRI3_BACK_ID(0);
   else
      return LOADER_DRI3_FRONT_ID;
}

struct loader_dri3_extensions {
   const __DRIcoreExtension *core;
   const __DRIimageDriverExtension *image_driver;
   const __DRI2flushExtension *flush;
   const __DRI2configQueryExtension *config;
   const __DRItexBufferExtension *tex_buffer;
   const __DRIimageExtension *image;
};

struct loader_dri3_drawable;

struct loader_dri3_vtable {
   int (*get_swap_interval)(struct loader_dri3_drawable *);
   int (*clamp_swap_interval)(struct loader_dri3_drawable *, int);
   void (*set_swap_interval)(struct loader_dri3_drawable *, int);
   void (*set_drawable_size)(struct loader_dri3_drawable *, int, int);
   bool (*in_current_context)(struct loader_dri3_drawable *);
   __DRIcontext *(*get_dri_context)(struct loader_dri3_drawable *);
   void (*flush_drawable)(struct loader_dri3_drawable *, unsigned);
   void (*show_fps)(struct loader_dri3_drawable *, uint64_t);
};

#define LOADER_DRI3_NUM_BUFFERS (1 + LOADER_DRI3_MAX_BACK)

struct loader_dri3_drawable {
   xcb_connection_t *conn;
   __DRIdrawable *dri_drawable;
   xcb_drawable_t drawable;
   int width;
   int height;
   int depth;
   uint8_t have_back;
   uint8_t have_fake_front;
   uint8_t is_pixmap;
   uint8_t flipping;

   /* Information about the GPU owning the buffer */
   __DRIscreen *dri_screen;
   bool is_different_gpu;

   /* Present extension capabilities
    */
   uint32_t present_capabilities;

   /* SBC numbers are tracked by using the serial numbers
    * in the present request and complete events
    */
   uint64_t send_sbc;
   uint64_t recv_sbc;

   /* Last received UST/MSC values for pixmap present complete */
   uint64_t ust, msc;

   /* Last received UST/MSC values from present notify msc event */
   uint64_t notify_ust, notify_msc;

   /* Serial numbers for tracking wait_for_msc events */
   uint32_t send_msc_serial;
   uint32_t recv_msc_serial;

   struct loader_dri3_buffer *buffers[LOADER_DRI3_NUM_BUFFERS];
   int cur_back;
   int num_back;

   uint32_t *stamp;

   xcb_present_event_t eid;
   xcb_gcontext_t gc;
   xcb_special_event_t *special_event;

   bool first_init;

   struct loader_dri3_extensions *ext;
   struct loader_dri3_vtable *vtable;
};

void
loader_dri3_set_swap_interval(struct loader_dri3_drawable *draw,
                              int interval);

void
loader_dri3_drawable_fini(struct loader_dri3_drawable *draw);

int
loader_dri3_drawable_init(xcb_connection_t *conn,
                          xcb_drawable_t drawable,
                          __DRIscreen *dri_screen,
                          bool is_different_gpu,
                          const __DRIconfig *dri_config,
                          struct loader_dri3_extensions *ext,
                          struct loader_dri3_vtable *vtable,
                          struct loader_dri3_drawable*);

bool loader_dri3_wait_for_msc(struct loader_dri3_drawable *draw,
                              int64_t target_msc,
                              int64_t divisor, int64_t remainder,
                              int64_t *ust, int64_t *msc, int64_t *sbc);

int64_t
loader_dri3_swap_buffers_msc(struct loader_dri3_drawable *draw,
                             int64_t target_msc, int64_t divisor,
                             int64_t remainder, unsigned flush_flags,
                             bool force_copy);

int
loader_dri3_wait_for_sbc(struct loader_dri3_drawable *draw,
                         int64_t target_sbc, int64_t *ust,
                         int64_t *msc, int64_t *sbc);

int loader_dri3_query_buffer_age(struct loader_dri3_drawable *draw);

void
loader_dri3_flush(struct loader_dri3_drawable *draw,
                  unsigned flags,
                  enum __DRI2throttleReason throttle_reason);

void
loader_dri3_copy_sub_buffer(struct loader_dri3_drawable *draw,
                            int x, int y,
                            int width, int height,
                            bool flush);

void
loader_dri3_copy_drawable(struct loader_dri3_drawable *draw,
                          xcb_drawable_t dest,
                          xcb_drawable_t src);

void
loader_dri3_wait_x(struct loader_dri3_drawable *draw);

void
loader_dri3_wait_gl(struct loader_dri3_drawable *draw);

int loader_dri3_open(xcb_connection_t *conn,
                     xcb_window_t root,
                     uint32_t provider);

__DRIimage *
loader_dri3_create_image(xcb_connection_t *c,
                         xcb_dri3_buffer_from_pixmap_reply_t *bp_reply,
                         unsigned int format,
                         __DRIscreen *dri_screen,
                         const __DRIimageExtension *image,
                         void *loaderPrivate);

int
loader_dri3_get_buffers(__DRIdrawable *driDrawable,
                        unsigned int format,
                        uint32_t *stamp,
                        void *loaderPrivate,
                        uint32_t buffer_mask,
                        struct __DRIimageList *buffers);

#endif
