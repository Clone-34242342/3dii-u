#!/usr/bin/env python3
"""Packs titles into WUPs and puts them on the SD card, each copy compared with the original: ship_card.py <otp.bin> <card, like D:> [game ... | all]. The local copy goes once the card's is right.
The packer holds a whole game in memory, so each pack waits for the PC to have memory to spare. Its exit code is what counts: a failed one can leave a title.tmd behind.
Waits up to 12 hours for the card if it isn't in. WUP Installer's Install all takes everything in install/. The build it replaces goes to dist/previous on this PC.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import time
from wup_safety import check_output, check_title_replacement, publication_lock, stage_locked_outputs, staged_outputs, wup_title_id

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
FOLDERS = {'sticker_star': 'Paper_Mario_Sticker_Star', 'nsmb2': 'New_Super_Mario_Bros_2', 'dream_team': 'Mario_Luigi_Dream_Team', 'paper_jam': 'Mario_Luigi_Paper_Jam',
           'superstar_saga': 'Mario_Luigi_Superstar_Saga', 'bowsers_inside': 'Mario_Luigi_Bowsers_Inside_Story', 'layton_azran': 'Layton_Azran_Legacy', 'layton_mask': 'Layton_Miracle_Mask',
           'tomodachi': 'Tomodachi_Life'}
OUT = {'layton_mask': 'build_layton/layton_mask', 'layton_azran': 'build_layton/layton_azran', 'tomodachi': 'build'}
PREVIOUS = ROOT + '/dist/previous'  # the build each title had on the card before this one


def pack(otp, game):
    target = f'{ROOT}/dist/{FOLDERS[game]}'
    title = f'{ROOT}/{OUT.get(game, "build_mario/" + game)}/title'
    try:
        check_output(target, title, replace=True)
        with staged_outputs(target) as (new,):
            r = subprocess.run([sys.executable, ROOT + '/tools/pack_game_wup.py', otp, title, new], cwd=ROOT,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            if r.returncode:
                raise OSError(r.stdout[-200:])
    except (OSError, ValueError) as e:
        print(f'{game}: packing failed: {e}', flush=True)
        return None
    return target


def verified_copy(source, target):
    shutil.copytree(source, target)
    mismatches = filecmp.cmpfiles(source, target, os.listdir(source), shallow=False)[1:]
    if any(mismatches):
        raise OSError(f'The copy from {source} differs: {mismatches}')


def copy(source, card):
    name = os.path.basename(source)
    target = f'{card}/install/{name}'
    kept = f'{PREVIOUS}/{name}'
    check_output(target, source)
    check_output(kept, source)
    check_output(kept, target)
    try:
        with publication_lock(source, target, kept):
            check_output(source, replace=True)
            title_id = wup_title_id(source)
            check_title_replacement(target, title_id)
            # Keep the card's old build on the PC so it can be restored without a rebuild.
            have_previous = os.path.isdir(target) and bool(os.listdir(target))
            targets = [kept, target] if have_previous else [target]
            if have_previous:
                check_title_replacement(kept, title_id)
            names = os.listdir(source)
            with stage_locked_outputs(*targets) as staged:
                verified_copy(source, staged[-1])
                if have_previous:
                    verified_copy(target, staged[0])
            shutil.rmtree(source)
    except (OSError, ValueError) as e:
        return f'{name}: COPY FAILED: {e}'
    return f'{name}: on the card, compared, {len(names)} files'


def main():
    otp, card, *games = sys.argv[1:]
    card = card.rstrip('/\\')
    games = list(FOLDERS) if games in ([], ['all']) else games
    for _ in range(2160):
        if os.path.isdir(card + '/install'):
            break
        time.sleep(20)
    else:
        sys.exit('no card after 12 hours')
    bad = 0
    for game in games:
        packed = pack(otp, game)
        said = copy(packed, card) if packed else f'{game}: PACK FAILED'
        bad += 'FAILED' in said or 'DIFFERS' in said
        print(said, flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
