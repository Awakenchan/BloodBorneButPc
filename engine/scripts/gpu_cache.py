"""The GPU cache before a start, as bb-start.exe keeps it on Windows (start/main.cpp).

When the shader compiler changed (out/gpu-cache.id), each cache folder is recompiled by bb-shader-lab
from the shader states kept beside its shaders, so its pipelines are built at the start instead of
in play; a folder it cannot recompile is removed. Then each shader catalog shipped in
engine/shader-catalog is seeded once into the cache (its pipelines built before play too).
BB_SHADER_STATES=0 / BB_SHADER_CATALOG=0: not done.
"""
import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path


def log(text):
    print(text, flush=True)


def lab_run(lab, *args):
    """bb-shader-lab's exit code and output (its log lines on stderr pass through)."""
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = os.pathsep.join(p for p in (str(lab.parent / 'gpu'), env.get('LD_LIBRARY_PATH', '')) if p)
    try:
        done = subprocess.run([str(lab), *map(str, args)], stdout=subprocess.PIPE, text=True,
                              errors='replace', env=env)
    except OSError as error:
        return -1, str(error)
    return done.returncode, done.stdout


def line_after(output, prefix):
    at = output.rfind(prefix)
    if at < 0:
        return None
    return output[at + len(prefix):].split('\n', 1)[0].strip()


def recompile(cache, lab):
    """Every cache folder recompiled for this engine; False when one could not be (removed)."""
    if os.environ.get('BB_SHADER_STATES', '1') == '0' or not lab.is_file():
        return False
    ok = True
    for folder in sorted(p for p in cache.iterdir() if p.is_dir() and (p / 'profile.bin').is_file()):
        code, output = lab_run(lab, '--recompile-cache', folder)
        summary = line_after(output, 'Shader cache:')
        if code == 0 and summary is not None:
            log(f'GPU cache {folder.name}: {summary}')
        else:
            shutil.rmtree(folder, ignore_errors=True)
            log(f'GPU cache {folder.name}: not recompiled (exit {code}); its shaders compile again in play')
            ok = False
    return ok


def reset_stale(cache, identity, legacy, lab):
    stamp = cache / 'engine.txt'
    try:
        stamped = stamp.read_text().strip()
    except OSError:
        stamped = None
    if stamped is not None:
        if not identity or stamped == identity:
            return
        if legacy and stamped == legacy:
            stamp.write_text(identity + '\n')
            return
    if cache.is_dir() and recompile(cache, lab):
        stamp.write_text(identity + '\n')
        log(f'GPU cache: the shader compiler changed ({identity}); recompiled before the start')
        return
    removed = False
    if cache.is_dir():
        for entry in cache.iterdir():
            if entry == stamp or entry.suffix.lower() == '.vkcache':
                continue
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry, ignore_errors=True)
            else:
                entry.unlink(missing_ok=True)
            removed = True
    cache.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity + '\n')
    if removed:
        log(f'GPU cache: cleared ({identity}: the shader compiler changed); shaders compile again on this start')


def gpu_vendors():
    """The PCI vendors of this PC's display adapters (Linux: /sys/class/drm)."""
    vendors = set()
    for card in Path('/sys/class/drm').glob('card*/device/vendor'):
        try:
            vendors.add(int(card.read_text().strip(), 16))
        except (OSError, ValueError):
            pass
    return vendors


def catalog_vendor(catalog):
    try:
        words = (catalog / 'gpu.txt').read_text().split()
        return int(words[1], 16) if words[0] == 'vendor' else 0
    except (OSError, IndexError, ValueError):
        return 0


def cache_vendor(folder):
    """The vendor of the GPU the engine last ran a cache on: its driver cache beside the folder
    ("BPBC", the vendor at 8); 0 before the first start."""
    try:
        with open(str(folder) + '.vkcache', 'rb') as f:
            header = f.read(12)
    except OSError:
        return 0
    return int.from_bytes(header[8:12], 'little') if len(header) == 12 and header[:4] == b'BPBC' else 0


def seed(cache, catalogs, game, eboot, lab):
    if os.environ.get('BB_SHADER_CATALOG', '1') == '0' or not lab.is_file() or not catalogs.is_dir():
        return
    by_game = {}
    for entry in sorted(p for p in catalogs.iterdir() if p.is_dir()):
        if (entry / 'profile.bin').is_file():
            by_game.setdefault(entry.name, []).append(entry)
            continue
        for sub in sorted(p for p in entry.iterdir() if p.is_dir() and (p / 'profile.bin').is_file()):
            by_game.setdefault(sub.name, []).append(sub)
    present = gpu_vendors()
    for name, candidates in sorted(by_game.items()):
        target = cache / name
        vendor = cache_vendor(target) or next((v for v in (0x10DE, 0x1002, 0x8086) if v in present), 0)
        catalog = next((c for c in candidates if vendor and catalog_vendor(c) == vendor), None)
        if catalog is None:
            continue
        if not target.exists():
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(catalog / 'profile.bin', target / 'profile.bin')
            log(f"GPU cache {catalog.name}: first start; the shader catalog's GPU profile used until the engine checks it")
        if not (target / 'profile.bin').is_file():
            continue
        files = [p for p in catalog.iterdir()]
        marker = target / f'catalog-{len(files)}-{sum(p.stat().st_size for p in files if p.is_file())}.applied'
        if marker.exists():
            continue
        code, output = lab_run(lab, '--seed-cache', catalog, target, game, eboot)
        first = line_after(output, 'Shader catalog:')
        if first is not None:
            log(f'GPU cache {catalog.name}: {first}')
        last = line_after(output, 'Shader cache:')
        if code == 0 and last is not None:
            log(f'GPU cache {catalog.name}: {last}')
        marker.write_text(f'{code}\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=Path, required=True, help='BB_GPU_USER_DIR/cache')
    p.add_argument('--engine', type=Path, required=True, help='the engine folder (shader-catalog)')
    p.add_argument('--out', type=Path, required=True, help='engine/out: bb-shader-lab, gpu-cache.id')
    p.add_argument('--package', type=Path, help='package.json (the engine id when no gpu-cache.id)')
    p.add_argument('--game', type=Path, help='the game folder: the shader catalogs are seeded too (out/eboot.elf)')
    a = p.parse_args()
    lab = a.out / 'bb-shader-lab'
    identity, legacy = '', ''
    try:
        identity = json.loads(a.package.read_text()).get('engine', '') if a.package else ''
    except (OSError, ValueError):
        pass
    try:
        gpu_id = (a.out / 'gpu-cache.id').read_text().strip()
        if gpu_id:
            identity = 'gpu ' + gpu_id
        old = (a.out / 'gpu-cache.legacy.id').read_text().strip()
        if old:
            legacy = 'gpu ' + old
    except OSError:
        pass
    reset_stale(a.cache, identity, legacy, lab)
    if a.game:
        seed(a.cache, a.engine / 'shader-catalog', a.game, a.out / 'eboot.elf', lab)
