"""Device-independent, one-shot xiuyun startup animation."""
import math
from pathlib import Path

import cv2
from PyQt5.QtCore import Qt, QElapsedTimer, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QImage, QPainter, QPixmap
from PyQt5.QtWidgets import QApplication, QWidget

from utils.path_helper import get_resource_path


class StartupSplash(QWidget):
    finished = pyqtSignal()

    def __init__(self, resource_path=None):
        super().__init__(None, Qt.SplashScreen | Qt.FramelessWindowHint)
        self.setWindowTitle('xiuyun · DMF')
        self.setFocusPolicy(Qt.StrongFocus)
        self.resource_path = Path(resource_path or get_resource_path('assets/startup/xiuyun.mp4'))
        self.capture = None
        self.frame = QPixmap()
        self.frame_index = -1
        self.fps = 60.0
        self.duration_ms = 2000
        self.elapsed = QElapsedTimer()
        self.done = False
        self.started = False
        self.timer = QTimer(self)
        self.timer.setTimerType(Qt.PreciseTimer)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self.advance)
        available = QApplication.primaryScreen().availableGeometry()
        width = min(960, int(available.width()*.85), int(available.height()*.85*16/9))
        self.setFixedSize(width, round(width*9/16))
        self.move(available.center() - self.rect().center())

    def start(self):
        if self.started or self.done:
            return
        self.started = True
        try:
            if not self.resource_path.is_file():
                raise ValueError('Missing startup video')
            # Use the bundled file decoder, never a camera or system player.
            self.capture = cv2.VideoCapture(str(self.resource_path), cv2.CAP_FFMPEG)
            self.fps = self.capture.get(cv2.CAP_PROP_FPS)
            count = self.capture.get(cv2.CAP_PROP_FRAME_COUNT)
            if not self.capture.isOpened() or not math.isfinite(self.fps) or self.fps <= 0 or count <= 0:
                raise ValueError('Invalid startup video')
            self.read_frame()
        except (OSError, ValueError, cv2.error):
            QTimer.singleShot(0, self.finish)
            return
        self.show()
        self.elapsed.start()
        self.timer.start()

    def read_frame(self):
        ok, image = self.capture.read()
        if not ok:
            raise ValueError('Cannot read startup frame')
        self.frame_index += 1
        image = cv2.resize(image, (self.width(), self.height()), interpolation=cv2.INTER_AREA)
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        h, w, _ = rgb.shape
        self.frame = QPixmap.fromImage(QImage(rgb.data, w, h, rgb.strides[0], QImage.Format_RGB888).copy())
        self.update()

    def advance(self):
        if self.done:
            return
        elapsed = self.elapsed.elapsed()
        if elapsed >= self.duration_ms:
            self.finish()
            return
        target = int(elapsed*self.fps/1000)
        if target <= self.frame_index:
            return
        try:
            # Drop late frames instead of stretching the two-second animation.
            while self.frame_index + 1 < target:
                if not self.capture.grab():
                    raise ValueError('Truncated startup video')
                self.frame_index += 1
            self.read_frame()
        except (ValueError, cv2.error):
            self.finish()

    def finish(self):
        if self.done:
            return
        self.done = True
        self.timer.stop()
        if self.capture is not None:
            self.capture.release()
            self.capture = None
        self.finished.emit()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(3, 9, 26))
        if not self.frame.isNull():
            painter.drawPixmap(self.rect(), self.frame)

    def mousePressEvent(self, event):
        self.finish()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key_Escape:
            self.finish()
        else:
            super().keyPressEvent(event)

    def closeEvent(self, event):
        self.finish()
        event.accept()


def show_with_startup(window_factory, on_ready=None, resource_path=None):
    """Keep the splash alive until the main window is visible, even on decode failure."""
    app = QApplication.instance()
    splash = StartupSplash(resource_path)
    app.startup_splash = splash
    opened = False

    def show_main():
        nonlocal opened
        if opened:
            return
        opened = True
        window = window_factory()
        app.main_window = window
        window.show()
        splash.close()
        if on_ready is not None:
            on_ready(window)

    splash.finished.connect(show_main)
    QTimer.singleShot(0, splash.start)
    return splash
