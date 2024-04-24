/*
 * SPDX-License-Identifier: MIT
 */

#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "cursor-drm.h"

#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 16

/* 4-bit intensity + 4-bit alpha-- hand editable */
/* a plus-sign marks the cursor focal point */
const char *cursor_arrow =
    "00 00 ff ff 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 ff 0f+ff 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 ff 0f 0f ff 00 00 00 00 00 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f ff 00 00 00 00 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f ff 00 00 00 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f ff 00 00 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f ff 00 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f 0f ff 00 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f ff 00 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f ff 00 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f ff 00 00 "
    "00 00 ff 0f 0f 0f 0f 0f 0f ff ff ff ff ff 00 00 "
    "00 00 ff 0f 0f 0f ff 0f 0f ff 00 00 00 00 00 00 "
    "00 00 ff 0f 0f ff 00 ff 0f 0f ff 00 00 00 00 00 "
    "00 00 ff 0f ff 00 00 ff 0f 0f ff 00 00 00 00 00 "
    "00 00 ff ff 00 00 00 00 ff ff 00 00 00 00 00 00 "
;

const char *cursor_hand =
    "00 00 00 00 00 00 ff ff 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 ff 0f+0f ff 00 00 00 00 00 00 00 "
    "00 00 00 00 00 ff 0f 0f ff 00 00 00 00 00 00 00 "
    "00 00 00 00 00 ff 0f 0f ff 00 00 00 00 00 00 00 "
    "00 00 00 00 00 ff 0f 0f ff ff ff ff ff 00 00 00 "
    "00 00 00 00 00 ff 0f 0f ff 0f 0f 0f 0f ff ff 00 "
    "00 00 ff ff 00 ff 0f 0f ff 0f 0f 0f 0f 0f 0f ff "
    "00 ff 0f 0f ff ff 0f 0f 0f 0f 0f 0f 0f 0f 0f ff "
    "00 ff 0f 0f 0f ff 0f 0f 0f 0f 0f 0f 0f 0f 0f ff "
    "00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f ff "
    "00 00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f ff "
    "00 00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f ff 00 "
    "00 00 00 00 ff 0f 0f 0f 0f 0f 0f 0f 0f 0f ff 00 "
    "00 00 00 00 00 ff 0f 0f 0f 0f 0f 0f 0f ff 00 00 "
    "00 00 00 00 00 00 ff 0f 0f 0f 0f ff 0f ff 00 00 "
    "00 00 00 00 00 00 ff ff ff ff ff 00 ff ff 00 00 "
;

const char *cursor_ibeam = 
    "00 00 00 00 0f 0f 00 00 00 00 0f 0f 00 00 00 00 "
    "00 00 00 00 00 00 0f 00 00 0f 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f+0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 0f 0f 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 0f 00 00 0f 00 00 00 00 00 00 "
    "00 00 00 00 0f 0f 00 00 00 00 0f 0f 00 00 00 00 "
;

/* cursor-specific state-- dont duplicate drm state */
static struct {
    uint32_t fbuf_id;
    uint32_t plane_id;
    uint32_t handle;

    uint32_t width;
    uint32_t height;
    uint32_t *pixmap;
    uint32_t pixlen;
    uint32_t format;

    int px;
    int py;
    const char *pattern;
} cursor = { 0 };

/* allocate overlay plane and pixmap */
gboolean init_cursor(int drm_fd, int crtc_idx)
{
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        g_warning("cursor: no universal planes");
        return FALSE;
    }

    /* find an unused overlay plane */
    int plane_id = -1;
    uint32_t format = 0;
    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (planes == NULL) {
        g_warning("cursor: no planes");
        return FALSE;
    }

    for (int i = 0; (i < planes->count_planes) && (plane_id < 0); ++i) {
        drmModePlane *plane = drmModeGetPlane(drm_fd, planes->planes[i]);
        if (plane == NULL)
            continue;

        /* this plane must be usable by the drm crtc */
        int match = (plane->possible_crtcs>>crtc_idx) & 1;
        /* this plane must be free to use */
        if ((plane->crtc_id == 0) && (plane->fb_id == 0))
            match |= 2;
        /* ensure it is an overlay plane */
        drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, plane->plane_id, DRM_MODE_OBJECT_ANY);
        for (int j = 0; j < props->count_props; ++j) {
            drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[j]);
            if ((strcmp(prop->name, "type") == 0) && (props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY))
                match |= 4;
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);

        /* make sure plane has a supported pixel format */
        format = 0;
        for (int j = 0; j < plane->count_formats; ++j)
            if ((plane->formats[j] == DRM_FORMAT_ARGB8888) || (plane->formats[j] == DRM_FORMAT_RGBA8888))
              format = plane->formats[j];

        if ((match == 7) && (format != 0)) {
            plane_id = plane->plane_id;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    if (plane_id < 0) {
        g_warning("cursor: no usable cursor plane");
        return FALSE;
    }

    /* allocate pixel map for cursor rendering */
    struct drm_mode_create_dumb dumb = { .width = CURSOR_WIDTH, .height = CURSOR_HEIGHT, .bpp = 32 };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb))
        g_warning("cursor: DRM_IOCTL_MODE_CREATE_DUMB failed %s", strerror(errno));

    uint32_t handles[4] = { dumb.handle, 0, 0, 0 };
    uint32_t pitches[4] = { dumb.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(drm_fd, dumb.width, dumb.height, format, handles, pitches, offsets, &cursor.fbuf_id, 0))
        g_warning("cursor: drmModeAddFB2 failed %s", strerror(errno));

    struct drm_mode_map_dumb dmap = { .handle = dumb.handle };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &dmap) == 0)
        cursor.pixmap = mmap(NULL, dumb.size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, dmap.offset);
    cursor.pixlen = dumb.size;
    cursor.width = dumb.width;
    cursor.height = dumb.height;
    cursor.handle = dumb.handle;
    cursor.plane_id = plane_id;
    cursor.format = format;
    return TRUE;
}

/* parse human readable cursor pattern into pixmap (NULL==blank pixmap) */
int set_cursor(const char *pattern)
{
    uint32_t *pix = cursor.pixmap;
    if ((pix == NULL) || (pattern == NULL))
        return(-1);

    if (strlen(pattern) < cursor.width*cursor.height*3) {
        const char *name = pattern;
        pattern = NULL;
        if (strcmp(name, "default") == 0)
            pattern = cursor_arrow;
        else if (strcmp(name, "pointer") == 0)
            pattern = cursor_hand;
        else if (strcmp(name, "text") == 0)
            pattern = cursor_ibeam;
        else if (strcmp(name, "hidden") == 0)
            pattern = "";
        if (pattern == NULL)
            return(-2);
    }


    /* remember last pattern since can get spammed */
    if (pattern == cursor.pattern)
      return(0);
    cursor.pattern = pattern;

    /* parse cursor pattern and save offset to focus pixel */
    cursor.px = cursor.py = 0;
    for (int n = 0; n < cursor.width*cursor.height; ++n) {
        uint8_t i = 0;
        uint8_t a = 0;
        if ((pattern[0] != 0) && (pattern[1] != 0)) {
            i = (pattern[0] & 0x40) ? (pattern[0] & 15)-9 : (pattern[0] & 15);
            a = (pattern[1] & 0x40) ? (pattern[1] & 15)-9 : (pattern[1] & 15);
            pattern += 2;
            if (pattern[0] == '+') {
                cursor.px = n%cursor.width;
                cursor.py = n/cursor.width;
            }
            if (pattern[0] != 0)
                ++pattern;
        }

        uint32_t pixel = 0;
        if (cursor.format == DRM_FORMAT_RGBA8888)
            pixel = (i<<24)|(i<<16)|(i<<8)|(a<<0);
        else if (cursor.format == DRM_FORMAT_ARGB8888)
            pixel = (a<<24)|(i<<16)|(i<<8)|(i<<0);
        *pix++ = pixel;
    }
    return(0);
}

/* reposition cursor overlay to desired coordinates */
void move_cursor(int drm_fd, int crtc_id, int x, int y)
{
    drmModeSetPlane(drm_fd, cursor.plane_id, crtc_id, cursor.fbuf_id, 0,
        x+cursor.px, y+cursor.py, cursor.width, cursor.height,
        0, 0, cursor.width<<16, cursor.height<<16);
}

/* clean up any remaining state */
void clear_cursor(int drm_fd, int crtc_id)
{
    if (cursor.pixmap != NULL) {
        /* older pi vc4 drm driver would not free plane w/o two calls */
        cursor.fbuf_id = -1;
        move_cursor(drm_fd, crtc_id, 0, 0);
        move_cursor(drm_fd, crtc_id, 0, 0);
        /* done with underlying pixel memory */
        munmap(cursor.pixmap, cursor.pixlen);
        struct drm_mode_destroy_dumb free = { .handle = cursor.handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &free);
    }
    memset(&cursor, 0, sizeof(cursor));
}

