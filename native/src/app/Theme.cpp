#include "app/Theme.h"

namespace rq {

QString applicationStyleSheet() {
  return QStringLiteral(R"(
    QWidget { background: #17181b; color: #d7d9dd; font-size: 10pt; }
    QListWidget#navigation { background: #101114; border: none; padding-top: 6px; outline: 0; }
    QListWidget#navigation::item { padding: 10px 16px; color: #aeb2b9; }
    QListWidget#navigation::item:selected { background: #24262b; color: #e3c374; border-left: 3px solid #c9a24b; }
    QLabel#brand { background: #101114; color: #e3c374; font-size: 12pt; font-weight: 600; padding: 12px 16px 6px 16px; }
    QLabel#pageTitle { color: #e3c374; font-size: 15pt; font-weight: 600; }
    QLabel#muted { color: #9a9ea6; }
    QGroupBox { border: 1px solid #33363c; border-radius: 4px; margin-top: 14px; padding: 10px; }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #c7cad0; }
    QPushButton { background: #2a2c32; border: 1px solid #4a4d55; border-radius: 3px; padding: 6px 14px; }
    QPushButton:hover { border-color: #c9a24b; }
    QPushButton:focus { border-color: #e3c374; }
    QPushButton:disabled { color: #6c7078; border-color: #33363c; }
    QLineEdit, QPlainTextEdit { background: #101114; border: 1px solid #3a3d44; border-radius: 3px; padding: 5px; }
    QLineEdit:focus, QPlainTextEdit:focus { border-color: #c9a24b; }
    QComboBox, QDateEdit { background: #101114; border: 1px solid #3a3d44; border-radius: 3px; padding: 5px; min-width: 70px; min-height: 20px; }
    QDateEdit { min-width: 100px; }
    QLineEdit { min-height: 20px; }
    QComboBox:focus, QDateEdit:focus { border-color: #c9a24b; }
    QScrollBar:vertical { background: #101114; width: 12px; margin: 0; }
    QScrollBar:horizontal { background: #101114; height: 12px; margin: 0; }
    QScrollBar::handle { background: #3a3d44; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 2px; }
    QScrollBar::handle:hover { background: #c9a24b; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
    QComboBox QAbstractItemView { background: #101114; selection-background-color: #24262b; selection-color: #e3c374; }
    QTableView { background: #101114; gridline-color: #24262b; border: 1px solid #33363c; selection-background-color: #24262b; selection-color: #e3c374; }
    QHeaderView::section { background: #17181b; color: #aeb2b9; border: none; border-bottom: 1px solid #33363c; padding: 5px; }
    QSplitter::handle { background: #24262b; height: 3px; }
    QProgressBar { background: #101114; border: 1px solid #3a3d44; border-radius: 3px; text-align: center; }
    QProgressBar::chunk { background: #c9a24b; }
    QStatusBar { background: #101114; color: #aeb2b9; }
    QStatusBar QLabel { background: transparent; }
  )");
}

}  // namespace rq
