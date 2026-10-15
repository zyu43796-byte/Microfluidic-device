"""Presentation-only purple workspace. Reuses existing device controls and signals."""
from PyQt5.QtCore import Qt, QObject, QEvent
from PyQt5.QtGui import QFont, QIcon, QPainter, QColor
from PyQt5.QtWidgets import (QWidget, QFrame, QLabel, QPushButton, QVBoxLayout,
                             QHBoxLayout, QGridLayout, QScrollArea, QTabWidget,
                             QSizePolicy, QApplication, QAbstractItemView)
from utils.path_helper import get_resource_path


STYLE = '''
QMainWindow, QWidget#workspace { background: #F4F3F9; }
QWidget { font-family: "Microsoft YaHei UI", "Segoe UI"; font-size: 13px; color: #343047; }
QLabel { background: transparent; border: none; }
QGroupBox { background: white; border: 1px solid #E6E2F0; border-radius: 16px;
            margin-top: 0px; padding-top: 35px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: padding; subcontrol-position: top left;
                   left: 16px; top: 12px; color: #51446F; }
QFrame#header { background: #FFFFFF; border: 1px solid #E6E2F0; border-radius: 16px; }
QLabel#brand { color: #7563B8; font-family: "Segoe UI"; font-size: 28px; font-weight: 600; }
QLabel#pageTitle { font-size: 19px; font-weight: 600; color: #343047; }
QLabel#subtitle, QLabel[role="muted"] { color: #746D87; font-size: 12px; }
QLabel#headerStatus { background: #F0EBFA; border-radius: 15px; color: #63528D; padding: 8px 14px; }
QLabel#notification { color: #6C647E; font-size: 12px; }
QLabel#notification[alert="true"] { color: #B12443; font-weight: 600; }
QPushButton { background: #F5F3FA; border: 1px solid #E2DDEE; border-radius: 8px;
              padding: 7px 10px; min-height: 18px; color: #51446F; }
QPushButton:hover { background: #EAE3F7; border-color: #B5A6D9; }
QPushButton:pressed { background: #D8CBEF; border-color: #8A70C5; }
QPushButton:focus { border: 2px solid #9881D1; }
QPushButton[role="primary"] { background: #7563B8; border-color: #7563B8; color: white; font-weight: 600; }
QPushButton[role="primary"]:hover { background: #6753AA; }
QPushButton[role="primary"]:pressed { background: #584494; }
QPushButton[role="danger"] { background: #C83753; border-color: #C83753; color: white; font-weight: 600; }
QPushButton[role="danger"]:hover { background: #B52743; }
QPushButton[role="danger"]:pressed { background: #9D2039; }
QPushButton[role="quiet"] { background: transparent; border: none; text-align: left; padding-left: 2px; }
QPushButton:disabled { background: #F1EFF5; border-color: #E6E2ED; color: #91899F; }
QLineEdit, QComboBox, QSpinBox { background: #F8F7FC; border: 1px solid #DDD6EC;
                              border-radius: 8px; padding: 6px 8px; min-height: 20px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #8A70C5; background: white; }
QLineEdit:read-only { background: #F0EEF5; color: #716981; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: #91899F; background: #F1EFF5; }
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox::up-button, QSpinBox::down-button { width: 18px; border: none; background: #EDE7F6; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #DCD0EF; }
QComboBox QAbstractItemView { background: white; selection-background-color: #7563B8; selection-color: white; padding: 5px; }
QCheckBox { spacing: 7px; }
QCheckBox::indicator { width: 15px; height: 15px; }
QTabWidget::pane { border: none; background: white; border-radius: 12px; padding-top: 7px; }
QTabBar::tab { background: #F3F0F9; color: #706780; padding: 8px 16px; border: none; margin-right: 4px; border-radius: 8px; }
QTabBar::tab:selected { background: #7563B8; color: white; }
QTabBar::tab:hover:!selected { background: #E5DDF4; }
QTabBar::tab:disabled { color: #A59CB2; }
QGraphicsView { background: #FAFAFE; border: 1px solid #E3DDED; border-radius: 12px; }
QTextBrowser, QTableWidget { background: #FFFFFF; border: 1px solid #E5DFEF; border-radius: 8px;
                           selection-background-color: #7563B8; selection-color: white; }
QTableWidget { alternate-background-color: #F7F4FC; gridline-color: #EEEAF5; }
QHeaderView::section { background: #F1ECF9; color: #62527D; padding: 7px; border: none; font-weight: 600; }
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: #F4F1F8; width: 7px; margin: 0px; }
QScrollBar::handle:vertical { background: #CEC4E2; border-radius: 3px; min-height: 26px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
QScrollBar:horizontal { background: #F4F1F8; height: 7px; margin: 0px; }
QScrollBar::handle:horizontal { background: #CEC4E2; border-radius: 3px; min-width: 26px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }
QSlider::groove:horizontal { height: 6px; background: #E8E0F3; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #927ACB; border-radius: 3px; }
QSlider::handle:horizontal { background: #7563B8; border: 2px solid white; width: 15px; margin: -6px 0; border-radius: 9px; }
QLabel#label_cam_view { background: #F1EDF8; color: #85739E; border: 1px dashed #D6CBE5; border-radius: 12px; }
QLabel[role="status"] { background: #F5F1FA; border-radius: 9px; padding: 8px; color: #675581; }
QToolTip { background: #3E3456; color: white; border: none; padding: 6px; }
'''


class WorkspaceColumns(QHBoxLayout):
    def hasHeightForWidth(self):
        # Nested word-wrapped labels must not force the scroll area's minimum
        # height to the sum of every card's preferred (rather than minimum) size.
        return False


class PeripheralTabSizer(QObject):
    """Keep each peripheral tab at one third of its panel's usable width."""
    def eventFilter(self, tabs, event):
        if event.type() in (QEvent.Resize, QEvent.Show):
            # QSS width excludes the existing four-pixel right margin.
            width = max(1, tabs.contentsRect().width() // 3 - 4)
            if getattr(self, '_tab_width', None) != width:
                self._tab_width = width
                tabs.tabBar().setStyleSheet(
                    f'QTabBar#peripheralTabs::tab {{ width: {width}px; '
                    'padding-left: 0px; padding-right: 0px; }')
        return super().eventFilter(tabs, event)


def role(widget, value):
    widget.setProperty('role', value)


def clear_layout(layout):
    while layout.count():
        layout.takeAt(0)
    for i in range(layout.columnCount() if isinstance(layout, QGridLayout) else 0):
        layout.setColumnStretch(i, 0)
    for i in range(layout.rowCount() if isinstance(layout, QGridLayout) else 0):
        layout.setRowStretch(i, 0)


def scroll_page(widget):
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QFrame.NoFrame)
    area.setWidget(widget)
    widget.setAutoFillBackground(False)
    area.viewport().setAutoFillBackground(False)
    return area


def notify(window, text, error=False):
    window.notification_label.setText(text)
    window.notification_label.setToolTip(text)
    window.notification_label.setProperty('alert', error)
    window.notification_label.style().unpolish(window.notification_label)
    window.notification_label.style().polish(window.notification_label)


def install_workspace(w):
    app = QApplication.instance()
    if app.style().objectName().lower() != 'fusion':
        app.setStyle('Fusion')
    # Remove local legacy appearance overrides; all signals and actual widgets remain.
    swatches = set(w.light_color_buttons)
    for child in w.findChildren(QWidget):
        if child not in swatches:
            child.setStyleSheet('')
        child.setFont(QFont('Microsoft YaHei UI', 9))
    surface = QWidget()
    surface.setObjectName('workspace')
    root = QVBoxLayout(surface)
    root.setContentsMargins(18, 14, 18, 12)
    root.setSpacing(14)

    header = QFrame()
    header.setObjectName('header')
    top = QHBoxLayout(header)
    top.setContentsMargins(20, 10, 16, 10)
    brand = QLabel('xiuyun')
    brand.setObjectName('brand')
    top.addWidget(brand)
    titles = QVBoxLayout()
    title = QLabel('数字微流控工作台')
    title.setObjectName('pageTitle')
    subtitle = QLabel('DIGITAL MICROFLUIDICS  /  CONTROL STUDIO')
    subtitle.setObjectName('subtitle')
    titles.addWidget(title)
    titles.addWidget(subtitle)
    titles.setSpacing(2)
    top.addSpacing(18)
    top.addLayout(titles)
    top.addStretch()
    w.header_status = QLabel('●  未连接')
    w.header_status.setObjectName('headerStatus')
    top.addWidget(w.header_status)
    top.addSpacing(10)
    role(w.stop_button, 'danger')
    w.stop_button.setMinimumWidth(190)
    top.addWidget(w.stop_button)
    root.addWidget(header)

    body_widget = QWidget()
    body_widget.setMinimumHeight(600)
    body = WorkspaceColumns(body_widget)
    body.setContentsMargins(0, 0, 0, 0)
    body.setSpacing(16)
    root.addWidget(scroll_page(body_widget), 1)
    left = QWidget()
    left.setFixedWidth(264)
    left_layout = QVBoxLayout(left)
    left_layout.setContentsMargins(0, 0, 0, 0)
    left_layout.setSpacing(14)
    left_layout.addWidget(w.groupBox1_1)
    left_layout.addWidget(w.groupBox1_2)
    left_layout.addStretch()
    body.addWidget(scroll_page(left))
    body.itemAt(0).widget().setFixedWidth(274)

    w.groupBox1_1.setTitle('设备连接')
    g = w.gird_layout
    clear_layout(g)
    g.setContentsMargins(14, 6, 14, 14)
    g.setSpacing(8)
    w.comboBox.setSizeAdjustPolicy(w.comboBox.AdjustToMinimumContentsLengthWithIcon)
    w.comboBox.setMinimumContentsLength(12)
    g.addWidget(w.comboBox, 0, 0, 1, 3)
    for col, button in enumerate((w.button_open_serial_port, w.button_flush_serial_port, w.button_close_serial_port)):
        g.addWidget(button, 1, col)
    w.button_open_serial_port.setText('连接')
    w.button_close_serial_port.setText('断开')
    role(w.button_open_serial_port, 'primary')
    for row, (button, label, text) in enumerate(((w.button1, w.label1_button, '压实联锁'),
                 (w.button2, w.label2_button, '位置开关'), (w.button3, w.label3_button, '综合联锁')), 2):
        button.setText(text + '  ↻')
        button.setToolTip('点击刷新联锁状态')
        role(button, 'quiet')
        g.addWidget(button, row, 0, 1, 2)
        g.addWidget(label, row, 2)
        role(label, 'status')

    w.groupBox1_2.setTitle('AC 驱动参数')
    g = w.grid_layout2
    clear_layout(g)
    g.setContentsMargins(14, 6, 14, 14)
    g.setSpacing(9)
    for row, (label, field, title) in enumerate(((w.button4, w.lineEdit1, '电压 / V'),
                   (w.button5, w.lineEdit2, '频率 / Hz'), (w.button6, w.lineEdit3_1, '相位 / μs'))):
        label.setText(title)
        label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        field.setMaximumWidth(106)
        field.setAlignment(Qt.AlignRight)
        g.addWidget(label, row, 0)
        g.addWidget(field, row, 1)
    w.mapping_check.setText('已核对电极编号\n与实物对应')
    g.addWidget(w.mapping_check, 3, 0, 1, 2)
    g.addWidget(w.ac_on_button, 4, 0, 1, 2)
    g.addWidget(w.ac_off_button, 5, 0, 1, 2)
    g.addWidget(w.state_label, 6, 0, 1, 2)
    role(w.ac_on_button, 'primary')
    role(w.state_label, 'muted')

    middle = QVBoxLayout()
    middle.setSpacing(14)
    middle.addWidget(w.groupBox2, 1)
    middle.addWidget(w.groupBox3, 0)
    body.addLayout(middle, 6)
    w.groupBox2.setTitle('电极阵列 · 颜色仅表示界面选择')
    w.layout_V_Middle_V1.setContentsMargins(14, 5, 14, 12)
    w.layout_V_Middle_V1.setSpacing(8)
    w.graphicsView1.setRenderHint(QPainter.Antialiasing, True)
    w.graphicsView1.setFrameShape(QFrame.NoFrame)
    w.graphicsView1.setMinimumSize(300, 120)
    # Reserve scrollbar space to keep fitInView from oscillating the layout;
    # scrollbars still let users pan after zooming into electrode details.
    w.graphicsView1.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOn)
    w.graphicsView1.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOn)
    w.layout_V_Middle_V1.setStretchFactor(w.graphicsView1, 1)
    w.graphicsView1.setBackgroundBrush(QColor('#FAFAFE'))
    legend = QWidget()
    legend_layout = QGridLayout(legend)
    legend_layout.setContentsMargins(0, 0, 0, 0)
    legend_layout.setHorizontalSpacing(14)
    for i, (text, fill, border, style) in enumerate([
        ('未选', '#E8E8F1', '#9991AD', 'solid'),
        ('仅选中', '#DBEAFE', '#3577B9', 'dashed'),
        ('驱动选择', '#6C48C5', '#48258F', 'solid'),
        ('固定 · 锁标记', '#9C2869', '#F39BBE', 'solid')]):
        row = QHBoxLayout()
        square = QLabel()
        square.setFixedSize(14, 14)
        square.setStyleSheet(f'background:{fill};border:2px {style} {border};border-radius:3px;')
        row.addWidget(square)
        label = QLabel(text)
        role(label, 'muted')
        row.addWidget(label)
        row.addStretch()
        legend_layout.addLayout(row, 0, i)
    w.layout_V_Middle_V1.addWidget(legend)
    note = QLabel('颜色表示界面选择，不代表实测电压或液滴位置。')
    role(note, 'muted')
    note.setWordWrap(True)
    # Explain the colors in a tooltip; keep the live output state visible below the map.
    legend.setToolTip(note.text())
    note.deleteLater()
    w.layout_V_Middle_V1.removeWidget(w.output_label)
    w.layout_V_Middle_V1.addWidget(w.output_label)
    w.output_label.setWordWrap(True)
    role(w.output_label, 'muted')
    w.output_label.setMinimumHeight(18)

    w.groupBox3.setTitle('操作控制')
    w.groupBox3.setFixedHeight(200)
    w.tabWidget.currentChanged.connect(lambda index: w.groupBox3.setFixedHeight(290 if index == 1 else 200))
    w.layout_V_Middle_V2.setContentsMargins(14, 4, 14, 10)
    w.tabWidget.setTabText(0, '手动操作')
    w.tabWidget.setTabText(1, '自动流程')
    w.tabWidget.tabBar().setTabVisible(2, False)
    g = w.grid_layout3
    clear_layout(g)
    g.setContentsMargins(2, 4, 2, 0)
    g.setSpacing(4)
    for button, text, row, col in ((w.button_up, '↑', 0, 1), (w.button_left, '←', 1, 0),
                    (w.button_right, '→', 1, 2), (w.button_down, '↓', 2, 1)):
        button.setIcon(QIcon())
        button.setText(text)
        button.setFixedSize(42, 32)
        g.addWidget(button, row, col)
    instructions = QLabel('右键：切换驱动选择\nCtrl + 右键：多选\nShift + 左键：固定 / 解除')
    instructions.setWordWrap(True)
    role(instructions, 'muted')
    g.addWidget(instructions, 0, 3, 2, 2)
    w.clear_button.setText('清空电极选择')
    g.addWidget(w.clear_button, 2, 3, 1, 2)
    for button, text in ((w.button_zoomin, '+'), (w.button_zoomout, '−')):
        button.setIcon(QIcon())
        button.setText(text)
        button.setFixedSize(32, 28)
    zoom = QHBoxLayout()
    zoom.addWidget(QLabel('缩放'))
    zoom.addWidget(w.button_zoomout)
    zoom.addWidget(w.button_zoomin)
    zoom.addStretch()
    g.addLayout(zoom, 0, 5, 3, 1)
    g.setColumnStretch(3, 1)
    w.tab2_tab.verticalHeader().setDefaultSectionSize(32)
    w.tab2_tab.setShowGrid(False)
    w.layout_V_Middle_V3.setContentsMargins(0, 4, 0, 0)
    w.layout_V_Middle_V3.setSpacing(6)
    role(w.button_sta, 'primary')
    role(w.button_end, 'danger')
    w.button_add.setText('加一帧')
    w.button_del.setText('删帧')

    right = QVBoxLayout()
    right.setSpacing(14)
    right.addWidget(w.groupBox4, 3)
    right.addWidget(w.groupBox5, 7)
    body.addLayout(right, 4)
    w.groupBox4.setTitle('相机预览')
    w.layout_V_Right_V1.setContentsMargins(14, 5, 14, 12)
    w.layout_V_Right_V1.setSpacing(8)
    w.label_cam_view.setFrameShape(QFrame.NoFrame)
    w.label_cam_view.setMinimumSize(180, 60)
    w.label_cam_view.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)
    w.label_cam_view.setText('相机未开启\n\n请在开启 AC / 磁杆前打开')
    row = w.layout_V_Right_V1_H1
    clear_layout(row)
    row.setSpacing(6)
    for widget in (w.camera_index, w.button_open_camera, w.button_close_camera, w.button_take_picture):
        row.addWidget(widget)
    w.button_open_camera.setText('打开')
    w.button_close_camera.setText('关闭')
    w.button_record_picture.hide()
    role(w.button_open_camera, 'primary')
    w.groupBox5.setTitle('外设控制')
    w.tool_tabs.tabBar().setObjectName('peripheralTabs')
    w.peripheral_tab_sizer = PeripheralTabSizer(w.tool_tabs)
    w.tool_tabs.installEventFilter(w.peripheral_tab_sizer)
    w.layout_V_Right_V2.setContentsMargins(14, 5, 14, 12)
    for index, panel in enumerate((w.light_panel, w.magnet_panel)):
        title = w.tool_tabs.tabText(index)
        w.tool_tabs.removeTab(index)
        w.tool_tabs.insertTab(index, scroll_page(panel), title)
        panel.setAutoFillBackground(False)
    for label in w.light_panel.findChildren(QLabel):
        label.setWordWrap(True)
    w.light_brightness.setMaximumWidth(82)
    w.light_interval.setMaximumWidth(110)
    w.tool_tabs.setCurrentIndex(0)
    # Long peripheral instructions scroll within their card on smaller screens.
    for button in (w.light_apply_button, w.magnet_panel.on_button, w.magnet_panel.timed_button):
        role(button, 'primary')
    role(w.magnet_panel.stop_button, 'danger')
    for label in (w.light_status, w.magnet_panel.status):
        role(label, 'status')
    role(w.magnet_panel.note, 'muted')
    w.magnet_panel.timed_button.setText('定时动作')
    w.magnet_panel.timed_button.setToolTip('动作保持指定时间后返回默认极性')
    for label in w.magnet_panel.findChildren(QLabel):
        if label.text().startswith('磁杆控制 · PC12'):
            label.setText('磁杆动作')
    w.magnet_panel.note.setText('OFF 返回默认极性，不等于断电或机械停止。\n'
        '指令回读不代表到位。单独控制磁杆时，返回默认会结束授权并关闭灯带；AC开启时仅切换磁杆。\n'
        '全停关闭AC、灯带并使磁杆返回默认。定时包含通信与系统调度误差。')

    bottom = QHBoxLayout()
    w.log_toggle = QPushButton('运行记录  ▾')
    w.log_toggle.setCheckable(True)
    role(w.log_toggle, 'quiet')
    bottom.addWidget(w.log_toggle)
    w.notification_label = QLabel('设备尚未连接 · 请先核对联锁与电极编号')
    w.notification_label.setObjectName('notification')
    w.notification_label.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
    w.notification_label.setWordWrap(True)
    bottom.addWidget(w.notification_label, 1)
    root.addLayout(bottom)
    w.log_tabs = QTabWidget()
    w.log_tabs.addTab(w.log_information, '运行日志')
    w.log_tabs.addTab(w.serial_information, '串口数据')
    w.log_tabs.setFixedHeight(150)
    w.log_tabs.hide()
    root.addWidget(w.log_tabs)
    def toggle_logs(checked):
        w.log_tabs.setVisible(checked)
        w.log_toggle.setText('运行记录  ▴' if checked else '运行记录  ▾')
    w.log_toggle.toggled.connect(toggle_logs)
    # Small screens and expanded logs scroll the body rather than overlap controls.
    body_widget.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)
    w.centralwidget = surface
    w.setCentralWidget(surface)
    down = get_resource_path('assets/ui/chevron-down.svg').replace('\\', '/')
    up = get_resource_path('assets/ui/chevron-up.svg').replace('\\', '/')
    w.setStyleSheet(STYLE + f'''
        QComboBox::down-arrow, QSpinBox::down-arrow {{ image: url("{down}"); width: 10px; height: 7px; }}
        QSpinBox::up-arrow {{ image: url("{up}"); width: 10px; height: 7px; }}
    ''')
    w.setMinimumSize(1120, 700)
    available = QApplication.primaryScreen().availableGeometry()
    w.resize(min(1600, available.width()-30), min(950, available.height()-50))
    w.move(available.center()-w.rect().center())


def update_summary(w):
    c = w.control
    text = '●  已连接 · AC 已开启' if c.ready and c.armed else ('●  已连接 · AC 已关闭' if c.ready else '●  未就绪 / 未连接')
    if w.header_status.text() != text:
        w.header_status.setText(text)
