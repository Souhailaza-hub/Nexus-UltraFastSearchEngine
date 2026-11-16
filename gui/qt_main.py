"""NEXUS Search desktop interface (Phase 5)."""
from __future__ import annotations

import ctypes
import os
import re
import sys
import time
from datetime import datetime, timedelta, timezone
from PySide6.QtCore import QEvent, QObject, QTimer, Qt, Signal
from PySide6.QtGui import QAction, QFont, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QFrame,
    QHeaderView,
    QHBoxLayout,
    QGridLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from engine_client import EngineClient


class UiSignals(QObject):
    search_done = Signal(int, object, float)
    search_failed = Signal(int, str)
    index_done = Signal(object, str)
    stats_done = Signal(object)
    snapshot_done = Signal(bool, str)


class MainWindow(QMainWindow):
    HOTKEY_ID = 0x4E58
    WM_HOTKEY = 0x0312

    def __init__(self, auto_index=True):
        super().__init__()
        self.client = EngineClient()
        self.signals = UiSignals(self)
        self.signals.search_done.connect(self._show_results)
        self.signals.search_failed.connect(self._search_failed)
        self.signals.index_done.connect(self._index_finished)
        self.signals.stats_done.connect(self._show_stats)
        self.signals.snapshot_done.connect(self._snapshot_finished)
        self._search_generation = 0
        self._stats_pending = False
        self._index_pending = False
        self._hotkey_registered = False
        self.initial_index_folder = "C:\\"
        self.current_index_folder = self.initial_index_folder

        self.setWindowTitle("NEXUS Search")
        self.resize(1040, 680)
        self.setMinimumSize(760, 480)
        self._build_ui()
        self._install_shortcuts()
        self._install_hotkey()

        self.debounce = QTimer(self)
        self.debounce.setSingleShot(True)
        self.debounce.setInterval(120)
        self.debounce.timeout.connect(self._start_search)

        self.stats_timer = QTimer(self)
        self.stats_timer.setInterval(1200)
        self.stats_timer.timeout.connect(self._request_stats)
        self.stats_timer.start()
        self._request_stats()
        if auto_index:
            QTimer.singleShot(0, self._start_initial_indexing)

    def _build_ui(self):
        root = QWidget(self)
        root.setObjectName("appRoot")
        layout = QVBoxLayout(root)
        layout.setContentsMargins(28, 22, 28, 20)
        layout.setSpacing(12)

        heading = QHBoxLayout()
        heading.setSpacing(10)
        title = QLabel("NEXUS Search")
        title.setObjectName("title")
        subtitle = QLabel("Find files across your indexed folders")
        subtitle.setObjectName("subtitle")
        heading.addWidget(title)
        divider = QFrame()
        divider.setObjectName("headingDivider")
        divider.setFixedSize(1, 20)
        heading.addWidget(divider)
        heading.addWidget(subtitle)
        heading.addStretch(1)
        self.index_button = QPushButton("Index folder…")
        self.index_button.setObjectName("primaryButton")
        self.index_button.clicked.connect(self._choose_folder)
        heading.addWidget(self.index_button)
        layout.addLayout(heading)

        folder_row = QHBoxLayout()
        folder_row.setContentsMargins(2, 0, 2, 0)
        folder_title = QLabel("INDEX FOLDER")
        folder_title.setObjectName("folderCaption")
        self.index_folder_label = QLabel(self.current_index_folder)
        self.index_folder_label.setObjectName("folderPath")
        self.index_folder_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        folder_row.addWidget(folder_title)
        folder_row.addWidget(self.index_folder_label)
        folder_row.addStretch(1)
        layout.addLayout(folder_row)

        search_row = QHBoxLayout()
        search_row.setSpacing(8)
        self.query = QLineEdit()
        self.query.setObjectName("searchBox")
        self.query.setPlaceholderText("Search by file or folder name")
        self.query.setClearButtonEnabled(True)
        self.query.installEventFilter(self)
        self.query.setMinimumHeight(46)
        self.mode = QComboBox()
        self.mode.addItem("Prefix", "prefix")
        self.mode.addItem("Substring", "substring")
        self.mode.addItem("Exact", "exact")
        self.mode.setFixedWidth(118)
        self.mode.setMinimumHeight(46)
        self.filters_button = QPushButton("Filters  +")
        self.filters_button.setObjectName("secondaryButton")
        self.filters_button.setCheckable(True)
        self.filters_button.setMinimumHeight(46)
        self.filters_button.setFixedWidth(104)
        self.filters_button.toggled.connect(self._toggle_filters)
        search_row.addWidget(self.query, 1)
        search_row.addWidget(self.mode)
        search_row.addWidget(self.filters_button)
        layout.addLayout(search_row)

        self.filter_panel = QFrame()
        self.filter_panel.setObjectName("filterPanel")
        filter_layout = QGridLayout(self.filter_panel)
        filter_layout.setContentsMargins(14, 12, 14, 12)
        filter_layout.setHorizontalSpacing(10)
        filter_layout.setVerticalSpacing(9)
        self.extension_filter = QLineEdit()
        self.extension_filter.setPlaceholderText("Extension (e.g. .pdf)")
        self.path_filter = QLineEdit()
        self.path_filter.setPlaceholderText("Limit results to this path")
        self.volume_filter = QComboBox()
        self.volume_filter.addItem("All drives", "")
        for volume in self.client.engine.discover_volumes():
            if volume.ready:
                self.volume_filter.addItem(f"{volume.root_path} · {volume.filesystem or volume.drive_type}", volume.root_path)
        self.kind_filter = QComboBox()
        self.kind_filter.addItem("Files & folders", "all")
        self.kind_filter.addItem("Files", "files")
        self.kind_filter.addItem("Folders", "folders")
        self.min_size_filter = QLineEdit()
        self.min_size_filter.setPlaceholderText("Minimum size")
        self.max_size_filter = QLineEdit()
        self.max_size_filter.setPlaceholderText("Maximum size")
        self.modified_after_filter = QLineEdit()
        self.modified_after_filter.setPlaceholderText("Modified after · YYYY-MM-DD")
        self.modified_before_filter = QLineEdit()
        self.modified_before_filter.setPlaceholderText("Modified before · YYYY-MM-DD")
        self.extension_filter.setMinimumWidth(135)
        self.path_filter.setMinimumWidth(220)
        self.volume_filter.setMinimumWidth(155)
        self.kind_filter.setMinimumWidth(140)
        self.min_size_filter.setMinimumWidth(145)
        self.max_size_filter.setMinimumWidth(145)
        self.modified_after_filter.setMinimumWidth(180)
        self.modified_before_filter.setMinimumWidth(180)
        for column, stretch in enumerate((1, 2, 1, 1)):
            filter_layout.setColumnStretch(column, stretch)
        filter_layout.addWidget(self.extension_filter, 0, 0)
        filter_layout.addWidget(self.path_filter, 0, 1)
        filter_layout.addWidget(self.volume_filter, 0, 2)
        filter_layout.addWidget(self.kind_filter, 0, 3)
        filter_layout.addWidget(self.min_size_filter, 1, 0)
        filter_layout.addWidget(self.max_size_filter, 1, 1)
        filter_layout.addWidget(self.modified_after_filter, 1, 2)
        filter_layout.addWidget(self.modified_before_filter, 1, 3)
        self.filter_panel.hide()
        layout.addWidget(self.filter_panel)

        results_heading = QHBoxLayout()
        results_heading.setContentsMargins(2, 5, 2, 0)
        results_label = QLabel("Results")
        results_label.setObjectName("sectionTitle")
        self.result_count = QLabel("Search your indexed folders")
        self.result_count.setObjectName("mutedLabel")
        results_heading.addWidget(results_label)
        results_heading.addStretch(1)
        results_heading.addWidget(self.result_count)
        layout.addLayout(results_heading)

        self.table = QTableWidget(0, 5)
        self.table.setHorizontalHeaderLabels(["Name", "Path", "Type", "Size", "Modified"])
        self.table.setObjectName("resultsTable")
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setAlternatingRowColors(True)
        self.table.setShowGrid(False)
        self.table.setWordWrap(False)
        self.table.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.table.verticalHeader().setDefaultSectionSize(38)
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.ResizeToContents)
        self.table.doubleClicked.connect(lambda _index: self._open_selected())
        layout.addWidget(self.table, 1)

        footer = QHBoxLayout()
        self.status = QLabel("Index idle · choose a folder to begin")
        self.status.setObjectName("status")
        self.duration = QLabel("")
        self.duration.setObjectName("status")
        footer.addWidget(self.status, 1)
        footer.addWidget(self.duration)
        layout.addLayout(footer)
        self.setCentralWidget(root)
        self.query.textChanged.connect(lambda _text: self._queue_search())
        self.query.returnPressed.connect(self._open_selected)
        self.mode.currentIndexChanged.connect(lambda _index: self._queue_search())
        for widget in (self.extension_filter, self.path_filter, self.min_size_filter,
                       self.max_size_filter, self.modified_after_filter, self.modified_before_filter):
            widget.textChanged.connect(lambda _text: self._queue_search())
        self.volume_filter.currentIndexChanged.connect(lambda _index: self._queue_search())
        self.kind_filter.currentIndexChanged.connect(lambda _index: self._queue_search())

        menu = self.menuBar().addMenu("File")
        save_action = QAction("Save index snapshot…", self)
        save_action.triggered.connect(self._save_snapshot)
        load_action = QAction("Load index snapshot…", self)
        load_action.triggered.connect(self._load_snapshot)
        exit_action = QAction("Exit", self)
        exit_action.triggered.connect(self.close)
        menu.addAction(save_action)
        menu.addAction(load_action)
        menu.addSeparator()
        menu.addAction(exit_action)
        self.setStyleSheet("""
            QMainWindow, QWidget#appRoot { background: #171a1f; color: #e3e7ec; font-family: 'Segoe UI'; font-size: 9pt; }
            QLabel#title { font-size: 15pt; font-weight: 650; color: #f0f2f5; }
            QLabel#subtitle, QLabel#mutedLabel { color: #929ba7; }
            QLabel#folderCaption { color: #77818e; font-size: 8pt; font-weight: 650; letter-spacing: 1px; }
            QLabel#folderPath { color: #c4ccd5; }
            QLabel#sectionTitle { color: #c5cbd3; font-size: 9pt; font-weight: 650; }
            QLabel#status { color: #98a1ad; }
            QFrame#headingDivider { background: #363c45; }
            QPushButton { border-radius: 6px; padding: 0 13px; font-weight: 550; }
            QPushButton#primaryButton { background: #426d99; border: 1px solid #426d99; color: #ffffff; }
            QPushButton#primaryButton:hover { background: #507eac; }
            QPushButton#secondaryButton { background: #22262d; border: 1px solid #3a414b; color: #d4d9e0; }
            QPushButton#secondaryButton:hover, QPushButton#secondaryButton:checked { background: #2b323b; border-color: #526173; }
            QLineEdit, QComboBox { background: #20242b; border: 1px solid #363d47; border-radius: 6px; padding: 0 11px; color: #e2e6eb; selection-background-color: #355675; }
            QLineEdit#searchBox { font-size: 11pt; padding-left: 14px; border-color: #414955; }
            QLineEdit:focus, QComboBox:focus { border: 1px solid #6285aa; }
            QLineEdit::placeholder { color: #77818e; }
            QComboBox::drop-down { border: none; width: 25px; }
            QFrame#filterPanel { background: #1d2127; border: 1px solid #30363f; border-radius: 8px; }
            QFrame#filterPanel QLineEdit, QFrame#filterPanel QComboBox { min-height: 36px; }
            QTableWidget#resultsTable { background: #1c2026; alternate-background-color: #20252c; color: #dfe4ea; border: 1px solid #30363f; border-radius: 8px; outline: 0; }
            QTableWidget#resultsTable::item { padding-left: 8px; border-bottom: 1px solid #282d35; }
            QTableWidget#resultsTable::item:selected { color: #f1f4f7; background: #2e4359; }
            QHeaderView::section { background: #20242b; color: #929ba7; font-size: 8pt; font-weight: 600; padding: 9px 8px; border: none; border-bottom: 1px solid #343b45; }
            QMenuBar { background: #171a1f; color: #cbd1d9; }
            QMenuBar::item { padding: 5px 9px; }
            QMenu, QMenu::item { background: #22262d; color: #e0e4e9; }
            QMenu::item:selected { background: #2e4359; }
        """)

    def _toggle_filters(self, enabled):
        self.filter_panel.setVisible(enabled)
        self.filters_button.setText("Filters  −" if enabled else "Filters  +")

    def _install_shortcuts(self):
        self._shortcut("Ctrl+Space", self._show_and_focus)
        self._shortcut("Escape", self.hide)
        self._shortcut("Return", self._open_selected)
        self._shortcut("Enter", self._open_selected)
        self._shortcut("Ctrl+Return", self._reveal_selected)
        self._shortcut("Ctrl+Enter", self._reveal_selected)
        self._shortcut("Ctrl+C", self._copy_selected_path)
        self._shortcut("Ctrl+Backspace", self.query.clear)

    def _shortcut(self, sequence, callback):
        shortcut = QShortcut(QKeySequence(sequence), self)
        shortcut.setContext(Qt.ShortcutContext.ApplicationShortcut)
        shortcut.activated.connect(callback)

    def eventFilter(self, watched, event):
        if watched is self.query and event.type() == QEvent.Type.KeyPress:
            modifiers = event.modifiers()
            key = event.key()
            if modifiers & Qt.KeyboardModifier.ControlModifier:
                if key == Qt.Key.Key_Backspace:
                    self.query.clear()
                    return True
                if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                    self._reveal_selected()
                    return True
                if key == Qt.Key.Key_C and self._selected_path():
                    self._copy_selected_path()
                    return True
            if key == Qt.Key.Key_Escape:
                self.hide()
                return True
            if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                self._open_selected()
                return True
            if key in (Qt.Key.Key_Up, Qt.Key.Key_Down) and self.table.rowCount():
                current = self.table.currentRow()
                step = -1 if key == Qt.Key.Key_Up else 1
                row = 0 if current < 0 else max(0, min(self.table.rowCount() - 1, current + step))
                self.table.setCurrentCell(row, 0)
                self.table.setFocus()
                return True
        return super().eventFilter(watched, event)

    def _install_hotkey(self):
        try:
            self._hotkey_registered = bool(ctypes.windll.user32.RegisterHotKey(None, self.HOTKEY_ID, 0x0002, 0x20))
            if not self._hotkey_registered:
                self.status.setText("Ctrl+Space works while NEXUS is focused")
        except (AttributeError, OSError):
            self._hotkey_registered = False

    def nativeEvent(self, event_type, message):
        if self._hotkey_registered:
            try:
                address = int(message)
                msg_id = ctypes.cast(address, ctypes.POINTER(ctypes.c_uint32))[0]
                if msg_id == self.WM_HOTKEY:
                    self._show_and_focus()
                    return True, 0
            except (TypeError, ValueError, OSError):
                pass
        return super().nativeEvent(event_type, message)

    def _queue_search(self):
        self._search_generation += 1
        self.debounce.start()

    def _start_search(self):
        text = self.query.text().strip()
        if not text:
            self.table.setRowCount(0)
            self.duration.clear()
            self.result_count.setText("Search your indexed folders")
            return
        generation = self._search_generation
        mode = self.mode.currentData()
        started = time.perf_counter()
        try:
            filters = self._collect_filters()
        except ValueError as exc:
            self.duration.setText(f"Filter error: {exc}")
            return
        future = self.client.search_async(text, mode, 100, filters)

        def finished(done):
            elapsed_ms = (time.perf_counter() - started) * 1000
            try:
                self.signals.search_done.emit(generation, done.result(), elapsed_ms)
            except Exception as exc:  # delivered to the GUI thread
                self.signals.search_failed.emit(generation, str(exc))
        future.add_done_callback(finished)

    @staticmethod
    def _parse_size(value):
        value = value.strip()
        if not value:
            return None
        match = re.fullmatch(r"(\d+(?:\.\d+)?)\s*(B|KB|MB|GB|TB)?", value, re.IGNORECASE)
        if not match:
            raise ValueError("sizes use a number with optional B, KB, MB, GB, or TB")
        multiplier = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3, "TB": 1024**4}
        unit = (match.group(2) or "B").upper()
        return int(float(match.group(1)) * multiplier[unit])

    @staticmethod
    def _parse_date(value, end_of_day=False):
        value = value.strip()
        if not value:
            return None
        try:
            day = datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        except ValueError as exc:
            raise ValueError("dates use YYYY-MM-DD") from exc
        if end_of_day:
            day += timedelta(days=1)
            return int((day - datetime(1601, 1, 1, tzinfo=timezone.utc)).total_seconds() * 10_000_000) - 1
        return int((day - datetime(1601, 1, 1, tzinfo=timezone.utc)).total_seconds() * 10_000_000)

    def _collect_filters(self):
        kind = self.kind_filter.currentData()
        return {
            "extension": self.extension_filter.text().strip(),
            "path_prefix": self.path_filter.text().strip(),
            "volume_id": self.volume_filter.currentData() or "",
            "files_only": kind == "files",
            "folders_only": kind == "folders",
            "min_size_bytes": self._parse_size(self.min_size_filter.text()),
            "max_size_bytes": self._parse_size(self.max_size_filter.text()),
            "modified_after": self._parse_date(self.modified_after_filter.text()),
            "modified_before": self._parse_date(self.modified_before_filter.text(), end_of_day=True),
        }

    def _show_results(self, generation, records, elapsed_ms):
        if generation != self._search_generation:
            return
        self.table.setRowCount(len(records))
        self.result_count.setText("100 shown" if len(records) == 100 else f"{len(records)} matches")
        for row, record in enumerate(records):
            values = [record.name, record.path, record.extension or ("Folder" if record.is_directory else "File"),
                      self._format_size(record.size_bytes, record.is_directory), self._format_time(record.modified_time)]
            for column, value in enumerate(values):
                item = QTableWidgetItem(value)
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, record.path)
                self.table.setItem(row, column, item)
        if records:
            self.table.selectRow(0)
        self.duration.setText(f"{elapsed_ms:.2f} ms · {len(records)} results")

    def _search_failed(self, generation, message):
        if generation == self._search_generation:
            self.duration.setText(f"Search error: {message}")

    @staticmethod
    def _format_size(size, is_directory):
        if is_directory:
            return "—"
        value = float(size)
        for unit in ("B", "KB", "MB", "GB", "TB"):
            if value < 1024 or unit == "TB":
                return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
            value /= 1024

    @staticmethod
    def _format_time(filetime):
        if not filetime:
            return "—"
        try:
            timestamp = datetime(1601, 1, 1, tzinfo=timezone.utc) + timedelta(microseconds=filetime / 10)
            return timestamp.astimezone().strftime("%Y-%m-%d %H:%M")
        except (OverflowError, ValueError):
            return "—"

    def _choose_folder(self):
        path = QFileDialog.getExistingDirectory(
            self, "Choose a folder to index", self.initial_index_folder
        )
        if not path:
            return
        self._launch_indexing(path)

    def _start_initial_indexing(self):
        self._launch_indexing(self.initial_index_folder)

    def _launch_indexing(self, path):
        self.current_index_folder = path
        self.index_folder_label.setText(path)
        self.status.setText(f"Indexing {path}…")
        self.index_button.setEnabled(False)
        self._index_pending = True
        future = self.client.index_async(path)

        def finished(done):
            error = ""
            try:
                done.result()
            except Exception as exc:
                error = str(exc)
            self.signals.index_done.emit(None, error)
        future.add_done_callback(finished)

    def _index_finished(self, _unused, error):
        self._index_pending = False
        self.index_button.setEnabled(True)
        if error:
            self.status.setText(f"Index error: {error}")
        else:
            self.status.setText("Index scan finished")
            self._queue_search()
            self._request_stats()

    def _request_stats(self):
        if self._stats_pending:
            return
        self._stats_pending = True
        future = self.client.stats_async()
        future.add_done_callback(lambda done: self._stats_finished(done))

    def _stats_finished(self, future):
        try:
            stats = future.result()
            self.signals.stats_done.emit(stats)
        except Exception:
            self.signals.stats_done.emit(None)

    def _show_stats(self, stats):
        self._stats_pending = False
        if stats is None:
            return
        busy = f"Indexing {self.current_index_folder}" if stats.indexing else "Ready"
        self.status.setText(f"{busy} · {stats.files:,} files · {stats.directories:,} folders · {stats.errors} errors · {stats.estimated_memory_bytes / (1024 * 1024):.1f} MiB")

    def _selected_path(self):
        row = self.table.currentRow()
        if row < 0:
            return None
        item = self.table.item(row, 0)
        return item.data(Qt.ItemDataRole.UserRole) if item else None

    def _open_selected(self):
        path = self._selected_path()
        if path:
            try:
                os.startfile(path)
            except OSError as exc:
                QMessageBox.warning(self, "Could not open file", str(exc))

    def _reveal_selected(self):
        path = self._selected_path()
        if path:
            os.spawnl(os.P_NOWAIT, os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "explorer.exe"),
                      "explorer.exe", "/select," + path)

    def _copy_selected_path(self):
        path = self._selected_path()
        if path:
            QApplication.clipboard().setText(path)

    def _show_and_focus(self):
        self.show()
        self.raise_()
        self.activateWindow()
        self.query.setFocus()

    def _save_snapshot(self):
        path, _ = QFileDialog.getSaveFileName(self, "Save index snapshot", "nexus-index.nxs", "NEXUS snapshots (*.nxs)")
        if not path:
            return
        self.status.setText("Saving snapshot…")
        future = self.client.save_snapshot_async(path)
        def finished(done):
            try:
                self.signals.snapshot_done.emit(bool(done.result()), "save")
            except Exception as exc:
                self.signals.snapshot_done.emit(False, f"save: {exc}")
        future.add_done_callback(finished)

    def _load_snapshot(self):
        path, _ = QFileDialog.getOpenFileName(self, "Load index snapshot", "", "NEXUS snapshots (*.nxs)")
        if not path:
            return
        self.status.setText("Loading snapshot…")
        future = self.client.load_snapshot_async(path)
        def finished(done):
            try:
                self.signals.snapshot_done.emit(bool(done.result()), "load")
            except Exception as exc:
                self.signals.snapshot_done.emit(False, f"load: {exc}")
        future.add_done_callback(finished)

    def _snapshot_finished(self, success, operation):
        if operation == "save":
            self.status.setText("Snapshot saved" if success else "Snapshot save failed")
        elif operation == "load":
            self.status.setText("Snapshot loaded" if success else "Snapshot load failed")
            if success:
                self._queue_search()
                self._request_stats()
        else:
            self.status.setText(operation)

    def closeEvent(self, event):
        if self._hotkey_registered:
            ctypes.windll.user32.UnregisterHotKey(None, self.HOTKEY_ID)
            self._hotkey_registered = False
        self.client.close()
        event.accept()


def main():
    smoke = "--smoke" in sys.argv[1:]
    app = QApplication(sys.argv)
    app.setApplicationName("NEXUS Search")
    app.setFont(QFont("Segoe UI", 9))
    window = MainWindow(auto_index=not smoke)
    window.show()
    if smoke:
        QTimer.singleShot(500, window.close)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
