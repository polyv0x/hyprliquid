#!/usr/bin/env python3
"""Opaque scene and transparent content windows for native compositor captures."""
import argparse
import sys

from PySide6.QtCore import Qt, QRectF
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen
from PySide6.QtWidgets import QApplication, QWidget

parser = argparse.ArgumentParser()
parser.add_argument('role', choices=['backdrop', 'measured', 'platform', 'lens', 'darker', 'ultra_darker'])
parser.add_argument('--image')
parser.add_argument('--label', default='PLATFORM')
parser.add_argument('--compact', action='store_true')
parser.add_argument('--dark', action='store_true')
parser.add_argument('--width', type=int)
parser.add_argument('--height', type=int)
args = parser.parse_args()


def font(size, weight=QFont.Weight.Normal, spacing=0):
    result = QFont('Lato')
    result.setPixelSize(size)
    result.setWeight(weight)
    if spacing:
        result.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, spacing)
    return result


class DemoWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Hyprliquid demo / ' + args.role)
        self.setWindowFlags(Qt.WindowType.Window | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.backdrop = args.role == 'backdrop'
        self.scene = QImage(args.image) if args.image else None
        if self.backdrop and (self.scene is None or self.scene.isNull()):
            raise RuntimeError('Cannot load the rendered backdrop')
        self.resize(args.width or (1920 if self.backdrop else 900),
                    args.height or (1200 if self.backdrop else 540))

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        painter.fillRect(self.rect(), Qt.GlobalColor.transparent)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
        if self.backdrop:
            painter.drawImage(self.rect(), self.scene)
            painter.setPen(QColor('#f1f4fa'))
            painter.setFont(font(68, QFont.Weight.Bold))
            painter.drawText(76, 122, 'Hyprliquid')
            painter.setPen(QColor('#a4adca'))
            painter.setFont(font(25))
            painter.drawText(80, 165, 'Glass after dark.' if args.dark else 'Glass for Hyprland.')
            painter.setFont(font(15, QFont.Weight.DemiBold, 2.8))
            painter.setPen(QColor('#a4b4c7'))
            painter.drawText(QRectF(self.width() - 510, 89, 430, 36), Qt.AlignmentFlag.AlignRight, args.label)
            painter.setPen(QPen(QColor(183, 211, 221, 60), 1))
            painter.drawLine(80, self.height() - 99, self.width() - 80, self.height() - 99)
            painter.setFont(font(16))
            painter.setPen(QColor('#9bb7c5'))
            painter.drawText(80, self.height() - 59, 'Real windows. Native glass.')
            painter.drawText(QRectF(self.width() - 700, self.height() - 81, 620, 30),
                             Qt.AlignmentFlag.AlignRight, 'Refraction  /  Blur  /  Light')
        else:
            # These are ordinary sharp application pixels. The plugin supplies
            # the entire glass body behind the transparent window background.
            margin = 38 if args.compact else 46
            painter.setFont(font(13, QFont.Weight.DemiBold, 2.4))
            painter.setPen(QColor(238, 243, 255, 185))
            painter.drawText(margin, 49, 'HYPRLIQUID / DARK' if args.dark else 'HYPRLIQUID')
            painter.setFont(font(38 if args.compact else 46, QFont.Weight.Bold))
            painter.setPen(QColor('#ffffff'))
            painter.drawText(margin, 107, 'Ultra dark' if args.role == 'ultra_darker' else args.role.capitalize())
            painter.setFont(font(18))
            painter.setPen(QColor(247, 249, 255, 220))
            captions = {'measured': 'Soft blur. Clear content.',
                        'platform': 'Refraction, with a softer edge.',
                        'lens': 'A different view through glass.',
                        'darker': 'Deep tones. Soft light.',
                        'ultra_darker': 'A quieter glass surface.'}
            painter.drawText(margin, 143, captions[args.role])
            painter.setPen(QPen(QColor(255, 255, 255, 75), 1))
            painter.drawLine(margin, self.height() - 83, self.width() - margin, self.height() - 83)
            painter.setFont(font(15))
            painter.setPen(QColor(255, 255, 255, 215))
            painter.drawText(margin, self.height() - 46, 'Transparent window · sharp foreground')
        painter.end()


app = QApplication(sys.argv[:1])
app.setApplicationName('hyprliquid-demo-' + args.role)
app.setDesktopFileName('hyprliquid-demo-' + args.role)
window = DemoWindow()
window.show()
sys.exit(app.exec())
