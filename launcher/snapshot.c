#include <wpe/webkit-web-extension.h>
//#include <wpe/webkitdom.h>

#define preload_js "preload.js"

static GRegex *_dat_fn = NULL;
static const char *_ctrl = NULL;

// load cookie/state data
static char *func_load(const char *file)
{
  gchar *data = NULL;
  if (g_regex_match(_dat_fn, file, 0, NULL)) {
    g_file_get_contents(file, &data, NULL, NULL);
  }
  //g_print("func_load(%s):\n%s\n", file, data);
  g_print("func_load(%s): %ld\n", file, strlen(data));
  return(data);
}

// save cookie/state data
static void func_save(const char *file, const char *data)
{
  //g_print("func_save(%s):\n%s\n", file, data);
  g_print("func_save(%s): %ld\n", file, strlen(data));
  if (g_regex_match(_dat_fn, file, 0, NULL)) {
    g_file_set_contents(file, data, -1, NULL);
  }

  if (g_strcmp0(file, "preload.js") == 0) {
    g_file_set_contents(file, data, -1, NULL);
  }
}

// signal browser to quit
static void func_quit()
{
  kill(getppid(), SIGQUIT);
}

// execute on page show (control origin only)
static void on_show(const char *file)
{
  g_print("on_show\n");
  unlink(preload_js);
}

// execute on page close (non-control origins)
static void on_hide(const char *file)
{
  g_print("on_hide: %s\n", file);
  JSCContext *ctx = jsc_context_get_current();

  const char *js = "JSON.stringify(localStorage);";
  JSCValue *obj = jsc_context_evaluate_with_source_uri(ctx, js, -1, "fake source", 1);
  char *str = jsc_value_to_string(obj);
  //g_print("on_hide1: obj=%s\n", str);
  g_file_set_contents(file, str, -1, NULL);
  g_free(str);
  g_object_unref(obj);
  g_object_unref(ctx);
}

// signaled on window load: insert js helper script
static void on_load(WebKitScriptWorld *world, WebKitWebPage *page, WebKitFrame *frame, void *userdata)
{
  const char *uri = webkit_frame_get_uri(frame);
  if (g_strcmp0(uri, "about:blank") == 0)
    return;

  // format origin for this page (could be main or iframe)
  char fn_js[1024];
  char fn_imp[1024];
  char fn_exp[1024];
  GString *ori = g_string_new(uri);
  g_string_replace(ori, "://", "_", 1);
  if (strchr(ori->str, '/') != NULL)
    g_string_truncate(ori, strchr(ori->str, '/')-ori->str);
  if (strchr(ori->str, ':') == NULL)
    g_string_append(ori, ":0");
  g_string_replace(ori, ":", "_", 1);

  // determine if js control origin
  _ctrl = getenv("COG_PRELOAD_JS");
  if ((_ctrl != NULL) && (g_strcmp0(_ctrl, uri) != 0))
    _ctrl = NULL;
  g_print("origin: %s %p\n", ori->str, _ctrl);

  g_snprintf(fn_imp, sizeof(fn_imp), "%s.imp", ori->str);
  g_snprintf(fn_exp, sizeof(fn_exp), "%s.exp", ori->str);
  g_string_free(ori, TRUE);

  JSCContext *ctx = webkit_frame_get_js_context_for_script_world(frame, world);
  JSCValue *win = jsc_context_get_value(ctx, "window");

  // import storage if available
  char *data = NULL;
  g_file_get_contents(fn_imp, &data, NULL, NULL);
  if (data != NULL) {
    JSCValue *str = jsc_value_new_string(ctx, data);
    jsc_value_object_set_property(win, "ls", str);
    g_object_unref(str);
    g_print("import:\n%s\n", data);
    const char *js = "localStorage.clear(); Object.entries(JSON.parse(window.ls)).forEach(([k,v])=>{localStorage.setItem(k,v); });";
    JSCValue *obj = jsc_context_evaluate(ctx, js, -1);
    g_object_unref(obj);
    g_free(data);
    unlink(fn_imp);
  }

  // setup pageshow (control) / pagehide (other)
  JSCValue *args[2] = {
    jsc_value_new_string(ctx, _ctrl ? "pageshow" : "pagehide"),
    jsc_value_new_function(ctx, NULL, G_CALLBACK(_ctrl ? on_show : on_hide), g_strdup(fn_exp), g_free, G_TYPE_NONE, 0),
  };
  JSCValue *retn = jsc_value_object_invoke_methodv(win, "addEventListener", sizeof(args)/sizeof(args[0]), args);
  g_object_unref(retn);
  for (int i = 0; i < sizeof(args)/sizeof(args[0]); ++i)
    g_object_unref(args[i]);

  // load the script for this domain
  gchar *jss = NULL;
  //g_file_get_contents(fn_js, &jss, NULL, NULL);
  g_file_get_contents(preload_js, &jss, NULL, NULL);
  if ((_ctrl != NULL) || (jss != NULL)) {
    // setup callback functions (script must save copies if it needs them later)
    JSCValue *load = jsc_value_new_function(ctx, NULL, G_CALLBACK(func_load), NULL, NULL,
        G_TYPE_STRING, 1, G_TYPE_STRING);
    jsc_value_object_set_property(win, "ext_load", load);
    g_object_unref(load);

    JSCValue *save = jsc_value_new_function(ctx, NULL, G_CALLBACK(func_save), NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
    jsc_value_object_set_property(win, "ext_save", save);
    g_object_unref(save);

    JSCValue *quit = jsc_value_new_function(ctx, NULL, G_CALLBACK(func_quit), NULL, NULL,
        G_TYPE_NONE, 0);
    jsc_value_object_set_property(win, "ext_quit", quit);
    g_object_unref(quit);
  }

  if (jss != NULL) {
    // validate the script
    JSCException *except = NULL;
    jsc_context_check_syntax(ctx, jss, -1, JSC_CHECK_SYNTAX_MODE_SCRIPT, fn_js, 1, &except);
    if (except != NULL) {
      const char *uri = jsc_exception_get_source_uri(except);
      int line = jsc_exception_get_line_number(except);
      const char *msg = jsc_exception_get_message(except);
      g_print("%s/%d: %s\n", uri, line, msg);
      g_object_unref(except);
      g_free(jss);
    } else {
      // execute script
      g_print("exec %s\n", fn_js);
      JSCValue *obj = jsc_context_evaluate_with_source_uri(ctx, jss, -1, fn_js, 1);
      g_object_unref(obj);
      g_free(jss);
    }

    // remove import/export (script can copy if needs later)
    if (_ctrl == NULL) {
      g_print("remove functions\n");
      jsc_value_object_delete_property(win, "ext_load");
      jsc_value_object_delete_property(win, "ext_save");
      jsc_value_object_delete_property(win, "ext_quit");
    }
  }

  g_object_unref(win);
  g_object_unref(ctx);
}

// initial callback for web-process extentions
G_MODULE_EXPORT void webkit_web_extension_initialize(WebKitWebExtension *ext)
{
  _dat_fn = g_regex_new("^([a-z0-9][-_a-z0-9.]{1,31}).(imp|exp|dat)$", G_REGEX_CASELESS, 0, NULL);
  g_signal_connect(webkit_script_world_get_default(), "window-object-cleared", G_CALLBACK(on_load), NULL);
}

G_MODULE_EXPORT void webkit_web_extension_initialize_with_user_data(WebKitWebExtension *ext, const GVariant *userdata)
{
  webkit_web_extension_initialize(ext);
}
