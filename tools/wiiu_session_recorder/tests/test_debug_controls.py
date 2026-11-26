import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from runtime_channel import checked_command

class DebugControls(unittest.TestCase):
    def test_read_only_state_status(self):
        self.assertEqual(checked_command('state status'),'state status')

    def test_explicit_debug_commands(self):
        for c in ('state save','state save-ram','state load','state load-file','set pause 1','set pause 0',
                  'set speed 100','set speed 200','set speed 400','set copyrect 0','set lightguard 1',
                  'ram read 08000000','ram write 08000000 1234 abcd'):
            with self.assertRaises(ValueError): checked_command(c)
            self.assertEqual(checked_command(c,True),c)

    def test_malformed_or_unsupported_requests(self):
        for c in ('state clear','state load extra','state load\nquit','set pause 2','set speed 999999',
                  'ram read 8000001','ram write 8000000 01','ram write 8000000 01 02 extra',
                  'ram read 100000000','ram write 8000000 ff 100000000'):
            with self.assertRaises(ValueError): checked_command(c,True)

if __name__=='__main__': unittest.main()
