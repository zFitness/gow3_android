"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
The built-in file's lines carry signatures (Pattern/At) that find each site in any build.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.02"), or None when it cannot be read."""
    return game_sfo(game).get('APP_VER')


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def eboot_image(elf):
    """The PT_LOAD file bytes of eboot.elf at their virtual addresses (image offsets)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize) for i in range(phnum)]
    loads=[(vaddr,offset,filesz) for kind,_,offset,vaddr,_,filesz,_,_ in loads if kind==1]
    image=bytearray(max(vaddr+filesz for vaddr,_,filesz in loads))
    for vaddr,offset,filesz in loads: image[vaddr:vaddr+filesz]=elf[offset:offset+filesz]
    return bytes(image)


# Signatures find a patch site in any build whose code matches, wherever the code moved: the
# original bytes around the site, "??" where a build may differ (rip-relative displacements,
# rel32 branch targets). tools/make_signatures.py writes them from the reference build.
def parse_pattern(text):
    return [None if b=='??' else int(b,16) for b in text.split()]


def find_pattern(image, pattern, limit=2):
    """Image offsets where pattern matches, at most limit of them."""
    runs,start=[],None
    for i,b in enumerate(pattern+[None]):
        if b is not None and start is None: start=i
        if b is None and start is not None: runs.append((start,i)); start=None
    if not runs: raise ValueError('pattern without fixed bytes')
    a,b=max(runs,key=lambda r:r[1]-r[0])
    anchor=bytes(pattern[a:b])
    found,at=[],image.find(anchor)
    while at>=0 and len(found)<limit:
        s=at-a
        if s>=0 and s+len(pattern)<=len(image) and all(p is None or image[s+i]==p for i,p in enumerate(pattern)):
            found.append(s)
        at=image.find(anchor,at+1)
    return found


def locate(image, element, prefix=''):
    """实际映像必须唯一匹配特征码；无映像的参考编译才允许 Address。"""
    pattern=element.get(prefix+'Pattern')
    if not pattern:
        if image is not None:
            raise ValueError('missing signature')
        return int(element.get('Address'),0)-EBOOT_BASE
    found=find_pattern(image,parse_pattern(pattern))
    if len(found)!=1:
        raise LookupError('signature not found' if not found else 'signature found more than once')
    return found[0]+int(element.get(prefix+'At','0'),0)


def resolve_targets(image, meta, sites):
    """Image offsets of a patch's named Targets: a site (Pattern/At), or the address a rip-relative
    or rel32 operand points to (Ref: the operand's offset in its Pattern; Tail: the instruction's
    bytes after the operand)."""
    targets={}
    for target in meta.iter('Target'):
        name=target.get('Name')
        if target.get('Line'):
            index=int(target.get('Line'))
            if not 0 <= index < len(sites) or sites[index] is None:
                raise ValueError('invalid target line reference')
            targets[name]=sites[index]
            continue
        at=locate(image,target)
        if target.get('Ref'):
            if at < 0 or at + 4 > len(image):
                raise ValueError('reference operand outside the image')
            disp,=struct.unpack_from('<i',image,at)
            at=at+4+int(target.get('Tail','0'))+disp
        targets[name]=at
    return targets


def fill(value, at, targets):
    """Value bytes with each {rel:NAME:TAIL} replaced by the rel32 to target NAME, measured from the
    end of the instruction (TAIL bytes after the operand)."""
    out=bytearray()
    for part in re.split(r'(\{[^}]*\})',value.replace(' ','')):
        if not part.startswith('{'):
            out+=bytes.fromhex(part)
            continue
        _,name,tail=part[1:-1].split(':')
        out+=struct.pack('<i',targets[name]-(at+len(out)+4+int(tail)))
    return bytes(out)


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments, image=None, skipped=None):
    """Writes of the named patches. With the game's image, each site is found by its signature
    (any build); without it, at the reference build's Address. A Line with a Target writes at that
    Target instead: data the code points to, such as a .bss variable (no bytes of its own to find).
    A patch whose sites are not all found is left out whole, with the reason in skipped[name].
    app_version None: any version."""
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and app_version in (None,meta.get('AppVer')) and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    writes=[]
    for name in dict.fromkeys(names):
        try:
            if name not in found:
                raise ValueError(f'patch not found for app version {app_version}')
            lines=list(found[name].iter('Line'))
            if not lines:
                raise ValueError('empty patch')
            sites=[(None if line.get('Target') else locate(image,line)) if image is not None else int(line.get('Address'),0)-EBOOT_BASE
                   for line in lines]
            targets=(resolve_targets(image,found[name],sites) if image is not None else
                     {t.get('Name'):int(t.get('Address'),0)-EBOOT_BASE for t in found[name].iter('Target')})
            if any(not any(start <= target < end for start,end in segments)
                   for target in targets.values()):
                raise ValueError('target outside the eboot')
            sites=[targets[line.get('Target')] if line.get('Target') else site for line,site in zip(lines,sites)]
            ours=[]
            for line,offset in zip(lines,sites):
                data=fill(line.get('Value'),offset,targets) if '{' in line.get('Value','') else encode(line)
                if not 0 < len(data) <= 4096 or not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {offset+EBOOT_BASE:#x} or write length is outside the eboot')
                ours.append((offset,data))
        except (LookupError, ValueError, TypeError, struct.error, OverflowError) as error:
            if skipped is None: raise ValueError(f'{name}: {error}')
            skipped[name]=str(error)
            continue
        # 整项验证完成后才提交，避免后续行失败时留下部分写入。
        writes.extend(ours)
    return writes


PATCHES_DIR=Path(__file__).resolve().parent.parent/'patches'


def _scan_profiles():
    """受支持版本集由补丁目录派生：Title ID 与 AppVer 都在补丁定义里声明。

    代码里不再维护第二份标题清单——新增一个受支持版本只需要新增一份补丁定义。
    """
    import xml.etree.ElementTree as ET
    profiles=[]
    for path in sorted(PATCHES_DIR.glob('*.xml')):
        try:
            root=ET.parse(path).getroot()
        except (OSError, ET.ParseError):
            continue
        ids={e.text.strip() for e in root.findall('.//TitleID/ID') if e.text}
        titles=[m.get('Title') for m in root.findall('Metadata') if m.get('Title')]
        versions=sorted({m.get('AppVer') for m in root.findall('Metadata') if m.get('AppVer')})
        label=titles[0] if titles else ''
        for title in sorted(ids):
            for version in versions:
                profiles.append(({title}, path, version, label))
    return profiles


# Title ID 与 APP_VER 决定候选资格；每项特征码决定实际适用性。
GAMES=_scan_profiles()
# The build the built-in Address values and third-party patch files are written for.
REFERENCE_SHA256='d85c8135d330c3b601bf5dc3f1dd86bd6119fb8a9fce372bed2c9785f9a79299'
DMEM_RETAIL_MB=5056  # runtime_memory.c POOL_SIZE default


def game_sfo(game):
    """The values of the game folder's param.sfo, or {} when it cannot be read (prepare.read_sfo)."""
    if not game:
        return {}
    try:
        from prepare import read_sfo
    except ImportError:
        return {}
    return read_sfo(Path(game) / 'sce_sys/param.sfo') or {}


def game_title_id(game):
    return game_sfo(game).get('TITLE_ID')


def game_profile(title_id, title=None, *, app_version=None):
    """按 Title ID 和版本选目录；未知版本保留默认记录，由身份门禁明确拒绝。"""
    profiles=[g for g in GAMES if title_id in g[0]]
    return next((g for g in profiles if g[2]==app_version), profiles[0] if profiles else None)


def game_profile_for(game):
    """读取 SFO 的明确 Title ID，不根据目录名或标题文本猜测。"""
    values=game_sfo(game) if game else {}
    return game_profile(values.get('TITLE_ID'),values.get('TITLE'),app_version=values.get('APP_VER'))


def eboot_sha256(game):
    try:
        path=Path(game)/'eboot.bin'
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except (OSError, TypeError):
        return None


def game_patchable(game):
    """仅表示可尝试逐项验证，不能当作补丁已经生效。"""
    return patch_profile_status(game)['reason']=='eligible'


def patch_profile_status(game):
    """只读身份预检查；位点尚未扫描，eligible 不等于 validated。"""
    title=game_title_id(game)
    version=game_app_version(game)
    profile=game_profile(title,app_version=version)
    result={'title_id':title,'app_version':version,'supported':bool(profile and version==profile[2]),
            'eboot_sha256':None,'eboot_matches':None,'patch_sites_match':None,'reason':None}
    result['eboot_sha256']=eboot_sha256(game)
    if result['eboot_sha256'] is None:
        result['reason']='missing-or-unreadable-eboot'
        return result
    if not profile:
        result['reason']='unsupported-title'
    elif version!=profile[2]:
        result['reason']='unsupported-app-version'
    else:
        result['reason']='eligible'
    return result


def patch_status(out):
    """The last start's patch report (patches.py writes it): {'applied': [...], 'skipped': {...}}."""
    try:
        return json.loads((Path(out)/'patch_status.json').read_text())
    except (OSError, ValueError):
        return None


def available_patches(xml, app_version):
    """Names of the patches the XML has for this app version."""
    return {m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version}


# God of War III notes: the texture fix and the resolution patches both resize the video arena;
# enable one, never both. A chosen resolution patch replaces the default texture fix.
EXCLUSIVE_PREFIX='Resolution Patch'
REPLACED_BY_EXCLUSIVE={'Bug Fix - Texture Corruption Fix'}
# Fixes with nothing to choose: not offered in the launcher and applied even with its saved list,
# which predates them (the startup black screen fix sets a value the game overwrites at once).
ALWAYS_ON={'Bug Fix - Startup Black Screen'}


def selected_patches(xml, app_version, extra, only=False):
    """The file's isEnabled patches for this version, then the names in extra (";"-separated).
    only: extra is the whole selection (the launcher's), without the file's defaults but with
    the ALWAYS_ON ones."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    names=[] if only else [m.get('Name') for m in metas if m.get('isEnabled','false').lower()=='true']
    names+=[n for n in (n.strip() for n in extra.split(';')) if n and n not in names]
    # 各身份可显式关闭常开策略，旧 XML 未声明时保留既有行为。
    names+=[m.get('Name') for m in metas
            if m.get('AlwaysOn',str(m.get('Name') in ALWAYS_ON)).lower()=='true'
            and m.get('Name') not in names]
    exclusive=[n for n in names if n.startswith(EXCLUSIVE_PREFIX)]
    if len(exclusive)>1:
        raise ValueError(f'choose one resolution patch, not {exclusive}')
    fps=[n for n in names if n.startswith('Frame Rate Patch')]
    if len(fps)>1:
        raise ValueError(f'choose one frame rate patch, not {fps}')
    return [n for n in names if not (exclusive and n in REPLACED_BY_EXCLUSIVE)]


def patch_requirements(xml, names, app_version):
    """(direct memory MiB, VBlank Hz) the selected patches' notes ask for, None when not needed.
    shadPS4 notes say "Requires setting DMEM to N MB": N is on top of the retail pool."""
    dmem=vblank=None
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        note=meta.get('Note','')
        if m:=re.search(r'DMEM to (\d+) MB',note): dmem=max(dmem or 0,DMEM_RETAIL_MB+int(m.group(1)))
        if m:=re.search(r'VBlank to (\d+) FPS',note): vblank=max(vblank or 0,int(m.group(1)))
    return dmem,vblank


def external_patches(directory, app_version, exclude, ids):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml (third-party
    shadPS4/GoldHEN files in the data directory's patches/ folder).
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        # 老 runtime 可能遗留共享目录文件，不将其再次作为第三方默认补丁加载。
        if path.name=='God_of_War_III_Remastered.xml':
            continue
        if path.resolve()==Path(exclude).resolve() or path.resolve() in {g[1].resolve() for g in GAMES}:
            continue  # built-in files (patches/ in a checkout) are not also offered as external
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        file_ids={e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids&ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments, skipped=None, applied=None):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not 0 < len(data) <= 4096 or not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except (ValueError, TypeError, OverflowError, struct.error) as error:
            if skipped is not None: skipped[key]=str(error)
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        if applied is not None: applied.append(key)
        print(f'Patches: external {key} ({len(ours)} writes)',file=sys.stderr)
    return writes


def patch_blob(writes):
    # 保留 loader 协议及完整指针槽的重定位规则。
    blob=struct.pack('<8sQQ',b'G3PATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    return blob


def atomic_write(path, data):
    """临时文件完整写出后替换；调用方必须在所有产物发布成功后才启动游戏。"""
    path=Path(path)
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.'+path.name+'.', delete=False) as f:
            temporary=Path(f.name)
            f.write(data)
        os.replace(temporary,path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_patches(out, writes):
    atomic_write(Path(out)/'patches.bin',patch_blob(writes))


def checked_output(game, out):
    """派生目录不能位于玩家游戏目录内，包括符号链接别名。"""
    game,out=Path(game).resolve(),Path(out).resolve()
    if out==game or game in out.parents:
        raise ValueError('output directory must be outside the game directory')
    return out


def prepared_elf(game, out, expected_sha):
    """从当前 SELF 核对派生 ELF，避免把旧映像与本轮身份拼在一起。"""
    from prepare import parse_self
    source=(Path(game)/'eboot.bin').read_bytes()
    if hashlib.sha256(source).hexdigest()!=expected_sha:
        raise ValueError('game source changed during patch preparation')
    elf=parse_self(source)[0]
    if (Path(out)/'eboot.elf').read_bytes()!=elf:
        raise ValueError('prepared eboot does not match current game; prepare it again')
    return bytes(elf)


def positive_env(env, key):
    value=str(env.get(key,''))
    return int(value) if value.isdigit() and int(value)>0 else None


def compile_plan(game, out, names=None, *, extra='', only=False, requested=None,
                 disabled_reason=None, env=None, base_dmem=DMEM_RETAIL_MB,
                 patches_dir=None, patches_config=None):
    """统一启动和独立 CLI 的门禁，返回本轮报告与字节写入，不执行游戏。"""
    out=checked_output(game,out)
    env={} if env is None else env
    identity=patch_profile_status(game)
    reason=identity['reason']
    if reason=='missing-or-unreadable-eboot':
        raise ValueError(reason)
    profile=game_profile(identity['title_id'],app_version=identity['app_version'])
    disabled=reason if reason!='eligible' else disabled_reason
    requested=list(requested if requested is not None else
                   (names if names is not None else filter(None,(n.strip() for n in extra.split(';')))))
    selected=list(dict.fromkeys(names or []))
    skipped={}
    writes=[]
    applied=[]
    external={'applied':[], 'skipped':{}, 'reason':None}
    budget=positive_env(env,'SPARTAN_DEVICE_MEM_MB')
    if not disabled:
        try:
            if names is None:
                selected=selected_patches(profile[1],profile[2],extra,only)
            # 即使调用方提供了已解析列表，也必须在最终编译门禁核对互斥。
            for prefix in ('Resolution Patch','Frame Rate Patch'):
                if sum(n.startswith(prefix) for n in selected)>1:
                    raise ValueError('patch-conflict')
            if any(n.startswith('Resolution Patch') for n in selected):
                selected=[n for n in selected if n not in REPLACED_BY_EXCLUSIVE]
        except ValueError:
            disabled='patch-conflict'
        required,_=patch_requirements(profile[1],selected,profile[2])
        if not disabled and required and budget and required>budget:
            disabled='insufficient-memory-budget'
    if not disabled:
        elf=prepared_elf(game,out,identity['eboot_sha256'])
        segments=eboot_segments(elf)
        writes=compile_patches(profile[1],selected,profile[2],segments,eboot_image(elf),skipped)
        applied=[n for n in selected if n not in skipped]
        identity['patch_sites_match']=not skipped if selected else None
        if patches_dir:
            # 1740 仅开放逐项签名校验的内置目录，不放行第三方固定地址写入。
            if identity['title_id']=='CUSA01740':
                external['reason']='profile-disallows-external-patches'
            elif identity['eboot_sha256']!=REFERENCE_SHA256:
                external['reason']='non-reference-eboot'
            else:
                found=external_patches(patches_dir,profile[2],profile[1],{identity['title_id']})
                writes+=compile_external(external_selection(found,patches_config),segments,
                                         external['skipped'],external['applied'])
    else:
        skipped={n:disabled for n in dict.fromkeys(requested+selected)}
        external['reason']=disabled
    minimum,fps=patch_requirements(profile[1],applied,profile[2]) if profile else (None,None)
    explicit_dmem=positive_env(env,'SPARTAN_DMEM_MB')
    explicit_vblank=positive_env(env,'SPARTAN_VBLANK_HZ')
    dmem=max(base_dmem,minimum or 0,explicit_dmem or 0)
    resolution=next((n.removeprefix('Resolution Patch - ') for n in applied
                     if n.startswith('Resolution Patch - ')), 'native')
    effective={'render_resolution':resolution, 'engine_fps':fps or 60,
               'vblank_hz':explicit_vblank or fps or 60,
               'vblank_source':'override' if explicit_vblank else 'compiled-patches',
               'dmem_mb':dmem, 'minimum_dmem_mb':minimum,
               'dmem_source':('override' if explicit_dmem and dmem==explicit_dmem else
                              'compiled-patches' if minimum and dmem==minimum else 'platform-default'),
               'device_mem_mb':budget, 'budget_checked':budget is not None}
    reason=disabled or ('partially-compiled' if skipped and applied else
                        'no-applicable-patches' if skipped else 'compiled' if writes else 'no-patches')
    report={'schema':'gow3.patch-status.v1', **identity, 'identity':identity,
            'sha256':identity['eboot_sha256'], 'reason':reason, 'phase':'compiled',
            'requested':requested, 'selected':selected, 'applied':applied, 'skipped':skipped,
            'disabled_reason':disabled, 'external':external, 'effective':effective,
            'write_count':len(writes), 'write_bytes':sum(len(data) for _,data in writes)}
    return report,writes


def publish_plan(out, report, writes):
    blob=patch_blob(writes)
    report['patches_sha256']=hashlib.sha256(blob).hexdigest()
    atomic_write(Path(out)/'patches.bin',blob)
    atomic_write(Path(out)/'patch_status.json',(json.dumps(report,indent=2)+'\n').encode())


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('SPARTAN_GAME_DIR','../CUSA01623')))
    a=p.parse_args()
    try:
        report,writes=compile_plan(a.game_dir,a.out,extra=a.extra,
                                  only=os.environ.get('SPARTAN_PATCHES_ONLY')=='1',env=os.environ,
                                  patches_dir=a.patches_dir,patches_config=a.patches_config)
        publish_plan(a.out,report,writes)
    except (OSError, ValueError, struct.error) as error:
        p.exit(1,f'Patches: {error}\n')
    print(f'Patches: {report["title_id"]}; {report["reason"]}; {len(writes)} writes')

if __name__=='__main__':
    main()
