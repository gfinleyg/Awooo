"""The HUD's zone legend (ClassZoneLegend.py) and the coming up list it sits under"""

from PyQt6.QtCore import QPoint
from PyQt6.QtGui import QColor, QImage, QRegion
from PyQt6.QtWidgets import QWidget

from AppState import AppState
import AppTheme
import FtpColors
from ClassLongVerticalLabelSeries import BLOCK_LABEL_WIDTH, LongVLabelSerial
from ClassZoneLegend import BAR_HEIGHT, LEGEND_HEIGHT, TEXT_GAP, ZoneLegend


def test_empty_list_shows_nothing(qapp):
    labels = LongVLabelSerial(True)
    labels.resize(500, 300)
    labels.setFullText("")
    labels.render_lines()
    assert not any(label.isVisibleTo(labels) for label in labels.qlabels)


def test_names_where_each_zone_ends(qapp):
    AppState.ftp = "200"
    AppState.ftpRanges[:] = FtpColors.power_ranges()
    legend = ZoneLegend()
    assert (legend.width(), legend.height()) == (BLOCK_LABEL_WIDTH, LEGEND_HEIGHT)
    assert legend.label_texts() == ["72W", "174W", "238W", "374W"]
    #Musical Cadence Mode: its cadence zones, in rpm
    AppState.workoutMode = AppState.cadenceMode = True
    AppState.ftpRanges[:] = FtpColors.cadence_ranges(40)
    assert legend.label_texts() == ["40rpm", "65rpm", "90rpm", "115rpm"]


def test_halos_text_that_doesnt_stand_out(qapp):
    AppTheme.THEME.reset()
    try:
        legend = ZoneLegend()
        #the default theme: black text on the gradient's mid-tones (on its own, it's its window: the middle)
        assert legend.haloed()
        #light text on a black window stands out
        AppTheme.THEME.colors = {"#28275e": "#000000", "#9fc5d1": "#1a1a1a", "#000000": "#ffffff"}
        assert not legend.haloed()
        #drawn with its halo: light around the dark text, past the text's own outline
        AppTheme.THEME.reset()
        image = QImage(legend.size(), QImage.Format.Format_ARGB32)
        image.fill(QColor("#5f6e98"))
        legend.render(image, QPoint(), QRegion(), QWidget.RenderFlag.DrawChildren)
        light = sum(image.pixelColor(x, y).lightness() > 200
                    for y in range(int(BAR_HEIGHT + TEXT_GAP), image.height()) for x in range(image.width()))
        assert light > 50
    finally:
        AppTheme.THEME.reset()
