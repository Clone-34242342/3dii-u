"""Self-check for covers.steamgriddb against a stand-in for the site on this PC, answering as its API description says it does
(steamgriddb.com/api/v2): which game, banner and icon it takes, that each is fetched once, that the key doesn't follow a redirect, and
what a wrong key, an unknown game and no connection give. Nothing leaves this PC. Run: python builder/test_covers.py"""
import http.server
import json
import os
import sys
import tempfile
import threading
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import covers  # noqa: E402

PNG, JPG = b'\x89PNG\r\n\x1a\n' + bytes(32), b'\xff\xd8\xff\xe0' + bytes(32)
GAMES = {'pokemon ultra': [{'id': 7, 'name': 'Pokémon Ultra Moon'}, {'id': 8, 'name': 'Pokemon Ultra Sun'}],
         'bare': [{'id': 9, 'name': 'Bare'}]}
asked = []


class Site(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        path = urllib.parse.unquote(url.path)
        asked.append(path)
        here = f'http://127.0.0.1:{self.server.server_port}'
        if path.startswith('/img/'):
            return self.send(200, JPG if path.endswith('wide') else PNG, 'image/png')
        if self.headers.get('Authorization') != 'Bearer good':
            return self.send(401, b'{"success":false,"errors":["Unauthorized"]}')
        if path == '/search/autocomplete/moved':
            return self.send(302, b'', location=here + '/search/autocomplete/bare')
        data = None
        if path.startswith('/search/autocomplete/'):
            data = next((g for term, g in GAMES.items() if covers.plain(path.rsplit('/', 1)[1]).startswith(covers.plain(term))), [])
        elif path == '/grids/game/8' and '920x430' in query['dimensions'][0]:
            data = [{'width': 460, 'height': 215, 'url': here + '/img/small'}, {'width': 920, 'height': 430, 'url': here + '/img/wide'},
                    {'width': 920, 'height': 430, 'url': here + '/img/second'}]
        elif path == '/icons/game/8':
            data = [{'width': 64, 'height': 64, 'url': here + '/img/tiny'}, {'width': 256, 'height': 256, 'url': here + '/img/icon'}]
        elif path == '/grids/game/9' and '512x512' in query['dimensions'][0]:
            data = [{'width': 512, 'height': 512, 'url': here + '/img/square'}]
        elif path in ('/grids/game/9', '/icons/game/9'):
            data = []
        if data is None:
            return self.send(404, b'{"success":false}')
        self.send(200, json.dumps({'success': True, 'data': data}).encode())

    def send(self, code, body, kind='application/json', location=None):
        self.send_response(code)
        if location:
            self.send_header('Location', location)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Site)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_port}'
with tempfile.TemporaryDirectory() as tmp:
    # the game of that name, not the site's first; the first banner of the larger size; the first icon big enough
    got = covers.steamgriddb('Pokémon Ultra Sun', 'good', tmp, '00040000001b5000', base)
    assert got == {'banner': os.path.join(tmp, 'sgdb_00040000001b5000_banner.jpg'), 'icon': os.path.join(tmp, 'sgdb_00040000001b5000_icon.png')}, got
    assert '/img/wide' in asked and '/img/icon' in asked and '/img/small' not in asked and '/img/tiny' not in asked, asked
    assert open(got['banner'], 'rb').read() == JPG and open(got['icon'], 'rb').read() == PNG
    before = len(asked)
    assert covers.steamgriddb('Pokémon Ultra Sun', 'good', tmp, '00040000001b5000', base) == got and len(asked) == before  # kept: nothing asked again

    # no wide grid and no icon: no banner, and a square grid stands in for the icon
    bare = covers.steamgriddb('bare', 'good', tmp, 'b', base)
    assert bare == {'banner': None, 'icon': os.path.join(tmp, 'sgdb_b_icon.png')} and '/img/square' in asked, bare

    assert covers.steamgriddb('Nothing by this name', 'good', tmp, 'n', base) == {'banner': None, 'icon': None}
    # a subtitle more or less is the same game; the site's other answers are other games, and none of them is taken
    assert covers.steamgriddb('Bare: Extended Cut', 'good', tmp, 'sub', base)['icon'] == os.path.join(tmp, 'sgdb_sub_icon.png')
    assert covers.steamgriddb('Pokémon Ultra Violet', 'good', tmp, 'other', base) == {'banner': None, 'icon': None}
    before = len(asked)  # a redirect isn't followed: the key would go along with it
    assert covers.steamgriddb('moved', 'good', tmp, 'moved', base) == {'banner': None, 'icon': None} and len(asked) == before + 1, asked[before:]
    try:
        covers.steamgriddb('bare', 'wrong', tmp, 'k', base)
        raise AssertionError('a wrong key went through')
    except covers.BadKey:
        pass
    server.shutdown()
    server.server_close()
    assert covers.steamgriddb('bare', 'good', tmp, 'offline', base) is None  # no connection, nothing kept
    assert covers.steamgriddb('bare', 'good', tmp, 'b', base) == bare        # what's kept still serves without one... its banner was never there
assert covers.plain('Pokémon  ULTRA-Sun™') == 'pokemonultrasun' and covers.game_code('CTR-P-AMKE') == 'AMKE' and covers.game_code('x') is None
print('covers: ok')
