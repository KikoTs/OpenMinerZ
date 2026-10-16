"""Audit and package the working source tree without private imports or build output."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MEDIA = {'.png', '.bmp', '.jpg', '.jpeg', '.webp', '.gif', '.wav', '.ogg', '.mp3',
         '.flac', '.glb', '.gltf', '.bin', '.ttf', '.otf'}
PRIVATE = {'.xnb', '.xgs', '.xsb', '.xwb', '.omrc', '.omsm', '.omsk', '.omza',
           '.omavatar', '.ocavatar', '.omhc', '.omfnt', '.pcap', '.pcapng', '.dmp'}
BUILD = {'.exe', '.dll', '.pdb', '.obj', '.lib', '.zip', '.7z', '.rar', '.pyc'}
PRIVATE_DIRS = {'out', 'artifacts', 'captures', 'screenshots', 'traces', 'deriveddata',
                'content-packs', 'contentpacks', '.git', '__pycache__',
                '.venv', 'vcpkg_installed', 'bin', 'obj'}
TEXT = {'.md', '.txt', '.cpp', '.hpp', '.h', '.inc', '.cs', '.py', '.ps1', '.bat',
        '.cmd', '.json', '.yml', '.yaml', '.luau', '.html', '.css', '.js', '.mjs', '.csproj'}
SECRET = re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|'
                    r'\b(?:ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{50,}|AKIA[A-Z0-9]{16})\b')
RUNTIME_BINARIES = {
    'openminerz.exe', 'openminerz_mod_sdk.exe', 'sdl3.dll', 'freetype.dll',
    'libpng16.dll', 'zlib1.dll', 'bz2.dll', 'brotlidec.dll', 'brotlicommon.dll',
    'steambridge/openminerz.steambridge32.exe',
    'audiobridge/openminerz.audioruntime.exe',
    'importtools/openminerz.contentbridge.exe',
}


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(['git', '-C', str(root), *args])


def source_paths(root: Path) -> list[str]:
    names = git(root, 'ls-files', '--cached', '--others', '--exclude-standard', '-z').decode().split('\0')
    return sorted({n.replace('\\', '/') for n in names if n and (root / n).exists()})


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def forbidden_path(name: str, runtime: bool = False) -> str | None:
    p = PurePosixPath(name)
    parts = [part.lower() for part in p.parts]
    if p.is_absolute() or '..' in parts or '\\' in name or ':' in name:
        return 'unsafe path'
    if any(part.startswith('.') and part != '.github' for part in parts[:-1]):
        return 'private hidden directory'
    if any(part in PRIVATE_DIRS for part in parts):
        return 'private, generated or local directory'
    if p.name.lower() in {'handoff.md', 'steam_appid.txt', '.private-import', 'import-manifest.json', 'cmakeuserpresets.json'}:
        return 'private workstation/import file'
    if p.name.lower().startswith('.env') and p.name != '.env.example':
        return 'environment override'
    suffix = p.suffix.lower()
    if suffix in PRIVATE:
        return 'original game content or private capture'
    if suffix in BUILD and not (runtime and name.lower() in RUNTIME_BINARIES):
        return 'unapproved binary, archive or build output'
    return None


def gltf_issues(name: str, data: bytes) -> list[str]:
    try:
        if name.endswith('.glb'):
            magic, version, length = struct.unpack_from('<4sII', data)
            chunk_size, chunk_type = struct.unpack_from('<II', data, 12)
            if magic != b'glTF' or version != 2 or length != len(data) or chunk_type != 0x4E4F534A:
                return ['invalid GLB header']
            model = json.loads(data[20:20 + chunk_size])
        else:
            model = json.loads(data)
        issues = []
        if any(clip.get('name', '').startswith('REF_') for clip in model.get('animations', [])):
            issues.append('retail reference animation')
        if '/assets/animations/' in name:
            if model.get('meshes') or model.get('skins'):
                issues.append('player animation pack contains a reference mesh or skin')
            if any('translation' in node or 'matrix' in node for node in model.get('nodes', [])):
                issues.append('player animation pack contains imported joint offsets')
        return issues
    except (ValueError, TypeError, struct.error) as exc:
        return [f'invalid model: {exc}']


def load_assets(root: Path) -> dict:
    manifest = json.loads((root / 'tools/release-assets.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported release asset manifest')
    records = manifest['files']
    if len({entry['path'] for entry in records}) != len(records):
        raise ValueError('Duplicate release asset manifest entry')
    return {entry['path']: entry for entry in records}


def audit_files(root: Path, names: list[str], assets: dict, runtime: bool = False) -> list[str]:
    errors = []
    for name in names:
        path = root / name
        reason = forbidden_path(name, runtime)
        if reason:
            errors.append(f'{name}: {reason}')
            continue
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            errors.append(f'{name}: links are not release files')
            continue
        data = path.read_bytes()
        suffix = path.suffix.lower()
        if suffix in MEDIA:
            approved = assets.get(name)
            if not approved or not approved.get('origin') or not approved.get('license'):
                errors.append(f'{name}: media lacks reviewed provenance')
            elif sha256(data) != approved['sha256']:
                errors.append(f'{name}: media changed since provenance review')
            if suffix in {'.glb', '.gltf'}:
                errors.extend(f'{name}: {issue}' for issue in gltf_issues(name, data))
        elif data.startswith((b'XNB', b'OMRC', b'OMAV', b'OMSK', b'OMSM')):
            errors.append(f'{name}: imported game data hidden behind another extension')
        if suffix in TEXT and SECRET.search(data.decode('utf-8', errors='replace')):
            errors.append(f'{name}: possible credential/private key')
    if not runtime:
        for name in assets:
            if name not in names:
                errors.append(f'{name}: approved media is missing; update its manifest deliberately')
    return errors


def audit_links(root: Path, names: list[str]) -> list[str]:
    errors = []
    for name in names:
        if not name.endswith('.md'):
            continue
        text = (root / name).read_text(encoding='utf-8')
        for target in re.findall(r'\]\(([^\s)]+)(?:\s+"[^"]*")?\)', text):
            if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'):
                continue
            target = target.split('#')[0]
            if target and not (root / name).parent.joinpath(target).exists():
                errors.append(f'{name}: broken link {target}')
    return errors


def audit_history(root: Path) -> list[str]:
    errors = []
    for line in git(root, 'rev-list', '--objects', '--all').decode().splitlines():
        _, _, name = line.partition(' ')
        if name:
            reason = forbidden_path(name)
            if reason:
                errors.append(f'history {name}: {reason}')
    return sorted(set(errors))


def fingerprint(root: Path, names: list[str]) -> str:
    result = hashlib.sha256()
    for name in names:
        result.update(name.encode() + b'\0' + hashlib.sha256((root / name).read_bytes()).digest())
    return result.hexdigest()


def package_source(root: Path, names: list[str], destination: Path) -> None:
    destination = destination.resolve()
    if destination.is_relative_to(root.resolve()) and not destination.is_relative_to(root.resolve() / 'out'):
        raise ValueError('Package inside ignored out/ or outside the source checkout')
    if destination.exists():
        raise ValueError('Archive exists; choose a new filename')
    destination.parent.mkdir(parents=True, exist_ok=True)
    before = fingerprint(root, names)
    try:
        with zipfile.ZipFile(destination, 'x', zipfile.ZIP_DEFLATED) as archive:
            for name in names:
                archive.write(root / name, 'OpenMinerZ/' + name)
            archive.writestr('OpenMinerZ/SOURCE_REVISION.txt',
                             f'Git base: {git(root, "rev-parse", "HEAD").decode().strip()}\n'
                             f'Working source SHA-256: {before}\n'
                             'This archive includes the audited working files, including uncommitted changes.\n')
        if fingerprint(root, names) != before:
            raise ValueError('Source changed during packaging; try again')
    except Exception:
        destination.unlink(missing_ok=True)
        raise
    destination.with_suffix(destination.suffix + '.sha256').write_text(
        sha256(destination.read_bytes()) + '  ' + destination.name + '\n', encoding='utf-8')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--history', action='store_true', help='also inspect Git history for forbidden paths')
    parser.add_argument('--source-zip', type=Path, help='write a reviewed working-source ZIP')
    parser.add_argument('--runtime', type=Path, help='audit an unpacked public runtime package')
    parser.add_argument('--fingerprint', action='store_true', help='print the audited source fingerprint')
    args = parser.parse_args()
    root = args.root.resolve()
    names = source_paths(root)
    assets = load_assets(root)
    errors = audit_files(root, names, assets) + audit_links(root, names)
    if args.history:
        errors += audit_history(root)
    if args.runtime:
        runtime = args.runtime.resolve()
        runtime_names = sorted(p.relative_to(runtime).as_posix() for p in runtime.rglob('*') if p.is_file())
        errors += audit_files(runtime, runtime_names, assets, runtime=True)
        errors += audit_links(runtime, runtime_names)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    if args.source_zip:
        package_source(root, names, args.source_zip)
    if args.fingerprint:
        print(fingerprint(root, names))
    else:
        print(f'Release audit passed: {len(names)} source files, {len(assets)} approved media files.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Release audit failed: {error}', file=sys.stderr)
        raise SystemExit(1)
