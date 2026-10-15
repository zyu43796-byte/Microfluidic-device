import os
import unittest

os.environ['QT_QPA_PLATFORM'] = 'offscreen'


class ModernUiTests(unittest.TestCase):
    def setUp(self):
        from PyQt5.QtWidgets import QApplication
        from adapted_main import MainWindow
        self.app = QApplication.instance() or QApplication([])
        self.w = MainWindow()
        self.w.poll_timer.stop()

    def tearDown(self):
        self.w.close()

    def test_peripheral_tabs_each_fill_one_third_of_panel(self):
        from PyQt5.QtWidgets import QTabBar
        baseline = QTabBar(self.w)
        baseline.addTab('灯光')
        baseline.addTab('磁杆')
        self.w.show()
        actual = self.w.tool_tabs.tabBar()
        for width, height in ((1600, 950), (1366, 768), (1120, 700)):
            self.w.resize(width, height)
            for _ in range(3):
                self.app.processEvents()
            for index in range(2):
                new = actual.tabRect(index)
                self.assertAlmostEqual(new.width() / self.w.tool_tabs.width(), 1 / 3, delta=.02)
                self.assertEqual(new.height(), baseline.tabRect(index).height())
        baseline.deleteLater()

    def test_electrode_states_are_visually_distinct_without_mutating_logic(self):
        electrode = self.w.scene._electrode_by_id(1)
        self.assertTrue(hasattr(electrode, 'appearance'), '电极状态视觉尚未实现')
        idle = electrode.appearance()
        electrode.setSelected(True)
        preview = electrode.appearance()
        self.assertEqual(self.w.scene.hardware_drive_ids(), [])
        electrode.setDriveStatus(True)
        drive = electrode.appearance()
        electrode.setPinned(True)
        pinned = electrode.appearance()
        self.assertEqual(len({state['fill'] for state in (idle, preview, drive, pinned)}), 4)
        self.assertTrue(preview['dashed'])
        self.assertTrue(pinned['locked'])
        self.assertEqual(self.w.scene.hardware_drive_ids(), [1])
        self.assertTrue(electrode.acceptHoverEvents())

    def test_workspace_has_visible_stop_and_collapsible_logs(self):
        self.assertTrue(hasattr(self.w, 'log_toggle'), '新工作台尚未实现')
        self.w.show()
        self.app.processEvents()
        self.assertTrue(self.w.stop_button.isVisible())
        self.assertFalse(self.w.log_tabs.isVisible())
        self.w.log_toggle.click()
        self.assertTrue(self.w.log_tabs.isVisible())
        self.assertEqual(self.w.log_tabs.widget(0), self.w.log_information)
        self.assertEqual(self.w.log_tabs.widget(1), self.w.serial_information)
        self.w.log_toggle.click()
        self.assertFalse(self.w.log_tabs.isVisible())

    def test_error_stays_visible_with_logs_collapsed(self):
        self.assertTrue(hasattr(self.w, 'notification_label'), '持续可见状态摘要尚未实现')
        self.w.on_control_event('error', '测试通信超时')
        self.w.refresh_enabled()
        self.assertIn('测试通信超时', self.w.notification_label.text())
        self.assertFalse(self.w.log_toggle.isChecked())
        self.assertIsNone(self.w.control.link)
        self.assertIsNone(self.w.camera_logic)

    def test_small_window_keeps_controls_inside_workspace(self):
        self.assertTrue(hasattr(self.w, 'log_toggle'), '新工作台尚未实现')
        self.w.resize(1366, 768)
        self.w.show()
        self.app.processEvents()
        self.assertLessEqual(self.w.width(), 1366)
        self.assertLessEqual(self.w.height(), 768)
        for widget in (self.w.stop_button, self.w.ac_on_button, self.w.graphicsView1,
                       self.w.tool_tabs, self.w.log_toggle):
            self.assertTrue(widget.isVisible())
            point = widget.mapTo(self.w, widget.rect().bottomRight())
            self.assertTrue(self.w.rect().contains(point), widget.objectName())

    def test_auto_table_and_map_do_not_collapse_when_logs_open(self):
        self.w.resize(1366, 768)
        self.w.show()
        self.w.tabWidget.setCurrentIndex(1)
        self.w.add_row([1], 1, '关')
        self.w.add_row([2], 1, '关')
        self.w.log_toggle.click()
        self.app.processEvents()
        self.assertGreaterEqual(self.w.tab2_tab.viewport().height(), 80)
        self.assertGreaterEqual(self.w.graphicsView1.height(), 120)
        self.assertTrue(self.w.stop_button.isVisible())

    def test_peripheral_tabs_do_not_require_horizontal_scrolling(self):
        self.w.resize(1366, 768)
        self.w.show()
        for index in range(self.w.tool_tabs.count()):
            self.w.tool_tabs.setCurrentIndex(index)
            self.app.processEvents()
            self.assertEqual(self.w.tool_tabs.widget(index).horizontalScrollBar().maximum(), 0)

    def test_local_errors_are_visible_without_opening_logs(self):
        self.w.comboBox.clear()
        self.w.open_port()
        self.assertIn('未找到串口', self.w.notification_label.text())
        self.assertTrue(self.w.notification_label.property('alert'))
        self.w.arm()
        self.assertIn('核对电极映射', self.w.notification_label.text())
        self.assertIsNone(self.w.control.link)

    def test_status_notifications_follow_control_events(self):
        for kind, data, expected in (
            ('connected', '', '握手完成'), ('armed', 80, '开启'),
            ('stopped', '停止已确认', '停止已确认'), ('disconnected', '', '断开')):
            self.w.on_control_event(kind, data)
            self.assertIn(expected, self.w.notification_label.text())

    def test_internal_layout_changes_refit_entire_electrode_map(self):
        from PyQt5.QtTest import QTest
        self.w.resize(1600, 950)
        self.w.show()
        QTest.qWait(50)
        for action in (lambda: self.w.log_toggle.click(),
                       lambda: self.w.tabWidget.setCurrentIndex(1)):
            action()
            QTest.qWait(50)
            mapped = self.w.graphicsView1.mapFromScene(self.w.scene.itemsBoundingRect()).boundingRect()
            self.assertTrue(self.w.graphicsView1.viewport().rect().contains(mapped), str(mapped))


if __name__ == '__main__':
    unittest.main()
