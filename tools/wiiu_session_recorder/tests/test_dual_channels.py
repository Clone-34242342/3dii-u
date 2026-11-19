import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recorder import Recorder, Controller


def wait_for(predicate, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('Timed out waiting for receiver state')


class RuntimeServer(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.done = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.client = None
        self.commands = []

    def run(self):
        pending = b''
        try:
            while not self.done.is_set():
                try:
                    if self.client is None:
                        self.client, _ = self.listener.accept()
                        self.client.settimeout(0.1)
                    data = self.client.recv(4096)
                    if not data:
                        self.client.close()
                        self.client = None
                        continue
                    pending += data
                    while b'\n' in pending:
                        command, pending = pending.split(b'\n', 1)
                        self.commands.append(command)
                        self.client.sendall(b'[runtime] reply ' + command + b'\n')
                except socket.timeout:
                    continue
                except OSError:
                    if self.client:
                        self.client.close()
                    self.client = None
        finally:
            if self.client:
                self.client.close()
            self.listener.close()


class DualChannelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.server = RuntimeServer()
        self.server.start()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.rec = Recorder(Path(self.tmp.name))
        self.controller = Controller(self.rec, '127.0.0.1', aroma_tcp_port=0,
                                     aroma_udp_port=self.udp.getsockname()[1], runtime_port=self.server.port)
        self.aroma = None

    def tearDown(self):
        self.controller.command('disconnect')
        self.controller.command('stop')
        if self.aroma:
            self.aroma.close()
        self.server.done.set()
        self.server.join(2)
        self.udp.close()
        self.tmp.cleanup()

    def connect(self):
        self.controller.command('connect')
        port = self.controller.receiver.listener.getsockname()[1]
        self.aroma = socket.create_connection(('127.0.0.1', port))
        wait_for(lambda: self.rec.connected and self.rec.runtime_connected)

    def test_shared_buttons_and_session_rotation(self):
        self.connect()
        first = Path(self.controller.command('start')['session'])
        self.aroma.sendall(b'AROMA-FIRST\0\r\n')
        wait_for(lambda: self.rec.bytes and self.rec.runtime_bytes)
        state = self.controller.command('mark')
        self.controller.command('stop')
        self.assertIn(b'AROMA-FIRST', (first/'console.log').read_bytes())
        self.assertNotIn(b'\0', (first/'console.log').read_bytes())
        self.assertIn(b'[runtime]', (first/'runtime.log').read_bytes())
        events = [json.loads(l) for l in (first/'events.jsonl').read_text().splitlines()]
        marker = next(x for x in events if x['event']=='graphics_glitch')
        self.assertGreater(marker['runtime_offset'], 0)
        old_size = (first/'runtime.log').stat().st_size
        old_last = self.rec.runtime_last_data
        self.server.client.sendall(b'DISCARD-WHILE-STOPPED\n')
        wait_for(lambda: self.rec.runtime_last_data != old_last)
        self.assertEqual((first/'runtime.log').stat().st_size, old_size)
        second = Path(self.controller.command('start')['session'])
        self.assertNotEqual(first, second)
        self.aroma.sendall(b'AROMA-SECOND\n')
        wait_for(lambda: self.rec.runtime_bytes and self.rec.bytes)
        self.controller.command('runtime', 'mem 80000000 80')
        wait_for(lambda: b'mem 80000000 80' in self.server.commands)
        with self.assertRaises(ValueError):self.controller.command('runtime', 'relaunch')
        self.controller.command('disconnect')
        state = self.controller.command('status')
        self.assertFalse(state['connected'])
        self.assertFalse(state['runtime_connected'])
        self.assertFalse(state['listening'])
        self.assertFalse(state['runtime_listening'])
        self.controller.command('stop')
        self.assertNotIn(b'DISCARD-WHILE-STOPPED', (second/'runtime.log').read_bytes())

    def test_runtime_reconnect_and_persistent_owner_snapshot(self):
        self.connect()
        self.controller.command('start')
        wait_for(lambda: b'st' in self.server.commands)
        for _ in range(2):
            self.server.client.sendall(b'[mem] 80000020 10001000 00000000 00000000 00000001\n')
            time.sleep(0.05)
        wait_for(lambda: b'mem 10001000 700' in self.server.commands)
        before = len(self.server.commands)
        self.server.client.shutdown(socket.SHUT_RDWR)
        wait_for(lambda: len(self.server.commands)>before)
        wait_for(lambda: self.rec.runtime_connected)
        self.controller.command('stop')
        events = [json.loads(l) for l in (self.rec.session/'events.jsonl').read_text().splitlines()]
        self.assertTrue(any(e['event']=='persistent_owner_observed' for e in events))
        self.assertTrue(any(e['event']=='runtime_disconnected' for e in events))
        self.assertGreaterEqual(sum(e['event']=='runtime_connected' for e in events), 1)

    def test_renderer_controls_require_explicit_action_and_are_bounded(self):
        self.connect()
        self.controller.command('start')
        with self.assertRaises(ValueError): self.controller.command('runtime', 'set cpuvs 1')
        for command in ['set cpuvs 1', 'set cpuvs 0', 'set vtxcache 0', 'set trace 64', 'set cpuattr 15', 'set drawtrace 1']:
            self.controller.command('renderer', command)
            wait_for(lambda: command.encode() in self.server.commands)
        for command in ['set trace 257', 'set cpuattr 16', 'set drawtrace 2', 'set cpuvs 2', 'fw 1', 'relaunch', 'set uberall 1']:
            with self.assertRaises(ValueError): self.controller.command('renderer', command)

    def test_discovers_split_mutex_banner_and_captures_owner(self):
        self.connect()
        self.controller.command('start')
        wait_for(lambda: b'st' in self.server.commands)
        self.aroma.sendall(b'[AFR4 MM] MUTEX index=0 address=10')
        self.aroma.sendall(b'002000 owner_offset=1C size=2C\r\n')
        wait_for(lambda: 0x1000201c in self.rec.heap_owner_fields())
        wait_for(lambda: b'mem 1000201c 20' in self.server.commands)
        for _ in range(2):
            self.server.client.sendall(b'[mem] 1000201c 10001000 00000001 00000000\n')
            time.sleep(0.05)
        wait_for(lambda: b'mem 10001000 700' in self.server.commands)
        self.controller.command('stop')
        events=[json.loads(l) for l in (self.rec.session/'events.jsonl').read_text().splitlines()]
        self.assertTrue(any(e.get('lock')=='heap-mutex-0' for e in events))
        self.aroma.sendall(b'Running MemoryMappingModule old-build\n')
        wait_for(lambda: len(self.rec.heap_owner_fields())==1)

    def test_disconnect_button_stops_runtime_after_aroma_receiver_ends(self):
        self.connect()
        self.controller.receiver.done.set()
        self.controller.receiver.join(2)
        self.assertTrue(self.controller.runtime_receiver.is_alive())
        result=self.controller.command('toggle_connection')
        self.assertFalse(result['runtime_listening'])
        self.assertFalse(result['runtime_connected'])

    def test_aroma_works_while_runtime_unavailable(self):
        self.controller.runtime_port = 1
        self.controller.command('start')
        self.controller.command('connect')
        self.aroma = socket.create_connection(('127.0.0.1', self.controller.receiver.listener.getsockname()[1]))
        self.aroma.sendall(b'AROMA-ONLY\n')
        wait_for(lambda: self.rec.bytes>0)
        self.assertTrue(self.rec.connected)
        self.assertFalse(self.rec.runtime_connected)
        self.assertTrue(self.controller.command('status')['runtime_listening'])


if __name__ == '__main__':unittest.main()
