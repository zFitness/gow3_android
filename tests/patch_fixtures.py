"""补丁链的人工 SFO/SELF/XML，不依赖玩家游戏文件。"""
from pathlib import Path

from test_game_update import make_sfo
from test_prepare import fixture
from prepare import parse_self


TEXTURE = 'Bug Fix - Texture Corruption Fix'
STARTUP = 'Bug Fix - Startup Black Screen'
FPS120 = 'Frame Rate Patch - 120 FPS'
FPS240 = 'Frame Rate Patch - 240 FPS'
RES720 = 'Resolution Patch - 720p'
RES4K = 'Resolution Patch - 4K'


def make_game(root, title='CUSA01715', version='01.02'):
    game = Path(root) / 'game'
    (game / 'sce_sys').mkdir(parents=True, exist_ok=True)
    (game / 'sce_sys/param.sfo').write_bytes(make_sfo([
        ('TITLE_ID', title), ('APP_VER', version), ('TITLE', 'God of War III')]))
    (game / 'eboot.bin').write_bytes(fixture())
    out = Path(root) / 'out'
    out.mkdir(exist_ok=True)
    (out / 'eboot.elf').write_bytes(parse_self(fixture())[0])
    return game, out


def make_xml(root):
    rows = []
    for name, pattern, note, enabled in (
        (TEXTURE, '01 02 03', 'DMEM to 2048 MB', True),
        (STARTUP, '0a 0b 0c', '', False),
        (FPS120, '04 05 06', 'VBlank to 120 FPS', False),
        (FPS240, '04 05 06', 'VBlank to 240 FPS', False),
        (RES720, '07 08 09', 'DMEM to 2048 MB', False),
        (RES4K, '07 08 09', 'DMEM to 6144 MB', False),
    ):
        rows.append(f'<Metadata Name="{name}" AppVer="01.02" Note="{note}" '
                    f'isEnabled="{str(enabled).lower()}"><PatchList>'
                    f'<Line Type="bytes" Address="0x400001" Pattern="{pattern}" Value="90"/>'
                    '</PatchList></Metadata>')
    xml = Path(root) / 'fixture.xml'
    xml.write_text('<Patch>' + ''.join(rows) + '</Patch>')
    return xml


def profiles(xml):
    return [({'CUSA01623', 'CUSA01715', 'CUSA01720'}, xml, '01.02', 'God of War III')]
