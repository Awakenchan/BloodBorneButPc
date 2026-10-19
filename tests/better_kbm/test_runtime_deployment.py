#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check runtime deployment with a real, cross-linked Windows DLL graph.
The PE fixtures are generated in a temporary directory and are never executed.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--runtime-dir', required=True, type=Path)
    parser.add_argument('--objdump', help='Optional llvm-objdump or objdump to compare import names')
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[2] / 'tools/deploy-runtime-dlls.py'
    spec = importlib.util.spec_from_file_location('runtime_deployment', script)
    deployment = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(deployment)
    with tempfile.TemporaryDirectory(prefix='runtime-dll-test-') as temporary:
        root = Path(temporary)
        sdk, output = root / 'sdk', root / 'out'
        sdk.mkdir(); output.mkdir()
        # This fixture deliberately uses a familiar DLL name; it is not zlib.
        (root / 'zlib.c').write_text('__declspec(dllexport) int runtime_z(void){return 1;}\n')
        (root / 'leaf.cpp').write_text('#include <iostream>\n#include <string>\nextern "C" __declspec(dllimport) int runtime_z(void);\nextern "C" __declspec(dllexport) int runtime_leaf(void){std::string s="runtime";std::cout<<s;return int(s.size())+runtime_z();}\n')
        (root / 'middle.c').write_text('__declspec(dllimport) int runtime_leaf(void);\n__declspec(dllexport) int runtime_middle(void){return runtime_leaf();}\n')
        (root / 'app.c').write_text('__declspec(dllimport) int runtime_middle(void);\nint main(void){return runtime_middle()!=8;}\n')
        for source, name, symbol, compiler, libraries in [
            ('zlib.c', 'zlib1.dll', 'runtime_z', args.cc, []),
            ('leaf.cpp', 'leaf.dll', 'runtime_leaf', args.cxx, ['-lruntime_z']),
            ('middle.c', 'MiDdLe.DLL', 'runtime_middle', args.cc, ['-lruntime_leaf']),
        ]:
            subprocess.run([compiler, '-shared', str(root / source), '-L' + str(sdk), *libraries,
                            '-Wl,--out-implib,' + str(sdk / ('lib' + symbol + '.dll.a')),
                            '-o', str(sdk / name)], check=True)
        subprocess.run([args.cc, str(root / 'app.c'), '-L' + str(sdk), '-lruntime_middle',
                        '-o', str(output / 'app.exe')], check=True)
        for name in ['libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll',
                     'libstdc++-6.dll', 'libgcc_s_seh-1.dll']:
            source = args.runtime_dir / name
            if source.is_file():
                shutil.copyfile(source, sdk / name)
        copied = deployment.deploy(output, sdk)
        assert {'middle.dll', 'leaf.dll', 'zlib1.dll'} <= {n.casefold() for n in copied}
        if args.objdump:
            for path in output.iterdir():
                report = subprocess.check_output([args.objdump, '--private-headers', str(path)], text=True)
                expected = set(re.findall(r'DLL Name:\s*(\S+)', report))
                assert set(deployment.imports(path)) == expected, path.name
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir()}
        assert deployment.deploy(output, sdk) == []
        assert before == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir()}
        missing = root / 'missing'; missing.mkdir()
        shutil.copyfile(output / 'app.exe', missing / 'app.exe')
        (sdk / 'zlib1.dll').rename(sdk / 'zlib.hidden')
        try:
            deployment.deploy(missing, sdk)
        except ValueError as error:
            assert 'zlib1.dll' in str(error)
        else:
            raise AssertionError('Missing transitive DLL was not reported')
        try:
            deployment.imports(script)
        except ValueError:
            pass
        else:
            raise AssertionError('A non-PE file was accepted')
    print('PASS: transitive PE imports, case-insensitive resolution, preserved output, repeat deployment, missing dependencies, and invalid images.')


if __name__ == '__main__':
    main()
