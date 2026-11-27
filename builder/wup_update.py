"""Build a small native Wii U game update, leaving base content and saves installed."""
from datetime import datetime, timezone
from contextlib import contextmanager
import tempfile
from pathlib import Path
import json
import os
import shutil
import xml.etree.ElementTree as ET

# Manual WUP reinstalls with changed content IDs were verified on hardware
# while keeping native version 5. Timestamps label builds, never order updates.
UPDATE_VERSION = 5
PARTS = ('binary', 'both')
APP_TYPE = '0800001B'



def native_base_id(title_id, program_id=None):
    """Use a reserved 1D ID with a zero variation byte for native grouping.

    nn_vctl needs the top nibble 1 for patch lookup. System Settings groups
    saves with the last byte cleared, but groups software by its complete ID.
    Encode the 3DS game's unique number in bits 8..23; never truncate it or
    derive it by masking a legacy WUP ID (which can have custom test bits).
    """
    from wup_safety import check_title_id
    check_title_id(title_id)
    if title_id >> 32 != 0x00050000:
        raise ValueError('Expected a base game title ID')
    if program_id is not None:
        if program_id >> 32 != 0x00040000 or program_id & 0xff:
            raise ValueError('Expected a 3DS base game ID with a zero variation byte')
        unique = (program_id & 0xffffffff) >> 8
        if unique > 0xffff:
            raise ValueError('3DS unique number exceeds the reserved WUP ID range; refusing to truncate it')
        return 0x000500001d000000 | (unique << 8)
    if title_id & 0xff or (title_id & 0xff000000) != 0x1d000000:
        raise ValueError('This legacy WUP ID cannot group software and saves correctly. Rebuild the base from the original 3DS game; existing saves need migration to its corrected ID.')
    return title_id


SHADER_FILES = ('shaders.gsh', 'shaders.idx', 'fallback-shaders.bin')


def remember_base(path, title, base_id):
    """Remember shader sizes for binary-only FST links, outside extraction cleanup."""
    native_base_id(base_id)
    content = Path(title)/'content'
    files = {name: (content/name).stat().st_size for name in SHADER_FILES if (content/name).is_file()}
    if not all(name in files for name in SHADER_FILES[:2]):
        raise ValueError('The base shader files are missing; no binary-only update inventory was saved')
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        json.dump(dict(base_title_id=f'{base_id:016X}', files=files), out, indent=2)
        out.write('\n')
    try: os.replace(temporary, path)
    finally: temporary.unlink(missing_ok=True)


def base_shaders(path, base_id):
    try:
        info = json.loads(Path(path).read_text(encoding='utf-8'))
        files = info['files']
        if int(info['base_title_id'], 16) != base_id or not isinstance(files, dict): raise ValueError()
        if not all(name in files for name in SHADER_FILES[:2]): raise ValueError()
        for name, size in files.items():
            if name not in SHADER_FILES or type(size) is not int or not 0 <= size <= 0xffffffff: raise ValueError()
        return files
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError('Binary-only WUP updates need the matching base shader inventory. Build the matching full WUP install once on this PC with the same build folder, or choose Binary + shaders.') from error


def update_build_info(now=None):
    now = now or datetime.now(timezone.utc)
    if now.tzinfo is None: raise ValueError('Build time must include a timezone')
    return UPDATE_VERSION, now.astimezone(timezone.utc)


@contextmanager
def title_output(destination, update_id):
    from win.objcache import file_locks
    from wup_safety import check_output
    check_output(destination)
    destination.parent.mkdir(parents=True,exist_ok=True)
    with file_locks(destination.parent/('.'+destination.name+'.lock')):
        if destination.exists():
            if destination.is_symlink() or any(p.is_symlink() for p in destination.rglob('*')):
                raise ValueError('Update output must not contain symbolic links')
            if set(p.name for p in destination.iterdir()) not in ({'code','content','meta'},{'code','content','meta','.3dii-base-links.json'}):
                raise ValueError('Existing update output has unrelated files')
            if int(ET.parse(destination/'code/app.xml').findtext('title_id'),16)!=update_id:
                raise ValueError('Existing update output belongs to another title')
        with tempfile.TemporaryDirectory(prefix='.wup-update-',dir=destination.parent) as temp:
            new, old = Path(temp)/'new', Path(temp)/'old'
            yield new
            existed=destination.exists()
            if existed: destination.replace(old)
            try: new.replace(destination)
            except BaseException:
                if existed: old.replace(destination)
                raise


def make_update(source, destination, parts='both', version=None, now=None, base_shader_files=None):
    if parts not in PARTS: raise ValueError('Choose binary or binary + shaders')
    now = now or datetime.now(timezone.utc)
    if version is None: version=UPDATE_VERSION
    if not 0 <= version <= 65535: raise ValueError('Wii U versions range from 0 to 65535')
    source, destination = Path(source), Path(destination)
    if destination.resolve()==source.resolve() or source.resolve() in destination.resolve().parents or destination.resolve() in source.resolve().parents:
        raise ValueError('Update and base title folders must be separate')
    app = ET.parse(source/'code/app.xml')
    base = int(app.findtext('title_id'),16)
    from wup_safety import check_title_id
    check_title_id(base)
    if base>>32 != 0x00050000: raise ValueError('An update needs a base game title')
    native_base_id(base)  # Reject legacy identities before creating any output.
    meta = ET.parse(source/'meta/meta.xml')
    if int(meta.findtext('title_id'),16)!=base: raise ValueError('Base metadata title ID mismatch')
    if not list((source/'code').glob('*.rpx')): raise ValueError('The updated executable is missing')
    if parts=='both' and not all((source/'content'/n).is_file() for n in ('shaders.gsh','shaders.idx')):
        raise ValueError('A shader update requires both shaders.gsh and shaders.idx')
    if parts == 'binary' and base_shader_files is None:
        base_shader_files = {name: (source/'content'/name).stat().st_size for name in SHADER_FILES if (source/'content'/name).is_file()}
    if parts == 'binary':
        if not all(name in base_shader_files for name in SHADER_FILES[:2]):
            raise ValueError('Binary-only updates require links to the matching base shader files')
        for name, size in base_shader_files.items():
            if name not in SHADER_FILES or type(size) is not int or not 0 <= size <= 0xffffffff:
                raise ValueError('Invalid base shader inventory')
    update = base | (0xe<<32)
    now = now or datetime.now(timezone.utc)
    with title_output(destination,update) as new:
        out=Path(new)
        for d in ('code','content','meta'): (out/d).mkdir(parents=True)
        for file in (source/'code').iterdir():
            if file.suffix in ('.rpx','.rpl') or file.name=='cos.xml': shutil.copy2(file,out/'code'/file.name)
        for tag,value in [('title_id',f'{update:016X}'),('title_version',f'{version:04X}'),('app_type',APP_TYPE)]:
            node=app.find(tag)
            if node is None: raise ValueError('Missing app.xml field: '+tag)
            node.text=value
        app.write(out/'code/app.xml',encoding='utf-8',xml_declaration=True)
        for file in (source/'meta').iterdir():
            if file.name!='meta.xml': shutil.copy2(file,out/'meta'/file.name)
        node=meta.find('title_version')
        if node is None: raise ValueError('Missing meta.xml title_version')
        node.text=str(version)  # meta.xml identifies the base game, not 0005000E.
        meta.write(out/'meta/meta.xml',encoding='utf-8',xml_declaration=True)
        if parts=='both':
            for name in ('shaders.gsh','shaders.idx','fallback-shaders.bin'):
                file=source/'content'/name
                if file.is_file(): shutil.copy2(file,out/'content'/name)
        # Nonempty content also carries a diagnostic identity; never copies a ROM,
        # save import, user settings, or shaders into a binary-only update.
        details=dict(base_title_id=f'{base:016X}',update_title_id=f'{update:016X}',version=version,
                     built_at=now.isoformat(),parts=parts,version_policy='fixed')
        (out/'content/3dii-update.json').write_text(json.dumps(details,indent=2)+'\n')
        links={}
        for file in sorted((source/'content').rglob('*')):
            if file.is_symlink(): raise ValueError('Base content must not contain symbolic links')
            if file.is_file():
                name=file.relative_to(source/'content').as_posix()
                if not (out/'content'/name).exists(): links[name]=file.stat().st_size
        if parts == 'binary':
            links.update(base_shader_files)
        # PC-only input to the packer. These names become native FST link records,
        # not payload copies. Nintendo's update installer links them to the base.
        (out/'.3dii-base-links.json').write_text(json.dumps(dict(base_title_id=f'{base:016X}',files=links),indent=2)+'\n')
    return details
