#include "fs.h"
#include "slab.h"
#include "heap.h"
#include "spinlock.h"
#include "clock.h"
#include "kprintf.h"
#include "string.h"
#include "panic.h"

#define NODE_MAGIC 0x52414D46u      /* "RAMF" */

struct node {
    uint32_t magic;
    uint32_t id;
    uint8_t type;                   /* enum fs_type */
    char name[FS_NAME_MAX];
    struct node *parent;
    struct node *child;             /* first child, children sorted by name */
    struct node *next;              /* next sibling */
    uint32_t nchildren;
    uint32_t open_count;
    uint64_t size, cap;
    uint8_t *data;
    uint64_t changed_ns;
};

struct open_file {
    struct node *node;
    uint64_t pos;
    uint32_t flags;
};

static struct spinlock fs_lock = SPINLOCK_INIT(LOCK_CLASS_FS);
static struct slab_cache *node_cache;
static struct node *root;
static struct open_file files[FS_MAX_OPEN];
static uint32_t nnodes, next_id = 1, nopen;
static uint64_t total_bytes;

static uint64_t now_ns(void)
{
    return clock_ready() ? clock_now_ns() : 0;
}

/* ---- nodes ---------------------------------------------------------------- */

static struct node *node_new(uint8_t type, const char *name, fs_status_t *err)
{
    if (nnodes >= FS_MAX_NODES) {
        *err = FS_ERR_NO_SPACE;
        return NULL;
    }
    struct node *n = slab_zalloc(node_cache);
    if (n == NULL) {
        *err = FS_ERR_NO_SPACE;
        return NULL;
    }
    n->magic = NODE_MAGIC;
    n->id = next_id++;
    n->type = type;
    strlcpy(n->name, name, FS_NAME_MAX);
    n->changed_ns = now_ns();
    nnodes++;
    return n;
}

static void node_free(struct node *n)
{
    total_bytes -= n->size;
    if (n->data != NULL)
        kfree(n->data);
    n->magic = 0;
    nnodes--;
    slab_free(node_cache, n);
}

/* Insert keeping the children sorted by name. */
static void link_child(struct node *dir, struct node *n)
{
    struct node **pp = &dir->child;
    while (*pp != NULL && strcmp((*pp)->name, n->name) < 0)
        pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    n->parent = dir;
    dir->nchildren++;
    dir->changed_ns = now_ns();
}

static void unlink_child(struct node *n)
{
    struct node *dir = n->parent;
    struct node **pp = &dir->child;
    while (*pp != n)
        pp = &(*pp)->next;
    *pp = n->next;
    dir->nchildren--;
    dir->changed_ns = now_ns();
}

static struct node *find_child(struct node *dir, const char *name, size_t len)
{
    for (struct node *c = dir->child; c != NULL; c = c->next)
        if (strncmp(c->name, name, len) == 0 && c->name[len] == '\0')
            return c;
    return NULL;
}

/* ---- paths ---------------------------------------------------------------- */

static bool name_ok(const char *s, size_t len)
{
    if (len == 0 || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return false;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == '/' || s[i] == 0x7F)
            return false;
    return true;
}

static fs_status_t check_path(const char *path)
{
    if (path == NULL || path[0] != '/')
        return FS_ERR_INVALID;
    size_t len = strlen(path);
    if (len >= FS_PATH_MAX)
        return FS_ERR_NAME_TOO_LONG;
    /* "." and ".." belong to fs_path_resolve; refuse them anywhere. */
    for (size_t i = 0; i < len; i++) {
        if (path[i] != '.' || (i > 0 && path[i - 1] != '/'))
            continue;
        size_t j = i + 1 + (path[i + 1] == '.');
        if (path[j] == '/' || path[j] == '\0')
            return FS_ERR_INVALID;
    }
    return FS_OK;
}

/* Walk an absolute path. With `parent_out`, stop before the last part and
 * return the folder holding it plus its name (which need not exist). */
static fs_status_t walk(const char *path, struct node **out, struct node **parent_out,
                        const char **last, size_t *last_len)
{
    fs_status_t st = check_path(path);
    if (st != FS_OK)
        return st;
    struct node *cur = root;
    const char *p = path;
    for (;;) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        const char *rest = p;
        while (*rest == '/')
            rest++;
        bool is_last = *rest == '\0';
        if (len >= FS_NAME_MAX)
            return FS_ERR_NAME_TOO_LONG;
        if (!name_ok(start, len))
            return FS_ERR_INVALID;          /* "." and ".." belong to fs_path_resolve */
        if (cur->type != FS_DIR)
            return FS_ERR_NOT_DIR;
        if (is_last && parent_out != NULL) {
            *parent_out = cur;
            *last = start;
            *last_len = len;
            return FS_OK;
        }
        struct node *next = find_child(cur, start, len);
        if (next == NULL)
            return FS_ERR_NOT_FOUND;
        cur = next;
    }
    if (parent_out != NULL)
        return FS_ERR_BUSY;                 /* the path is the root itself */
    *out = cur;
    return FS_OK;
}

static fs_status_t lookup(const char *path, struct node **out)
{
    return walk(path, out, NULL, NULL, NULL);
}

fs_status_t fs_path_resolve(const char *cwd, const char *path, char *out, size_t out_size)
{
    if (cwd == NULL || path == NULL || out == NULL || out_size < 2 || cwd[0] != '/')
        return FS_ERR_INVALID;
    size_t len = 1;
    out[0] = '/';
    out[1] = '\0';
    for (int pass = 0; pass < 2; pass++) {
        const char *p = pass == 0 ? (path[0] == '/' ? "" : cwd) : path;
        for (;;) {
            while (*p == '/')
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && *p != '/')
                p++;
            size_t n = (size_t)(p - start);
            if (n == 1 && start[0] == '.')
                continue;
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                while (len > 1 && out[len - 1] != '/')
                    len--;
                if (len > 1)
                    len--;                  /* drop the '/' too, unless root */
                out[len] = '\0';
                continue;
            }
            size_t need = len + (len > 1) + n;
            if (need + 1 > out_size || need + 1 > FS_PATH_MAX)
                return FS_ERR_NAME_TOO_LONG;
            if (len > 1)
                out[len++] = '/';
            memcpy(out + len, start, n);
            len += n;
            out[len] = '\0';
        }
    }
    return FS_OK;
}

/* ---- file data ------------------------------------------------------------ */

/* Make room for `size` bytes. */
static fs_status_t reserve(struct node *n, uint64_t size)
{
    if (size > FS_FILE_MAX)
        return FS_ERR_TOO_BIG;
    if (size > n->size && total_bytes + (size - n->size) > FS_TOTAL_MAX)
        return FS_ERR_NO_SPACE;
    if (size <= n->cap)
        return FS_OK;
    uint64_t cap = n->cap < 64 ? 64 : n->cap;
    while (cap < size)
        cap *= 2;
    if (cap > FS_FILE_MAX)
        cap = FS_FILE_MAX;
    uint8_t *data = kmalloc(cap);
    if (data == NULL)
        return FS_ERR_NO_SPACE;
    if (n->data != NULL) {
        memcpy(data, n->data, n->size);
        kfree(n->data);
    }
    n->data = data;
    n->cap = cap;
    return FS_OK;
}

static fs_status_t write_at(struct node *n, uint64_t pos, const void *buf, size_t len)
{
    if (pos > n->size)
        return FS_ERR_INVALID;
    if (len > FS_FILE_MAX || pos + len > FS_FILE_MAX)
        return FS_ERR_TOO_BIG;
    uint64_t end = pos + len;
    if (end > n->size) {
        fs_status_t st = reserve(n, end);
        if (st != FS_OK)
            return st;
        total_bytes += end - n->size;
        n->size = end;
    }
    if (len > 0)
        memcpy(n->data + pos, buf, len);
    n->changed_ns = now_ns();
    return FS_OK;
}

static void truncate_node(struct node *n)
{
    total_bytes -= n->size;
    n->size = 0;
    n->changed_ns = now_ns();
}

/* ---- create / remove ------------------------------------------------------ */

static fs_status_t create_locked(const char *path, uint8_t type, struct node **out)
{
    struct node *dir;
    const char *name;
    size_t len;
    fs_status_t st = walk(path, NULL, &dir, &name, &len);
    if (st != FS_OK)
        return st == FS_ERR_BUSY ? FS_ERR_EXISTS : st;
    struct node *existing = find_child(dir, name, len);
    if (existing != NULL) {
        if (out != NULL)
            *out = existing;
        return FS_ERR_EXISTS;
    }
    char buf[FS_NAME_MAX];
    memcpy(buf, name, len);
    buf[len] = '\0';
    struct node *n = node_new(type, buf, &st);
    if (n == NULL)
        return st;
    link_child(dir, n);
    if (out != NULL)
        *out = n;
    return FS_OK;
}

void fs_init(void)
{
    node_cache = slab_create("fs node", sizeof(struct node), 8, FS_MAX_NODES + 1);
    if (node_cache == NULL)
        panic("fs: cannot create the node cache");
    SPIN_GUARD(&fs_lock);
    fs_status_t st;
    root = node_new(FS_DIR, "", &st);
    if (root == NULL)
        panic("fs: cannot create the root folder");
}

fs_status_t fs_create(const char *path)
{
    SPIN_GUARD(&fs_lock);
    return create_locked(path, FS_FILE, NULL);
}

fs_status_t fs_mkdir(const char *path)
{
    SPIN_GUARD(&fs_lock);
    return create_locked(path, FS_DIR, NULL);
}

fs_status_t fs_unlink(const char *path)
{
    SPIN_GUARD(&fs_lock);
    struct node *n;
    fs_status_t st = lookup(path, &n);
    if (st != FS_OK)
        return st;
    if (n == root || n->open_count > 0)
        return FS_ERR_BUSY;
    if (n->type == FS_DIR && n->child != NULL)
        return FS_ERR_NOT_EMPTY;
    unlink_child(n);
    node_free(n);
    return FS_OK;
}

fs_status_t fs_stat(const char *path, struct fs_stat *out)
{
    if (out == NULL)
        return FS_ERR_INVALID;
    SPIN_GUARD(&fs_lock);
    struct node *n;
    fs_status_t st = lookup(path, &n);
    if (st != FS_OK)
        return st;
    out->type = n->type;
    out->size = n->type == FS_DIR ? n->nchildren : n->size;
    out->changed_ns = n->changed_ns;
    out->id = n->id;
    return FS_OK;
}

fs_status_t fs_list(const char *path, unsigned index, struct fs_dirent *out)
{
    if (out == NULL)
        return FS_ERR_INVALID;
    SPIN_GUARD(&fs_lock);
    struct node *n;
    fs_status_t st = lookup(path, &n);
    if (st != FS_OK)
        return st;
    if (n->type != FS_DIR)
        return FS_ERR_NOT_DIR;
    struct node *c = n->child;
    for (unsigned i = 0; i < index && c != NULL; i++)
        c = c->next;
    if (c == NULL)
        return FS_ERR_NOT_FOUND;
    strlcpy(out->name, c->name, FS_NAME_MAX);
    out->type = c->type;
    out->size = c->type == FS_DIR ? c->nchildren : c->size;
    return FS_OK;
}

/* ---- open files ----------------------------------------------------------- */

static struct open_file *file_for(int fd)
{
    if (fd < 0 || fd >= FS_MAX_OPEN || files[fd].node == NULL)
        return NULL;
    return &files[fd];
}

fs_status_t fs_open(const char *path, uint32_t flags, int *fd_out)
{
    if (fd_out == NULL || !(flags & (FS_O_READ | FS_O_WRITE))
        || (flags & ~(FS_O_READ | FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC | FS_O_APPEND)))
        return FS_ERR_INVALID;
    if ((flags & (FS_O_TRUNC | FS_O_APPEND | FS_O_CREATE)) && !(flags & FS_O_WRITE))
        return FS_ERR_INVALID;
    SPIN_GUARD(&fs_lock);
    int fd = -1;
    for (int i = 0; i < FS_MAX_OPEN && fd < 0; i++)
        if (files[i].node == NULL)
            fd = i;
    if (fd < 0)
        return FS_ERR_NO_SPACE;

    struct node *n;
    fs_status_t st = lookup(path, &n);
    if (st == FS_ERR_NOT_FOUND && (flags & FS_O_CREATE))
        st = create_locked(path, FS_FILE, &n);
    if (st != FS_OK)
        return st;
    if (n->type != FS_FILE)
        return FS_ERR_IS_DIR;
    if (flags & FS_O_TRUNC)
        truncate_node(n);
    n->open_count++;
    nopen++;
    files[fd] = (struct open_file){ .node = n, .pos = 0, .flags = flags };
    *fd_out = fd;
    return FS_OK;
}

fs_status_t fs_close(int fd)
{
    SPIN_GUARD(&fs_lock);
    struct open_file *f = file_for(fd);
    if (f == NULL)
        return FS_ERR_BAD_FD;
    f->node->open_count--;
    f->node = NULL;
    nopen--;
    return FS_OK;
}

fs_status_t fs_read(int fd, void *buf, size_t n, size_t *done)
{
    if (done != NULL)
        *done = 0;
    if (buf == NULL && n > 0)
        return FS_ERR_INVALID;
    SPIN_GUARD(&fs_lock);
    struct open_file *f = file_for(fd);
    if (f == NULL)
        return FS_ERR_BAD_FD;
    if (!(f->flags & FS_O_READ))
        return FS_ERR_NO_ACCESS;
    uint64_t left = f->node->size > f->pos ? f->node->size - f->pos : 0;
    size_t take = n < left ? n : (size_t)left;
    if (take > 0)
        memcpy(buf, f->node->data + f->pos, take);
    f->pos += take;
    if (done != NULL)
        *done = take;
    return FS_OK;
}

fs_status_t fs_write(int fd, const void *buf, size_t n, size_t *done)
{
    if (done != NULL)
        *done = 0;
    if (buf == NULL && n > 0)
        return FS_ERR_INVALID;
    SPIN_GUARD(&fs_lock);
    struct open_file *f = file_for(fd);
    if (f == NULL)
        return FS_ERR_BAD_FD;
    if (!(f->flags & FS_O_WRITE))
        return FS_ERR_NO_ACCESS;
    if (f->flags & FS_O_APPEND)
        f->pos = f->node->size;
    fs_status_t st = write_at(f->node, f->pos, buf, n);
    if (st != FS_OK)
        return st;
    f->pos += n;
    if (done != NULL)
        *done = n;
    return FS_OK;
}

fs_status_t fs_seek(int fd, uint64_t pos)
{
    SPIN_GUARD(&fs_lock);
    struct open_file *f = file_for(fd);
    if (f == NULL)
        return FS_ERR_BAD_FD;
    f->pos = pos < f->node->size ? pos : f->node->size;
    return FS_OK;
}

/* ---- whole files ---------------------------------------------------------- */

fs_status_t fs_read_file(const char *path, void *buf, size_t cap, size_t *len)
{
    int fd;
    fs_status_t st = fs_open(path, FS_O_READ, &fd);
    if (st != FS_OK)
        return st;
    st = fs_read(fd, buf, cap, len);
    fs_close(fd);
    return st;
}

fs_status_t fs_write_file(const char *path, const void *data, size_t len, bool append)
{
    int fd;
    fs_status_t st = fs_open(path, FS_O_WRITE | FS_O_CREATE | (append ? FS_O_APPEND : FS_O_TRUNC), &fd);
    if (st != FS_OK)
        return st;
    st = fs_write(fd, data, len, NULL);
    fs_close(fd);
    return st;
}

/* ---- the boot bundle (tar) ------------------------------------------------ */

static uint64_t octal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

/* Create every folder on the way to `path` (and `path` itself if `last`). */
static fs_status_t mkdir_all(const char *path, bool last)
{
    char part[FS_PATH_MAX];
    size_t len = strlen(path);
    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        if (i == len && !last)
            break;
        memcpy(part, path, i);
        part[i] = '\0';
        fs_status_t st = fs_mkdir(part);
        if (st != FS_OK && st != FS_ERR_EXISTS)
            return st;
    }
    return FS_OK;
}

int fs_load_tar(const void *data, size_t size)
{
    const uint8_t *base = data;
    int added = 0;
    size_t off = 0;
    while (off + 512 <= size) {
        const char *h = (const char *)base + off;
        if (h[0] == '\0')
            return added;                   /* end-of-archive block */
        /* Header checksum: the sum of all bytes with the checksum field
         * counted as spaces. */
        uint64_t sum = 0;
        for (size_t i = 0; i < 512; i++)
            sum += (i >= 148 && i < 156) ? ' ' : (uint8_t)h[i];
        if (sum != octal(h + 148, 8) || memcmp(h + 257, "ustar", 5) != 0)
            return -1;
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        if (off + 512 + fsize > size)
            return -1;

        char raw[FS_PATH_MAX], path[FS_PATH_MAX];
        size_t n = 0;
        if (h[345] != '\0') {               /* ustar prefix: "prefix/name" */
            for (size_t i = 0; i < 155 && h[345 + i] != '\0' && n + 1 < sizeof(raw); i++)
                raw[n++] = h[345 + i];
            raw[n++] = '/';
        }
        for (size_t i = 0; i < 100 && h[i] != '\0' && n + 1 < sizeof(raw); i++)
            raw[n++] = h[i];
        raw[n] = '\0';
        if (fs_path_resolve("/", raw, path, sizeof(path)) != FS_OK)
            return -1;

        if (type == '5') {
            if (mkdir_all(path, true) != FS_OK)
                return -1;
        } else if (type == '0' || type == '\0') {
            if (mkdir_all(path, false) != FS_OK
                || fs_write_file(path, base + off + 512, (size_t)fsize, false) != FS_OK)
                return -1;
            added++;
        }                                   /* links and others: skipped */
        off += 512 + ((fsize + 511) & ~511ull);
    }
    return off == size ? added : -1;        /* a cut-off header at the end */
}

/* ---- checks and numbers --------------------------------------------------- */

static bool check_node(struct node *n, unsigned depth, uint32_t *count, uint64_t *bytes)
{
    bool ok = true;
    if (n->magic != NODE_MAGIC || depth > FS_PATH_MAX / 2) {
        kprintf("[fs] check: damaged node or tree too deep\n");
        return false;
    }
    (*count)++;
    if (n->type == FS_FILE) {
        *bytes += n->size;
        if (n->size > n->cap || (n->cap > 0 && n->data == NULL) || n->child != NULL) {
            kprintf("[fs] check: file '%s' has bad size/capacity/children\n", n->name);
            ok = false;
        }
        return ok;
    }
    uint32_t kids = 0;
    for (struct node *c = n->child; c != NULL; c = c->next) {
        kids++;
        if (c->parent != n) {
            kprintf("[fs] check: '%s' does not point back to its folder\n", c->name);
            ok = false;
        }
        if (c->next != NULL && strcmp(c->name, c->next->name) >= 0) {
            kprintf("[fs] check: '%s' and '%s' are out of order or repeated\n", c->name, c->next->name);
            ok = false;
        }
        ok &= check_node(c, depth + 1, count, bytes);
    }
    if (kids != n->nchildren) {
        kprintf("[fs] check: folder '%s' counts %u entries, has %u\n", n->name, n->nchildren, kids);
        ok = false;
    }
    return ok;
}

bool fs_check(void)
{
    SPIN_GUARD(&fs_lock);
    uint32_t count = 0;
    uint64_t bytes = 0;
    bool ok = check_node(root, 0, &count, &bytes);
    if (count != nnodes || bytes != total_bytes) {
        kprintf("[fs] check: counted %u nodes / %lu bytes, records say %u / %lu\n",
                count, bytes, nnodes, total_bytes);
        ok = false;
    }
    uint32_t open = 0;
    for (unsigned i = 0; i < FS_MAX_OPEN; i++)
        open += files[i].node != NULL;
    if (open != nopen) {
        kprintf("[fs] check: %u open files, records say %u\n", open, nopen);
        ok = false;
    }
    return ok;
}

void fs_get_usage(struct fs_usage *out)
{
    SPIN_GUARD(&fs_lock);
    out->nodes = nnodes;
    out->max_nodes = FS_MAX_NODES;
    out->bytes = total_bytes;
    out->max_bytes = FS_TOTAL_MAX;
    out->open_files = nopen;
}

const char *fs_status_str(fs_status_t s)
{
    switch (s) {
    case FS_OK:                return "done";
    case FS_ERR_INVALID:       return "that name or path is not allowed";
    case FS_ERR_NOT_FOUND:     return "no such file or folder";
    case FS_ERR_EXISTS:        return "it already exists";
    case FS_ERR_NOT_DIR:       return "that is not a folder";
    case FS_ERR_IS_DIR:        return "that is a folder, not a file";
    case FS_ERR_NOT_EMPTY:     return "the folder is not empty";
    case FS_ERR_NAME_TOO_LONG: return "the name is too long";
    case FS_ERR_TOO_BIG:       return "the file would be too big";
    case FS_ERR_NO_SPACE:      return "no space left for files";
    case FS_ERR_BAD_FD:        return "that file is not open";
    case FS_ERR_BUSY:          return "it is in use";
    case FS_ERR_NO_ACCESS:     return "the file was not opened for that";
    }
    return "unknown error";
}
