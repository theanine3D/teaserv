"""Installation-local admin preferences; no server or GUI required."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from teaserv_admin import appsettings as settings


class PortableSettingsTests(unittest.TestCase):
    def test_packaged_installations_ignore_cwd_and_shared_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = root / 'profile'
            (profile / 'TeaservAdmin').mkdir(parents=True)
            (profile / 'TeaservAdmin/settings.json').write_text(
                json.dumps({'server_dir': 'deleted installation'}), encoding='utf-8')
            for name in ('first', 'second'):
                install = root / name
                (install / 'system').mkdir(parents=True)
                (install / 'system/config.json').write_text('{}', encoding='utf-8')
                with patch.object(sys, 'frozen', True, create=True), \
                     patch.object(sys, 'executable', str(install / 'teaserv-admin')), \
                     patch.object(settings.os, 'getcwd', return_value=str(root / 'other')), \
                     patch.dict(os.environ, {'APPDATA': str(profile), 'XDG_CONFIG_HOME': str(profile)}):
                    app = settings.AppSettings()
                    self.assertEqual(app.path, str(install / 'teaserv-admin-settings.json'))
                    self.assertEqual(app.server_dir, '')
                    self.assertEqual(settings.guess_server_dir(), str(install))
                    app['server_dir'] = str(install)
                    app.save()
                    self.assertEqual(settings.AppSettings().server_dir, str(install))
            self.assertNotEqual((root / 'first/teaserv-admin-settings.json').read_bytes(),
                                (root / 'second/teaserv-admin-settings.json').read_bytes())

    def test_source_checkout_uses_server_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'system').mkdir()
            module = root / 'tools/admin-gui/teaserv_admin/appsettings.py'
            with patch.object(sys, 'frozen', False, create=True), patch.object(settings, '__file__', str(module)):
                self.assertEqual(settings.default_settings_path(), str(root / 'teaserv-admin-settings.json'))

    def test_explicit_server_directory_selects_its_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(settings.default_settings_path(folder),
                             str(Path(folder) / 'teaserv-admin-settings.json'))

    def test_explicit_relative_settings_path_can_be_saved(self):
        # abspath() resolves against the real working directory, so change into the temporary folder;
        # patching getcwd alone left custom.json behind in the folder the tests ran from.
        with tempfile.TemporaryDirectory() as folder:
            previous = os.getcwd()
            os.chdir(folder)
            try:
                app = settings.AppSettings('custom.json')
                app['server_dir'] = 'chosen server'
                app.save()
                self.assertEqual(settings.AppSettings('custom.json').server_dir, 'chosen server')
                self.assertTrue(os.path.isfile(os.path.join(folder, 'custom.json')))
            finally:
                os.chdir(previous)

    def test_status_always_on_top_is_remembered(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'teaserv-admin-settings.json')
            app = settings.AppSettings(path)
            self.assertIs(app['status_always_on_top'], False)
            app['status_always_on_top'] = True
            app.save()
            self.assertIs(settings.AppSettings(path)['status_always_on_top'], True)


if __name__ == '__main__':
    unittest.main()
