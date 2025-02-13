/*
 * plog.c
 *
 * libwpe launcher with external media-player support. From javascript,
 * window.webkit.messageHandlers.plog.postMessage("key1=val1 key2=val2 ...");
 * Key/val pairs are parsed into environment variables: $PARM_key1=val1.
 * $PLOG_PLAYER=command-line where $-prefixed references are replaced with
 * the corresponding environment variable.
 *
 * Marks the default view as hidden/non-focus so it wont process inputs
 * while player is active.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-launcher.h"
#include <sys/wait.h>

struct plog_source {
    GSource s;
    GPollFD pfd;
    int pipe0;
    int pipe1;
    uint64_t elap;
    int pid;
    CogView *view;
};

static void
print_module_info(GIOExtension *extension, void *userdata G_GNUC_UNUSED)
{
    g_info("  %s - %d/%s",
           g_io_extension_get_name(extension),
           g_io_extension_get_priority(extension),
           g_type_name(g_io_extension_get_type(extension)));
}

/* check if player pipe needs service */
static gboolean
player_check(GSource *state)
{
    struct plog_source *source = (void *)state;
    return(!!source->pfd.revents);
}

/* get player progress updates and handle player termination */
static gboolean
player_dispatch(GSource *state, GSourceFunc callback, gpointer user_data)
{
    struct plog_source *source = (void *)state;
    WebKitWebView *web_view = (WebKitWebView *)source->view;
    g_print("player_dispatch\n");

    if (!(source->pfd.revents & (G_IO_ERR|G_IO_HUP))) {
        /* process child status updates */
        char stat[256];
        int n = read(source->pfd.fd, stat, sizeof(stat)-1);
        if (n > 0) {
            stat[n] = 0;
            /* parse from first digit */
            for (n = 0; (stat[n] != 0) && ((stat[n] < '0') || (stat[n] > '9')); ++n)
                ;
            n = atoi(stat+n);
            char js[256];
            g_snprintf(js, sizeof(js), "window.plog_prog && window.plog_prog(%d);", n);
            webkit_web_view_run_javascript(web_view, js, NULL, NULL, NULL);
            return(TRUE);
        }
        /* fall through to shutdown if pipe-read failed */
    }

    /* wait for player shutdown */
    int stat;
    waitpid(source->pid, &stat, 0);

    /* run the finalize script */
    char js[256];
    uint32_t elap = (g_get_monotonic_time()-source->elap)/1000000;
    g_snprintf(js, sizeof(js), "window.plog_done && window.plog_done(%d);", elap);
    webkit_web_view_run_javascript(web_view, js, NULL, NULL, NULL);

    /* restore focus so input processing resumes */
    struct wpe_view_backend *backend = cog_view_get_backend(source->view);
    wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_focused);
    wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_visible);
    
    /* tell gsource to terminate */
    close(source->pipe1);
    g_print("plog_terminate\n");
    return(FALSE);
}

static GSourceFuncs player_funcs = {
    .check = player_check,
    .dispatch = player_dispatch,
};

/* process javascript player message */
static void
on_message(WebKitUserContentManager *content, WebKitJavascriptResult *result, void *view)
{
    JSCValue *jsc = webkit_javascript_result_get_js_value(result);
    const gchar *tmpl = g_getenv("PLOG_PLAYER");
    if ((tmpl == NULL) || !jsc_value_is_string(jsc)) {
        g_warning("player: non-string message or $PLOG_PLAYER undefined");
        return;
    }

    /* assign all parms to PARM_ prefixed environment variables */
    gchar **parms = g_strsplit(jsc_value_to_string(jsc), " ", 8);
    for (int p = 0; parms[p] != NULL; ++p) {
        gchar *kv = parms[p];
        g_strstrip(kv);
        int i = 0;
        while ((kv[i] >= 'A') && (kv[i] <= 'Z'))
            ++i;
        if ((i > 0) && (i <= 8) && (kv[i] == '=')) {
            kv[i++] = 0;
            char key[16];
            g_snprintf(key, sizeof(key), "PARM_%s", kv);
            g_setenv(key, kv+i, true);
            g_print("player: %s=%s\n", key, kv+i);
        }
    }

    /* create the pipe for status updates and process termination tracking */
    int pipefd[2] = { 0 };
    if (pipe(pipefd) < 0) {
        g_warning("player: canot create pipes");
        return;
    }
    char pipe1[16];
    g_snprintf(pipe1, sizeof(pipe1), "%d", pipefd[1]);
    g_setenv("PARM_PIPE", pipe1, true);

    /* compile parms into command line */
    int argi;
    const char *argp[16];
    gchar **argv = g_strsplit(tmpl, " ", sizeof(argp)/sizeof(argp[0])-1);
    for (argi = 0; argv[argi] != NULL; ++argi) {
        g_strstrip(argv[argi]);
        const gchar *s = argv[argi];
        if (*s == '$')
            s = g_getenv(s+1);
        if (s == NULL)
            s = "";
        argp[argi] = s;
        g_print("argv(%d)=%s\n", argi, s);
    }
    argp[argi] = NULL;

    /* remove focus from view so input is not processed */
    struct wpe_view_backend *backend = cog_view_get_backend(view);
    wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_focused);
    wpe_view_backend_remove_activity_state(backend, wpe_view_activity_state_visible);

    /* hacky method to force a web-view update after removing focus */
    WebKitWebView *web_view = (WebKitWebView *)view;
    double zoom =  webkit_web_view_get_zoom_level(web_view);
    webkit_web_view_set_zoom_level(web_view, zoom+0.001);
    webkit_web_view_set_zoom_level(web_view, zoom);

    /* launch the media player */
    int pid = fork();
    if (pid == 0) {
        execvp(argp[0], (char **)argp);
        g_warning("execvp");
        exit(0);
    }
    g_strfreev(argv);
    if (pipefd[1] > 0)
        close(pipefd[1]);

    /* create player-source to monitor playback */
    struct plog_source *source = (void *)g_source_new(&player_funcs, sizeof(struct plog_source));
    source->view = view;
    source->pid = pid;
    source->elap = g_get_monotonic_time();
    source->pipe1 = pipefd[1];
    source->pfd.fd = pipefd[0];
    source->pfd.events = G_IO_IN|G_IO_ERR|G_IO_HUP;
    source->pfd.revents = 0;
    g_source_add_poll(&source->s, &source->pfd);
    g_source_set_name(&source->s, "cog: input");
    g_source_set_can_recurse(&source->s, TRUE);
    g_source_attach(&source->s, g_main_context_get_thread_default());
}

static void
on_cookie_get(WebKitCookieManager *manager, GAsyncResult *result, void *userdata)
{
  WebKitWebView *view = userdata;

  GString *jar = g_string_new(NULL);
  GList *list = webkit_cookie_manager_get_cookies_finish(manager, result, NULL);
  for (GList *p = list; p != NULL; p = p->next) {
    SoupCookie *c = p->data;
    gchar *hdr = soup_cookie_to_set_cookie_header(c);
    g_string_append_printf(jar, "%s\n", hdr);
    g_free(hdr);
  }
  g_list_free(list);

  WebKitUserMessage *msg = webkit_user_message_new("cookout", g_variant_new("s", jar->str));
  g_string_free(jar, TRUE);
  webkit_web_view_send_message_to_page(view, msg, NULL, NULL, NULL);
}

static gboolean
on_view_msg(WebKitWebView *view, WebKitUserMessage *msg, void *userdata)
{
  const gchar *uri = webkit_web_view_get_uri(view);
  const gchar *name = webkit_user_message_get_name(msg);

  g_print("on_view_msg: %s %s\n", name, uri);

  if (strcmp(name, "cookout") == 0) {
    WebKitWebContext *ctx = webkit_web_view_get_context(view);
    WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(ctx);
    webkit_cookie_manager_get_cookies(cm, uri, NULL, (GAsyncReadyCallback)on_cookie_get, view);
  }
  return(FALSE);
}

static void
on_activate(GApplication *self, void *user_data)
{
    CogLauncher *launcher = COG_LAUNCHER(self);
    CogViewport *viewport = cog_launcher_get_viewport(launcher);
    WebKitWebView *view = (WebKitWebView *)cog_viewport_get_visible_view(viewport);

    WebKitUserContentManager *content = webkit_web_view_get_user_content_manager(view);
    g_signal_connect(content, "script-message-received::plog", G_CALLBACK(on_message), view);
    webkit_user_content_manager_register_script_message_handler(content, "plog");

    g_signal_connect(view, "user-message-received", G_CALLBACK(on_view_msg), NULL);
}

int main(int argc, char *argv[])
{
    g_set_application_name("plog");
    g_info("%s:", COG_MODULES_PLATFORM_EXTENSION_POINT);
    cog_modules_foreach(COG_MODULES_PLATFORM, print_module_info, NULL);
    CogLauncher *launcher = cog_launcher_new(COG_SESSION_REGULAR);
    g_autoptr(GApplication) app = G_APPLICATION(launcher);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return(g_application_run(app, argc, argv));
}

