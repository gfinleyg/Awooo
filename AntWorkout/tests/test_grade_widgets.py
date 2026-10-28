"""The block grade list (ClassGradeWidgets.GradeList): scrolling, and flowing into columns (the synopsis's Block Grades)"""

from PyQt6.QtCore import QSize

from ClassGradeWidgets import GradeList


def rows(count):
    return [{"kind": "block", "label": f"{i}:00 @ 150W", "pct": 90, "grade": "A", "note": ""} for i in range(count)]


def test_scrolls_with_more_rows(qapp):
    grades = GradeList()
    grades.resize(600, 400)
    grades.rows = rows(20)
    #(400 px less the average and its gaps, 46 px a row: 7 rows)
    shown = grades.shown_rows()
    assert len(shown) == 7 and shown[-1]["kind"] == "more"
    grades.scroll_to(3)
    shown = grades.shown_rows()
    assert shown[0]["kind"] == "more" and shown[1]["label"] == grades.rows[3]["label"]
    #no further than the last row in the bottom slot
    grades.scroll_to(100)
    shown = grades.shown_rows()
    assert grades.scrollOffset == 20 - 7 + 1 and shown[-1]["label"] == grades.rows[-1]["label"]


def test_flows_into_columns(qapp):
    grades = GradeList()
    grades.columnFlow = True
    grades.rows = rows(20)
    #7 rows a column: 3 columns, each its full width, all the rows in view
    size = grades.fitted_size(QSize(2000, 400))
    assert size == QSize(3*GradeList.COLUMN_WIDTH + 2*GradeList.COLUMN_GAP, 391)
    grades.resize(size)
    assert grades.columns() == 3 and grades.shown_rows() == grades.rows
    #a few rows: one column, only as high as they need
    grades.rows = rows(2)
    assert grades.fitted_size(QSize(2000, 400)) == QSize(GradeList.COLUMN_WIDTH, 2*46 - 4 + 72 + 1)
    #no room for 3 columns at the narrowest: 2 narrower ones, the whole list scrolling through them
    grades.rows = rows(20)
    size = grades.fitted_size(QSize(700, 400))
    assert size == QSize(700, 391)
    grades.resize(size)
    assert grades.columns() == 2
    shown = grades.shown_rows()
    assert len(shown) == 14 and shown[-1]["kind"] == "more"
    grades.scroll_to(100)
    shown = grades.shown_rows()
    assert grades.scrollOffset == 20 - 14 + 1
    assert shown[0]["kind"] == "more" and shown[-1]["label"] == grades.rows[-1]["label"]
    #off (the workout screen's list): one column, whatever its width
    grades.columnFlow = False
    assert grades.columns() == 1
