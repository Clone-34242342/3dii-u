"""Runs without connecting to a user's console or touching their settings."""
import json
from pathlib import Path
import tempfile
import threading
import socket
import tkinter as tk
from unittest.mock import patch
from recorder import Recorder, Controller, WindowsControlServer, control, gui

def run(report):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        controller = Controller(Recorder(root / 'logs'), '127.0.0.1')
        with WindowsControlServer(root / 'control.json', controller) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                assert control('start', path=root/'control.json', windows=True)['recording']
                assert control('mark', 'test marker', path=root/'control.json', windows=True)['markers'] == 1
                assert not control('stop', path=root/'control.json', windows=True)['recording']
                with socket.create_connection(server.server_address, timeout=3) as bad:
                    bad.sendall(b'{"action":"start","token":"wrong"}\n')
                    assert 'error' in json.loads(bad.makefile('rb').readline())
            finally:
                server.shutdown()
                thread.join(2)
        assert not (root/'control.json').exists()
        original = tk.Tk.mainloop
        def limited(self):
            self.after(700, self.destroy)
            original(self)
        with patch.object(tk.Tk, 'mainloop', limited): gui(controller, False)
        sessions = list((root/'logs').glob('*'))
        assert len(sessions) == 1
        assert (sessions[0]/'events.jsonl').exists()
        report.write_text(json.dumps({'platform':__import__('sys').platform, 'gui':'passed', 'control':'authenticated loopback passed', 'record_start_mark_stop':'passed', 'continuous_screenshots':controller.recorder.periodic_capture})+'\n')
