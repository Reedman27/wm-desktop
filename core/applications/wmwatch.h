/* WMWatch - inotify watcher for every applications/ tree WMApps scans.
 * Recursive (nested vendor directories to any depth WMApps scans), picks up new
 * subdirectories and applications/ dirs that appear later, and only reports changes
 * that can affect the registry (*.desktop files, directories, queue overflow). */
#ifndef WMWATCH_H
#define WMWATCH_H

typedef struct WMWatch WMWatch;

/* NULL if inotify is unavailable. 'extra_dir' as for wmapps_scan(). */
WMWatch *wmwatch_open(const char *extra_dir);
int wmwatch_fd(const WMWatch *w);
/* Number of directories currently watched. */
int wmwatch_count(const WMWatch *w);
/* Drain pending events and arm watches for directories created meanwhile.
 * Returns 1 if the registry may have changed. Call when wmwatch_fd() is readable. */
int wmwatch_poll(WMWatch *w);
/* Re-arm without events (data dirs that did not exist at startup, e.g. a freshly
 * installed Flatpak). Returns the number of newly watched directories. */
int wmwatch_rearm(WMWatch *w);
void wmwatch_close(WMWatch *w);

#endif
