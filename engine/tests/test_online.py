# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import os
os.environ['BB_ONLINE_PAUSED'] = '0'
from online import (configure_online, game_server, http_base, ONLINE_ENV, GAME_STATUS_ORIGIN, HUNTERS_DREAM,
                    PLAY_LOG_HOSTS, DEFAULT_P2P_PORT, DEFAULT_SIGN_TIMEOUT, SHADNET_HOST, SHADNET_PORT,
                    SHADNET_WEBAPI)


def play_logs(host):
    return {h: f'https://{host}:18671' for h in PLAY_LOG_HOSTS}


class OnlineProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.settings = dict(online_enabled='1', online_connected='1',
            online_host='server.example', online_port='31413',
            online_webapi='http://server.example:31415', online_username='Hunter',
            online_upnp='0')
        self.env = {'BB_SHADNET_PASSWORD': 'synthetic-test-password', 'UNRELATED': 'keep'}

    def test_offline_clears_inherited_online_credentials_and_override(self):
        for settings in ({}, {'online_enabled':'0'}, {'online_enabled':'1','online_connected':'0'}):
            env = dict.fromkeys(ONLINE_ENV, 'inherited')
            env['UNRELATED'] = 'keep'
            self.assertIsNone(configure_online(settings, self.data, env))
            self.assertEqual(env, {'BB_ONLINE':'0','UNRELATED':'keep'})
            self.assertFalse((self.data/'network').exists())

    def test_custom_ports_preserved_and_only_game_origin_and_play_logs_redirected(self):
        self.settings['online_game_api'] = 'https://game.example:31443'
        self.settings['online_p2p_port'] = '32000'
        path = configure_online(self.settings, self.data, self.env)
        self.assertEqual(json.loads(path.read_text()),
                         {GAME_STATUS_ORIGIN:'https://game.example:31443', **play_logs('game.example')})
        self.assertEqual(self.env['BB_SHADNET_SERVER'], 'server.example:31413')
        self.assertEqual(self.env['BB_SHADNET_WEBAPI'], 'http://server.example:31415')
        self.assertEqual(self.env['BB_P2P_PORT'], '32000')
        self.assertEqual(self.env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(path.resolve()))
        self.assertEqual(self.env['UNRELATED'], 'keep')
        self.assertNotIn('synthetic-test-password', path.read_text())
        self.assertFalse(path.with_suffix('.json.tmp').exists())

    def test_game_server_defaults_and_pinned_port(self):
        self.env['BB_P2P_PORT'] = 'inherited'
        path = configure_online(self.settings, self.data, self.env)
        self.assertEqual(json.loads(path.read_text()), {GAME_STATUS_ORIGIN:HUNTERS_DREAM, **play_logs('thehuntersdream.com')})
        self.assertEqual(self.env['BB_P2P_PORT'], str(DEFAULT_P2P_PORT))
        self.assertEqual(self.env['BB_SIGN_TIMEOUT'], str(DEFAULT_SIGN_TIMEOUT))

    def test_sign_timeout_setting(self):
        for typed, expected in (('0', '0'), ('180', '180'), ('9999', '600'), ('abc', '30'), ('', '30')):
            self.settings['online_sign_timeout'] = typed
            configure_online(self.settings, self.data, self.env)
            self.assertEqual(self.env['BB_SIGN_TIMEOUT'], expected)

    def test_game_server_bare_name_and_old_shadnet_default(self):
        self.assertEqual(game_server('thehuntersdream.com'), 'https://thehuntersdream.com')
        self.assertEqual(game_server('http://srv.shadps4.net:31315'), HUNTERS_DREAM)
        self.assertEqual(game_server('http://game.example:8080'), 'http://game.example:8080')

    def test_empty_webapi_uses_server_port_31315(self):
        self.settings['online_webapi'] = ''
        configure_online(self.settings, self.data, self.env)
        self.assertEqual(self.env['BB_SHADNET_WEBAPI'], 'http://server.example:31315')

    def test_invalid_profile_fails_before_writing_cache(self):
        for key, values in {
            'online_host':['http://server.example', 'foo bar', '::1'],
            'online_port':['0','65536','not-a-port'],
            'online_webapi':['ftp://server.example','http://user:pass@server.example','http://server.example:0','http://server.example/api'],
            'online_username':['','has space','x'*17,'é'*9],
            'online_p2p_port':['0','65536','bad']}.items():
            for value in values:
                with self.subTest(key=key,value=value):
                    settings = dict(self.settings, **{key:value})
                    with self.assertRaises(ValueError): configure_online(settings,self.data,dict(self.env))
                    self.assertFalse((self.data/'network').exists())

    def test_no_password_no_cache(self):
        with self.assertRaises(ValueError): configure_online(self.settings,self.data,{})
        self.assertFalse((self.data/'network').exists())

    def test_shadnet_defaults_and_game_traffic_only_through_sce_http(self):
        settings = dict(self.settings, online_host='', online_port='', online_webapi='')
        path = configure_online(settings, self.data, self.env)
        self.assertEqual(self.env['BB_SHADNET_SERVER'], f'{SHADNET_HOST}:{SHADNET_PORT}')
        self.assertEqual(self.env['BB_SHADNET_WEBAPI'], SHADNET_WEBAPI)
        overrides = json.loads(path.read_text())
        self.assertEqual(overrides, {GAME_STATUS_ORIGIN:HUNTERS_DREAM, **play_logs('thehuntersdream.com')})
        self.assertNotIn(SHADNET_WEBAPI, overrides.values())

    def test_url_does_not_drop_port(self):
        self.assertEqual(http_base('https://server.example:34443/api/'),'https://server.example:34443/api')
        for url in ('http://server.example?x=1','http://server.example#x','http://server.example\n'):
            with self.assertRaises(ValueError): http_base(url)


class OnlinePausedTests(unittest.TestCase):
    def test_paused_starts_offline(self):
        import online
        env = {'BB_SHADNET_SERVER': 'x:1'}
        paused = online.ONLINE_PAUSED
        online.ONLINE_PAUSED = True
        try:
            self.assertIsNone(configure_online({'online_enabled': '1'}, '.', env))
        finally:
            online.ONLINE_PAUSED = paused
        self.assertEqual(env, {'BB_ONLINE': '0'})


if __name__ == '__main__': unittest.main()
