#!/usr/bin/env python3
"""Распаковка GOD-контейнера Xbox 360 (SVOD/XSF, раскладка Iso2God) в файлы игры.

Раскладка данных подтверждена исходниками iso2god-rs (src/god/mod.rs) и
эмпирически на дампе Rayman Origins (55530884):

- данные нарезаны блоками по 0x1000 байт (логический ISO-образ);
- каждая часть (DataNNNN) начинается с master-хеш-таблицы (0x1000, записи SHA-1
  по 20 байт) и содержит до 0xCB подчастей;
- подчасть = sub-хеш-таблица (0x1000) + до 0xCC блоков данных;
- логический блок B лежит в части B // 0xA1C4 по смещению
  0x1000 + (B % 0xA1C4) // 0xCC * 0xCD000 + 0x1000 + (B % 0xCC) * 0x1000.

Внутри логического образа — XDVDFS: магия "MICROSOFT*XBOX*MEDIA" по смещению
0x10000, корневой каталог (u32 LE sector, u32 LE size) по 0x10014.

Использование:
  god_extract.py list   <header_file>            — показать дерево файлов
  god_extract.py verify <header>                 — проверить SHA-1 всех блоков
  god_extract.py xtract <header> <out_dir>       — извлечь все файлы
"""

import hashlib
import struct
import sys
from pathlib import Path

BLOCK = 0x1000
BLOCKS_PER_PART = 0xA1C4
BLOCKS_PER_SUBPART = 0xCC
SUBPART_STRIDE = BLOCK + BLOCKS_PER_SUBPART * BLOCK  # sub hash + data
SECTOR = 0x800
GDF_MAGIC_OFFSET = 0x10000
GDF_MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTR_DIRECTORY = 0x10


class GodImage:
    """Доступ к логическому ISO-образу внутри GOD-частей."""

    def __init__(self, header_path):
        header_path = Path(header_path)
        data_dir = Path(str(header_path) + ".data")
        if not data_dir.is_dir():
            raise SystemExit(f"нет каталога частей: {data_dir}")
        self.parts = sorted(data_dir.glob("Data*"))
        if not self.parts:
            raise SystemExit(f"в {data_dir} нет файлов Data*")
        self._handles = {}

    def _handle(self, part_index):
        h = self._handles.get(part_index)
        if h is None:
            h = open(self.parts[part_index], "rb")
            self._handles[part_index] = h
        return h

    def _locate(self, block):
        part = block // BLOCKS_PER_PART
        bip = block % BLOCKS_PER_PART
        sub = bip // BLOCKS_PER_SUBPART
        bis = bip % BLOCKS_PER_SUBPART
        off = BLOCK + sub * SUBPART_STRIDE + BLOCK + bis * BLOCK
        return part, off

    def read(self, offset, size):
        out = bytearray()
        while size > 0:
            block = offset // BLOCK
            within = offset % BLOCK
            bis = (block % BLOCKS_PER_PART) % BLOCKS_PER_SUBPART
            n = min(size, (BLOCKS_PER_SUBPART - bis) * BLOCK - within)
            part, foff = self._locate(block)
            h = self._handle(part)
            h.seek(foff + within)
            chunk = h.read(n)
            if len(chunk) != n:
                raise SystemExit(
                    f"короткое чтение: part {part} off {foff + within:#x}, "
                    f"нужно {n}, получено {len(chunk)}"
                )
            out += chunk
            offset += n
            size -= n
        return bytes(out)

    def close(self):
        for h in self._handles.values():
            h.close()
        self._handles.clear()


def iter_subparts(part_path):
    """yield (sub_hash, data_blocks_bytes) для каждой подчасти файла части."""
    size = part_path.stat().st_size
    with open(part_path, "rb") as f:
        master = f.read(BLOCK)
        yield ("master", master)
        while f.tell() < size:
            sub_hash = f.read(BLOCK)
            if len(sub_hash) < BLOCK:
                raise SystemExit(f"{part_path}: обрезанная sub-хеш-таблица")
            remaining = size - f.tell()
            data_len = min(remaining, BLOCKS_PER_SUBPART * BLOCK)
            yield ("subpart", sub_hash, f, data_len)
            # вызывающий код обязан прочитать ровно data_len байт


def cmd_verify(header_path):
    img = GodImage(header_path)
    total_blocks = 0
    bad = 0
    for part_path in img.parts:
        master_entries = None
        sub_index = 0
        for item in iter_subparts(part_path):
            if item[0] == "master":
                master = item[1]
                master_entries = [
                    master[i : i + 20] for i in range(0, BLOCK, 20)
                ]
                continue
            _, sub_hash, f, data_len = item
            if master_entries[sub_index] != hashlib.sha1(sub_hash).digest():
                print(f"!! {part_path.name}: master-хеш подчасти {sub_index} не сошёлся")
                bad += 1
            nblocks = (data_len + BLOCK - 1) // BLOCK
            for i in range(nblocks):
                start = i * BLOCK
                block_data = f.read(min(BLOCK, data_len - start))
                expect = sub_hash[i * 20 : i * 20 + 20]
                if hashlib.sha1(block_data).digest() != expect:
                    print(
                        f"!! {part_path.name}: блок {total_blocks + i} "
                        f"(подчасть {sub_index}.{i}) — хеш не сошёлся"
                    )
                    bad += 1
            total_blocks += nblocks
            sub_index += 1
        print(f"{part_path.name}: ок ({total_blocks} блоков накопленно)")
    print(f"проверено блоков: {total_blocks}, ошибок: {bad}")
    return 1 if bad else 0


def walk_gdf(img, dir_sector, ordinal, prefix, out):
    """Обход дерева каталогов XDVDFS (как StfsContainerDevice::ReadEntrySVOD)."""
    base = dir_sector * SECTOR + ordinal * 4
    buf = img.read(base, 0xE)
    left, right, sector, length, attrib, namelen = struct.unpack("<HHIIBB", buf)
    name = img.read(base + 0xE, namelen).decode("latin-1")
    if left:
        walk_gdf(img, dir_sector, left, prefix, out)
    if attrib & ATTR_DIRECTORY:
        if length:
            walk_gdf(img, sector, 0, prefix + name + "/", out)
    else:
        out.append((prefix + name, sector, length))
    if right:
        walk_gdf(img, dir_sector, right, prefix, out)


def list_files(img):
    magic = img.read(GDF_MAGIC_OFFSET, 20)
    if magic != GDF_MAGIC:
        raise SystemExit(
            f"магия XDVDFS не найдена по {GDF_MAGIC_OFFSET:#x}: {magic!r}"
        )
    root_sector, root_size = struct.unpack(
        "<II", img.read(GDF_MAGIC_OFFSET + 0x14, 8)
    )
    files = []
    walk_gdf(img, root_sector, 0, "", files)
    return files, root_sector, root_size


def sanitize(name):
    return "".join(c if c not in '\\/:*?"<>|' else "_" for c in name)


def cmd_list(header_path):
    img = GodImage(header_path)
    files, root_sector, root_size = list_files(img)
    print(f"root: sector {root_sector:#x}, size {root_size:#x}")
    total = 0
    for name, sector, length in files:
        print(f"{length:>12}  {name}")
        total += length
    print(f"файлов: {len(files)}, суммарно: {total / 2**30:.2f} ГиБ")
    return 0


def cmd_extract(header_path, out_dir):
    img = GodImage(header_path)
    files, _, _ = list_files(img)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for i, (name, sector, length) in enumerate(files, 1):
        # сохраняем структуру каталогов (иначе localisation/x -> localisation_x)
        parts = [sanitize(p) for p in name.replace("\\", "/").split("/") if p]
        dest = out_dir.joinpath(*parts)
        dest.parent.mkdir(parents=True, exist_ok=True)
        data = img.read(sector * SECTOR, length)
        dest.write_bytes(data)
        print(f"[{i}/{len(files)}] {name} ({length} байт)")
    print(f"готово: {len(files)} файлов -> {out_dir}")
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "list":
        return cmd_list(argv[2])
    if cmd == "verify":
        return cmd_verify(argv[2])
    if cmd == "xtract":
        if len(argv) < 4:
            print("нужен каталог назначения")
            return 2
        return cmd_extract(argv[2], argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
