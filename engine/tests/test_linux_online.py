# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the Linux launch path with local preparation/probe stand-ins, without networking."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from paths import ROOT


class LinuxOnlineStartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lantern online ')
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.engine = self.data / 'engine'
        scripts = self.engine / 'scripts'
        scripts.mkdir(parents=True)
        shutil.copy2(ROOT / 'run.sh', self.engine / 'run.sh')
        for name in ('online.py', 'online_env.py'):
            shutil.copy2(ROOT / 'scripts' / name, scripts / name)
        # Only the configuration and its handoff are real; no game dump or GPU is needed.
        (scripts / 'mods.py').write_text('import sys\nprint(sys.argv[1])\n')
        for name in ('prepare.py', 'link_libc.py', 'link_modules.py', 'content_profile.py',
                     'engine_features.py', 'patches.py'):
            (scripts / name).touch()
        self.config = self.data / 'lantern.ini'
        self.game = self.data / 'game'
        self.game.mkdir()
        (self.game / 'eboot.bin').touch()
        self.result = self.data / 'environment.json'
        self.probe = self.data / 'probe'
        self.probe.write_text(f'#!{sys.executable}\n' + '''import json, os
from pathlib import Path
keys = ('BB_ONLINE', 'BB_NET_BACKEND', 'BB_THD_SERVER', 'BB_THD_AUTH', 'BB_P2P_PORT',
        'BB_SIGN_TIMEOUT', 'BB_THD_RULESET', 'SHADPS4_HTTP_HOST_OVERRIDES_JSON')
result = {key: os.environ.get(key) for key in keys}
result['account_present'] = (os.environ.get('BB_THD_TOKEN') == 'synthetic-private-token'
                             and os.environ.get('BB_THD_ONLINE_ID') == 'TestHunter')
result['legacy_password_present'] = bool(os.environ.get('BB_SHADNET_PASSWORD'))
Path(os.environ['TEST_RESULT']).write_text(json.dumps(result))
''')
        self.probe.chmod(0o755)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(('BB_', 'SHADPS4_HTTP_', 'PYTHON'))}
        self.env.update(PYTHON=sys.executable, BB_PREBUILT='1', BB_SAVE_LOG='0',
                        BB_PROBE=str(self.probe), BB_CONFIG=str(self.config),
                        BB_DATA_DIR=str(self.data), BB_GAME_DIR=str(self.game),
                        TEST_RESULT=str(self.result), BB_ONLINE_PAUSED='0')
        self.online = 'online_enabled=1\nonline_connected=1\nonline_pool=huntersdream\n'

    def run_startup(self, settings, account=True, inherited=None):
        self.config.write_text(settings)
        env = dict(self.env)
        if account:
            env.update(BB_THD_TOKEN='synthetic-private-token', BB_THD_ONLINE_ID='TestHunter')
        env.update(inherited or {})
        result = subprocess.run(['bash', 'run.sh'], cwd=self.engine, env=env, text=True,
                                capture_output=True, timeout=15)
        self.assertNotIn('synthetic-private-token', result.stdout + result.stderr)
        self.assertNotIn('synthetic-legacy-password', result.stdout + result.stderr)
        return result

    def test_signed_in_account_and_redirects_reach_game(self):
        result = self.run_startup(self.online, inherited={
            'BB_SHADNET_PASSWORD': 'synthetic-legacy-password'})
        self.assertEqual(result.returncode, 0, result.stderr)
        env = json.loads(self.result.read_text())
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_NET_BACKEND'], 'thd')
        self.assertEqual(env['BB_THD_SERVER'], 'https://thehuntersdream.com:18671')
        self.assertEqual(env['BB_THD_AUTH'], 'https://thehuntersdream.com')
        self.assertEqual(env['BB_P2P_PORT'], '3658')
        self.assertEqual(env['BB_SIGN_TIMEOUT'], '30')
        self.assertTrue(env['account_present'])
        self.assertFalse(env['legacy_password_present'])
        overrides = Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'])
        self.assertEqual(json.loads(overrides.read_text())['*.scej-network.jp'],
                         'https://thehuntersdream.com')
        self.assertNotIn('synthetic-private-token', overrides.read_text())

    def test_offline_and_disconnected_clear_inherited_state(self):
        for settings in ('online_enabled=0\n', self.online.replace('online_connected=1',
                                                                 'online_connected=0')):
            with self.subTest(settings=settings):
                result = self.run_startup(settings, inherited={
                    'BB_ONLINE': '1', 'BB_NET_BACKEND': 'thd',
                    'BB_SHADNET_PASSWORD': 'synthetic-legacy-password',
                    'SHADPS4_HTTP_HOST_OVERRIDES_JSON': 'stale'})
                self.assertEqual(result.returncode, 0, result.stderr)
                env = json.loads(self.result.read_text())
                self.assertEqual(env['BB_ONLINE'], '0')
                self.assertIsNone(env['BB_NET_BACKEND'])
                self.assertIsNone(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'])
                self.assertFalse(env['account_present'])
                self.assertFalse(env['legacy_password_present'])

    def test_missing_account_stops_before_game_launch(self):
        result = self.run_startup(self.online, account=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Sign in', result.stderr)
        self.assertFalse(self.result.exists())

    def test_invalid_configuration_stops_before_game_launch(self):
        result = self.run_startup(self.online + 'online_game_api=https://example.invalid/path\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Online configuration:', result.stderr)
        self.assertFalse(self.result.exists())

    def test_shell_metacharacters_remain_literal(self):
        ruleset = 'vanilla$(touch SHOULD_NOT_EXIST)'
        result = self.run_startup(self.online + f'online_thd_ruleset={ruleset}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.result.read_text())['BB_THD_RULESET'], ruleset)
        self.assertFalse((self.engine / 'SHOULD_NOT_EXIST').exists())


if __name__ == '__main__':
    unittest.main()
