"""Work on the boot build's one merged game source tree.

The tree is a local Git repository under build/game-tree/work. Its first commit
is the native-experimental export; every later commit becomes one patch in
patches/beachlife-decomp/game-series, applied last by the native-game profile.

  python3 tools/game_tree.py init      # (re)create the work tree from the series
  python3 tools/game_tree.py check     # compile every source file in the tree (--tree for a worktree)
  python3 tools/game_tree.py merge BRANCH -m MESSAGE   # merge a worker branch
  python3 tools/game_tree.py patches   # write the work tree's commits as patches
  python3 tools/game_tree.py rebase    # onto a new base after reconstruction patches changed
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT / 'patches' / 'beachlife-decomp'
SERIES = DECOMP / 'game-series'
TREE = ROOT / 'build' / 'game-tree'
WORK = Path(os.environ['GAME_TREE_WORK']) if os.environ.get('GAME_TREE_WORK') else TREE / 'work'
BASE_TAG = 'base'


def git(*args, cwd=None, capture=True, strip=True):
    result = subprocess.run(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', *args],
                            cwd=cwd or WORK, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    if not capture:
        return ''
    return result.stdout.strip() if strip else result.stdout


def series_entries():
    if not SERIES.is_file():
        return []
    return [line.strip() for line in SERIES.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.startswith('#')]


def revision():
    return json.loads((DECOMP / 'source.json').read_text(encoding='utf-8'))['revision']


def init(args):
    if WORK.exists():
        if not args.force:
            sys.exit(f'{WORK} exists; pass --force to recreate it (unexported commits are lost)')
        shutil.rmtree(WORK)
    export = TREE / 'export'
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'prepare_sources.py'), '--output', str(export),
                    '--decomp-only', '--decomp-revision', revision(),
                    '--decomp-profile', 'native-experimental'], check=True)
    shutil.copytree(export / 'beachlife-decomp', WORK)
    git('init', '--quiet', '--initial-branch=main')
    git('add', '-A')
    git('-c', 'user.name=game-tree', '-c', 'user.email=game-tree@localhost', 'commit', '--quiet',
        '-m', f'base: native-experimental export of {revision()}')
    git('tag', BASE_TAG)
    for name in series_entries():
        git('apply', str(DECOMP / name))
        git('add', '-A')
        subject = re.sub(r'^game-\d+-', '', name.removesuffix('.patch')).replace('-', ' ')
        git('-c', 'user.name=game-tree', '-c', 'user.email=game-tree@localhost', 'commit', '--quiet',
            '-m', subject)
    print(f'{WORK}: base plus {len(series_entries())} game-series patches')


def rebase(args):
    """Move the tree onto a new base after reconstruction patches changed.

    A function promoted into reconstruction-series by the matching track would
    otherwise be created twice (once there, once in game-series). The work
    repository becomes the fresh native-experimental export plus one commit
    holding the current tree; game-series is then that single diff."""
    worktrees = git('worktree', 'list', '--porcelain').count('worktree ') - 1
    if worktrees and not args.force:
        sys.exit('Merge and remove the worker worktrees first (or pass --force).')
    export = TREE / 'export'
    if export.exists():
        shutil.rmtree(export)
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'prepare_sources.py'), '--output', str(export),
                    '--decomp-only', '--decomp-revision', revision(),
                    '--decomp-profile', 'native-experimental'], check=True)
    rebased = TREE / 'work-rebased'
    if rebased.exists():
        shutil.rmtree(rebased)
    shutil.copytree(export / 'beachlife-decomp', rebased)
    git('init', '--quiet', '--initial-branch=main', cwd=rebased)
    git('add', '-A', cwd=rebased)
    git('-c', 'user.name=game-tree', '-c', 'user.email=game-tree@localhost', 'commit', '--quiet',
        '-m', f'base: native-experimental export of {revision()}', cwd=rebased)
    git('tag', BASE_TAG, cwd=rebased)
    # Replace every tracked file with the current tree (deletions included).
    for name in git('ls-files', cwd=rebased).splitlines():
        (rebased / name).unlink()
    archive = subprocess.run(['git', 'archive', 'HEAD'], cwd=WORK, stdout=subprocess.PIPE, check=True).stdout
    subprocess.run(['tar', '-x', '-C', str(rebased)], input=archive, check=True)
    git('add', '-A', cwd=rebased)
    git('commit', '--quiet', '-m', args.message, cwd=rebased)
    retired = TREE / f'work-before-rebase-{time.strftime("%Y%m%d-%H%M%S")}'
    shutil.move(str(WORK), str(retired))
    shutil.move(str(rebased), str(WORK))
    print(f'rebased; previous tree kept at {retired}')


def merge(args):
    # Always a merge commit: a fast-forward would rewrite main's first-parent
    # history and with it patches that are already committed.
    git('merge', '--no-ff', '--no-edit', '-m', args.message, args.branch, capture=False)


def slug(subject):
    return re.sub(r'[^a-z0-9]+', '-', subject.lower()).strip('-')[:60]


def patches(args):
    # Merged worker branches become one patch each (their first-parent diff).
    commits = git('rev-list', '--reverse', '--first-parent', f'{BASE_TAG}..HEAD').split()
    names = []
    for index, commit in enumerate(commits, 1):
        subject = git('log', '-1', '--format=%s', commit)
        name = f'game-{index:04d}-{slug(subject)}.patch'
        # Exact text: a diff can end with a one-space context line.
        diff = git('diff', '--no-color', '--no-renames', '--full-index', f'{commit}^', commit, strip=False)
        (DECOMP / name).write_text(diff, encoding='utf-8')
        names.append(name)
    for stale in DECOMP.glob('game-[0-9][0-9][0-9][0-9]-*.patch'):
        if stale.name not in names:
            stale.unlink()
    SERIES.write_text('# Boot build game tree, written by tools/game_tree.py patches.\n'
                      + ''.join(f'{name}\n' for name in names), encoding='utf-8')
    print(f'{len(names)} game-series patches')


# Files that need the Windows SDK (DirectInput, MSVC integer types) and are
# replaced by native adaptations in the boot build.
WINDOWS_ONLY = re.compile(r'#include\s*[<"](dinput|ddraw|windows|mss|dsound)\.h[>"]|__int64|__declspec|\b__?asm\s*\{',
                          re.IGNORECASE)
LOCAL_INCLUDE = re.compile(r'#include\s*"([^"]+)"')


SDK_INCLUDE = re.compile(r'#include\s*[<"](dinput|ddraw|windows|mss|dsound)\.h[>"]', re.IGNORECASE)
INLINE_ASM = re.compile(r'\b__?asm\s*\{')
COMPAT = ROOT / 'src' / 'platform' / 'compat' / 'include'


def windows_only(path, compat=None, seen=None):
    """True when the file or a local header it includes needs the Windows SDK.

    With a compat directory, SDK headers it provides and MSVC keywords no
    longer count. The compat prelude selects host CPU intrinsics in guarded
    assembly blocks; unguarded inline assembly still needs MSVC."""
    seen = set() if seen is None else seen
    if path in seen or not path.is_file():
        return False
    seen.add(path)
    text = path.read_text(encoding='utf-8', errors='replace')
    if compat:
        text = re.sub(r'#ifndef BEACHLIFE_COMPAT\n.*?#else\n', '', text, flags=re.S)
    if INLINE_ASM.search(text):
        return True
    if compat:
        provided = {entry.name.lower() for entry in Path(compat).glob('*.h')}
        if any(f'{name.lower()}.h' not in provided for name in SDK_INCLUDE.findall(text)):
            return True
    elif WINDOWS_ONLY.search(text):
        return True
    return any(windows_only(path.parent / name, compat, seen) for name in LOCAL_INCLUDE.findall(text))


def affected(src, tree):
    """Sources changed against main in a worker tree, plus every source that
    includes a changed header (directly or through other headers)."""
    out = subprocess.run(['git', 'diff', '--name-only', 'main', '--', 'src'], cwd=tree, text=True,
                         stdout=subprocess.PIPE).stdout.split()
    out += subprocess.run(['git', 'ls-files', '--others', '--exclude-standard', 'src'], cwd=tree,
                          text=True, stdout=subprocess.PIPE).stdout.split()
    changed = {Path(name).name for name in out}
    includers = {}
    for path in src.glob('*'):
        if path.suffix in ('.cpp', '.h'):
            for name in LOCAL_INCLUDE.findall(path.read_text(encoding='utf-8', errors='replace')):
                includers.setdefault(Path(name).name, set()).add(path.name)
    todo, seen = list(changed), set(changed)
    while todo:
        for user in includers.get(todo.pop(), ()):
            if user not in seen:
                seen.add(user)
                todo.append(user)
    return sorted(src / name for name in seen if name.endswith('.cpp') and (src / name).is_file())


def check(args):
    src = Path(args.tree).resolve() / 'src' if args.tree else WORK / 'src'
    files = sorted(src.glob('*.cpp'))
    if args.tree and not args.full and Path(args.tree).resolve() != WORK.resolve():
        # A worker tree: only what its changes can affect (the full tree costs
        # every worker minutes of CPU). The integrator checks main in full.
        files = affected(src, Path(args.tree).resolve())
        print(f'incremental check: {len(files)} affected sources (--full for all)')
    compiler = shutil.which('clang++') or shutil.which('g++')
    compat = COMPAT if COMPAT.is_dir() else None
    # The same game options as the boot build: MSVC prelude and 16-bit wchar_t.
    prelude = ['-fshort-wchar', '-include', str(compat / 'msvc_prelude.h'), f'-I{compat}'] if compat else []

    def compile_one(path):
        if windows_only(path, compat):
            return path, None
        result = subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-w', f'-I{src}',
                                 f'-I{ROOT / "src"}', *prelude, str(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return path, result.stdout if result.returncode else ''

    with ThreadPoolExecutor() as pool:
        results = list(pool.map(compile_one, files))
    skipped = [p for p, out in results if out is None]
    failed = [(p, out) for p, out in results if out]
    for path, out in failed:
        first = next((line for line in out.splitlines() if 'error' in line), out.splitlines()[0])
        print(f'FAIL {path.name}: {first}')
    # One body per retail address: merges must not keep two decompilations.
    owners = {}
    for path in files:
        for address in re.findall(r'//\s*FUNCTION:\s*BEACHLIFE\s+(0x[0-9a-fA-F]{8})',
                                  path.read_text(encoding='utf-8', errors='replace')):
            owners.setdefault(address.lower(), []).append(path.name)
    duplicates = {address: names for address, names in owners.items() if len(names) > 1}
    for address, names in sorted(duplicates.items()):
        print(f'DUPLICATE {address}: {", ".join(names)}')
    print(f'{len(files)} files: {len(files) - len(skipped) - len(failed)} compile, '
          f'{len(failed)} fail, {len(skipped)} Windows-only skipped, {len(duplicates)} duplicate addresses')
    return 1 if failed or duplicates else 0


def sources(args):
    """Print the boot build's source files: every tree file without a Windows-only API.

    With --compiling, files that do not compile yet are left out and reported;
    their functions become traps until they compile."""
    src = Path(args.tree).resolve() / 'src'
    files = [path for path in sorted(src.glob('*.cpp')) if not windows_only(path, args.compat)]
    if args.compiling:
        compiler = shutil.which('clang++') or shutil.which('g++')
        flags = (['-fshort-wchar', '-include', str(Path(args.compat) / 'msvc_prelude.h'), f'-I{args.compat}']
                 if args.compat else [])

        def compiles(path):
            return subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-w', f'-I{src}', f'-I{ROOT / "src"}',
                                   *flags, str(path)], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
        with ThreadPoolExecutor() as pool:
            results = list(pool.map(compiles, files))
        for path, ok in zip(files, results):
            if not ok:
                print(f'boot build: left out until it compiles: {path.name}', file=sys.stderr)
        files = [path for path, ok in zip(files, results) if ok]
    for path in files:
        print(path.as_posix())


def symbols(command, paths):
    names = set()
    # Darwin's argument limit is much smaller than Linux's. The whole game
    # has over a thousand objects, often under a long prepared-source path.
    for start in range(0, len(paths), 128):
        out = subprocess.check_output([*command, *map(str, paths[start:start + 128])], text=True)
        names.update(line.split()[0] for line in out.splitlines()
                     if line and not line.endswith(':') and len(line.split()) >= 2)
    return names


def system_symbols(compiler='c++', nm='nm', target_system='Linux', *, arch=None,
                   sysroot=None, compiler_args=()):
    names = set()
    if target_system == 'Darwin':
        if not sysroot or arch not in ('arm64', 'x86_64'):
            raise ValueError('Darwin traps require an explicit SDK and target architecture')
        library_dir = Path(sysroot) / 'usr/lib'
        paths = [library_dir / 'libSystem.tbd', library_dir / 'libc++.tbd']
        if not all(path.is_file() for path in paths):
            raise ValueError(f'Cannot locate libSystem/libc++ stubs in {sysroot}')
        # libSystem reexports its system shards. LLVM nm reads TAPI stubs,
        # including inlined sublibraries, without the host dyld shared cache.
        paths += sorted((library_dir / 'system').glob('*.tbd'))
        if (library_dir / 'libc++abi.tbd').is_file():
            paths.append(library_dir / 'libc++abi.tbd')
        runtime = subprocess.check_output(
            [compiler, *compiler_args, '--print-libgcc-file-name'], text=True).strip()
        if not Path(runtime).is_file():
            raise ValueError(f'Cannot locate Darwin compiler runtime: {runtime}')
        paths.append(Path(runtime))
        names = symbols([nm, '-P', '--defined-only', '--extern-only', f'--arch={arch}'], paths)
        # LLVM nm does not list simulator stubs' inlined sublibraries, so memcpy
        # would look missing and get a trap that replaces the real one. Read the
        # stubs' text too; a name they mention is never a game function.
        for path in paths:
            if path.suffix == '.tbd':
                names.update(re.findall(r"[_A-Za-z$][\w$.]*", path.read_text(errors='replace')))
        return names
    if target_system == 'Windows':
        # Inspect the target CRT/import libraries, never the Linux build host.
        # LLVM-MinGW uses libc++, UCRT and compiler-rt (not libstdc++/libgcc).
        libraries = ('libc++.dll.a', 'libc++.a', 'libc++abi.a', 'libunwind.dll.a', 'libmingw32.a',
                     'libmingwex.a', 'libmsvcrt.a', 'libucrt.a', 'libkernel32.a')
        paths = []
        for library in libraries:
            path = subprocess.check_output([compiler, f'-print-file-name={library}'], text=True).strip()
            if Path(path).is_file():
                paths.append(Path(path))
        path = subprocess.check_output([compiler, '--print-libgcc-file-name'], text=True).strip()
        if Path(path).is_file():
            paths.append(Path(path))
        if not paths:
            raise ValueError('Cannot locate the target Windows runtime libraries')
        return symbols([nm, '-P', '--defined-only'], paths)
    # Instrumented game objects also reference the compiler's sanitizer
    # runtimes. Trapping those hooks overrides the real runtime at link time
    # and can recurse before main (including inside __asan_init itself).
    for library in ('libc.so.6', 'libm.so.6', 'libstdc++.so.6', 'libgcc_s.so.1',
                    'libasan.so', 'libubsan.so'):
        path = subprocess.run([compiler, f'-print-file-name={library}'], text=True,
                              stdout=subprocess.PIPE, check=True).stdout.strip()
        if Path(path).is_file():
            names |= symbols([nm, '-D', '-P', '--defined-only'], [path])
    return {name.split('@')[0] for name in names}


def called(objects, target_system='Linux', *, readelf='readelf', objdump='llvm-objdump'):
    """Symbols referenced by a call relocation (functions, never data)."""
    # COFF REL32 is shared by calls and data references. Only header-declared
    # C functions (and demangled C++ functions) are safe trap candidates there.
    if target_system == 'Windows':
        return set()
    result = set()
    if target_system == 'Darwin':
        for start in range(0, len(objects), 128):
            out = subprocess.check_output([objdump, '-r', *map(str, objects[start:start + 128])], text=True)
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[1] in ('X86_64_RELOC_BRANCH', 'ARM64_RELOC_BRANCH26'):
                    result.add(parts[2])
        return result
    for obj in objects:
        out = subprocess.check_output([readelf, '-rW', str(obj)], text=True)
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 5 and parts[2] == 'R_X86_64_PLT32':
                result.add(parts[4])
    return result


# Linked from glibc's static helper library rather than libc.so.6.
SYSTEM_STATIC = {'atexit', 'at_quick_exit', '__stack_chk_fail_local'}


def declared_functions(tree):
    """Unmangled names that a tree or compat header declares with a parameter list."""
    names = set()
    folders = [COMPAT] + ([Path(tree) / 'src'] if tree else [])
    for folder in folders:
        for header in Path(folder).glob('*.h'):
            names.update(re.findall(r'\b([A-Za-z_]\w*)\s*\(', header.read_text(encoding='utf-8', errors='replace')))
    return names


def inventory_index(path):
    if not path:
        return {}
    index = {}
    for row in json.loads(Path(path).read_text(encoding='utf-8'))['functions']:
        index.setdefault(row['name'], row)
        if row.get('cpp_name'):
            index.setdefault(row['cpp_name'], row)
    return index


def qualified(demangled):
    """'A::B(int) const' -> 'A::B' for lookup in the inventory."""
    depth, end = 0, len(demangled)
    for i, char in enumerate(demangled):
        if char == '<':
            depth += 1
        elif char == '>':
            depth -= 1
        elif char == '(' and depth == 0:
            end = i
            break
    return demangled[:end]


def header_addresses(tree):
    """Map 'Class::Method' to an address noted beside its declaration in the tree headers."""
    if not tree:
        return {}
    result = {}
    declaration = re.compile(r'[~\w]+(?=\s*\()')
    address = re.compile(r'0x00([0-9a-fA-F]{6})\b')
    for header in sorted((Path(tree) / 'src').glob('*.h')):
        owner = None
        for line in header.read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.match(r'\s*(?:struct|class)\s+(\w+)', line)
            if match and not line.rstrip().endswith(';'):
                owner = match.group(1)
            found, method = address.search(line), declaration.search(line.split('//')[0])
            if owner and found and method:
                result.setdefault(f'{owner}::{method.group(0)}', '0x00' + found.group(1).lower())
    return result


def class_header(tree, name):
    pattern = re.compile(rf'^\s*(?:struct|class)\s+{re.escape(name)}\b(?!\s*;)')
    for header in sorted((Path(tree) / 'src').glob('*.h')):
        for line in header.read_text(encoding='utf-8', errors='replace').splitlines():
            if pattern.match(line) and not line.rstrip().endswith(';'):
                return header
    return None


def split_type(qual_type):
    """'unsigned char (int, float) const' -> ('unsigned char', 'int, float', ' const').

    The exception specification stays in the qualifiers: a definition must
    repeat it (for example a destructor declared noexcept(false))."""
    text = qual_type.strip()
    exception = ''
    match = re.search(r'\s+noexcept(?:\([^()]*\))?$', text)
    if match:
        exception, text = match.group(0), text[:match.start()].rstrip()
    qualifiers = ''
    while True:
        for word in (' const', ' volatile', ' &&', ' &'):
            if text.endswith(word):
                qualifiers = word + qualifiers
                text = text[:-len(word)].rstrip()
                break
        else:
            break
    depth = 0
    for i in range(len(text) - 1, -1, -1):
        depth += {')': 1, '(': -1}.get(text[i], 0)
        if depth == 0:
            return text[:i].strip(), text[i + 1:-1], qualifiers + exception
    raise ValueError(f'Cannot split function type: {qual_type}')


def header_inline_methods(tree):
    """'Class::Method' names some tree header defines inline outside the class."""
    names = set()
    for header in (Path(tree) / 'src').glob('*.h'):
        text = header.read_text(encoding='utf-8', errors='replace')
        names.update(re.findall(r'\binline\b[^;{]*?\b(\w+::~?\w+)\s*\(', text))
    return names


def class_traps(tree, name, defined, inline=frozenset(), compiler='clang++', compiler_args=()):
    """C++ trap definitions for every declared, undefined method of a class.

    Defining the class's methods here makes the compiler emit its vtable and
    type information, so a constructor can link before its virtuals exist."""
    header = class_header(tree, name)
    if not header:
        return None
    src = Path(tree) / 'src'
    out = subprocess.run([compiler, *compiler_args, '-std=c++20', '-fsyntax-only', '-w', f'-I{src}', f'-I{ROOT / "src"}',
                          '-fshort-wchar', '-include', str(COMPAT / 'msvc_prelude.h'), f'-I{COMPAT}',
                          '-x', 'c++', str(header), '-Xclang', '-ast-dump=json',
                          '-Xclang', f'-ast-dump-filter={name}'],
                         stdout=subprocess.PIPE, check=True, text=True).stdout
    decoder, index, record = json.JSONDecoder(), 0, None
    while index < len(out):
        while index < len(out) and out[index].isspace():
            index += 1
        if index >= len(out):
            break
        node, index = decoder.raw_decode(out, index)
        if node.get('kind') == 'CXXRecordDecl' and node.get('name') == name and node.get('completeDefinition'):
            record = node
            break
    if not record:
        return None
    methods = []
    for node in record.get('inner', []):
        kind = node.get('kind')
        if kind not in ('CXXMethodDecl', 'CXXDestructorDecl', 'CXXConstructorDecl'):
            continue
        if node.get('pure') or node.get('explicitlyDefaulted') or node.get('isImplicit') \
                or node.get('mangledName') in defined or f'{name}::{node["name"]}' in inline \
                or any(child.get('kind') == 'CompoundStmt' for child in node.get('inner', [])):
            continue
        returns, parameters, qualifiers = split_type(node['type']['qualType'])
        prefix = '' if kind != 'CXXMethodDecl' else returns + ' '
        methods.append({'symbol': node['mangledName'], 'name': f'{name}::{node["name"]}',
                        'definition': f'{prefix}{name}::{node["name"]}({parameters}){qualifiers}'})
    return {'header': header.name, 'methods': methods}


def symbol_family(symbol):
    """Constructor/destructor variants share one C++ definition."""
    for old, new in (('C1E', ('C2E', 'C3E')), ('D1E', ('D0E', 'D2E'))):
        if old in symbol:
            return {symbol, *(symbol.replace(old, other) for other in new)}
    return {symbol}


def traps(args):
    """Emit one loud trap per game function the tree does not define yet."""
    objects = [Path(line) for line in Path(args.objects).read_text().splitlines() if line.strip()]
    defined = symbols([args.nm, '-P', '--defined-only'], objects)
    undefined = symbols([args.nm, '-P', '-u'], objects) - defined
    if args.host:
        undefined -= symbols([args.nm, '-P', '--defined-only'], [Path(p) for p in args.host])
    # Keep raw linker identities for asm labels and AST comparisons. Only
    # classification/demangling removes Mach-O's one external-symbol prefix.
    def canonical(symbol):
        return symbol[1:] if args.target_system == 'Darwin' and symbol.startswith('_') else symbol
    undefined -= system_symbols(args.compiler, args.nm, args.target_system,
                                arch=args.target_arch, sysroot=args.sysroot,
                                compiler_args=args.compiler_arg)
    if args.target_system != 'Darwin':
        undefined -= SYSTEM_STATIC
    declarations = declared_functions(args.tree)
    calls = called(objects, args.target_system, readelf=args.readelf, objdump=args.objdump)
    calls |= {symbol for symbol in undefined if canonical(symbol) in declarations}
    names = sorted(undefined)
    demangled = subprocess.run([args.demangler, '--no-strip-underscore'],
                               input='\n'.join(map(canonical, names)), text=True,
                               stdout=subprocess.PIPE, check=True).stdout.splitlines()
    if len(demangled) != len(names):
        raise ValueError('Demangler returned an incomplete symbol list')
    index = inventory_index(args.inventory)
    by_address = {row['address']: row for row in index.values()}
    hints = header_addresses(args.tree)
    def address_of(qualified_name):
        row = index.get(qualified_name)
        hint = hints.get(qualified_name)
        if not row and hint:
            row = by_address.get(hint, {'address': hint, 'name': None})
        return row
    # Classes whose vtable or type information is missing get C++ trap methods.
    classes, covered = {}, set()
    inline = header_inline_methods(args.tree) if args.tree else set()
    for symbol, name in zip(names, demangled):
        match = re.match(r'(?:vtable|typeinfo|typeinfo name) for (\w+)$', name)
        if match and args.tree and match.group(1) not in classes:
            # AppleClang may be selected through /usr/bin/c++; its basename
            # must not silently switch the AST to a different PATH compiler.
            ast_compiler = args.compiler if args.target_system in ('Darwin', 'Windows') \
                or 'clang' in Path(args.compiler).name else 'clang++'
            classes[match.group(1)] = class_traps(args.tree, match.group(1), defined, inline,
                                                ast_compiler, args.compiler_arg)
    for traps_of_class in classes.values():
        for method in (traps_of_class or {}).get('methods', []):
            covered |= symbol_family(method['symbol'])
    functions, services, unresolved = [], [], []
    for symbol, name in zip(names, demangled):
        if name.startswith('beachlife_compat_'):
            service = name.removeprefix('beachlife_compat_')
            if service in declarations:
                services.append({'symbol': symbol, 'name': service})
            else:
                unresolved.append({'symbol': symbol, 'name': name})
            continue
        # beachlife_ C functions are host services (CRT, tweaks) the runtime defines.
        if name.startswith(('beachlife::', 'beachlife_', 'std::', '__gnu_cxx::', 'ImGui', 'SDL_',
                            '_GLOBAL_OFFSET_TABLE_', '__dso_handle')):
            continue  # host and library code, resolved by the link
        class_name = re.match(r'(?:vtable|typeinfo|typeinfo name) for (\w+)$', name)
        if symbol in covered or (class_name and classes.get(class_name.group(1))):
            continue  # defined by the class traps below
        if canonical(symbol).startswith('_Z') and '(' in name:
            row = index.get(qualified(name))
            hint = hints.get(qualified(name))
            if not row and hint:
                row = by_address.get(hint, {'address': hint, 'name': None})
            functions.append({'symbol': symbol, 'name': name,
                              'address': row['address'] if row else None,
                              'inventory_name': row['name'] if row else None})
        elif not canonical(symbol).startswith('_Z') and symbol in calls:
            services.append({'symbol': symbol, 'name': canonical(symbol)})
        else:
            unresolved.append({'symbol': symbol, 'name': name})
    class_methods = []
    for traps_of_class in classes.values():
        for method in (traps_of_class or {}).get('methods', []):
            row = address_of(method['name'])
            class_methods.append({**method, 'address': row['address'] if row else None,
                                  'inventory_name': row['name'] if row else None})
    Path(args.json).write_text(json.dumps({'functions': functions + class_methods,
                                           'host_services': services,
                                           'unresolved': unresolved}, indent=1) + '\n')
    headers = sorted({c['header'] for c in classes.values() if c})
    if headers and args.tree:
        headers = sorted(path.name for path in (Path(args.tree) / 'src').glob('*.h')
                         if not windows_only(path, COMPAT if COMPAT.is_dir() else None))
    lines = ['// Generated by tools/game_tree.py traps. Each function below is not',
             '// decompiled yet (or needs a host service); calling it stops the game.',
             '#include <cstdint>', *[f'#include "{header}"' for header in headers], '',
             'namespace beachlife::boot',
             '{',
             '[[noreturn]] void trap(const char* name, std::uint32_t address);',
             '[[noreturn]] void missing_service(const char* name);',
             '}', '', 'extern "C"', '{']
    for number, entry in enumerate(functions, 1):
        label = f'beachlife_trap_{number:05d}'
        address = int(entry['address'], 16) if entry['address'] else 0
        text = json.dumps(entry['name'])
        lines += [f'[[noreturn]] void {label}() __asm__("{entry["symbol"]}");',
                  f'[[noreturn]] void {label}() {{ beachlife::boot::trap({text}, 0x{address:08x}); }}']
    for number, entry in enumerate(services, 1):
        label = f'beachlife_service_{number:05d}'
        lines += [f'[[noreturn]] void {label}() __asm__("{entry["symbol"]}");',
                  f'[[noreturn]] void {label}() {{ beachlife::boot::missing_service("{entry["name"]}"); }}']
    lines += ['}', '']
    for method in class_methods:
        address = int(method['address'], 16) if method['address'] else 0
        lines.append(f'{method["definition"]} {{ beachlife::boot::trap({json.dumps(method["name"])}, '
                     f'0x{address:08x}); }}')
    Path(args.output).write_text('\n'.join(lines) + '\n')
    functions = functions + class_methods
    mapped = sum(1 for entry in functions if entry['address'])
    print(f'boot traps: {len(functions)} game functions ({mapped} in the inventory), '
          f'{len(services)} host services, {len(unresolved)} unresolved')
    for entry in unresolved:
        print(f'  unresolved (data or vtable; needs real decompiled source): {entry["name"]}')
    if args.require_complete and (functions or services or unresolved):
        # Keep the diagnostic JSON, but never leave a compilable output from a
        # failed release generation that an incremental build could reuse.
        Path(args.output).unlink(missing_ok=True)
        print('Release game requires zero game traps, host-service traps and unresolved symbols.',
              file=sys.stderr)
        return 1
    return 1 if unresolved and args.strict else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('init')
    command.add_argument('--force', action='store_true')
    command = commands.add_parser('check')
    command.add_argument('--tree', help='another worktree of the game tree (default: the main work tree)')
    command.add_argument('--full', action='store_true', help='check every source, not only affected ones')
    commands.add_parser('patches')
    command = commands.add_parser('rebase')
    command.add_argument('-m', '--message', default='game tree', help='subject of the squashed tree commit')
    command.add_argument('--force', action='store_true')
    command = commands.add_parser('merge')
    command.add_argument('branch')
    command.add_argument('-m', '--message', required=True)
    command = commands.add_parser('sources')
    command.add_argument('--tree', required=True, help='prepared beachlife-decomp directory')
    command.add_argument('--compat', help='compat SDK headers; files using only those compile')
    command.add_argument('--compiling', action='store_true', help='leave out files that do not compile yet')
    command = commands.add_parser('traps')
    command.add_argument('--compiler', default='clang++', help='target C++ compiler')
    command.add_argument('--nm', default='nm', help='target object symbol reader')
    command.add_argument('--demangler', default='c++filt')
    command.add_argument('--target-system', default='Linux', choices=('Linux', 'Windows', 'Darwin'))
    command.add_argument('--target-arch', choices=('arm64', 'x86_64'))
    command.add_argument('--sysroot', help='target SDK path (required for Darwin)')
    command.add_argument('--compiler-arg', action='append', default=[], help='target compiler option')
    command.add_argument('--readelf', default='readelf', help='ELF relocation reader')
    command.add_argument('--objdump', default='llvm-objdump', help='Mach-O relocation reader')
    command.add_argument('--objects', required=True, help='file listing the tree object files')
    command.add_argument('--host', nargs='*', help='host libraries that resolve symbols')
    command.add_argument('--inventory', help='progress inventory JSON for addresses')
    command.add_argument('--tree', help='prepared beachlife-decomp directory, for header address notes')
    command.add_argument('--output', required=True, help='generated C++ file')
    command.add_argument('--json', required=True, help='generated trap list')
    command.add_argument('--strict', action='store_true', help='fail on unresolved data symbols')
    command.add_argument('--require-complete', action='store_true', help='fail if any trap or unresolved symbol remains')
    args = parser.parse_args()
    return {'init': init, 'check': check, 'patches': patches, 'merge': merge, 'rebase': rebase,
            'sources': sources,
            'traps': traps}[args.command](args) or 0


if __name__ == '__main__':
    sys.exit(main())
