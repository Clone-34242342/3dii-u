"""Shared SD-card room configuration, emitted for every output format."""
import json
from pathlib import Path


def write_config(sd_root, defaults):
    settings = json.loads(Path(defaults).read_text(encoding='utf-8'))
    host = settings['host']
    port = settings['port']
    if (not isinstance(host, str) or any(c not in
            'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in host)):
        raise ValueError('Invalid multiplayer room hostname')
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise ValueError('Invalid multiplayer room port')
    target = Path(sd_root) / '3dii-u' / 'multiplayer-room.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        with target.open('x', encoding='utf-8', newline='\n') as output:
            output.write(f'host={host}\nport={port}\nnickname=auto\nconsole_id=auto\npassword=\n')
    except FileExistsError:
        pass  # The user's shared endpoint takes precedence over builder defaults.
    return target
