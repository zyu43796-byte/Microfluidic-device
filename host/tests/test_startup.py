import importlib.util
import os
from pathlib import Path
import unittest

os.environ['QT_QPA_PLATFORM'] = 'offscreen'


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('startup_animation'), '启动动画模块尚未实现')
        from PyQt5.QtWidgets import QApplication
        self.app = QApplication.instance() or QApplication([])
        self.previous_quit_setting = self.app.quitOnLastWindowClosed()
        self.app.setQuitOnLastWindowClosed(False)
        self.windows = []

    def tearDown(self):
        for window in getattr(self, 'windows', []):
            window.close()
        if hasattr(self, 'app'):
            self.app.processEvents()
            self.app.setQuitOnLastWindowClosed(self.previous_quit_setting)

    def splash(self, path=None):
        from startup_animation import StartupSplash
        splash = StartupSplash(path)
        self.windows.append(splash)
        return splash

    def test_real_video_finishes_once_and_releases_decoder(self):
        from PyQt5.QtTest import QTest, QSignalSpy
        splash = self.splash()
        spy = QSignalSpy(splash.finished)
        splash.start()
        self.assertTrue(splash.isVisible())
        QTest.qWait(400)
        self.assertEqual(len(spy), 0)
        self.assertFalse(splash.frame.isNull())
        QTest.qWait(1900)
        self.assertEqual(len(spy), 1)
        self.assertIsNone(splash.capture)
        self.assertFalse(splash.timer.isActive())
        splash.finish()
        self.assertEqual(len(spy), 1)

    def test_missing_and_corrupt_video_fall_back(self):
        from PyQt5.QtTest import QTest, QSignalSpy
        fixtures = Path(__file__).parent / 'fixtures'
        for path in (fixtures/'missing.mp4', fixtures/'startup.invalid'):
            with self.subTest(path=path.name):
                splash = self.splash(path)
                spy = QSignalSpy(splash.finished)
                splash.start()
                QTest.qWait(50)
                self.assertEqual(len(spy), 1)
                self.assertIsNone(splash.capture)

    def test_escape_skips_and_releases_decoder(self):
        from PyQt5.QtCore import Qt
        from PyQt5.QtTest import QTest, QSignalSpy
        splash = self.splash()
        spy = QSignalSpy(splash.finished)
        splash.start()
        QTest.keyClick(splash, Qt.Key_Escape)
        self.assertEqual(len(spy), 1)
        self.assertIsNone(splash.capture)

    def test_main_window_created_once_after_splash_and_remains_visible(self):
        from PyQt5.QtTest import QTest
        from PyQt5.QtWidgets import QWidget
        from startup_animation import show_with_startup
        created = []

        def factory():
            window = QWidget()
            self.windows.append(window)
            created.append(window)
            return window

        splash = show_with_startup(factory)
        self.windows.append(splash)
        self.assertEqual(created, [])
        QTest.qWait(2300)
        self.assertEqual(len(created), 1)
        self.assertTrue(created[0].isVisible())
        self.assertFalse(splash.isVisible())
        splash.finish()
        self.assertEqual(len(created), 1)

    def test_missing_asset_still_opens_main_window(self):
        from PyQt5.QtTest import QTest
        from PyQt5.QtWidgets import QWidget
        from startup_animation import show_with_startup
        created = []

        def factory():
            window = QWidget()
            self.windows.append(window)
            created.append(window)
            return window

        splash = show_with_startup(factory, resource_path=Path(__file__).parent/'fixtures/absent.mp4')
        self.windows.append(splash)
        QTest.qWait(100)
        self.assertEqual(len(created), 1)
        self.assertTrue(created[0].isVisible())


if __name__ == '__main__':
    unittest.main()
