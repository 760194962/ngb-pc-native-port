#!/usr/bin/env python3
"""把路线 A 已核对的 x2.1 解包目录硬链接到本目录 game\\x21（同一 E 盘，不占空间），
并按路线 A 的 docs/manifest_x21.tsv 核对：每个文件存在、大小一致，XBE 与 load_img.bin 复算 SHA-1。只读源目录。

用法：python -I tools/link_game.py
"""
import csv
import hashlib
import os

SRC = r"E:\games\emu\ninja gaiden black hurricane recomp\input\x21"
DST = r"E:\games\emu\ninja gaiden black Hurricane recomp2\game\x21"
MANIFEST = r"E:\games\emu\ninja gaiden black hurricane recomp\docs\manifest_x21.tsv"


def main():
    n = 0
    for root, _dirs, files in os.walk(SRC):
        rel = os.path.relpath(root, SRC)
        out = DST if rel == '.' else os.path.join(DST, rel)
        os.makedirs(out, exist_ok=True)
        for f in files:
            s = os.path.join(root, f)
            d = os.path.join(out, f)
            if not os.path.exists(d):
                os.link(s, d)
            n += 1
    print('linked files:', n)
    rows = [r for r in csv.reader(open(MANIFEST, encoding='utf-8'), delimiter='\t') if r]
    bad = checked = 0
    for r in rows:
        p = os.path.join(DST, *r[0].replace('/', os.sep).split(os.sep))
        if not os.path.exists(p):
            print('MISSING', r[0]); bad += 1; continue
        if os.path.getsize(p) != int(r[1]):
            print('SIZE', r[0]); bad += 1; continue
        if r[0].lower().endswith('.xbe') or r[0].lower() == 'load_img.bin':
            h = hashlib.sha1(open(p, 'rb').read()).hexdigest()
            if h != r[2]:
                print('SHA1', r[0]); bad += 1
            else:
                checked += 1
    print('manifest rows: %d  all sizes verified  sha1 verified: %d  bad: %d' % (len(rows), checked, bad))
    st = os.stat(os.path.join(DST, 'default.xbe'))
    print('default.xbe nlink:', st.st_nlink)
    for name, path in (('x21', os.path.join(DST, 'default.xbe')),
                       ('retail', r"E:\games\emu\ninja gaiden black hurricane recomp\input\retail\default.xbe")):
        print('%s default.xbe sha256: %s' % (name, hashlib.sha256(open(path, 'rb').read()).hexdigest()))


if __name__ == '__main__':
    main()
