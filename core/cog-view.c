/*
 * cog-view.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-view.h"
#include "cog-platform.h"
#include "cog-view-private.h"
#include "cog-utils.h"

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

    char tweaks[1024];
    struct {
        uint32_t fkey;
        uint32_t fmod;
        uint32_t tkey;
        uint32_t tmod;
    } keymap[1024];

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

/* a non-conflicting modifier for non-repeating remapped keys */
enum { wpe_input_keyboard_modifier_no_repeat = 1<<7 };

G_DECLARE_DERIVABLE_TYPE(CogCoreView, cog_core_view, COG, CORE_VIEW, CogView)
G_DEFINE_TYPE(CogCoreView, cog_core_view, COG_TYPE_VIEW)

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
on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, void *userdata)
{
    CogView *self = COG_VIEW(web_view);
    CogViewPrivate *priv = cog_view_get_instance_private(COG_VIEW(self));
    const gchar *uri = webkit_web_view_get_uri(web_view);

    /* load tweaks at WEBKIT_LOAD_COMMITTED for WEBKIT_LOAD_FINISHED listeners */
    if (event != WEBKIT_LOAD_COMMITTED) {
        /* load the tweaks-- uri first so takes precedent */
        const gchar *tweak_uri = cog_uri_get_env(uri, g_getenv("COG_TWEAKS_URI"));
        const gchar *tweak_all = g_getenv("COG_TWEAKS_ALL");
        g_snprintf(priv->tweaks, sizeof(priv->tweaks), " %s %s",
                tweak_uri ? tweak_uri : "", tweak_all ? tweak_all : "");
        g_ascii_strup(priv->tweaks, -1);
        g_free((void*)tweak_uri);
        g_free((void*)tweak_all);
    }

    if (event == WEBKIT_LOAD_FINISHED) {
        /* load any javascript injections-- all first so uri can override */
        const gchar *js_uri = cog_uri_get_env(uri, g_getenv("COG_EXECJS_URI"));
        if ((js_uri != NULL) && (js_uri[0] != 0))
            webkit_web_view_run_javascript(web_view, js_uri, NULL, NULL, NULL);
        g_free((void*)js_uri);
        const gchar *js_all = g_getenv("COG_EXECJS_ALL");
        if ((js_all != NULL) && (js_all[0] != 0))
            webkit_web_view_run_javascript(web_view, js_all, NULL, NULL, NULL);
        g_free((void*)js_all);
    }

    /* load keymap at finished-- uri first so takes precedent */
    const gchar *keymap[] = {
        cog_uri_get_env(uri, g_getenv("COG_KEYMAP_URI")),
        g_getenv("COG_KEYMAP_ALL"), NULL };
    if (event != WEBKIT_LOAD_FINISHED)
        return;

    int cnt = 0;
    for (int idx = 0; keymap[idx] != NULL; ++idx) {
        const gchar *s = keymap[idx];
        g_print("s(%d)=%s\n", idx, s);
        /* key=val[comment], ... where key/val are dec or hex */
        while (!!s && !!*s && (cnt+1 < sizeof(priv->keymap)/sizeof(priv->keymap[0]))) {
            while ((*s > 0) && (*s <= ' '))
                ++s;
            /* handle remap: key:mod=new-key:new-mod */
            priv->keymap[cnt].fkey = g_ascii_strtoll(s, (gchar **)&s, 0);
            priv->keymap[cnt].fmod = (*s == ':') ? g_ascii_strtoll(s+1, (gchar **)&s, 0) : 0;
            if (*s != '=')
                break;
            priv->keymap[cnt].tkey = g_ascii_strtoll(s+1, (gchar **)&s, 0);
            priv->keymap[cnt].tmod = (*s == ':') ? g_ascii_strtoll(s+1, (gchar **)&s, 0) : 0;
            if (*s == '!')
                priv->keymap[cnt].tmod |= wpe_input_keyboard_modifier_no_repeat;
            ++cnt;
            /* gobble text after key=val until comma to allow for mini-comments */
            while ((*s > 0) && (*s != ',') && (*s != '|'))
                ++s;
            if ((*s == ',') || (*s == '|'))
                ++s;
        }
        g_free((void*)keymap[idx]);
    }
    priv->keymap[cnt].fkey = priv->keymap[cnt].fmod = 0;

    /* dump the completed remap */
    for (int i = 0; !!priv->keymap[i].fkey; ++i) {
        g_debug("keymap(%d): %04x:%02x -> %04x:%02x", i,
            priv->keymap[i].fkey, priv->keymap[i].fmod, priv->keymap[i].tkey, priv->keymap[i].tmod);
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

    // listen for load-changed to load keymap tables
    g_signal_connect(self, "load-changed", G_CALLBACK(on_load_changed), NULL);
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

/* return tweaks value */
int cog_view_tweak_value(CogView *self, const gchar *key, int def)
{
    CogViewPrivate *priv = cog_view_get_instance_private(COG_VIEW(self));
    if ((priv->tweaks[0] == 0) || (key == NULL) || (key[0] == 0))
        return(def);

    int len = strlen(key);
    const gchar *s = priv->tweaks-1;
    while ((s = strstr(s+1, key)) != NULL) {
        if (((s == priv->tweaks) || (s[-1] <= ' ')) && (s[len] == '=')) {
            g_debug("cog_view_tweak_value: %s=%s\n", key, s+len+1);
            return(g_ascii_strtoll(s+len+1, NULL, 0));
        }
    }
    return(def);
}

/* check for and apply keymap entry */
gboolean
cog_view_remap_event(CogView *self, struct wpe_input_keyboard_event *event, int *repeat, int *gobble)
{
    CogViewPrivate *priv = cog_view_get_instance_private(COG_VIEW(self));

    uint32_t orig_key = event->key_code;
    uint32_t orig_mod = event->modifiers;
    for (int i = 0; !!priv->keymap[i].fkey; ++i) {
        if ((orig_key == priv->keymap[i].fkey) && (orig_mod == priv->keymap[i].fmod)) {
            event->key_code = priv->keymap[i].tkey;
            event->modifiers = priv->keymap[i].tmod;
            *repeat = 1;
            if (event->modifiers & wpe_input_keyboard_modifier_no_repeat)
                *repeat = 0, event->modifiers ^= wpe_input_keyboard_modifier_no_repeat;
            *gobble = (event->key_code == 0);
            g_print("remap: %d(%02x):%d(%02x) -> %d(%02x):%d(%02x) rep=%d gob=%d\n",
                orig_key, orig_key, orig_mod, orig_mod,
                event->key_code, event->key_code, event->modifiers, event->modifiers,
                *repeat, *gobble);
            break;
        }
    }
    return((event->key_code != orig_key) || (event->modifiers != orig_mod));
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
