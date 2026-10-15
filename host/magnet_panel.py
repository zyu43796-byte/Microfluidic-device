"""Compact magnet controls; no temperature controls or device access here."""
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton, QSpinBox


class MagnetPanel(QWidget):
    def __init__(self, apply, stop, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        title = QLabel('磁杆控制 · PC12 → 继电器 → 两线推杆')
        title.setWordWrap(True)
        layout.addWidget(title)
        tip = QLabel('无需开启 AC。动作前检查压实和位置联锁，动作保持期间持续检查通信。')
        tip.setWordWrap(True)
        layout.addWidget(tip)
        row = QHBoxLayout()
        self.on_button = QPushButton('动作保持 · ON')
        self.off_button = QPushButton('返回默认 · OFF')
        self.on_button.setStyleSheet('background:#0f766e;color:white;padding:8px;')
        self.off_button.setStyleSheet('padding:8px;')
        row.addWidget(self.on_button)
        row.addWidget(self.off_button)
        layout.addLayout(row)
        timing = QHBoxLayout()
        timing.addWidget(QLabel('动作时长'))
        self.duration = QSpinBox()
        self.duration.setRange(100, 5000)
        self.duration.setSingleStep(100)
        self.duration.setValue(500)
        self.duration.setSuffix(' ms')
        timing.addWidget(self.duration)
        self.timed_button = QPushButton('定时动作 → 返回默认')
        timing.addWidget(self.timed_button)
        layout.addLayout(timing)
        self.status = QLabel('串口未连接；磁杆状态未知。')
        self.status.setWordWrap(True)
        self.status.setMinimumHeight(45)
        layout.addWidget(self.status)
        self.stop_button = QPushButton('停止全部输出（Esc）')
        self.stop_button.setStyleSheet('background:#b91c1c;color:white;padding:7px;')
        layout.addWidget(self.stop_button)
        self.note = QLabel('OFF 是返回默认极性，不等于断电或机械停止；伸出/缩回方向以实物为准。\n'
                           '状态仅表示固件指令回读，不代表磁杆已到位。\n'
                           '单独控制磁杆时，返回默认会结束授权并关闭灯带；AC 开启时只切换磁杆。\n'
                           '停止全部会关闭 AC、灯带并使磁杆回到默认极性。定时为上位机近似计时。')
        self.note.setWordWrap(True)
        self.note.setStyleSheet('color:#64748b;')
        layout.addWidget(self.note)
        layout.addStretch()
        self.on_button.clicked.connect(lambda: apply('on'))
        self.off_button.clicked.connect(lambda: apply('off'))
        self.timed_button.clicked.connect(lambda: apply('timed'))
        self.stop_button.clicked.connect(stop)

    def set_available(self, available):
        for widget in (self.on_button, self.off_button, self.timed_button, self.duration):
            widget.setEnabled(available)
