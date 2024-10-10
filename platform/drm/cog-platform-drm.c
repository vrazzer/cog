/*
 * cog-platform-x11.c
 * Copyright (C) 2019-2022 Igalia S.L.
 * Copyright (C) 2020 Michal Artazov <michal@artazov.cz>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include "cog-drm-renderer.h"
#include "cursor-drm.h"
#include "../common/cog-cursors.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <libinput.h>
#include <linux/input.h>
#include <libudev.h>
#include <string.h>
#include <wayland-server.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <xkbcommon/xkbcommon.h>

#include <epoxy/egl.h>

#include "../common/egl-proc-address.h"

#ifndef LIBINPUT_CHECK_VERSION
#    define LIBINPUT_CHECK_VERSION(a, b, c)                                                         \
        ((LIBINPUT_VER_MAJOR > (a)) || (LIBINPUT_VER_MAJOR == (a) && (LIBINPUT_VER_MINOR > (b))) || \
         (LIBINPUT_VER_MAJOR == (a) && (LIBINPUT_VER_MINOR == (b)) && (LIBINPUT_VER_MICRO >= (c))))
#endif /* !LIBINPUT_CHECK_VERSION */

#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

#define KEY_STARTUP_DELAY 500000
#define KEY_REPEAT_DELAY  100000

#ifndef g_debug_once
#    define g_debug_once(...)                                                                     \
        G_STMT_START                                                                              \
        {                                                                                         \
            static int G_PASTE(_GDebugOnceBoolean_, __LINE__) = 0; /* (atomic) */                 \
            if (g_atomic_int_compare_and_exchange(&G_PASTE(_GDebugOnceBoolean_, __LINE__), 0, 1)) \
                g_debug(__VA_ARGS__);                                                             \
        }                                                                                         \
        G_STMT_END
#endif /* !g_debug_once */

struct _CogDrmPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogDrmPlatform {
    CogPlatform            parent;
    CogView               *web_view;
    CogDrmRenderer        *renderer;
    CogGLRendererRotation  rotation;
    GList                 *rotatable_input_devices;
    bool                   use_gles;
};

enum {
    PROP_0,
    PROP_ROTATION,
    PROP_RENDERER,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = {
    NULL,
};

G_DECLARE_FINAL_TYPE(CogDrmPlatform, cog_drm_platform, COG, DRM_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogDrmPlatform,
    cog_drm_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "drm", 200);)

static struct {
    int fd;
    drmModeRes *base_resources;
    drmModePlaneRes *plane_resources;

    struct {
        drmModeConnector *obj;
        uint32_t obj_id;
    } connector;
    struct {
        drmModeCrtc *obj;
        uint32_t obj_id;
        uint32_t index;
    } crtc;
    struct {
        drmModePlane *obj;
        uint32_t obj_id;
    } plane;

    drmModeModeInfo *mode;
    drmModeEncoder *encoder;

    uint32_t width;
    uint32_t height;
    uint32_t refresh;
    double   device_scale;

    bool atomic_modesetting;
    bool addfb2_modifiers;
    bool mode_set;
} drm_data = {
    .fd = -1,
    .base_resources = NULL,
    .plane_resources = NULL,
    .connector =
        {
            NULL,
            0,
        },
    .crtc =
        {
            NULL,
            0,
            0,
        },
    .plane =
        {
            NULL,
            0,
        },
    .mode = NULL,
    .encoder = NULL,
    .width = 0,
    .height = 0,
    .refresh = 0,
    .device_scale = 1.0,
    .atomic_modesetting = true,
    .mode_set = false,
};

static struct {
    gboolean enabled;
    gboolean hidden;
    gboolean scroll;
    unsigned int x;
    unsigned int y;
    unsigned int screen_width;
    unsigned int screen_height;
} cursor = {
    .enabled = FALSE,
    .hidden = TRUE,
    .scroll = TRUE,
};

static struct {
    struct gbm_device *device;
} gbm_data = {
    .device = NULL,
};

static struct {
    EGLDisplay display;
} egl_data = {
    .display = EGL_NO_DISPLAY,
};

typedef struct keyboard_event {
    uint32_t time;
    uint32_t key;
} keyboard_event;

static struct {
    struct udev *udev;
    struct libinput *libinput;

    uint32_t input_width;
    uint32_t input_height;

    struct wpe_input_keyboard_event key_repeat_event;
    uint32_t last_modifiers;

    struct wpe_input_touch_event_raw touch_points[10];
    enum wpe_input_touch_event_type last_touch_type;
    int last_touch_id;
} input_data = {
    .udev = NULL,
    .libinput = NULL,
    .input_width = 0,
    .input_height = 0,
    .key_repeat_event = { 0 },
    .last_touch_type = wpe_input_touch_event_type_null,
    .last_touch_id = 0,
    .last_modifiers = 0,
};

static struct {
    GSource *drm_source;
    GSource *input_source;
    GSource *key_repeat_source;
} glib_data = {
    .drm_source = NULL,
    .input_source = NULL,
    .key_repeat_source = NULL,
};

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static struct {
    struct wpe_view_backend *backend;
} wpe_view_data;

static void
init_config(CogDrmPlatform *self, CogShell *shell, const char *params_string)
{
    drm_data.device_scale = cog_shell_get_device_scale_factor (shell);
    g_debug ("init_config: overriding device_scale value, using %.2f from shell",
             drm_data.device_scale);

    GKeyFile *key_file = cog_shell_get_config_file (shell);

    if (key_file) {
        {
            g_autoptr(GError) lookup_error = NULL;

            gboolean value = g_key_file_get_boolean(key_file, "drm", "disable-atomic-modesetting", &lookup_error);
            if (!lookup_error) {
                drm_data.atomic_modesetting = !value;
                g_debug("init_config: atomic modesetting reconfigured to value '%s'",
                        drm_data.atomic_modesetting ? "true" : "false");
            }
        }

        {
            g_autoptr(GError) lookup_error = NULL;

            gdouble value = g_key_file_get_double(key_file, "drm", "device-scale-factor", &lookup_error);
            if (!lookup_error) {
                drm_data.device_scale = value;
                g_debug("init_config: overriding device_scale value, using %.2f from config", drm_data.device_scale);
            }
        }

        {
            g_autofree char *value = g_key_file_get_string(key_file, "drm", "renderer", NULL);
            if (g_strcmp0(value, "gles") == 0)
                self->use_gles = true;
            else if (g_strcmp0(value, "modeset") != 0)
                self->use_gles = false;
            else if (value)
                g_warning("Invalid renderer '%s', using default.", value);
        }
    }

    if (params_string) {
        g_auto(GStrv) params = g_strsplit(params_string, ",", 0);
        for (unsigned i = 0; params[i]; i++) {
            g_auto(GStrv) kv = g_strsplit(params[i], "=", 2);
            if (g_strv_length(kv) != 2) {
                g_warning("Invalid parameter syntax '%s'.", params[i]);
                continue;
            }

            const char *k = g_strstrip(kv[0]);
            const char *v = g_strstrip(kv[1]);

            if (g_strcmp0(k, "renderer") == 0) {
                if (g_strcmp0(v, "modeset") == 0)
                    self->use_gles = false;
                else if (g_strcmp0(v, "gles") == 0)
                    self->use_gles = true;
                else
                    g_warning("Invalid value '%s' for parameter '%s'.", v, k);
            } else if (g_strcmp0(k, "rotation") == 0) {
                char       *endp = NULL;
                const char *str = v ? v : "";
                uint64_t    val = g_ascii_strtoull(str, &endp, 10);
                if (val > 3 || *endp != '\0')
                    g_warning("Invalid value '%s' for parameter '%s'.", v, k);
                else
                    self->rotation = val;
            } else {
                g_warning("Invalid parameter '%s'.", k);
            }
        }
    }
}

static void
clear_drm (void)
{

    g_clear_pointer (&drm_data.base_resources, drmModeFreeResources);
    g_clear_pointer (&drm_data.plane_resources, drmModeFreePlaneResources);

    g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);

    g_clear_pointer (&drm_data.plane.obj, drmModeFreePlane);
    g_clear_pointer (&drm_data.crtc.obj, drmModeFreeCrtc);
    g_clear_pointer (&drm_data.connector.obj, drmModeFreeConnector);

    if (drm_data.fd != -1) {
        close (drm_data.fd);
        drm_data.fd = -1;
    }
}

static gboolean
check_drm(void)
{
    drmDevice *devices[64];
    memset(devices, 0, sizeof(*devices) * 64);

    int num_devices = drmGetDevices2(0, devices, 64);
    if (num_devices < 0)
        return FALSE;

    gboolean supported = FALSE;
    for (int i = 0; !supported && i < num_devices; ++i) {
        if (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) {
            supported = TRUE;
            break;
        }
    }

    drmFreeDevices(devices, num_devices);
    return supported;
}

static int32_t
find_crtc_for_encoder(const drmModeRes *resources, const drmModeEncoder *encoder)
{
    for (int i = 0; i < resources->count_crtcs; i++) {
        const uint32_t crtc_mask = 1 << i;
        const uint32_t crtc_id = resources->crtcs[i];
        if (encoder->possible_crtcs & crtc_mask && encoder->crtc_id == crtc_id) {
            return crtc_id;
        }
    }

    /* no match found */
    return -1;
}

static gboolean
init_drm(void)
{
    drmDevice *devices[64];
    memset(devices, 0, sizeof(*devices) * 64);

    int num_devices = drmGetDevices2(0, devices, 64);
    if (num_devices < 0)
        return FALSE;

    for (int i = 0; i < num_devices; ++i) {
        drmDevice* device = devices[i];
        g_debug ("init_drm: enumerated device %p, available_nodes %d",
                 device, device->available_nodes);

        if (device->available_nodes & (1 << DRM_NODE_PRIMARY))
            g_debug ("init_drm:   DRM_NODE_PRIMARY: %s", device->nodes[DRM_NODE_PRIMARY]);
        if (device->available_nodes & (1 << DRM_NODE_CONTROL))
            g_debug ("init_drm:   DRM_NODE_CONTROL: %s", device->nodes[DRM_NODE_CONTROL]);
        if (device->available_nodes & (1 << DRM_NODE_RENDER))
            g_debug ("init_drm:   DRM_NODE_RENDER: %s", device->nodes[DRM_NODE_RENDER]);
    }

    for (int i = 0; i < num_devices; ++i) {
        drmDevice* device = devices[i];
        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        drm_data.fd = open (device->nodes[DRM_NODE_PRIMARY], O_RDWR);
        if (drm_data.fd < 0)
            continue;

        drm_data.base_resources = drmModeGetResources (drm_data.fd);
        if (drm_data.base_resources) {
            g_debug ("init_drm: using device %p, DRM_NODE_PRIMARY %s",
                     device, device->nodes[DRM_NODE_PRIMARY]);
            break;
        }

        close (drm_data.fd);
        drm_data.fd = -1;
    }

    drmFreeDevices(devices, num_devices);

    if (!drm_data.base_resources)
        return FALSE;

    if (drm_data.atomic_modesetting) {
        int ret = drmSetClientCap (drm_data.fd, DRM_CLIENT_CAP_ATOMIC, 1);
        if (ret) {
            drm_data.atomic_modesetting = false;
            g_debug ("init_drm: atomic mode not usable, falling back to non-atomic mode");
        }
    }

    g_debug ("init_drm: %d connectors available", drm_data.base_resources->count_connectors);
    for (int i = 0; i < drm_data.base_resources->count_connectors; ++i) {
        drmModeConnector *connector = drmModeGetConnector (drm_data.fd,
                                                           drm_data.base_resources->connectors[i]);

        g_debug ("init_drm:  connector id %u, type %u, %sconnected, %d usable modes",
                 connector->connector_id, connector->connector_type,
                 (connector->connection == DRM_MODE_CONNECTED) ? "" : "not ",
                 connector->count_modes);

        for (int j = 0; j < connector->count_modes; ++j) {
            drmModeModeInfo *mode = &connector->modes[j];
            g_debug ("init_drm:    [%d]: '%s', %ux%u@%u, flags %u, type %u %s%s",
                     j, mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh,
                     mode->flags, mode->type,
                     (mode->type & DRM_MODE_TYPE_PREFERRED) ? "(preferred) " : "",
                     (mode->type & DRM_MODE_TYPE_DEFAULT) ? "(default) " : "");
        }

        g_clear_pointer (&connector, drmModeFreeConnector);
    }

    for (int i = 0; i < drm_data.base_resources->count_connectors; ++i) {
        drm_data.connector.obj = drmModeGetConnector (drm_data.fd, drm_data.base_resources->connectors[i]);
        if (drm_data.connector.obj->connection == DRM_MODE_CONNECTED)
            break;

        g_clear_pointer (&drm_data.connector.obj, drmModeFreeConnector);
    }
    if (!drm_data.connector.obj)
        return FALSE;

    g_debug("init_drm: using connector id %d, type %d", drm_data.connector.obj->connector_id,
            drm_data.connector.obj->connector_type);

    const char *user_selected_mode = g_getenv("COG_PLATFORM_DRM_VIDEO_MODE");

    int user_max_width = 0;
    int user_max_height = 0;
    int user_max_refresh = 0;

    const char *user_mode_max = g_getenv("COG_PLATFORM_DRM_MODE_MAX");
    if (user_mode_max) {
        if (sscanf(user_mode_max, "%dx%d@%d", &user_max_width, &user_max_height, &user_max_refresh) < 2 ||
            user_max_width < 0 || user_max_height < 0 || user_max_refresh < 0) {
            fprintf(stderr, "invalid value for COG_PLATFORM_DRM_MODE_MAX\n");
            user_max_width = 0;
            user_max_height = 0;
            user_max_refresh = 0;
        }
    }

    for (int i = 0, area = 0; i < drm_data.connector.obj->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm_data.connector.obj->modes[i];
        if (user_selected_mode && strcmp(user_selected_mode, current_mode->name) != 0) {
            continue;
        }

        if (user_max_width && current_mode->hdisplay > user_max_width) {
            continue;
        }
        if (user_max_height && current_mode->vdisplay > user_max_height) {
            continue;
        }
        if (user_max_refresh && current_mode->vrefresh > user_max_refresh) {
            continue;
        }

        if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
            drm_data.mode = current_mode;
            break;
        }

        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            drm_data.mode = current_mode;
            area = current_area;
        }
    }
    if (!drm_data.mode)
        return FALSE;

    g_debug("init_drm: using mode [%ld] '%s' @ %dHz",
            (long)((drm_data.mode - drm_data.connector.obj->modes) / sizeof(drmModeModeInfo *)), drm_data.mode->name,
            drm_data.mode->vrefresh);

    /* Try the currently connected encoder+crtc */
    for (int i = 0; i < drm_data.base_resources->count_encoders; ++i) {
        drm_data.encoder = drmModeGetEncoder(drm_data.fd, drm_data.base_resources->encoders[i]);
        if (!drm_data.encoder) {
            /* cannot retrieve encoder, ignoring... */
            continue;
        }

        const int32_t crtc_id = find_crtc_for_encoder(drm_data.base_resources, drm_data.encoder);
        if (crtc_id != -1) {
            drm_data.crtc.obj_id = crtc_id;
            break;
        }

        g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    }

    if (!drm_data.encoder) {
        fprintf(stderr, "no crtc for encoder found!\n");
        return FALSE;
    }

    drm_data.connector.obj_id = drm_data.connector.obj->connector_id;

    drm_data.crtc.obj = drmModeGetCrtc (drm_data.fd, drm_data.crtc.obj_id);
    for (int i = 0; i < drm_data.base_resources->count_crtcs; ++i) {
        if (drm_data.base_resources->crtcs[i] == drm_data.crtc.obj_id) {
            drm_data.crtc.index = i;
            break;
        }
    }

    drm_data.plane_resources = drmModeGetPlaneResources (drm_data.fd);
    if (!drm_data.plane_resources)
        return FALSE;

    for (int i = 0; i < drm_data.plane_resources->count_planes; ++i) {
        uint32_t plane_id = drm_data.plane_resources->planes[i];
        drmModePlane *plane = drmModeGetPlane (drm_data.fd, plane_id);
        if (!plane)
            continue;

        if (!(plane->possible_crtcs & (1 << drm_data.crtc.index))) {
            drmModeFreePlane (plane);
            continue;
        }

        g_clear_pointer (&drm_data.plane.obj, drmModeFreePlane);
        drm_data.plane.obj_id = 0;

        drm_data.plane.obj = plane;
        drm_data.plane.obj_id = plane_id;
        bool is_primary = false;

        drmModeObjectProperties *plane_props = drmModeObjectGetProperties (drm_data.fd, plane_id, DRM_MODE_OBJECT_PLANE);

        for (int j = 0; j < plane_props->count_props; ++j) {
            drmModePropertyRes *prop = drmModeGetProperty (drm_data.fd, plane_props->props[j]);
            is_primary = !g_strcmp0 (prop->name, "type")
                              && plane_props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY;
            drmModeFreeProperty (prop);

            if (is_primary)
                break;
        }

        drmModeFreeObjectProperties (plane_props);

        if (is_primary)
            break;
    }

    drm_data.width = drm_data.mode->hdisplay;
    drm_data.height = drm_data.mode->vdisplay;
    drm_data.refresh = drm_data.mode->vrefresh;

    g_clear_pointer(&drm_data.base_resources, drmModeFreeResources);
    g_clear_pointer(&drm_data.plane_resources, drmModeFreePlaneResources);

    /* save connector info for any external video helper apps that need it */
    char conn_info[256];
    g_snprintf(conn_info, sizeof(conn_info), "%d:%d", drm_data.fd, drm_data.connector.obj_id);
    g_setenv("COG_PLATFORM_DRM_CONN_INFO", conn_info, true);
    return TRUE;
}

static void
clear_gbm (void)
{
    g_clear_pointer (&gbm_data.device, gbm_device_destroy);
}

static gboolean
init_gbm (void)
{
    gbm_data.device = gbm_create_device (drm_data.fd);
    if (!gbm_data.device)
        return FALSE;

    return TRUE;
}


static void
clear_egl (void)
{
    if (egl_data.display != EGL_NO_DISPLAY)
        eglTerminate (egl_data.display);
    eglReleaseThread ();
}

static gboolean
init_egl (void)
{
    static PFNEGLGETPLATFORMDISPLAYEXTPROC s_eglGetPlatformDisplay = NULL;
    if (!s_eglGetPlatformDisplay)
        s_eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) load_egl_proc_address ("eglGetPlatformDisplayEXT");

    if (s_eglGetPlatformDisplay)
        egl_data.display = s_eglGetPlatformDisplay (EGL_PLATFORM_GBM_KHR, gbm_data.device, NULL);
    else
        egl_data.display = eglGetDisplay((EGLNativeDisplayType) gbm_data.device);

    if (!egl_data.display) {
        clear_egl ();
        return FALSE;
    }

    if (!eglInitialize (egl_data.display, NULL, NULL)) {
        clear_egl ();
        return FALSE;
    }

    return TRUE;
}

/* dispatch key event with optional key remapping */
static gboolean
input_dispatch_key_event(CogView *view, struct wpe_input_keyboard_event *event, int remap)
{
    int repeat = 1, gobble = 0;
    if (!cog_view_remap_event(view, event, &repeat, &gobble) && remap)
        return(false);
    if (cog_view_try_handle_key_binding(view, event))
        repeat = 0, gobble = 1;
    if (!gobble)
        wpe_view_backend_dispatch_keyboard_event(wpe_view_data.backend, event);

    if (!repeat)
        ;
    else if (!event->pressed) {
        g_print("input_dispatch_key_event: clear repeat\n");
        input_data.key_repeat_event.time = 0;
        g_source_set_ready_time(glib_data.key_repeat_source, -1);
    } else if (!!memcmp(&input_data.key_repeat_event, event, sizeof(*event))) {
        g_print("input_dispatch_key_event: start repeat\n");
        memcpy(&input_data.key_repeat_event, event, sizeof(*event));
        g_source_set_ready_time(glib_data.key_repeat_source, g_get_monotonic_time()+KEY_STARTUP_DELAY);
    }
    return(true);
}

/* resend the pre-staged key-event */
static gboolean
input_dispatch_repeat_event(GSource *base, GSourceFunc callback, gpointer user_data)
{
    if (!input_data.key_repeat_event.time) {
        g_print("input_dispatch_repeat_event: clear repeat\n");
        g_source_set_ready_time(glib_data.key_repeat_source, -1);
    } else {
        g_print("input_dispatch_repeat_event: send repeat\n");
        g_source_set_ready_time(glib_data.key_repeat_source, g_get_monotonic_time()+KEY_REPEAT_DELAY);
        wpe_view_backend_dispatch_keyboard_event(wpe_view_data.backend, &input_data.key_repeat_event);
    }
    return G_SOURCE_CONTINUE;
}

static void
input_handle_key_event(CogView *view, struct libinput_event_keyboard *key_event)
{
    struct wpe_input_xkb_context *default_context = wpe_input_xkb_context_get_default ();
    g_print("input_handle_key_event\n");

    // if wpe is unable to prepare an xkb context (e.g. environment without
    // any prepared keymap data), ignore the key event
    if (!default_context)
        return;

    // libwpe (<=v1.14.1) may provide a broken context; ensure an underlying
    // xkb_context has been configured
    struct xkb_context *ctx = wpe_input_xkb_context_get_context(default_context);
    if (!ctx)
        return;

    struct xkb_state *context_state = wpe_input_xkb_context_get_state (default_context);

    // if wpe cannot determine the xkb state (e.g. required keymap does are not
    // available in this environment), ignore the key event
    if (!context_state)
        return;

    // Explanation for the offset-by-8, copied from Weston:
    //   evdev XKB rules reflect X's  broken keycode system, which starts at 8
    uint32_t key = libinput_event_keyboard_get_key (key_event) + 8;
    enum libinput_key_state state = libinput_event_keyboard_get_key_state (key_event);
    uint32_t time = libinput_event_keyboard_get_time (key_event);
    uint32_t keysym = wpe_input_xkb_context_get_key_code (default_context, key, !!state);
    xkb_state_update_key (context_state, key, !!state ? XKB_KEY_DOWN : XKB_KEY_UP);
    uint32_t modifiers = wpe_input_xkb_context_get_modifiers (default_context,
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout (context_state, XKB_STATE_LAYOUT_EFFECTIVE));
    g_print("input: %d(%02x):%d(%02x) -> %d(%02x):%d(%02x)\n",
            key, key, state, state,
            keysym, keysym, modifiers, modifiers);

    /* dispatch the key event w/optional remap */
    struct wpe_input_keyboard_event event = {
            .time = time,
            .key_code = keysym,
            .hardware_key_code = key,
            .pressed = (!!state),
            .modifiers = modifiers,
    };
    input_dispatch_key_event(view, &event, false);
}

static void
input_handle_touch_event (enum libinput_event_type touch_type, struct libinput_event_touch *touch_event)
{
    uint32_t time = libinput_event_touch_get_time (touch_event);

    enum wpe_input_touch_event_type event_type = wpe_input_touch_event_type_null;
    switch (touch_type) {
        case LIBINPUT_EVENT_TOUCH_DOWN:
            event_type = wpe_input_touch_event_type_down;
            break;
        case LIBINPUT_EVENT_TOUCH_UP:
            event_type = wpe_input_touch_event_type_up;
            break;
        case LIBINPUT_EVENT_TOUCH_MOTION:
            event_type = wpe_input_touch_event_type_motion;
            break;
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            struct wpe_input_touch_event event = {
                .touchpoints = input_data.touch_points,
                .touchpoints_length = G_N_ELEMENTS (input_data.touch_points),
                .type = input_data.last_touch_type,
                .id = input_data.last_touch_id,
                .time = time,
            };

            wpe_view_backend_dispatch_touch_event (wpe_view_data.backend, &event);

            for (int i = 0; i < G_N_ELEMENTS (input_data.touch_points); ++i) {
                struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[i];
                if (touch_point->type != wpe_input_touch_event_type_up)
                    continue;

                memset (touch_point, 0, sizeof (struct wpe_input_touch_event_raw));
                touch_point->type = wpe_input_touch_event_type_null;
            }

            return;
        }
        default:
            g_assert_not_reached ();
            return;
    }

    int id = libinput_event_touch_get_seat_slot (touch_event);
    if (id < 0 || id >= G_N_ELEMENTS (input_data.touch_points))
        return;

    input_data.last_touch_type = event_type;
    input_data.last_touch_id = id;

    struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[id];
    touch_point->type = event_type;
    touch_point->time = time;
    touch_point->id = id;

    if (touch_type == LIBINPUT_EVENT_TOUCH_DOWN
        || touch_type == LIBINPUT_EVENT_TOUCH_MOTION) {
        touch_point->x = libinput_event_touch_get_x_transformed (touch_event,
                                                                 input_data.input_width);
        touch_point->y = libinput_event_touch_get_y_transformed (touch_event,
                                                                 input_data.input_height);
    }
}

static void
input_handle_pointer_motion_event(CogView *view, struct libinput_event_pointer *pointer_event, bool absolute)
{
    if (!cursor.enabled)
        return;
    if (cursor.hidden)
        return;

    if (absolute) {
        cursor.x = libinput_event_pointer_get_absolute_x_transformed(pointer_event, cursor.screen_width);
        cursor.y = libinput_event_pointer_get_absolute_y_transformed(pointer_event, cursor.screen_height);
    } else {
        cursor.x += libinput_event_pointer_get_dx(pointer_event);
        cursor.y += libinput_event_pointer_get_dy(pointer_event);
    }

    if (cursor.x < 0) {
        cursor.x = 0;
    } else if (cursor.x > cursor.screen_width - 1) {
        cursor.x = cursor.screen_width - 1;
    }

    if (cursor.y < 0) {
        cursor.y = 0;
    } else if (cursor.y > cursor.screen_height - 1) {
        cursor.y = cursor.screen_height - 1;
    }

    move_cursor(drm_data.fd, drm_data.crtc.obj_id, cursor.x, cursor.y);

    struct wpe_input_pointer_event event = {
        .type = wpe_input_pointer_event_type_motion,
        .time = libinput_event_pointer_get_time(pointer_event),
        .x = cursor.x,
        .y = cursor.y,
        .button = !!input_data.last_modifiers ? 1 : 0,
        .state = !!input_data.last_modifiers,
        .modifiers = input_data.last_modifiers,
    };
    wpe_view_backend_dispatch_pointer_event(wpe_view_data.backend, &event);
    //g_print("pointer.movement: btn=%d st=%d mod=%08x\n", event.button, event.state, event.modifiers);
}

static void
input_handle_pointer_button_event (CogView *view, struct libinput_event_pointer *pointer_event)
{
    uint32_t button = libinput_event_pointer_get_button(pointer_event);
    uint32_t state = libinput_event_pointer_get_button_state(pointer_event);
    uint32_t time = libinput_event_pointer_get_time(pointer_event);
    if (!cursor.enabled)
        return;

    /* handle pointer-event remapping to keyboard-event */
    /* map libinput codes into unused wpe 0xf000..0xf3ff range */
    if ((button > 0) && (button <= KEY_MAX)) {
        struct wpe_input_keyboard_event event = {
            .time = time,
            .key_code = 0xf000+button,
            .hardware_key_code = button,
            .pressed = state,
            .modifiers = 0,
        };
        //g_print("check-ptr: code=%04x hw=%04x state=%d\n", event.key_code, event.hardware_key_code, event.pressed);

        /* dispatch the key only if remapped (else handle below as pointer event) */
        if (input_dispatch_key_event(view, &event, true))
            return;
    }

    /* note that BTN_RIGHT can trigger weirdness likely because drm has no default context menu */
    /* javascript can listen for 'context' (and call preventDefault) to draw a custom menu */
    uint32_t modifiers = 0;
    if (button == BTN_LEFT)
        button = 1, modifiers = wpe_input_pointer_modifier_button1;
    else if (button == BTN_RIGHT)
        button = 2, modifiers = wpe_input_pointer_modifier_button2;
    else if (button == BTN_MIDDLE)
        button = 3, modifiers = wpe_input_pointer_modifier_button3;
    else
        button = 0, modifiers = 0;

    /* setup as pointer event */
    struct wpe_input_pointer_event event = {
        .type = wpe_input_pointer_event_type_button,
        .time = time,
        .x = cursor.x,
        .y = cursor.y,
        .state = state,
        .button = button,
        .modifiers = modifiers,
    };
    wpe_view_backend_dispatch_pointer_event(wpe_view_data.backend, &event);
    //g_print("pointer.event: btn=%d st=%d mod=%08x\n", event.button, event.state, event.modifiers);

    if (!state)
        modifiers = 0;
    input_data.last_modifiers = modifiers;
}

#if LIBINPUT_CHECK_VERSION(1, 19, 0)
static void
input_handle_pointer_discrete_scroll_event(struct libinput_event_pointer *pointer_event)
{
    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        event.y_axis = -drm_data.device_scale * libinput_event_pointer_get_scroll_value_v120(
                                                    pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
        event.x_axis = drm_data.device_scale * libinput_event_pointer_get_scroll_value_v120(
                                                   pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}

static void
input_handle_pointer_smooth_scroll_event(struct libinput_event_pointer *pointer_event)
{
    if (cursor.hidden)
        return;

    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        event.y_axis = drm_data.device_scale *
                       libinput_event_pointer_get_scroll_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
        event.x_axis = drm_data.device_scale *
                       libinput_event_pointer_get_scroll_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}
#else
static void
input_handle_pointer_axis_event(struct libinput_event_pointer *pointer_event)
{
    if (cursor.hidden)
        return;

    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_get_axis_source(pointer_event) == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
        event.base.type |= wpe_input_axis_event_type_motion;
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            event.y_axis = -120.0 * libinput_event_pointer_get_axis_value_discrete(
                                        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
            event.x_axis = 120.0 * libinput_event_pointer_get_axis_value_discrete(
                                       pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    } else {
        event.base.type |= wpe_input_axis_event_type_motion_smooth;
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            event.y_axis = libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
            event.x_axis =
                libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    }

    event.x_axis *= drm_data.device_scale;
    event.y_axis *= drm_data.device_scale;

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}
#endif /* !LIBINPUT_CHECK_VERSION(1, 19, 0) */

static bool
input_device_needs_config(struct libinput_device *device)
{
    static const enum libinput_device_capability device_caps[] = {
        LIBINPUT_DEVICE_CAP_GESTURE,     LIBINPUT_DEVICE_CAP_POINTER, LIBINPUT_DEVICE_CAP_TABLET_PAD,
        LIBINPUT_DEVICE_CAP_TABLET_TOOL, LIBINPUT_DEVICE_CAP_TOUCH,
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(device_caps); i++)
        if (libinput_device_has_capability(device, device_caps[i]))
            return !!libinput_device_config_rotation_is_available(device);

    return false;
}

static void
input_configure_device(struct libinput_device *device, CogDrmPlatform *platform)
{
    enum libinput_config_status status = libinput_device_config_rotation_set_angle(device, platform->rotation * 90);

    const char *name = libinput_device_get_name(device);
    const int   id_vendor = libinput_device_get_id_vendor(device);
    const int   id_product = libinput_device_get_id_product(device);

    switch (status) {
    case LIBINPUT_CONFIG_STATUS_SUCCESS:
        g_debug("%s: Rotation set for %s (%04x:%04x)", __func__, name, id_vendor, id_product);
        break;
    case LIBINPUT_CONFIG_STATUS_UNSUPPORTED:
        g_debug("%s: Rotation unsupported for %s (%04x:%04x)", __func__, name, id_vendor, id_product);
        break;
    case LIBINPUT_CONFIG_STATUS_INVALID:
        g_debug("%s: Rotation %u invalid for %s (%04x:%04x)", __func__, platform->rotation * 90, name, id_vendor,
                id_product);
        break;
    }
}

static void
input_handle_device_added(struct libinput_device *device)
{
    g_debug("Input device %p added: %s (%04x:%04x)",
            device,
            libinput_device_get_name(device),
            libinput_device_get_id_vendor(device),
            libinput_device_get_id_product(device));

    if (input_device_needs_config(device)) {
        CogDrmPlatform *platform = libinput_get_user_data(libinput_device_get_context(device));
        platform->rotatable_input_devices =
            g_list_append(platform->rotatable_input_devices, libinput_device_ref(device));
        input_configure_device(device, platform);
    }
}

static void
input_handle_device_removed(struct libinput_device *device)
{
    g_debug("Input device %p removed: %s (%04x:%04x)",
            device,
            libinput_device_get_name(device),
            libinput_device_get_id_vendor(device),
            libinput_device_get_id_product(device));

    CogDrmPlatform *platform = libinput_get_user_data(libinput_device_get_context(device));

    GList *item = g_list_find(platform->rotatable_input_devices, device);
    if (item) {
        platform->rotatable_input_devices = g_list_remove_link(platform->rotatable_input_devices, item);
        g_list_free_full(item, (GDestroyNotify) libinput_device_unref);
    }
}

static void
input_process_events (void)
{
    g_assert(input_data.libinput);

    CogDrmPlatform *platform = libinput_get_user_data(input_data.libinput);

    libinput_dispatch(input_data.libinput);
    while (true) {
        struct libinput_event *event = libinput_get_event (input_data.libinput);
        if (!event)
            break;

        /* view must be visible and focused to receive input */
        uint32_t state = wpe_view_backend_get_activity_state(wpe_view_data.backend);
        state &= wpe_view_activity_state_visible+wpe_view_activity_state_focused;
        if (state != wpe_view_activity_state_visible+wpe_view_activity_state_focused) {
            input_data.key_repeat_event.time = 0;
            break;
        }

        enum libinput_event_type event_type = libinput_event_get_type (event);
        switch (event_type) {
        case LIBINPUT_EVENT_NONE:
            return;

        case LIBINPUT_EVENT_DEVICE_ADDED:
            input_handle_device_added(libinput_event_get_device(event));
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED:
            input_handle_device_removed(libinput_event_get_device(event));
            break;

        case LIBINPUT_EVENT_KEYBOARD_KEY:
            input_handle_key_event(platform->web_view, libinput_event_get_keyboard_event(event));
            break;

        case LIBINPUT_EVENT_TOUCH_CANCEL:
            break;
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_UP:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_FRAME:
            input_handle_touch_event(event_type, libinput_event_get_touch_event(event));
            break;

        case LIBINPUT_EVENT_POINTER_MOTION:
            input_handle_pointer_motion_event(platform->web_view, libinput_event_get_pointer_event(event), false);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            input_handle_pointer_motion_event(platform->web_view, libinput_event_get_pointer_event(event), true);
            break;

        case LIBINPUT_EVENT_POINTER_BUTTON:
            input_handle_pointer_button_event(platform->web_view, libinput_event_get_pointer_event(event));
            break;

        case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
        case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
        case LIBINPUT_EVENT_GESTURE_SWIPE_END:
            g_debug_once("%s: GESTURE_SWIPE_{BEGIN,UPDATE,END} unimplemented", __func__);
            break;

        case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
        case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
        case LIBINPUT_EVENT_GESTURE_PINCH_END:
            g_debug_once("%s: GESTURE_PINCH_{BEGIN,UPDATE,END} unimplemented", __func__);
            break;

#if LIBINPUT_CHECK_VERSION(1, 2, 0)
        case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
            g_debug_once("%s: TABLET_TOOL_AXIS unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
            g_debug_once("%s: TABLET_TOOL_PROXIMITY unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_TIP:
            g_debug_once("%s: TABLET_TOOL_TIP unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
            g_debug_once("%s: TABLET_TOOL_BUTTON unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 2, 0) */

#if LIBINPUT_CHECK_VERSION(1, 3, 0)
        case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
            g_debug_once("%s: TABLET_PAD_BUTTON unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_PAD_RING:
            g_debug_once("%s: TABLET_PAD_RING unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_PAD_STRIP:
            g_debug_once("%s: TABLET_PAD_STRIP unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 3, 0) */

#if LIBINPUT_CHECK_VERSION(1, 7, 0)
        case LIBINPUT_EVENT_SWITCH_TOGGLE:
            g_debug_once("%s: SWITCH_TOGGLE unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 7, 0) */

#if LIBINPUT_CHECK_VERSION(1, 15, 0)
        case LIBINPUT_EVENT_TABLET_PAD_KEY:
            g_debug_once("%s: TABLET_PAD_KEY unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 15, 0) */

#if LIBINPUT_CHECK_VERSION(1, 19, 0)
        case LIBINPUT_EVENT_POINTER_AXIS:
            /* Deprecated, use _SCROLL_{WHEEL,FINGER,CONTINUOUS} below. */
            break;
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            input_handle_pointer_discrete_scroll_event(libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            input_handle_pointer_smooth_scroll_event(libinput_event_get_pointer_event(event));
            break;

        case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
        case LIBINPUT_EVENT_GESTURE_HOLD_END:
            g_debug_once("%s: GESTURE_HOLD_{BEGIN,END} unimplemented", __func__);
            break;
#else
        case LIBINPUT_EVENT_POINTER_AXIS:
            input_handle_pointer_axis_event(libinput_event_get_pointer_event(event));
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 19, 0) */
        default:
            break;
        }

        libinput_event_destroy (event);
    }
}

static int
input_interface_open_restricted (const char *path, int flags, void *user_data)
{
    return open (path, flags);
}

static void
input_interface_close_restricted (int fd, void *user_data)
{
    close (fd);
}

static void
clear_input(CogDrmPlatform *platform)
{
    if (platform->rotatable_input_devices) {
        g_list_free_full(platform->rotatable_input_devices, (GDestroyNotify) libinput_device_unref);
        platform->rotatable_input_devices = NULL;
    }
    g_clear_pointer (&input_data.libinput, libinput_unref);
    g_clear_pointer (&input_data.udev, udev_unref);
}

static void
update_logical_input_size(CogGLRendererRotation rotation)
{
    switch (rotation) {
    case COG_GL_RENDERER_ROTATION_0:
    case COG_GL_RENDERER_ROTATION_180:
        input_data.input_width = drm_data.mode->hdisplay;
        input_data.input_height = drm_data.mode->vdisplay;
        break;
    case COG_GL_RENDERER_ROTATION_90:
    case COG_GL_RENDERER_ROTATION_270:
        input_data.input_width = drm_data.mode->vdisplay;
        input_data.input_height = drm_data.mode->hdisplay;
        break;
    }
}

static gboolean
init_input(CogDrmPlatform *platform)
{
    update_logical_input_size(platform->rotation);

    static struct libinput_interface interface = {
        .open_restricted = input_interface_open_restricted,
        .close_restricted = input_interface_close_restricted,
    };

    input_data.udev = udev_new ();
    if (!input_data.udev)
        return FALSE;

    input_data.libinput = libinput_udev_create_context(&interface, platform, input_data.udev);
    if (!input_data.libinput)
        return FALSE;

    int ret = libinput_udev_assign_seat (input_data.libinput, "seat0");
    if (ret)
        return FALSE;

    for (int i = 0; i < G_N_ELEMENTS (input_data.touch_points); ++i) {
        struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[i];

        memset (touch_point, 0, sizeof (struct wpe_input_touch_event_raw));
        touch_point->type = wpe_input_touch_event_type_null;
    }

    return TRUE;
}


struct drm_source {
    GSource source;
    GPollFD pfd;
    drmEventContext event_context;
};

struct input_source {
    GSource source;
    GPollFD pfd;
};

static gboolean
input_source_check (GSource *base)
{
    struct input_source *source = (struct input_source *) base;
    return !!source->pfd.revents;
}

static gboolean
input_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct input_source *source = (struct input_source *) base;
    if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return FALSE;

    input_process_events ();
    source->pfd.revents = 0;
    return TRUE;
}

static void
cog_drm_platform_shell_device_factor_changed(CogShell *shell, GParamSpec *param_spec, gpointer data)
{
    drm_data.device_scale = cog_shell_get_device_scale_factor(shell);
}

static void
clear_glib (void)
{
    if (glib_data.drm_source)
        g_source_destroy (glib_data.drm_source);
    g_clear_pointer (&glib_data.drm_source, g_source_unref);

    if (glib_data.input_source)
        g_source_destroy (glib_data.input_source);
    g_clear_pointer (&glib_data.input_source, g_source_unref);

    if (glib_data.key_repeat_source)
        g_source_destroy (glib_data.key_repeat_source);
    g_clear_pointer (&glib_data.key_repeat_source, g_source_unref);
}

static gboolean
init_glib(CogDrmPlatform *self)
{
    static GSourceFuncs input_source_funcs = {
        .check = input_source_check,
        .dispatch = input_source_dispatch,
    };

    static GSourceFuncs key_repeat_source_funcs = {
        .dispatch = input_dispatch_repeat_event,
    };

    glib_data.input_source = g_source_new (&input_source_funcs,
                                           sizeof (struct input_source));
    {
        struct input_source *source = (struct input_source *) glib_data.input_source;
        source->pfd.fd = libinput_get_fd (input_data.libinput);
        source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        source->pfd.revents = 0;
        g_source_add_poll (glib_data.input_source, &source->pfd);

        g_source_set_name (glib_data.input_source, "cog: input");
        g_source_set_can_recurse (glib_data.input_source, TRUE);
        g_source_attach (glib_data.input_source, g_main_context_get_thread_default ());
    }

    /* changed repeat back to g_source_new as g_timeout_source_new only fires once per keydown */
    glib_data.key_repeat_source = g_source_new (&key_repeat_source_funcs, sizeof (GSource));
    g_source_set_name(glib_data.key_repeat_source, "cog: key repeat");
    g_source_set_can_recurse (glib_data.key_repeat_source, TRUE);
    g_source_set_priority (glib_data.key_repeat_source, G_PRIORITY_DEFAULT_IDLE);
    g_source_attach (glib_data.key_repeat_source, g_main_context_get_thread_default ());
    return(TRUE);
}

static void *
check_supported(void *data G_GNUC_UNUSED)
{
    if (check_drm())
        return GINT_TO_POINTER(TRUE);
    return GINT_TO_POINTER(FALSE);
}

static gboolean
cog_drm_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    /* get_view_backend() might not been called yet */
    g_assert(wpe_view_data.backend);
    return wpe_view_data.backend;
}

static gboolean
cog_drm_platform_setup(CogPlatform *platform, CogShell *shell, const char *params, GError **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    CogDrmPlatform *self = COG_DRM_PLATFORM(platform);

    init_config(COG_DRM_PLATFORM(platform), shell, params);
    g_signal_connect(shell, "notify::device-scale-factor", G_CALLBACK(cog_drm_platform_shell_device_factor_changed), NULL);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_drm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize DRM");
        return FALSE;
    }

    if (g_getenv ("COG_PLATFORM_DRM_CURSOR")) {
        if (init_cursor(drm_data.fd, drm_data.crtc.index)) {
            cursor.enabled = true;
            cursor.x = drm_data.width/2;
            cursor.y = drm_data.height/2;
            set_cursor("default");
            move_cursor(drm_data.fd, drm_data.crtc.obj_id, cursor.x, cursor.y);
        } else {
            g_warning ("Failed to initialize cursor");
        }
    }

    if (!init_gbm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GBM");
        return FALSE;
    }

    if (!init_egl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize EGL");
        return FALSE;
    }

    if (self->use_gles) {
        self->renderer = cog_drm_gles_renderer_new(gbm_data.device,
                                                   egl_data.display,
                                                   drm_data.plane.obj_id,
                                                   drm_data.crtc.obj_id,
                                                   drm_data.connector.obj_id,
                                                   drm_data.mode,
                                                   drm_data.atomic_modesetting);
    } else {
        self->renderer = cog_drm_modeset_renderer_new(gbm_data.device,
                                                      drm_data.plane.obj_id,
                                                      drm_data.crtc.obj_id,
                                                      drm_data.connector.obj_id,
                                                      drm_data.mode,
                                                      drm_data.atomic_modesetting);
    }
    if (cog_drm_renderer_supports_rotation(self->renderer, self->rotation)) {
        cog_drm_renderer_set_rotation(self->renderer, self->rotation);
    } else {
        g_warning("Renderer '%s' does not support rotation %u (%u degrees).", self->renderer->name, self->rotation,
                  self->rotation * 90);
        self->rotation = COG_GL_RENDERER_ROTATION_0;
    }

    if (!init_input(COG_DRM_PLATFORM(platform))) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize input");
        return FALSE;
    }

    if (!init_glib(self)) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    if (self->renderer->initialize) {
        if (!self->renderer->initialize(self->renderer, error))
            return FALSE;
    }
    g_debug("%s: Renderer '%s' initialized.", __func__, self->renderer->name);

    wpe_fdo_initialize_for_egl_display (egl_data.display);

    cog_gamepad_setup(gamepad_provider_get_view_backend_for_gamepad);

    return TRUE;
}

static void
cog_drm_platform_finalize(GObject *object)
{
    CogDrmPlatform *self = COG_DRM_PLATFORM(object);

    g_idle_remove_by_data(&wpe_view_data);

    g_clear_pointer(&self->renderer, cog_drm_renderer_destroy);

    clear_glib();
    clear_input(self);
    clear_egl();
    clear_gbm();
    clear_cursor(drm_data.fd, drm_data.crtc.obj_id);
    clear_drm();

    G_OBJECT_CLASS(cog_drm_platform_parent_class)->finalize(object);
}

static WebKitWebViewBackend *
cog_drm_platform_get_view_backend(CogPlatform *platform, WebKitWebView *related_view, GError **error)
{
    CogDrmPlatform *self = COG_DRM_PLATFORM(platform);
    wpe_host_data.exportable = self->renderer->create_exportable(self->renderer,
                                                                 drm_data.width / drm_data.device_scale,
                                                                 drm_data.height / drm_data.device_scale);
    g_assert (wpe_host_data.exportable);

    wpe_view_data.backend = wpe_view_backend_exportable_fdo_get_view_backend (wpe_host_data.exportable);
    g_assert (wpe_view_data.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (wpe_view_data.backend,
                                     (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     wpe_host_data.exportable);
    g_assert (wk_view_backend);

    return wk_view_backend;
}

static gboolean
set_target_refresh_rate(gpointer user_data)
{
    wpe_view_backend_set_target_refresh_rate(wpe_view_data.backend, drm_data.refresh * 1000);
    return G_SOURCE_REMOVE;
}

static void
on_mouse_target_changed(WebKitWebView *view, WebKitHitTestResult *hitTestResult, guint mouseModifiers)
{
    CogCursorNames cursor_names = cog_cursors_get_names_for_hit_test(hitTestResult);

    /* pass list of names to set_cursor-- stop at success */
    int set = 0;
    for (int i = 0; !set && cursor_names[i]; ++i) {
      set = !set_cursor(cursor_names[i]);
    }
    if (!set)
      set_cursor("pointer");
}

/* import the tweak values on page-load change */
static void
on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, void *userdata)
{
    if (event == WEBKIT_LOAD_FINISHED) {
        CogView *view = COG_VIEW(web_view);
        /* cursor=0 hides the cursor */
        cursor.hidden = !cog_view_tweak_value(view, "CURSOR", 1);
        /* scroll=0 ignores wheel-scroll events */
        cursor.scroll =  cog_view_tweak_value(view, "SCROLL", 1);
        if (cursor.hidden)
            set_cursor("hidden");
    }
}

static void
cog_drm_platform_init_web_view(CogPlatform *platform, WebKitWebView *view)
{
    COG_DRM_PLATFORM(platform)->web_view = COG_VIEW(view);

    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_set_process_model(ctx, WEBKIT_PROCESS_MODEL_MULTIPLE_SECONDARY_PROCESSES);

    wpe_view_backend_dispatch_set_device_scale_factor(wpe_view_data.backend, drm_data.device_scale);
    wpe_view_backend_add_activity_state(wpe_view_data.backend, wpe_view_activity_state_focused);

    g_idle_add(G_SOURCE_FUNC(set_target_refresh_rate), &wpe_view_data);

    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
}

static void
cog_drm_platform_set_property(GObject *object, unsigned prop_id, const GValue *value, GParamSpec *pspec)
{
    CogDrmPlatform *self = COG_DRM_PLATFORM(object);
    switch (prop_id) {
    case PROP_ROTATION: {
        CogGLRendererRotation rotation = g_value_get_uint(value);
        if (rotation == self->rotation)
            return;

        if (!self->renderer) {
            update_logical_input_size(self->rotation = rotation);
        } else if (cog_drm_renderer_set_rotation(self->renderer, rotation)) {
            update_logical_input_size(self->rotation = rotation);
            if (self->rotatable_input_devices)
                g_list_foreach(self->rotatable_input_devices, (GFunc) input_configure_device, self);
        } else {
            g_critical("%s: Could not set %u rotation (%u degrees), unsupported", __func__, rotation, rotation * 90);
        }
        break;
    }
    case PROP_RENDERER: {
        const char *name = g_value_get_string(value);
        if (g_strcmp0(name, "modeset") == 0) {
            self->use_gles = false;
        } else if (g_strcmp0(name, "gles") == 0) {
            self->use_gles = true;
        } else {
            g_warning("%s: Invalid renderer name '%s'.", __func__, name);
        }
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_drm_platform_get_property(GObject *object, unsigned prop_id, GValue *value, GParamSpec *pspec)
{
    CogDrmPlatform *self = COG_DRM_PLATFORM(object);
    switch (prop_id) {
    case PROP_ROTATION:
        g_value_set_uint(value, self->rotation);
        break;
    case PROP_RENDERER:
        g_value_set_string(value, self->use_gles ? "gles" : "modeset");
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_drm_platform_class_init(CogDrmPlatformClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_drm_platform_finalize;
    object_class->set_property = cog_drm_platform_set_property;
    object_class->get_property = cog_drm_platform_get_property;

    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_drm_platform_is_supported;
    platform_class->setup = cog_drm_platform_setup;
    platform_class->get_view_backend = cog_drm_platform_get_view_backend;
    platform_class->init_web_view = cog_drm_platform_init_web_view;

    /**
     * CogDrmPlatform:rotation:
     *
     * Rotation applied to the output. The value is the number of 90 degree
     * increments applied counter-clockwise, which means the possible values
     * are:
     *
     * - `0`: No rotation.
     * - `1`: 90 degrees.
     * - `2`: 180 degrees.
     * - `3`: 270 degrees..
     */
    s_properties[PROP_ROTATION] =
        g_param_spec_uint("rotation", "Output rotation", "Number of counter-clockwise 90 degree rotation increments", 0,
                          3, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /** CogDrmPlatform:renderer:
     *
     * Mechanism used to present the output. Possible values are:
     *
     * - `modeset`: Present content by attaching rendered buffers to a
     *   KMS plane. Does not support rotation at the moment.
     * - `gles`: Use OpenGL ES to present content by drawing quads textured
     *   with the contents of rendered buffers. Supports all rotations by
     *   modifying the texture UV-mapping.
     */
    s_properties[PROP_RENDERER] =
        g_param_spec_string("renderer",
                            "Output renderer",
                            "Mechanism used to produce output on the screen",
                            "modeset",
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);
}

static void
cog_drm_platform_class_finalize(CogDrmPlatformClass *klass)
{
}

static void
cog_drm_platform_init(CogDrmPlatform *self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_drm_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_drm_platform_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_drm_unload(GIOModule *module G_GNUC_UNUSED)
{
}
