#ifndef BHARAT_FS_H
#define BHARAT_FS_H

/* Files in memory (ramfs): the V1 file system.
 *
 * A tree of folders and files kept in kernel memory: each folder or file
 * is a node from a slab cache; a file's bytes live in one heap block that
 * grows as it is written. Nothing is saved to disk: the tree is rebuilt
 * at every boot from a read-only bundle (a tar archive the bootloader
 * loads next to the kernel), so /etc/motd and the sample files are always
 * there. BharatFS (Phase 17) replaces this with files that survive a
 * reboot.
 *
 * Paths are absolute ("/home/asha/notes.txt"). fs_path_resolve() turns a
 * path typed in the shell (relative, with "." and "..") into one.
 *
 * Open files are numbered (0..FS_MAX_OPEN-1), like Unix file descriptors.
 * A file that is open cannot be deleted (FS_ERR_BUSY).
 *
 * Everything is checked: names (1..FS_NAME_MAX-1 characters, no '/'),
 * path length, file size (FS_FILE_MAX), the number of nodes (FS_MAX_NODES)
 * and the total bytes stored (FS_TOTAL_MAX). Hitting a limit returns an
 * error and changes nothing.
 *
 * One spinlock ("file system") guards the whole tree; file data is copied
 * while holding it (files are small).
 *
 * Limits: no owners or permissions (Phase 18); no links; no timestamps
 * beyond "changed at" in nanoseconds since boot. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_NAME_MAX    32          /* including the final 0 */
#define FS_PATH_MAX    256         /* including the final 0 */
#define FS_FILE_MAX    (256 * 1024)
#define FS_TOTAL_MAX   (8 * 1024 * 1024)
#define FS_MAX_NODES   1024
#define FS_MAX_OPEN    32

typedef enum {
    FS_OK = 0,
    FS_ERR_INVALID,       /* bad argument or bad path */
    FS_ERR_NOT_FOUND,     /* no such file or folder */
    FS_ERR_EXISTS,        /* something with that name is already there */
    FS_ERR_NOT_DIR,       /* a folder was expected */
    FS_ERR_IS_DIR,        /* a file was expected */
    FS_ERR_NOT_EMPTY,     /* a folder that still has things in it */
    FS_ERR_NAME_TOO_LONG,
    FS_ERR_TOO_BIG,       /* the file would exceed FS_FILE_MAX */
    FS_ERR_NO_SPACE,      /* node or byte limit reached, or out of memory */
    FS_ERR_BAD_FD,        /* not an open file number */
    FS_ERR_BUSY,          /* the file is open, or the folder is the root */
    FS_ERR_NO_ACCESS,     /* opened without the right (read or write) */
} fs_status_t;

/* fs_open flags */
#define FS_O_READ    (1u << 0)
#define FS_O_WRITE   (1u << 1)
#define FS_O_CREATE  (1u << 2)    /* create the file if it is missing */
#define FS_O_TRUNC   (1u << 3)    /* empty it on open */
#define FS_O_APPEND  (1u << 4)    /* every write goes to the end */

enum fs_type {
    FS_FILE = 1,
    FS_DIR = 2,
};

struct fs_stat {
    uint8_t type;                 /* enum fs_type */
    uint64_t size;                /* bytes (file) or entries (folder) */
    uint64_t changed_ns;
    uint32_t id;                  /* node number, unique while it exists */
};

struct fs_dirent {
    char name[FS_NAME_MAX];
    uint8_t type;
    uint64_t size;
};

struct fs_usage {
    uint32_t nodes, max_nodes;
    uint64_t bytes, max_bytes;
    uint32_t open_files;
};

/* Create the empty root folder. */
void fs_init(void);

/* Fill the tree from a ustar (tar) archive in memory. Folders are created
 * as needed; files already present are replaced. Returns the number of
 * files added, or -1 if the archive is damaged (what was read before the
 * damage stays). */
int fs_load_tar(const void *data, size_t size);

fs_status_t fs_open(const char *path, uint32_t flags, int *fd_out);
fs_status_t fs_close(int fd);
/* Read/write at the file's position and move it on. */
fs_status_t fs_read(int fd, void *buf, size_t n, size_t *done);
fs_status_t fs_write(int fd, const void *buf, size_t n, size_t *done);
/* Move the position (clamped to the file size). */
fs_status_t fs_seek(int fd, uint64_t pos);

fs_status_t fs_create(const char *path);       /* empty file */
fs_status_t fs_mkdir(const char *path);
fs_status_t fs_unlink(const char *path);       /* a file or an empty folder */
fs_status_t fs_stat(const char *path, struct fs_stat *out);

/* Entry number `index` (0, 1, ...) of a folder, in name order; NOT_FOUND
 * past the last one. */
fs_status_t fs_list(const char *path, unsigned index, struct fs_dirent *out);

/* Whole-file helpers: read up to `cap` bytes, or replace the contents. */
fs_status_t fs_read_file(const char *path, void *buf, size_t cap, size_t *len);
fs_status_t fs_write_file(const char *path, const void *data, size_t len, bool append);

/* Make an absolute, clean path from `path` taken relative to the folder
 * `cwd` (itself absolute): "." and ".." and repeated '/' are removed.
 * ".." at the root stays at the root. Pure string work; nothing is looked
 * up. Returns FS_ERR_NAME_TOO_LONG / FS_ERR_INVALID on bad input. */
fs_status_t fs_path_resolve(const char *cwd, const char *path, char *out, size_t out_size);

/* Check the tree: parent links, sorted unique names, sizes and the usage
 * counts. Prints problems; returns true if consistent. */
bool fs_check(void);

void fs_get_usage(struct fs_usage *out);
const char *fs_status_str(fs_status_t s);

#endif /* BHARAT_FS_H */
