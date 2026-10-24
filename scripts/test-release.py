#!/usr/bin/env python3
"""Verify release packaging preserves every allocated ELF section and debug link."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent


def sections(path):
    data = path.read_bytes()
    assert data[:4] == b'\x7fELF'
    endian = {1: '<', 2: '>'}[data[5]]
    header_format, section_format = {
        1: ('HHIIIIIHHHHHH', 'IIIIIIIIII'),
        2: ('HHIQQQIHHHHHH', 'IIQQQQIIQQ'),
    }[data[4]]
    header = struct.unpack_from(endian + header_format, data, 16)
    table = [struct.unpack_from(endian + section_format, data, header[5] + i * header[10])
             for i in range(header[11])]
    strings = table[header[12]]
    names = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for name, kind, flags, address, offset, size, *_ in table:
        name = names[name:names.index(0, name)].decode()
        result[name] = (flags, address, size, b'' if kind == 8 else data[offset:offset + size])
    return endian, result


def main():
    source = ROOT / 'hyprliquid.so'
    original = source.read_bytes()
    source_inode = source.stat().st_ino
    with tempfile.TemporaryDirectory(prefix='hyprliquid-release-test-') as temporary:
        output = Path(temporary) / 'release'
        command = ['python3', str(ROOT / 'scripts/package-release.py'), str(source), str(output)]

        def run(*extra, success=True):
            result = subprocess.run(command + list(extra), capture_output=True, text=True)
            assert (result.returncode == 0) == success, result.stdout + result.stderr

        run()
        destination = output / hashlib.sha256(original).hexdigest()
        plugin, debug = destination / 'hyprliquid.so', destination / 'hyprliquid.so.debug'
        endian, before = sections(source)
        _, after = sections(plugin)
        _, symbols = sections(debug)
        assert {k: v for k, v in before.items() if v[0] & 2} == {
            k: v for k, v in after.items() if v[0] & 2
        }, 'Release changed allocated code/data, exports, unwind information or build ID'
        assert '.debug_info' in symbols and '.debug_info' not in after
        debuglink = after['.gnu_debuglink'][3]
        name = debuglink[:debuglink.index(0)].decode()
        crc_offset = (len(name) + 4) & ~3
        assert name == debug.name
        assert struct.unpack_from(endian + 'I', debuglink, crc_offset)[0] == zlib.crc32(debug.read_bytes())
        manifest = json.loads((destination / 'manifest.json').read_text())
        assert manifest['source_sha256'] == hashlib.sha256(original).hexdigest()
        for name, entry in manifest['artifacts'].items():
            artifact = (destination / name).read_bytes()
            assert entry == {'sha256': hashlib.sha256(artifact).hexdigest(), 'bytes': len(artifact)}
        run()  # Repeated packaging reuses exactly the same pair.
        run('--objcopy', '/bin/false', success=False)
        assert list(output.iterdir()) == [destination], 'Failed packaging published a partial release'
        debug.write_bytes(b'corrupted test artifact')
        run(success=False)
        assert debug.read_bytes() == b'corrupted test artifact', 'Existing artifacts were overwritten'
        assert source.read_bytes() == original and source.stat().st_ino == source_inode
    print('PASS: unchanged allocated ELF sections, matching symbols/CRC, atomic failure and unchanged input')


if __name__ == '__main__':
    main()
