from paths import ROOT
import unittest
from run_windows import configure_frame_rate, large_scene


class WindowsFrameRateTests(unittest.TestCase):
    def test_supported_presets_override_inherited_unbounded_values(self):
        for fps in ('30', '60', '90'):
            with self.subTest(fps=fps):
                env = {'BB_FPS': fps, 'BB_FPS_LIMIT': '0', 'BB_VBLANK_HZ': '480'}
                self.assertEqual(configure_frame_rate(env), fps)
                self.assertEqual(env['BB_FPS_LIMIT'], fps)
                self.assertEqual(env['BB_VBLANK_HZ'], '90' if fps == '90' else '60')

    def test_default_and_unknown_presets_are_capped_at_60(self):
        for value in (None, 'Unlimited', '', '0', 'bad'):
            with self.subTest(value=value):
                env = {'BB_VBLANK_HZ': '0', 'BB_FPS_LIMIT': '0'}
                if value is not None:
                    env['BB_FPS'] = value
                self.assertEqual(configure_frame_rate(env), '60')
                self.assertEqual(env['BB_FPS'], '60')
                self.assertEqual(env['BB_FPS_LIMIT'], '60')
                self.assertEqual(env['BB_VBLANK_HZ'], '60')

    def test_120_and_144_are_their_own_presets(self):
        for fps in ('120', '144'):
            with self.subTest(fps=fps):
                env = {'BB_FPS': fps, 'BB_FPS_LIMIT': '0', 'BB_VBLANK_HZ': '0'}
                self.assertEqual(configure_frame_rate(env), fps)
                self.assertEqual(env['BB_FPS'], fps)
                self.assertEqual(env['BB_FPS_LIMIT'], fps)
                self.assertEqual(env['BB_VBLANK_HZ'], fps)

    def test_other_high_rates_use_the_uncapped_patch_with_a_limit(self):
        for fps in ('165', '480'):
            with self.subTest(fps=fps):
                env = {'BB_FPS': fps, 'BB_FPS_LIMIT': '0', 'BB_VBLANK_HZ': '60'}
                self.assertEqual(configure_frame_rate(env), 'uncap')
                self.assertEqual(env['BB_FPS'], 'uncap')
                self.assertEqual(env['BB_FPS_LIMIT'], fps)
                self.assertEqual(env['BB_VBLANK_HZ'], '0')

    def test_uncapped_keeps_its_limit(self):
        for inherited, expected in (('0', '0'), ('165', '165'), ('bad', '0')):
            with self.subTest(limit=inherited):
                env = {'BB_FPS': 'uncap', 'BB_FPS_LIMIT': inherited, 'BB_VBLANK_HZ': '60'}
                self.assertEqual(configure_frame_rate(env), 'uncap')
                self.assertEqual(env['BB_FPS_LIMIT'], expected)
                self.assertEqual(env['BB_VBLANK_HZ'], '0')


if __name__ == '__main__':
    unittest.main()


class MemoryPoolTests(unittest.TestCase):
    def test_the_large_pool_follows_the_scene_not_the_output(self):
        self.assertFalse(large_scene('1706x960'))
        self.assertFalse(large_scene('1916x1078'))
        self.assertFalse(large_scene('1920x1080'))
        self.assertTrue(large_scene('2560x1440'))
        self.assertTrue(large_scene('2258x1270'))
        self.assertTrue(large_scene('3840x2160'))
