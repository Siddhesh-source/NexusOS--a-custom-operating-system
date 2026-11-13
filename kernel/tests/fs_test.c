/* Phase 10 self-tests: files in memory (ramfs) and the boot bundle. */

#include "fs_test.h"
#include "selftest.h"
#include "fs.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define CHECK(c, d) st_check((c), (d))

static bool resolves(const char *cwd, const char *path, const char *want)
{
    char out[FS_PATH_MAX];
    return fs_path_resolve(cwd, path, out, sizeof(out)) == FS_OK && strcmp(out, want) == 0;
}

static void test_paths(void)
{
    st_section("Paths");
    CHECK(resolves("/", "a/b", "/a/b") && resolves("/home/asha", "notes.txt", "/home/asha/notes.txt"),
          "relative paths join the current folder");
    CHECK(resolves("/home/asha", "/etc//motd", "/etc/motd") && resolves("/x", "", "/x"),
          "absolute paths ignore it; repeated '/' collapse; empty means here");
    CHECK(resolves("/home/asha", "..", "/home") && resolves("/home", "../../..", "/")
          && resolves("/a/b", "./c/../d/.", "/a/b/d"), "'.' and '..' are applied; '..' stops at the root");
    char small[8];
    CHECK(fs_path_resolve("/", "abcdefghij", small, sizeof(small)) == FS_ERR_NAME_TOO_LONG
          && fs_path_resolve("rel", "x", small, sizeof(small)) == FS_ERR_INVALID
          && fs_path_resolve(NULL, "x", small, sizeof(small)) == FS_ERR_INVALID,
          "too long for the buffer, relative cwd and missing arguments are refused");
}

static void test_files(void)
{
    st_section("Create, write, read, list, delete");
    CHECK(fs_mkdir("/t") == FS_OK && fs_mkdir("/t") == FS_ERR_EXISTS, "make a folder; again is refused");
    CHECK(fs_create("/t/a.txt") == FS_OK && fs_create("/t/a.txt") == FS_ERR_EXISTS, "create an empty file");

    int fd;
    size_t n;
    CHECK(fs_open("/t/a.txt", FS_O_WRITE, &fd) == FS_OK && fs_write(fd, "hello ", 6, &n) == FS_OK
          && n == 6 && fs_write(fd, "world", 5, &n) == FS_OK && fs_close(fd) == FS_OK,
          "open for writing, write twice, close");
    char buf[32];
    memset(buf, 0, sizeof(buf));
    CHECK(fs_read_file("/t/a.txt", buf, sizeof(buf), &n) == FS_OK && n == 11
          && memcmp(buf, "hello world", 11) == 0, "read it back: \"hello world\"");

    CHECK(fs_open("/t/a.txt", FS_O_READ, &fd) == FS_OK && fs_seek(fd, 6) == FS_OK
          && fs_read(fd, buf, 3, &n) == FS_OK && n == 3 && memcmp(buf, "wor", 3) == 0
          && fs_read(fd, buf, 32, &n) == FS_OK && n == 2 && fs_read(fd, buf, 32, &n) == FS_OK && n == 0,
          "seek, read in pieces, then 0 bytes at the end");
    CHECK(fs_write(fd, "x", 1, &n) == FS_ERR_NO_ACCESS, "writing a file opened only for reading is refused");
    CHECK(fs_unlink("/t/a.txt") == FS_ERR_BUSY, "an open file cannot be deleted");
    fs_close(fd);
    CHECK(fs_close(fd) == FS_ERR_BAD_FD && fs_read(99, buf, 1, &n) == FS_ERR_BAD_FD
          && fs_close(-1) == FS_ERR_BAD_FD, "closed, out-of-range and negative file numbers are refused");

    CHECK(fs_write_file("/t/a.txt", "!", 1, true) == FS_OK
          && fs_read_file("/t/a.txt", buf, sizeof(buf), &n) == FS_OK && n == 12 && buf[11] == '!',
          "append adds to the end");
    CHECK(fs_write_file("/t/a.txt", "new", 3, false) == FS_OK
          && fs_read_file("/t/a.txt", buf, sizeof(buf), &n) == FS_OK && n == 3,
          "write without append replaces the contents");

    fs_create("/t/c.txt");
    fs_mkdir("/t/b");
    struct fs_dirent e0, e1, e2, e3;
    CHECK(fs_list("/t", 0, &e0) == FS_OK && fs_list("/t", 1, &e1) == FS_OK && fs_list("/t", 2, &e2) == FS_OK
          && fs_list("/t", 3, &e3) == FS_ERR_NOT_FOUND && strcmp(e0.name, "a.txt") == 0
          && strcmp(e1.name, "b") == 0 && e1.type == FS_DIR && strcmp(e2.name, "c.txt") == 0
          && e0.size == 3, "list gives entries in name order with type and size, then the end");

    struct fs_stat st;
    CHECK(fs_stat("/t", &st) == FS_OK && st.type == FS_DIR && st.size == 3
          && fs_stat("/t/a.txt", &st) == FS_OK && st.type == FS_FILE && st.size == 3,
          "stat gives type and size");
    CHECK(fs_unlink("/t") == FS_ERR_NOT_EMPTY, "a folder with things in it cannot be deleted");
    CHECK(fs_unlink("/t/a.txt") == FS_OK && fs_unlink("/t/b") == FS_OK && fs_unlink("/t/c.txt") == FS_OK
          && fs_unlink("/t") == FS_OK && fs_stat("/t", &st) == FS_ERR_NOT_FOUND,
          "delete the files, then the empty folder");
    CHECK(fs_check(), "the tree is consistent");
}

static void test_bad_input(void)
{
    st_section("Bad paths and wrong types");
    int fd;
    struct fs_stat st;
    CHECK(fs_create("relative") == FS_ERR_INVALID && fs_create(NULL) == FS_ERR_INVALID
          && fs_stat("", &st) == FS_ERR_INVALID, "relative, missing and empty paths are refused");
    CHECK(fs_create("/a/../b") == FS_ERR_INVALID && fs_mkdir("/.") == FS_ERR_INVALID,
          "'.' and '..' must be resolved first (fs_path_resolve)");
    CHECK(fs_create("/no/such/file") == FS_ERR_NOT_FOUND && fs_open("/nothing", FS_O_READ, &fd) == FS_ERR_NOT_FOUND,
          "missing folders and files are reported");
    char longname[FS_NAME_MAX + 4];
    longname[0] = '/';
    memset(longname + 1, 'n', FS_NAME_MAX);
    longname[FS_NAME_MAX + 1] = 0;
    CHECK(fs_create(longname) == FS_ERR_NAME_TOO_LONG, "a 32-character name is too long");
    char longpath[FS_PATH_MAX + 8];
    for (unsigned i = 0; i < FS_PATH_MAX + 4; i += 2) {
        longpath[i] = '/';
        longpath[i + 1] = 'd';
    }
    longpath[FS_PATH_MAX + 4] = 0;
    CHECK(fs_stat(longpath, &st) == FS_ERR_NAME_TOO_LONG, "a path of 260 characters is too long");
    CHECK(fs_create("/bad\x01name") == FS_ERR_INVALID, "control characters in names are refused");

    fs_mkdir("/d");
    fs_create("/f");
    CHECK(fs_open("/d", FS_O_READ, &fd) == FS_ERR_IS_DIR && fs_list("/f", 0, NULL) == FS_ERR_INVALID
          && fs_create("/f/x") == FS_ERR_NOT_DIR, "a folder is not a file and a file is not a folder");
    struct fs_dirent e;
    CHECK(fs_list("/f", 0, &e) == FS_ERR_NOT_DIR, "listing a file is refused");
    CHECK(fs_open("/f", 0, &fd) == FS_ERR_INVALID && fs_open("/f", FS_O_READ | FS_O_TRUNC, &fd) == FS_ERR_INVALID
          && fs_open("/f", FS_O_READ | 0x80, &fd) == FS_ERR_INVALID, "bad open flags are refused");
    CHECK(fs_unlink("/") == FS_ERR_BUSY && fs_mkdir("/") == FS_ERR_EXISTS, "the root cannot be deleted or made");
    fs_unlink("/d");
    fs_unlink("/f");
    CHECK(fs_check(), "nothing was changed by refused calls");
}

static void test_limits(void)
{
    st_section("Limits");
    struct fs_usage u0, u1;
    fs_get_usage(&u0);

    /* The biggest allowed file, then one byte more. */
    uint8_t *big = kmalloc(FS_FILE_MAX + 1);
    bool made = big != NULL;
    if (made)
        memset(big, 'z', FS_FILE_MAX + 1);
    CHECK(made && fs_write_file("/big", big, FS_FILE_MAX, false) == FS_OK, "a file of exactly 256 KiB is allowed");
    CHECK(made && fs_write_file("/big", "!", 1, true) == FS_ERR_TOO_BIG, "one more byte is refused (too big)");
    CHECK(made && fs_write_file("/big2", big, FS_FILE_MAX + 1, false) == FS_ERR_TOO_BIG,
          "writing 256 KiB + 1 at once is refused");
    struct fs_stat st;
    CHECK(fs_stat("/big", &st) == FS_OK && st.size == FS_FILE_MAX, "the refused append changed nothing");
    fs_unlink("/big");
    fs_unlink("/big2");

    /* Fill the byte limit with full-size files: the one that does not fit
     * is refused and nothing is half-written. */
    unsigned stored = 0;
    fs_status_t last = FS_OK;
    char name[16];
    fs_mkdir("/fill");
    for (unsigned i = 0; made && i < FS_TOTAL_MAX / FS_FILE_MAX + 2 && last == FS_OK; i++) {
        ksnprintf(name, sizeof(name), "/fill/%u", i);
        last = fs_write_file(name, big, FS_FILE_MAX, false);
        stored += last == FS_OK;
    }
    fs_get_usage(&u1);
    CHECK(last == FS_ERR_NO_SPACE && u1.bytes <= FS_TOTAL_MAX && stored >= 1,
          "files fill memory up to the 8 MiB limit, then 'no space'");
    CHECK(fs_check(), "the tree is consistent when full");
    for (unsigned i = 0; i < stored + 1; i++) {
        ksnprintf(name, sizeof(name), "/fill/%u", i);
        fs_unlink(name);
    }
    fs_unlink("/fill");
    kfree(big);

    /* The node limit. */
    unsigned nodes = 0;
    fs_mkdir("/many");
    for (unsigned i = 0; i < FS_MAX_NODES + 1; i++) {
        ksnprintf(name, sizeof(name), "/many/%u", i);
        if (fs_create(name) != FS_OK)
            break;
        nodes++;
    }
    ksnprintf(name, sizeof(name), "/many/%u", nodes);
    CHECK(nodes == FS_MAX_NODES - u0.nodes - 1 && fs_create(name) == FS_ERR_NO_SPACE,
          "files can be made up to the 1024-node limit, then 'no space'");
    for (unsigned i = 0; i < nodes; i++) {
        ksnprintf(name, sizeof(name), "/many/%u", i);
        fs_unlink(name);
    }
    fs_unlink("/many");

    int fds[FS_MAX_OPEN + 1];
    unsigned opened = 0;
    fs_create("/o");
    for (unsigned i = 0; i < FS_MAX_OPEN + 1; i++)
        if (fs_open("/o", FS_O_READ, &fds[opened]) == FS_OK)
            opened++;
    CHECK(opened == FS_MAX_OPEN - u0.open_files, "at most 32 files are open at once");
    for (unsigned i = 0; i < opened; i++)
        fs_close(fds[i]);
    fs_unlink("/o");

    fs_get_usage(&u1);
    CHECK(u1.nodes == u0.nodes && u1.bytes == u0.bytes && u1.open_files == u0.open_files && fs_check(),
          "after cleaning up, usage is back where it started");
}

/* A tiny tar archive made here: one folder and one file. */
static void put_octal(char *dst, size_t width, uint64_t v)
{
    dst[width - 1] = '\0';
    for (size_t i = width - 1; i-- > 0;) {
        dst[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

static void tar_header(char *h, const char *name, char type, uint64_t size)
{
    memset(h, 0, 512);
    strlcpy(h, name, 100);
    put_octal(h + 100, 8, 0644);
    put_octal(h + 124, 12, size);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    uint64_t sum = 0;
    memset(h + 148, ' ', 8);
    for (unsigned i = 0; i < 512; i++)
        sum += (uint8_t)h[i];
    put_octal(h + 148, 7, sum);
}

static void test_tar(bool bundle_loaded)
{
    st_section("Boot bundle (tar)");
    char *tar = kzalloc(512 * 5);
    bool ok = tar != NULL;
    if (ok) {
        tar_header(tar, "tt/", '5', 0);
        tar_header(tar + 512, "tt/sub/x.txt", '0', 3);
        memcpy(tar + 1024, "abc", 3);
    }
    char buf[8];
    size_t n;
    CHECK(ok && fs_load_tar(tar, 512 * 5) == 1 && fs_read_file("/tt/sub/x.txt", buf, sizeof(buf), &n) == FS_OK
          && n == 3 && memcmp(buf, "abc", 3) == 0, "a folder and a file (in a folder not listed) are created");
    if (ok)
        tar[512 + 148] ^= 1;               /* break the second header's checksum */
    CHECK(ok && fs_load_tar(tar, 512 * 5) == -1, "a damaged header is detected");
    CHECK(ok && fs_load_tar(tar, 600) == -1, "a cut-off archive is detected");
    fs_unlink("/tt/sub/x.txt");
    fs_unlink("/tt/sub");
    fs_unlink("/tt");
    kfree(tar);

    CHECK(bundle_loaded, "the boot bundle was loaded");
    struct fs_stat st;
    CHECK(fs_stat("/etc/motd", &st) == FS_OK && st.type == FS_FILE && st.size > 0
          && fs_stat("/etc/users", &st) == FS_OK && fs_stat("/home", &st) == FS_OK && st.type == FS_DIR,
          "/etc/motd, /etc/users and /home came from it");
    char motd[16];
    CHECK(fs_read_file("/etc/motd", motd, 15, &n) == FS_OK && n == 15 && memcmp(motd, "Welcome to", 10) == 0,
          "/etc/motd starts with \"Welcome to\"");
    CHECK(fs_check(), "the tree is consistent");
}

bool fs_run_tests(bool bundle_loaded)
{
    st_begin();
    kprintf("\n[test] ===== Phase 10: files in memory =====\n");
    test_paths();
    test_files();
    test_bad_input();
    test_limits();
    test_tar(bundle_loaded);
    return st_end("Files");
}
