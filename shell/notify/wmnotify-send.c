#define _POSIX_C_SOURCE 200809L
/* wmnotify-send - send a notification (like notify-send) without needing libnotify.
 *   wmnotify-send [-a app] [-i icon] [-u low|normal|critical] [-t ms] [-r id] [-p]
 *                 [-v 0..100] [-A key=label]... [-e desktop-entry] [-w] summary [body]
 * -p prints the id (use it with -r to update the same pop-up, e.g. a volume or brightness bar with -v).
 * -w waits (up to 60 s) for the pop-up to be closed or an action picked, and prints "key" or "closed:<reason>". */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void hint_str(DBusMessageIter *dict, const char *k, const char *v) {
    DBusMessageIter e, var; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &var); dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&e, &var); dbus_message_iter_close_container(dict, &e);
}
static void hint_int(DBusMessageIter *dict, const char *k, int type, const char *sig, const void *v) {
    DBusMessageIter e, var; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &var); dbus_message_iter_append_basic(&var, type, v);
    dbus_message_iter_close_container(&e, &var); dbus_message_iter_close_container(dict, &e);
}

int main(int argc, char **argv) {
    const char *app = "wmnotify-send", *icon = "", *entry = NULL, *keys[8], *labels[8]; int nact = 0, timeout = -1, urgency = 1, value = -1, print = 0, wait = 0;
    unsigned replaces = 0; int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1]; if (o == 'p') { print = 1; continue; } if (o == 'w') { wait = 1; continue; }
        if (i + 1 >= argc) { fprintf(stderr, "wmnotify-send: -%c needs an argument\n", o); return 2; }
        char *v = argv[++i];
        switch (o) {
        case 'a': app = v; break; case 'i': icon = v; break; case 'e': entry = v; break;
        case 't': timeout = atoi(v); break; case 'r': replaces = (unsigned)atoi(v); break; case 'v': value = atoi(v); break;
        case 'u': urgency = !strcmp(v, "low") ? 0 : !strcmp(v, "critical") ? 2 : 1; break;
        case 'A': { char *eq = strchr(v, '='); if (nact < 8 && eq) { *eq = 0; keys[nact] = v; labels[nact++] = eq + 1; } else { fputs("wmnotify-send: -A wants key=label\n", stderr); return 2; } break; }
        default: fprintf(stderr, "wmnotify-send: unknown option -%c\n", o); return 2;
        }
    }
    if (i >= argc) { fputs("usage: wmnotify-send [options] summary [body]\n", stderr); return 2; }
    const char *summary = argv[i], *body = i + 1 < argc ? argv[i + 1] : "";
    DBusError err; dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c) { fprintf(stderr, "wmnotify-send: no session bus: %s\n", err.message); return 1; }
    if (wait) dbus_bus_add_match(c, "type='signal',interface='org.freedesktop.Notifications'", &err);
    DBusMessage *m = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
    DBusMessageIter it, arr, dict; dbus_uint32_t rep = replaces; dbus_int32_t to = timeout; unsigned char urg = (unsigned char)urgency;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &rep);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    for (int k = 0; k < nact; k++) { dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &keys[k]); dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &labels[k]); }
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    hint_int(&dict, "urgency", DBUS_TYPE_BYTE, "y", &urg);
    if (value >= 0) { dbus_int32_t v = value > 100 ? 100 : value; hint_int(&dict, "value", DBUS_TYPE_INT32, "i", &v); }
    if (entry) hint_str(&dict, "desktop-entry", entry);
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &to);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err); dbus_message_unref(m);
    if (!r) { fprintf(stderr, "wmnotify-send: %s (is wmnotify running?)\n", err.message ? err.message : "no reply"); return 1; }
    dbus_uint32_t id = 0; dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID); dbus_message_unref(r);
    if (print) printf("%u\n", id);
    if (wait) {
        time_t end = time(NULL) + 60;
        while (time(NULL) < end) {
            dbus_connection_read_write(c, 500); DBusMessage *s;
            while ((s = dbus_connection_pop_message(c))) {
                dbus_uint32_t sid = 0; const char *key = NULL; dbus_uint32_t reason = 0;
                if (dbus_message_is_signal(s, "org.freedesktop.Notifications", "ActionInvoked") &&
                    dbus_message_get_args(s, NULL, DBUS_TYPE_UINT32, &sid, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID) && sid == id) { puts(key); return 0; }
                if (dbus_message_is_signal(s, "org.freedesktop.Notifications", "NotificationClosed") &&
                    dbus_message_get_args(s, NULL, DBUS_TYPE_UINT32, &sid, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID) && sid == id) { printf("closed:%u\n", reason); return 0; }
                dbus_message_unref(s);
            }
        }
    }
    return 0;
}
