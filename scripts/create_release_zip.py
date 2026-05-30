#!/usr/bin/env python3
import os
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: create_release_zip.py <zip-path> <pak-dir>", file=sys.stderr)
        return 2

    zip_path = sys.argv[1]
    pak_dir = sys.argv[2]
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(pak_dir):
            dirs.sort()
            files.sort()
            rel_root = root.replace(os.sep, "/")
            zf.write(root, rel_root + "/")
            for name in files:
                path = os.path.join(root, name)
                arcname = path.replace(os.sep, "/")
                zf.write(path, arcname)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
