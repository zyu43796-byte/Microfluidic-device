import importlib.util
import os
import unittest

os.environ['QT_QPA_PLATFORM'] = 'offscreen'


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('adapted_main'), 'adapted UI entry is missing')
        from PyQt5.QtWidgets import QApplication
        from adapted_main import MainWindow
        self.app = QApplication.instance() or QApplication([])
        self.w = MainWindow()
        self.w.poll_timer.stop()

    def tearDown(self):
        if hasattr(self, 'w'):
            self.w.close()

    def test_no_hardware_open_at_start_and_unsupported_tabs_disabled(self):
        self.assertIsNone(self.w.control.link)
        self.assertIsNone(self.w.camera_logic)
        self.assertFalse(self.w.tabWidget.isTabEnabled(2))
        self.assertFalse(self.w.tabWidget2.isEnabled())
        self.assertIn('未知', self.w.label1_button.text())
        self.assertTrue(self.w.lineEdit3_2.isHidden())
        self.assertTrue(self.w.repeat_button.isHidden())
        self.assertIn('持续', self.w.windowTitle())

    def test_stop_clears_even_pinned_selection(self):
        electrode = self.w.scene._electrode_by_id(1)
        electrode.setPinned(True)
        self.w.stop_output()
        self.assertFalse(electrode.isPinned)
        self.assertFalse(electrode.isDrive)

    def test_magnet_tab_present_without_temperature_feature(self):
        self.assertTrue(hasattr(self.w, 'tool_tabs'), '右下角工具标签尚未实现')
        self.assertEqual([self.w.tool_tabs.tabText(i) for i in range(self.w.tool_tabs.count())],
                         ['灯光', '磁杆'])
        panel = self.w.magnet_panel
        self.assertFalse(panel.on_button.isEnabled())
        self.assertFalse(panel.off_button.isEnabled())
        self.assertEqual((panel.duration.minimum(), panel.duration.maximum()), (100, 5000))
        self.assertIn('不等于断电', panel.note.text())

    def test_magnet_buttons_drive_without_enabling_ac(self):
        self.assertTrue(hasattr(self.w, 'magnet_panel'), '磁杆页面尚未实现')
        from test_magnet import MagnetSerial
        from test_control import Clock
        clock, port = Clock(), MagnetSerial()
        self.w.control.clock = clock
        self.w.control.connect(port)

        def settle():
            for _ in range(1500):
                clock.now += .01
                self.w.control.tick()
                if not self.w.control.busy:
                    self.w.refresh_enabled()
                    return
            self.fail('magnet GUI workflow did not finish')

        settle()
        panel = self.w.magnet_panel
        panel.on_button.click()
        settle()
        self.assertTrue(self.w.control.magnet_active)
        self.assertFalse(self.w.control.armed)
        self.assertIn('ON', panel.status.text())
        self.assertFalse(self.w.button_open_camera.isEnabled())
        self.assertFalse(self.w.camera_index.isEnabled())
        panel.off_button.click()
        settle()
        self.assertFalse(self.w.control.magnet_active)
        self.assertIn('默认', panel.status.text())
        panel.duration.setValue(200)
        panel.timed_button.click()
        settle()
        self.assertEqual(port.writes.count('magnet on --json'), 2)
        self.assertEqual(port.writes.count('magnet off --json'), 2)
        self.w.control.stop(disconnect=True)
        settle()
        self.assertFalse(panel.on_button.isEnabled())

    def test_table_validation_and_no_silent_bad_id(self):
        from PyQt5.QtWidgets import QTableWidgetItem
        self.w.add_row([1, 2], .2, '关')
        self.assertEqual(self.w.read_rows(), [([1, 2], 200, '关')])
        self.w.tab2_tab.setItem(0, 0, QTableWidgetItem('[61]'))
        with self.assertRaises(ValueError):
            self.w.read_rows()

    def test_manual_click_without_arm_does_not_claim_success(self):
        self.w.scene._electrode_by_id(1).setDriveStatus(True)
        self.w.send_selection()
        self.assertEqual(self.w.scene.hardware_drive_ids(), [])
        self.assertIn('AC 模式未开启', self.w.log_information.toPlainText())

    def test_ac_switch_and_light_panel_replace_authorization_and_plot(self):
        self.assertEqual(self.w.ac_on_button.text(), '开启 AC 模式')
        self.assertEqual(self.w.ac_off_button.text(), '关闭 AC')
        self.assertNotIn('授权', self.w.state_label.text())
        self.assertTrue(self.w.tabWidget2.isHidden())
        self.assertEqual(self.w.light_choice.count(), 14)
        self.assertEqual(self.w.light_brightness.maximum(), 255)
        self.assertFalse(self.w.light_apply_button.isEnabled())
        self.assertFalse(self.w.light_off_button.isEnabled())

    def test_light_panel_sends_confirmed_color_and_off_without_ac(self):
        from test_control import Clock, FirmwareSerial
        clock, port = Clock(), FirmwareSerial()
        self.w.control.clock = clock
        original_write = port.write

        def write(data):
            if data.startswith(b'ledstrip '):
                port.writes.append(data.decode().strip())
                if data.startswith(b'ledstrip off'):
                    port.rx.extend(b'ledstrip off count=30 rgb=0,0,0\r\n')
                else:
                    port.rx.extend(b'ledstrip color count=30 color=red brightness=42 rgb=42,0,0 ms=0\r\n')
                return len(data)
            return original_write(data)

        port.write = write
        self.w.control.connect(port)

        def settle():
            for _ in range(1000):
                clock.now += .01
                self.w.control.tick()
                if not self.w.control.busy:
                    self.w.refresh_enabled()
                    return
            self.fail('Lighting GUI workflow did not finish')

        settle()
        self.assertTrue(self.w.light_apply_button.isEnabled())
        self.assertFalse(self.w.ac_on_button.isEnabled())
        self.w.light_color_buttons[1].click()
        self.w.light_slider.setValue(42)
        self.assertEqual(self.w.light_choice.currentData(), 'red')
        self.assertEqual(self.w.light_brightness.value(), 42)
        self.assertFalse(self.w.light_interval.isEnabled())
        self.w.light_apply_button.click()
        settle()
        self.assertEqual(port.writes[-1], 'ledstrip color 30 red 42')
        self.assertIn('已确认', self.w.light_status.text())
        self.w.light_off_button.click()
        settle()
        self.assertEqual(port.writes[-1], 'ledstrip off 30')
        self.assertFalse(self.w.control.armed)
        self.assertFalse(any('arm on' in x for x in port.writes))
        self.w.control.stop(disconnect=True)
        settle()

    def test_ac_off_remains_clickable_during_startup_and_cancels_it(self):
        from test_control import Clock, FirmwareSerial
        clock, port = Clock(), FirmwareSerial()
        self.w.control.clock = clock
        self.w.control.connect(port)

        def settle():
            for _ in range(1000):
                clock.now += .01
                self.w.control.tick()
                if not self.w.control.busy:
                    self.w.refresh_enabled()
                    return
            self.fail('AC GUI workflow did not finish')

        settle()
        self.w.mapping_check.setChecked(True)
        self.w.ac_on_button.click()
        self.w.refresh_enabled()
        self.assertTrue(self.w.control.busy)
        self.assertTrue(self.w.ac_off_button.isEnabled())
        self.assertTrue(self.w.stop_button.isEnabled())
        self.w.ac_off_button.click()
        settle()
        self.assertFalse(self.w.control.armed)
        self.assertEqual(port.writes[-1], 'all_stop --json')
        self.assertNotIn('hv mode ac --json', port.writes)
        self.w.control.stop(disconnect=True)
        settle()

    def test_real_topology_contains_exactly_allowed_electrodes(self):
        from control import VALID_IDS
        self.assertEqual({e.ID for e in self.w.scene._iter_electrodes()}, VALID_IDS)

    def test_arrow_shortcuts_do_not_apply_while_editing_parameters(self):
        for button in (self.w.button_up, self.w.button_down, self.w.button_left, self.w.button_right):
            self.assertTrue(button.shortcut().isEmpty(), 'Window-wide arrow shortcut can trigger output while editing')

    def test_full_gui_workflow_uses_only_ascii_and_confirms_off(self):
        from test_control import Clock, FirmwareSerial
        clock = Clock()
        port = FirmwareSerial()
        self.w.control.clock = clock
        self.w.mapping_check.setChecked(True)
        self.w.control.connect(port)

        def settle():
            for _ in range(1000):
                clock.now += .01
                self.w.control.tick()
                if not self.w.control.busy:
                    return
            self.fail('GUI workflow did not finish')

        settle()
        before_status = len(port.writes)
        self.w.request_status()
        settle()
        self.assertEqual(port.writes[before_status:], ['gpio read --json', 'sys'])
        self.w.arm()
        settle()
        self.w.scene._electrode_by_id(1).setDriveStatus(True)
        self.w.send_selection()
        settle()
        self.assertIn('连续 AC 输出中', self.w.output_label.text())
        self.assertEqual(port.mode, 'ac')
        self.w.move_electrodes(0, -1)
        settle()
        self.assertEqual(self.w.scene.hardware_drive_ids(), [2])
        self.assertEqual(port.mode, 'ac')
        self.w.clear_selection()
        settle()
        self.assertEqual(port.logic, [0] * 16)
        self.assertEqual(port.mode, 'off')
        self.assertIn('AC 已关闭', self.w.output_label.text())
        self.w.control.stop(disconnect=True)
        settle()
        self.assertFalse(port.is_open)
        self.assertTrue(all(line.split()[0] in ('sys', 'gpio', 'hv', 'array', 'all_stop') for line in port.writes))
