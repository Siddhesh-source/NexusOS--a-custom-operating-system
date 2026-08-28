from __future__ import annotations
import io
import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path
from typing import Dict, List, Tuple

SECTOR = 512
ESP_START_LBA = 2048
CLUSTER_SECTORS = 2
FAT_COUNT = 2
RESERVED_SECTORS = 32


def lba_to_chs(lba: int) -> Tuple[int, int, int]:
    if lba >= 16 * 63 * 1024:
        return (0xFF, 0xFF, 0xFF)
    cyl, rem = divmod(lba, 255 * 63)
    head, rem = divmod(rem, 63)
    sect = rem + 1
    return (head & 0xFF, (sect & 0x3F) | ((cyl >> 2) & 0xC0), cyl & 0xFF)


def write_mbr(buf: io.BytesIO, esp_start: int, esp_size: int) -> None:
    mbr = bytearray(SECTOR)
    sc = lba_to_chs(esp_start)
    ec = lba_to_chs(esp_start + esp_size - 1)
    struct.pack_into("<BBBBBBBBII", mbr, 446,
                     0x80, sc[0], sc[1], sc[2], 0x0C,
                     ec[0], ec[1], ec[2], esp_start, esp_size)
    mbr[510:512] = b"\x55\xAA"
    buf.seek(0)
    buf.write(mbr)


class Fat32:
    def __init__(self, buf: io.BytesIO, part_offset: int, part_sectors: int):
        self.buf = buf
        self.part_offset = part_offset
        self.part_sectors = part_sectors
        self.cluster_sectors = CLUSTER_SECTORS
        self.cluster_bytes = self.cluster_sectors * SECTOR
        data_sectors = part_sectors - RESERVED_SECTORS
        fat_sectors = 1
        while True:
            data_after = data_sectors - FAT_COUNT * fat_sectors
            clusters = data_after // self.cluster_sectors
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        self.fat_sectors = fat_sectors
        self.cluster_count = (data_sectors - FAT_COUNT * fat_sectors) // self.cluster_sectors
        self.fat_start = RESERVED_SECTORS
        self.data_start = RESERVED_SECTORS + FAT_COUNT * fat_sectors
        self.next_free = 3
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.fat[2] = 0x0FFFFFFF

    def _write_sector(self, lba: int, data: bytes) -> None:
        self.buf.seek(lba * SECTOR)
        self.buf.write(data)

    def _cluster_lba(self, cluster: int) -> int:
        return self.data_start + (cluster - 2) * self.cluster_sectors

    def alloc(self) -> int:
        c = self.next_free
        if c > self.cluster_count + 1:
            raise RuntimeError("out of clusters")
        self.next_free += 1
        return c

    def write_file(self, data: bytes) -> int:
        if not data:
            return 0
        n = (len(data) + self.cluster_bytes - 1) // self.cluster_bytes
        clusters = [self.alloc() for _ in range(n)]
        for i, c in enumerate(clusters):
            self.fat[c] = 0x0FFFFFFF if i == n - 1 else clusters[i + 1]
        for i, c in enumerate(clusters):
            chunk = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            chunk = chunk.ljust(self.cluster_bytes, b"\x00")
            lba = self._cluster_lba(c) + self.part_offset
            for s in range(self.cluster_sectors):
                self._write_sector(lba + s, chunk[s * SECTOR:(s + 1) * SECTOR])
        return clusters[0]

    @staticmethod
    def short_name(base: str, ext: str) -> bytes:
        b = base.upper()[:8].ljust(8)
        e = ext.upper()[:3].ljust(3)
        return (b + e).encode("ascii")

    @staticmethod
    def lfn_checksum(short: bytes) -> int:
        s = 0
        for b in short:
            s = (((s & 1) << 7) | (s >> 1)) + b
            s &= 0xFF
        return s

    def make_lfn(self, long_name: str, short: bytes) -> bytes:
        chk = self.lfn_checksum(short)
        chars = list(long_name) + ["\x00"]
        while len(chars) % 13 != 0:
            chars.append("\uFFFF")
        groups = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        out = bytearray()
        for idx, group in enumerate(reversed(groups)):
            seq = len(groups) - idx
            seq_byte = (0x40 | seq) if seq == len(groups) else seq
            entry = bytearray(32)
            entry[0] = seq_byte
            entry[11] = 0x0F
            entry[13] = chk
            gi = 0
            for start, count in ((1, 5), (14, 6), (28, 2)):
                for _ in range(count):
                    ch = group[gi] if gi < len(group) else "\uFFFF"
                    struct.pack_into("<H", entry, start, ord(ch) & 0xFFFF)
                    gi += 1
                    start += 2
            out += entry
        return bytes(out)

    def make_entry(self, short_11: bytes, cluster: int, size: int,
                   is_dir: bool = False) -> bytes:
        e = bytearray(32)
        e[0:11] = short_11
        e[11] = 0x10 if is_dir else 0x20
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def make_dot(self, name: bytes, cluster: int) -> bytes:
        e = bytearray(32)
        e[0:11] = name.ljust(11)
        e[11] = 0x10
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        return bytes(e)

    def finalize(self) -> None:
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.cluster_sectors
        struct.pack_into("<H", bs, 14, RESERVED_SECTORS)
        bs[16] = FAT_COUNT
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 24, 63)
        bs[26:28] = b"\xFF\x00"
        struct.pack_into("<I", bs, 28, self.part_offset)
        struct.pack_into("<I", bs, 32, self.part_sectors)
        struct.pack_into("<I", bs, 36, self.fat_sectors)
        struct.pack_into("<I", bs, 44, 2)
        struct.pack_into("<H", bs, 48, 1)
        struct.pack_into("<H", bs, 50, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        bs[67:71] = os.urandom(4)
        bs[71:82] = b"NEXOS ESP ".ljust(11)
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"

        fsi = bytearray(SECTOR)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<I", fsi, 484, 0x61417272)
        struct.pack_into("<I", fsi, 488, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 492, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 508, 0xAA550000)
        fsi[510:512] = b"\xAA\x55"

        for rel, data in [(0, bs), (1, fsi), (6, bs), (7, fsi)]:
            self._write_sector(self.part_offset + rel, data)

        fat_data = bytearray()
        for v in self.fat:
            fat_data += struct.pack("<I", v & 0x0FFFFFFF)
        fat_data += b"\x00" * (self.fat_sectors * SECTOR - len(fat_data))
        for i in range(FAT_COUNT):
            base = self.part_offset + self.fat_start + i * self.fat_sectors
            for s in range(self.fat_sectors):
                self._write_sector(base + s,
                                   fat_data[s * SECTOR:(s + 1) * SECTOR])


def write_dir_data(fat: Fat32, first: int, data: bytes) -> None:
    if first == 0:
        return
    clusters, c = [], first
    while True:
        clusters.append(c)
        nxt = fat.fat[c] & 0x0FFFFFFF
        if nxt >= 0x0FFFFFF8:
            break
        c = nxt
    for i, c in enumerate(clusters):
        chunk = data[i * fat.cluster_bytes:(i + 1) * fat.cluster_bytes]
        if not chunk:
            break
        chunk = chunk.ljust(fat.cluster_bytes, b"\x00")
        lba = fat._cluster_lba(c) + fat.part_offset
        for s in range(fat.cluster_sectors):
            fat._write_sector(lba + s, chunk[s * SECTOR:(s + 1) * SECTOR])


class Node:
    def __init__(self, name: str):
        self.name = name
        self.long_name = name
        self.short_base = name.upper()[:8]
        self.short_ext = ""
        self.cluster = 0
        self.entries: List[bytes] = []
        self.children: Dict[str, "Node"] = {}


def build_tree(fat: Fat32,
               files: List[Tuple[str, str, str, bytes, bool]]) -> bytes:
    root = Node("")

    def add_node(path, short_base, short_ext, data):
        parts = path.split("/")
        cur = root
        for part in parts[:-1]:
            if part not in cur.children:
                child = Node(part)
                child.long_name = part
                cur.children[part] = child
            cur = cur.children[part]
        leaf = parts[-1]
        cluster = fat.write_file(data)
        short = fat.short_name(short_base, short_ext)
        need_lfn = leaf.upper() != (short_base.upper() + (("." + short_ext) if short_ext.strip() else ""))
        lfn_bytes = fat.make_lfn(leaf, short) if need_lfn else b""
        entry = fat.make_entry(short, cluster, len(data))
        cur.entries.append(lfn_bytes + entry)

    for path, sb, se, data, _ in files:
        add_node(path, sb, se, data)

    def assign(node: Node, cluster: int, parent: int, is_root: bool) -> bytes:
        node.cluster = cluster
        body = bytearray()
        if not is_root:
            body += fat.make_dot(b".", cluster)
            body += fat.make_dot(b"..", parent)
        for e in node.entries:
            body += e
        for child in node.children.values():
            cc = fat.alloc()
            fat.fat[cc] = 0x0FFFFFFF
            need_lfn = child.long_name.upper() != child.short_base
            lfn = fat.make_lfn(child.long_name, fat.short_name(child.short_base, "")) if need_lfn else b""
            entry = fat.make_entry(fat.short_name(child.short_base, ""), cc, 0, is_dir=True)
            body += lfn + entry
            write_dir_data(fat, cc, assign(child, cc, cluster, False))
        return bytes(body)

    return assign(root, 2, 0, True)


def build_nexus_image(image_path: Path, kernel_elf: Path, limine_dir: Path,
                      limine_exe: Path, boot_dir: Path, size_mb: int) -> None:
    disk_size = size_mb * 1024 * 1024
    esp_size = (disk_size // SECTOR) - ESP_START_LBA
    esp_size -= esp_size % CLUSTER_SECTORS

    image_path.parent.mkdir(parents=True, exist_ok=True)
    with open(image_path, "wb") as f:
        f.truncate(disk_size)

    with open(image_path, "r+b") as f:
        buf = io.BytesIO(f.read())
        write_mbr(buf, ESP_START_LBA, esp_size)
        fat = Fat32(buf, ESP_START_LBA, esp_size)

        files: List[Tuple[str, str, str, bytes, bool]] = []
        files.append(("EFI/BOOT/BOOTX64.EFI", "BOOTX64", "EFI",
                      (limine_dir / "BOOTX64.EFI").read_bytes(), False))
        if (limine_dir / "BOOTIA32.EFI").exists():
            files.append(("EFI/BOOT/BOOTIA32.EFI", "BOOTIA32", "EFI",
                          (limine_dir / "BOOTIA32.EFI").read_bytes(), False))
        files.append(("limine-bios.sys", "LIMINEB", "SYS",
                      (limine_dir / "limine-bios.sys").read_bytes(), False))
        conf = (boot_dir / "limine.conf").read_bytes()
        files.append(("limine.conf", "LIMINEC", "CNF", conf, False))
        files.append(("boot/limine/limine.conf", "LIMINEC", "CNF", conf, False))
        files.append(("EFI/BOOT/limine.conf", "LIMINEC", "CNF", conf, False))
        files.append(("startup.nsh", "STARTUP", "NSH",
                      b"fs0:\r\n\\EFI\\BOOT\\BOOTX64.EFI\r\n", False))
        files.append(("boot/nexus/kernel", "KERNEL", "   ",
                      kernel_elf.read_bytes(), False))

        root_body = build_tree(fat, files)
        write_dir_data(fat, 2, root_body)
        fat.finalize()
        f.seek(0)
        f.write(buf.getvalue())

    r = subprocess.run([str(limine_exe), "bios-install", str(image_path)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout)
        raise RuntimeError(f"limine bios-install failed (exit {r.returncode})")


if __name__ == "__main__":
    raise SystemExit("use build.py, not fat32.py")
