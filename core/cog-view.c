/*
 * cog-view.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-view.h"
#include "cog-platform.h"
#include "cog-view-private.h"

/**
 * CogView:
 *
 * Convenience base class for web views.
 *
 * Most of the functionality dealing with web views when using the
 * Cog core library uses #CogView instead of [class@WPEWebKit.WebView].
 * The base class is extended to delegate the creation of its
 * [struct@WPEWebKit.WebViewBackend] to the current [class@CogPlatform]
 * implementation. Optionally, platform plug-in implementations can
 * provide their own view class implementation by overriding
 * the [func@CogPlatform.get_view_type] method.
 *
 * A number of utility functions are also provided.
 */

typedef struct {
    gboolean use_key_bindings;

    GWeakRef viewport; /* Weak reference to the associated CogViewport */
} CogViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(CogView, cog_view, WEBKIT_TYPE_WEB_VIEW)

struct _CogCoreViewClass {
    CogViewClass parent_class;
};

enum {
    PROP_0,
    PROP_USE_KEY_BINDINGS,
    PROP_VIEWPORT,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = {
    NULL,
};

G_DECLARE_DERIVABLE_TYPE(CogCoreView, cog_core_view, COG, CORE_VIEW, CogView)
G_DEFINE_TYPE(CogCoreView, cog_core_view, COG_TYPE_VIEW)

static struct {
    uint32_t fr_key;
    uint32_t fr_mod;
    uint32_t to_key;
    uint32_t to_mod;
} g_keymap[1024] = { 0 };

static struct {
    uint32_t key;
    int32_t val;
} g_custom[256] = { 0 };

static GObject *
cog_view_constructor(GType type, unsigned n_properties, GObjectConstructParam *properties)
{
    GObject *object = G_OBJECT_CLASS(cog_view_parent_class)->constructor(type, n_properties, properties);

    CogViewClass *view_class = COG_VIEW_GET_CLASS(object);
    if (view_class->create_backend) {
        WebKitWebViewBackend *backend = (*view_class->create_backend)((CogView *) object);

        GValue backend_value = G_VALUE_INIT;
        g_value_take_boxed(g_value_init(&backend_value, WEBKIT_TYPE_WEB_VIEW_BACKEND), backend);
        g_object_set_property(object, "backend", &backend_value);
    } else {
        g_error("Type %s did not define the create_backend vfunc", g_type_name(type));
    }

    return object;
}

static void
cog_view_set_property(GObject *object, unsigned prop_id, const GValue *value, GParamSpec *pspec)
{
    CogView *self = COG_VIEW(object);
    switch (prop_id) {
    case PROP_USE_KEY_BINDINGS:
        cog_view_set_use_key_bindings(self, g_value_get_boolean(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_view_get_property(GObject *object, unsigned prop_id, GValue *value, GParamSpec *pspec)
{
    CogView *self = COG_VIEW(object);
    switch (prop_id) {
    case PROP_USE_KEY_BINDINGS:
        g_value_set_boolean(value, cog_view_get_use_key_bindings(self));
        break;
    case PROP_VIEWPORT:
        g_value_take_object(value, cog_view_get_viewport(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
cog_view_class_init(CogViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->set_property = cog_view_set_property;
    object_class->get_property = cog_view_get_property;
    object_class->constructor = cog_view_constructor;

    /**
     * CogView:use-key-bindings: (default-value enabled)
     *
     * Whether to use the built-in key binding handling.
     *
     * Since: 0.20
     */
    s_properties[PROP_USE_KEY_BINDINGS] =
        g_param_spec_boolean("use-key-bindings", NULL, NULL, TRUE,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * CogView:viewport: (default-value null)
     *
     * Viewport where the CogView belongs.
     *
     * Since: 0.20
     */
    s_properties[PROP_VIEWPORT] =
        g_param_spec_object("viewport", NULL, NULL, G_TYPE_OBJECT, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, s_properties);
}

static void
cog_view_init(CogView *self)
{
    CogViewPrivate *priv = cog_view_get_instance_private(self);

    // Init the weak reference to the CogViewport.
    g_weak_ref_init(&priv->viewport, NULL);
}

static inline struct wpe_view_backend *
cog_view_get_backend_internal(CogView *self)
{
    WebKitWebViewBackend *backend = webkit_web_view_get_backend((WebKitWebView *) self);
    return webkit_web_view_backend_get_wpe_backend(backend);
}

/**
 * cog_view_get_backend:
 * @self: A view.
 *
 * Get the backend for the view.
 *
 * Returns: (transfer none) (not nullable): The WPE view backend for the view.
 */
struct wpe_view_backend *
cog_view_get_backend(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), NULL);
    return cog_view_get_backend_internal(self);
}

static void *
_cog_view_get_impl_type_init(GType *type)
{
    *type = cog_core_view_get_type();

    CogPlatform *platform = cog_platform_get();
    if (platform) {
        CogPlatformClass *platform_class = COG_PLATFORM_GET_CLASS(platform);
        if (platform_class->get_view_type) {
            GType view_type = (*platform_class->get_view_type)();
            *type = view_type;
        }
    }

    g_debug("%s: using %s", G_STRFUNC, g_type_name(*type));
    return NULL;
}

/**
 * cog_view_get_impl_type:
 *
 * Get the type of the web view implementation in use.
 *
 * This function always returns a valid type. If the If the active
 * [class@CogPlatform] does not provide a custom view type, the default
 * built-in type included as part of the Cog core library is returned.
 *
 * When creating web views, this function must be used to retrieve the
 * type to use, e.g.:
 *
 * ```c
 * WebKitWebView *view = g_object_new(cog_view_get_impl_type(), NULL);
 * ```
 *
 * In most cases it should be possible to use the convenience function
 * [ctor@View.new], which uses this function internally.
 *
 * Returns: Type of the view implementation in use.
 */
GType
cog_view_get_impl_type(void)
{
    static GType view_type;
    static GOnce once = G_ONCE_INIT;

    g_once(&once, (GThreadFunc) _cog_view_get_impl_type_init, &view_type);

    return view_type;
}

/**
 * cog_view_new: (constructor)
 * @first_property_name: Name of the first property.
 * @...: Value of the first property, followed optionally by more name/value
 *    pairs, followed by %NULL.
 *
 * Creates a new instance of a view implementation and sets its properties.
 *
 * Returns: (transfer full): A new web view.
 */
CogView *
cog_view_new(const char *first_property_name, ...)
{
    va_list args;
    va_start(args, first_property_name);
    void *view = g_object_new_valist(cog_view_get_impl_type(), first_property_name, args);
    va_end(args);
    return view;
}

static WebKitWebViewBackend *
cog_core_view_create_backend(CogView *view G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;

    WebKitWebViewBackend *backend = cog_platform_get_view_backend(cog_platform_get(), NULL, &error);
    if (!backend)
        g_error("%s: Could not create view backend, %s", G_STRFUNC, error->message);

    g_debug("%s: backend %p", G_STRFUNC, backend);
    return backend;
}

static void
cog_core_view_class_init(CogCoreViewClass *klass)
{
    CogViewClass *view_class = COG_VIEW_CLASS(klass);
    view_class->create_backend = cog_core_view_create_backend;
}

static void
cog_core_view_init(CogCoreView *self)
{
}

gboolean
cog_view_try_handle_key_binding(CogView *self, const struct wpe_input_keyboard_event *event)
{
    static const float DEFAULT_ZOOM_STEP = 0.1f;

    if (!event->pressed)
        return FALSE;

    /*
     * TODO: F11, fullscreen. It depends on platform-specific behaviour, which
     *       means it may end up being a vfunc in CogView, or in CogPlatform.
     */

    /* Ctrl+W, exit the application */
    if (event->modifiers == wpe_input_keyboard_modifier_control && event->key_code == WPE_KEY_w) {
        GApplication *app = g_application_get_default();
        if (app)
            g_application_quit(app);
        else
            exit(EXIT_SUCCESS);
        return TRUE;
    }

    /* Ctrl+Plus, zoom in */
    if (event->modifiers == wpe_input_keyboard_modifier_control && event->key_code == WPE_KEY_plus) {
        const double level = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(self));
        webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(self), level + DEFAULT_ZOOM_STEP);
        return TRUE;
    }

    /* Ctrl+Minus, zoom out */
    if (event->modifiers == wpe_input_keyboard_modifier_control && event->key_code == WPE_KEY_minus) {
        const double level = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(self));
        webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(self), level - DEFAULT_ZOOM_STEP);
        return TRUE;
    }

    /* Ctrl+0, restore zoom level to 1.0 */
    if (event->modifiers == wpe_input_keyboard_modifier_control && event->key_code == WPE_KEY_0) {
        webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(self), 1.0f);
        return TRUE;
    }

    /* Alt+Left, navigate back */
    if (event->modifiers == wpe_input_keyboard_modifier_alt && event->key_code == WPE_KEY_Left) {
        webkit_web_view_go_back(WEBKIT_WEB_VIEW(self));
        return TRUE;
    }

    /* Alt+Right, navigate forward */
    if (event->modifiers == wpe_input_keyboard_modifier_alt && event->key_code == WPE_KEY_Right) {
        webkit_web_view_go_forward(WEBKIT_WEB_VIEW(self));
        return TRUE;
    }

    /* Ctrl+R or F5, reload */
    if ((event->modifiers == wpe_input_keyboard_modifier_control && event->key_code == WPE_KEY_r) ||
        (!event->modifiers && event->key_code == WPE_KEY_F5)) {
        webkit_web_view_reload(WEBKIT_WEB_VIEW(self));
        return true;
    }

    /* Ctrl+Shift+R or Shift+F5, reload ignoring cache */
    if ((event->modifiers == (wpe_input_keyboard_modifier_control | wpe_input_keyboard_modifier_shift) &&
         event->key_code == WPE_KEY_R) ||
        (event->modifiers == wpe_input_keyboard_modifier_shift && event->key_code == WPE_KEY_F5)) {
        webkit_web_view_reload_bypass_cache(WEBKIT_WEB_VIEW(self));
        return TRUE;
    }

    /* Virtual WPE_KEY_HomePage: pop all history until first webpage (home) */
    if (event->key_code == WPE_KEY_HomePage) {
        g_warning("pop goes the history\n");
        WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(WEBKIT_WEB_VIEW(self));
        WebKitBackForwardListItem *item = NULL;
        for (int i = 0; webkit_back_forward_list_get_nth_item(list, i-1) != NULL; --i)
            item = webkit_back_forward_list_get_nth_item(list, i-1);
        if (item != NULL)
            webkit_web_view_go_to_back_forward_list_item(WEBKIT_WEB_VIEW(self), item);
        return TRUE;
    }

    return FALSE;
}

/**
 * cog_view_handle_key_event:
 * @self: A view.
 * @event: (not nullable) (transfer none): A key input event.
 *
 * Sends a keyboard event to the web view.
 *
 * Platform implementations must call this method instead of directly using
 * [wpe_view_backend_dispatch_keyboard_event()](https://webplatformforembedded.github.io/libwpe/view-backend.html#wpe_view_backend_dispatch_keyboard_event)
 * in order to give the embedding application the chance to handle keyboard
 * bindings. See [id@cog_view_set_use_key_bindings] for more details.
 *
 * Since: 0.20
 */
void
cog_view_handle_key_event(CogView *self, const struct wpe_input_keyboard_event *event)
{
    g_return_if_fail(COG_IS_VIEW(self));
    g_return_if_fail(event);

    CogViewPrivate *priv = cog_view_get_instance_private(self);
    if (priv->use_key_bindings && cog_view_try_handle_key_binding(self, event))
        return;

    struct wpe_input_keyboard_event event_copy = *event;
    wpe_view_backend_dispatch_keyboard_event(cog_view_get_backend_internal(self), &event_copy);
}

gboolean
cog_view_remap_event(struct wpe_input_keyboard_event *event)
{
    uint32_t key = event->key_code;
    uint32_t mod = event->modifiers;
    for (int i = 0; (i < sizeof(g_keymap)/sizeof(g_keymap[0])) && !!g_keymap[i].fr_key; ++i) {
        if ((key == g_keymap[i].fr_key) && (mod == g_keymap[i].fr_mod)) {
            event->key_code = g_keymap[i].to_key;
            event->modifiers = g_keymap[i].to_mod;
            if (event->key_code == WPE_KEY_VoidSymbol)
                event->time = 0;
            g_warning("remap-fr: %d(%02x):%d(%02x)", key, key, mod, mod);
            g_warning("remap-to: %d(%02x):%d(%02x)", event->key_code, event->key_code, event->modifiers, event->modifiers);
            break;
        }
    }
    return((event->key_code != key) || (event->modifiers != mod));
}

int32_t
cog_view_remap_parm(const char *skey, int32_t def)
{
    uint32_t key = 0;
    for (; (*skey >= 'a') && (*skey <= 'z'); ++skey)
        if (key < 0x01000000)
            key = (key<<8) | *skey;

    for (int i = 0; (i < sizeof(g_custom)/sizeof(g_custom[0])) && !!g_custom[i].key; ++i) {
        if (g_custom[i].key == key)
            return(g_custom[i].val);
    }
    return(def);
}

/* install event remaps and custom parms for this page */
gboolean
cog_view_remap_import(const char *uri)
{
    /* clear keymap and custom parms */
    memset(&g_keymap[0], 0, sizeof(g_keymap[0]));
    memset(&g_custom[0], 0, sizeof(g_custom[0]));

    /* use COG_URL_KEYMAP to determine specific mapping for this url */
    const gchar *cmp = g_getenv("COG_KEYMAP_URL");
    if (cmp == NULL)
        return(0);
    g_warning("on_load_changed: cmp=%s uri=(%s)", cmp, uri);

    int env = -1;
    gchar **seg = g_strsplit(cmp, ",", 256);
    if (seg == NULL)
        return(0);
    for (int i = 0; seg[i] != NULL; ++i) {
        if (seg[i][0] == '#')
            env = i;
        else if (g_strstr_len(uri, -1, seg[i]) != NULL)
            break;
    }

    if (env >= 0) {
        g_warning("keymapping via %s", seg[env]);
        const gchar *map = g_getenv(seg[env]+1);
        if (map == NULL)
            g_warning("COG_URL_KEYMAP referenced missing env(%s)", seg[env]+1);

        /* key=val[comment], ... where key/val are dec or hex */
        int ki = 0, ci = 0;
        for (const gchar *s = map; (s != NULL) && (*s != 0);) {
            while ((*s > 0) && (*s <= ' '))
                ++s;
            if (*s == '$') {
                /* handle custom parms: $key=val */
                g_custom[ci].key = 0;
                for (++s; (*s >= 'a') && (*s <= 'z'); ++s) {
                    if (g_custom[ci].key < 0x01000000)
                        g_custom[ci].key = (g_custom[ci].key<<8) | *s;
                }
                if (*s != '=')
                    break;
                g_custom[ci].val = g_ascii_strtoll(s+1, (gchar **)&s, 0);
                if (ci+1 < sizeof(g_custom)/sizeof(g_custom[0]))
                    ++ci;
            } else {
                /* handle remap: key:mod=new-key:new-mod */
                g_keymap[ki].fr_key = g_ascii_strtoll(s, (gchar **)&s, 0);
                g_keymap[ki].fr_mod = (*s == ':') ? g_ascii_strtoll(s+1, (gchar **)&s, 0) : 0;
                if (*s != '=')
                    break;
                g_keymap[ki].to_key = g_ascii_strtoll(s+1, (gchar **)&s, 0);
                g_keymap[ki].to_mod = (*s == ':') ? g_ascii_strtoll(s+1, (gchar **)&s, 0) : 0;
                if (ki+1 < sizeof(g_keymap)/sizeof(g_keymap[0]))
                    ++ki;
            }
            /* gobble chars after key=val until comma to allow for comments */
            while ((*s > 0) && (*s != ','))
                ++s;
            if (*s == ',')
                ++s;
        }
        g_keymap[ki].fr_key = g_keymap[ki].fr_mod = 0;
        g_custom[ci].key = 0;
    }
    g_strfreev(seg);
    return(0);
}

/**
 * cog_view_set_use_key_bindings: (set-property use-key-bindings)
 * @self: A view.
 * @enable: Whether to enable the key bindings.
 *
 * Sets whether to enable usage of the built-in key bindings.
 *
 * In order for the view to process key bindings, platform implementations
 * need to use [id@cog_view_handle_key_event] to send events to the view.
 *
 * The following key bindings are supported:
 *
 * | Binding                     | Action                               |
 * |:----------------------------|:-------------------------------------|
 * | `Ctrl-W`                    | Exit the application.                |
 * | `Ctrl-+`                    | Zoom in.                             |
 * | `Ctrl--`                    | Zoom out.                            |
 * | `Ctrl-0`                    | Restore default zoom level.          |
 * | `Alt-Left`                  | Go to previous page in history.      |
 * | `Alt-Right`                 | Go to next page in history.          |
 * | `Ctrl-R` / `F5`             | Reload current page.                 |
 * | `Ctrl-Shift-R` / `Shift-F5` | Reload current page ignoring caches. |
 *
 * Since: 0.20
 */
void
cog_view_set_use_key_bindings(CogView *self, gboolean enable)
{
    g_return_if_fail(COG_IS_VIEW(self));

    CogViewPrivate *priv = cog_view_get_instance_private(COG_VIEW(self));

    enable = !!enable;
    if (priv->use_key_bindings == enable)
        return;

    priv->use_key_bindings = enable;
    g_object_notify_by_pspec(G_OBJECT(self), s_properties[PROP_USE_KEY_BINDINGS]);
}

/**
 * cog_view_set_viewport
 * @self: A view.
 * @viewport: The viewport where the View belongs.
 *
 * Since: 0.20
 */
void
cog_view_set_viewport(CogView *self, CogViewport *viewport)
{
    g_return_if_fail(COG_IS_VIEW(self));

    CogViewPrivate *priv = cog_view_get_instance_private(self);

    // Sets a reference to the viewport
    g_weak_ref_set(&priv->viewport, viewport);

    g_object_notify_by_pspec(G_OBJECT(self), s_properties[PROP_VIEWPORT]);
}

/**
 * cog_view_get_use_key_bindings: (get-property use-key-bindings)
 * @self: A view.
 *
 * Gets whether the built-in key bindings are enabled.
 *
 * Returns: Whether the built-in key bindings are in use.
 *
 * Since: 0.20
 */
gboolean
cog_view_get_use_key_bindings(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), FALSE);

    CogViewPrivate *priv = cog_view_get_instance_private(COG_VIEW(self));
    return priv->use_key_bindings;
}

/**
 * cog_view_get_viewport: (get-property viewport)
 * @self: A view.
 *
 * Gets viewport where the view is attached.
 *
 * Returns: (transfer full) (nullable): The viewport what the view belongs.
 *
 * Since: 0.20
 */
CogViewport *
cog_view_get_viewport(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), NULL);
    return g_weak_ref_get(&((CogViewPrivate *) cog_view_get_instance_private(self))->viewport);
}

/**
 * cog_view_is_visible:
 * @self: A view.
 *
 * Gets whether the view is visible in a viewport.
 *
 * Returns: True if the view is visible in its viewport and false in any
 * other case.
 *
 * Since: 0.20
 */
gboolean
cog_view_is_visible(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), false);

    g_autoptr(CogViewport) viewport = cog_view_get_viewport(self);
    return viewport && self == cog_viewport_get_visible_view(viewport);
}

/**
 * cog_view_set_visible:
 * @self: A view.
 *
 * Set the view as visible in its viewport.
 *
 * Returns: %TRUE if the view was set as visible, or %FALSE if the view
 *    does is not attached to a viewport.
 *
 * Since: 0.20
 */
gboolean
cog_view_set_visible(CogView *self)
{
    g_assert(COG_IS_VIEW(self));

    g_autoptr(CogViewport) viewport = cog_view_get_viewport(self);
    if (!viewport)
        return FALSE;

    cog_viewport_set_visible_view(viewport, self);

    return TRUE;
}
