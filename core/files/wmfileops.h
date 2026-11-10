/* WMFileOps - the file operations behind wmfiles: no shell, no following symlinks, never overwrites.
 * Every function returns 0 on success or -1 with a readable reason in wmfo_error(). */
#ifndef WMFILEOPS_H
#define WMFILEOPS_H
#include <stddef.h>

const char *wmfo_error(void);
/* "name", or "name (copy)", "name (copy 2)", ... inserted before the extension, whichever is free in dir. */
void wmfo_unique(const char *dir, const char *name, char *out, size_t n);
int wmfo_mkdir(const char *parent, const char *name, char *out, size_t n);     /* out: the created path */
int wmfo_rename(const char *path, const char *newname, char *out, size_t n);   /* same folder; refuses to replace */
int wmfo_copy(const char *src, const char *dstdir, char *out, size_t n);       /* recursive; renames on clash */
int wmfo_move(const char *src, const char *dstdir, char *out, size_t n);       /* rename(2), else copy + delete */
/* Moves to the XDG trash ($XDG_DATA_HOME/Trash or ~/.local/share/Trash) with a .trashinfo so file managers
 * can restore it. Refuses "/", $HOME and anything that contains the trash itself. */
int wmfo_trash(const char *path);
/* Same, and tells where it ended up (inside Trash/files) so the move can be undone with wmfo_restore. */
int wmfo_trash_to(const char *path, char *out, size_t n);
/* Moves a trashed file back to its recorded origin (a free name if taken); removes its .trashinfo. */
int wmfo_restore(const char *trashed, char *out, size_t n);
#endif
