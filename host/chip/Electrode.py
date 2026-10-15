from PyQt5.QtCore import QPoint, QRectF, Qt
from PyQt5.QtGui import QColor, QPolygon, QPen, QPainterPath, QFont
from PyQt5.QtWidgets import QGraphicsItem, QStyleOptionGraphicsItem


class Electrode(QGraphicsItem):
    name = "Electrode"

    def __init__(self, event=None, pos_list=None, ID=None, DriveID=None):
        super().__init__()
        if pos_list is None:
            pos_list = [[], []]
        self.isDrive = False
        self.isPinned = False
        self.showDriveID = True
        self.isTransparent = False
        self.ID = ID
        self.DriveID = self.ID if DriveID is None else DriveID
        self.x_cord = list(pos_list[0])
        self.x_cord.append(self.x_cord[0])
        self.y_cord = list(pos_list[1])
        self.y_cord.append(self.y_cord[0])
        self.W = max(self.x_cord) - min(self.x_cord)
        self.H = max(self.y_cord) - min(self.y_cord)
        self.x0 = int(min(self.x_cord))
        self.y0 = int(min(self.y_cord))
        self.xcenter = sum(self.x_cord) / len(self.x_cord)
        self.ycenter = sum(self.y_cord) / len(self.y_cord)
        self.points = [QPoint(int(x), int(y)) for x, y in zip(self.x_cord, self.y_cord)]
        self.setFlags(QGraphicsItem.ItemIsSelectable)
        self.setFlag(QGraphicsItem.ItemIsFocusable, False)
        self.setCacheMode(QGraphicsItem.NoCache)
        self.hovered = False
        self.setAcceptHoverEvents(True)

    def setDriveStatus(self, status=True):
        if self.isPinned and not status:
            return
        self.isDrive = status
        self.update()

    def changeDriveStatus(self):
        if self.isPinned:
            return
        self.isDrive = not self.isDrive
        self.update()

    def changeSelectStatus(self):
        if self.isPinned:
            return
        self.setSelected(not self.isSelected())
        self.update()

    def setPinned(self, pinned=True):
        self.isPinned = pinned
        if pinned:
            self.isDrive = True
            self.setSelected(True)
        self.update()

    def togglePinned(self):
        self.setPinned(not self.isPinned)

    def boundingRect(self):
        return QRectF(self.x0, self.y0, self.W, self.H)

    def appearance(self):
        if self.isPinned:
            fill, border, ink = '#9C2869', '#F39BBE', '#FFFFFF'
        elif self.isDrive:
            fill, border, ink = '#6C48C5', '#48258F', '#FFFFFF'
        elif self.isSelected():
            fill, border, ink = '#DBEAFE', '#3577B9', '#174373'
        else:
            fill, border, ink = '#E8E8F1', '#9991AD', '#37304C'
        if self.hovered and not self.isDrive and not self.isSelected():
            fill, border = '#FAF7FF', '#7563B8'
        return dict(fill=fill, border=border, ink=ink,
                    dashed=self.isSelected() and not self.isDrive,
                    locked=self.isPinned)

    def hoverEnterEvent(self, event):
        self.hovered = True
        self.setToolTip(f'电极 {self.ID} · 右键驱动选择 / Shift+左键固定\n颜色是界面选择，不代表实测输出。')
        self.update()
        super().hoverEnterEvent(event)

    def hoverLeaveEvent(self, event):
        self.hovered = False
        self.update()
        super().hoverLeaveEvent(event)

    def paint(self, painter, option, widget):
        look = self.appearance()
        color = QColor(look['fill'])
        if self.isTransparent:
            color = QColor(color.red(), color.green(), color.blue(), 150)
        painter.save()
        painter.setClipPath(self.shape())
        pen = QPen(QColor(look['border']), 2.4 if self.isPinned or self.hovered else 1.1)
        pen.setCosmetic(True)
        if look['dashed']:
            pen.setStyle(Qt.DashLine)
        painter.setPen(pen)
        painter.setBrush(color)
        painter.drawPolygon(QPolygon(self.points), len(self.points))
        zoom = max(.01, QStyleOptionGraphicsItem.levelOfDetailFromTransform(painter.worldTransform()))
        if self.showDriveID and self.W*zoom >= 9:
            font = QFont('Segoe UI')
            screen_size = min(self.W, self.H)*zoom
            font.setPixelSize(max(1, round(min(12, max(6, screen_size*.36))/zoom)))
            font.setBold(True)
            painter.setFont(font)
            painter.setPen(QColor(look['ink']))
            painter.drawText(self.boundingRect(), Qt.AlignCenter, str(int(self.DriveID)))
        if self.isPinned:
            # Small lock in the corner: pinned is distinguishable beyond color alone.
            x, y = self.x0+self.W-10, self.y0+5
            painter.setPen(QPen(QColor('#FFE3EF'), 1))
            painter.setBrush(QColor('#FFE3EF'))
            painter.drawRoundedRect(QRectF(x, y+3, 6, 5), 1, 1)
            painter.setBrush(Qt.NoBrush)
            painter.drawArc(QRectF(x+1, y, 4, 6), 0, 180*16)
        painter.restore()

    def shape(self):
        path = QPainterPath()
        for i, (x, y) in enumerate(zip(self.x_cord, self.y_cord)):
            if i == 0:
                path.moveTo(int(x), int(y))
            else:
                path.lineTo(int(x), int(y))
        path.closeSubpath()
        return path
