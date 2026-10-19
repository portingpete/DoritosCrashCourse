"""Check the independent engine source boundary and its actual linker map."""
import argparse
from pathlib import Path
import json
import sys

ROOT=Path(__file__).resolve().parents[1]
FORBIDDEN=('PPCContext','PPC_FUNC','PPC_LOAD','PPC_STORE','PPC_CALL','runtime/native/',
           'renderer/native/','DoritosNativeImport','guest_runner','DoritosPPC','XenonUtils','XenonRecomp')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path)
    args=parser.parse_args()
    failures=[]; files=[]
    for path in (ROOT/'engine').rglob('*'):
        if path.suffix not in ('.h','.cpp','.txt'): continue
        value=path.read_text(encoding='utf-8')
        files.append(path.relative_to(ROOT).as_posix())
        for token in FORBIDDEN:
            if token.casefold() in value.casefold(): failures.append(f'{path.name}: forbidden dependency {token}')
    linkmap=None
    if args.executable:
        linkmap=args.executable.with_suffix('.map')
        if not args.executable.is_file() or not linkmap.is_file(): failures.append('Executable and native linker map are required')
        else:
            text=linkmap.read_text(encoding='utf-8',errors='replace')
            for token in FORBIDDEN:
                if token.casefold() in text.casefold(): failures.append('Linker map contains '+token)
            for token in ('enginecore','d3d11'):
                if token not in text.casefold(): failures.append('Linker map missing expected native library '+token)
    result={'passed':not failures,'source_files':files,'linkmap':str(linkmap) if linkmap else None,'failures':failures}
    print(json.dumps(result,indent=2)); return 0 if result['passed'] else 1


if __name__=='__main__': sys.exit(main())
