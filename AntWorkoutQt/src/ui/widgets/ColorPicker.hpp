#pragma once

// ColorPicker.exe's window (ColorPicker.py): a standalone RGBA color picker for choosing QColor(r, g, b, a) values: a
// saturation/value square for the hue, hue and alpha bars, a swatch, HEX and R/G/B/A boxes, and the QColor(...) text to
// copy.

#include <QClipboard>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include <optional>

namespace awo::colorpicker {

// saturation (x) / value (y) for the current hue; click or drag to pick
class SVSquare : public QWidget {
    Q_OBJECT

public:
    SVSquare() {
        setMinimumSize(300, 200);
        setCursor(Qt::CrossCursor);
    }
    void setHsv(double h, double s, double v) {
        hue_ = h;
        sat_ = s;
        val_ = v;
        update();
    }

signals:
    void changed(double saturation, double value);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(rect());
        // white -> the pure hue, left to right
        QLinearGradient across(r.topLeft(), r.topRight());
        across.setColorAt(0, QColor(255, 255, 255));
        across.setColorAt(1, QColor::fromHsvF(static_cast<float>(hue_), 1, 1));
        p.fillRect(r, across);
        // see-through -> black, top to bottom
        QLinearGradient down(r.topLeft(), r.bottomLeft());
        down.setColorAt(0, QColor(0, 0, 0, 0));
        down.setColorAt(1, QColor(0, 0, 0, 255));
        p.fillRect(r, down);
        // the selection ring
        const QPointF pt(sat_ * (r.width() - 1), (1 - val_) * (r.height() - 1));
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.setBrush(QColor::fromHsvF(static_cast<float>(hue_), static_cast<float>(sat_), static_cast<float>(val_)));
        p.drawEllipse(pt, 8, 8);
    }
    void mousePressEvent(QMouseEvent* e) override { pick(e->position()); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) {
            pick(e->position());
        }
    }

private:
    void pick(const QPointF& pos) {
        sat_ = std::clamp(pos.x() / std::max(1, width() - 1), 0.0, 1.0);
        val_ = 1 - std::clamp(pos.y() / std::max(1, height() - 1), 0.0, 1.0);
        update();
        emit changed(sat_, val_);
    }

    double hue_ = 0, sat_ = 0, val_ = 1;
};

// a horizontal slider bar drawn as a gradient: the hue rainbow or an alpha ramp
class Bar : public QWidget {
    Q_OBJECT

public:
    explicit Bar(bool hue) : hue_(hue) {
        setFixedHeight(16);
        setMinimumWidth(200);
        setCursor(Qt::PointingHandCursor);
    }
    void setValue(double pos, std::optional<QColor> color = std::nullopt) {
        pos_ = pos;
        if (color) {
            color_ = *color;
        }
        update();
    }

signals:
    void changed(double value);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(8, 2, -8, -2);
        QLinearGradient g(r.topLeft(), r.topRight());
        if (hue_) {
            for (int i = 0; i < 7; ++i) {
                g.setColorAt(i / 6.0, QColor::fromHsvF(static_cast<float>(std::fmod(i / 6.0, 1.0)), 1, 1));
            }
        } else {
            // a checkerboard behind the alpha ramp so the see-through shows
            p.save();
            p.setClipRect(r);
            constexpr int cell = 6;
            int yi = 0;
            for (int y = static_cast<int>(r.top()); y < static_cast<int>(r.bottom()) + 1; y += cell, ++yi) {
                int xi = 0;
                for (int x = static_cast<int>(r.left()); x < static_cast<int>(r.right()) + 1; x += cell, ++xi) {
                    p.fillRect(x, y, cell, cell, (xi + yi) % 2 ? QColor(200, 200, 200) : QColor(255, 255, 255));
                }
            }
            p.restore();
            QColor c0(color_), c1(color_);
            c0.setAlpha(0);
            c1.setAlpha(255);
            g.setColorAt(0, c0);
            g.setColorAt(1, c1);
        }
        p.fillRect(r, g);
        // the knob
        const double x = r.left() + pos_ * r.width();
        p.setPen(QPen(QColor(90, 90, 90), 1));
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(QPointF(x, height() / 2.0), 7, 7);
    }
    void mousePressEvent(QMouseEvent* e) override { pick(e->position()); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) {
            pick(e->position());
        }
    }

private:
    void pick(const QPointF& pos) {
        const int w = width() - 16;
        pos_ = std::clamp((pos.x() - 8) / std::max(1, w), 0.0, 1.0);
        update();
        emit changed(pos_);
    }

    bool hue_;
    double pos_ = 0;
    QColor color_{0, 0, 0};
};

// a plain swatch of the color chosen
class Preview : public QWidget {
public:
    Preview() { setFixedSize(56, 56); }
    void setColor(const QColor& c) {
        color_ = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(120, 120, 120), 1));
        p.setBrush(color_);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }

private:
    QColor color_{0, 0, 0};
};

class ColorPicker : public QWidget {
    Q_OBJECT

public:
    explicit ColorPicker(const QColor& start) {
        setWindowTitle(QStringLiteral("QColor Picker"));
        auto* root = new QVBoxLayout(this);
        square_ = new SVSquare;
        connect(square_, &SVSquare::changed, this, [this](double s, double v) {
            s_ = s;
            v_ = v;
            refresh();
        });
        root->addWidget(square_, 1);

        auto* row = new QHBoxLayout;
        preview_ = new Preview;
        row->addWidget(preview_);
        auto* bars = new QVBoxLayout;
        hueBar_ = new Bar(true);
        connect(hueBar_, &Bar::changed, this, [this](double h) {
            h_ = std::min(h, 0.9999);
            refresh();
        });
        alphaBar_ = new Bar(false);
        connect(alphaBar_, &Bar::changed, this, [this](double a) {
            a_ = a;
            refresh();
        });
        bars->addWidget(hueBar_);
        bars->addWidget(alphaBar_);
        row->addLayout(bars, 1);
        root->addLayout(row);

        auto* fields = new QGridLayout;
        hexEdit_ = new QLineEdit;
        hexEdit_->setMaxLength(9);
        hexEdit_->setMinimumWidth(90);
        connect(hexEdit_, &QLineEdit::editingFinished, this, [this] {
            const QColor c(hexEdit_->text().trimmed());
            if (c.isValid()) {
                setColor(c);
            } else {
                refresh();
            }
        });
        fields->addWidget(new QLabel(QStringLiteral("HEX")), 0, 0, Qt::AlignCenter);
        fields->addWidget(hexEdit_, 1, 0);
        const QStringList names = {QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")};
        for (int i = 0; i < 4; ++i) {
            auto* spin = new QSpinBox;
            spin->setRange(0, 255);
            spin->setMinimumWidth(70);
            spin->setAlignment(Qt::AlignCenter);
            connect(spin, &QSpinBox::valueChanged, this, [this] {
                if (!updating_) {
                    setColor(QColor(spins_[0]->value(), spins_[1]->value(), spins_[2]->value(), spins_[3]->value()));
                }
            });
            fields->addWidget(new QLabel(names[i]), 0, i + 1, Qt::AlignCenter);
            fields->addWidget(spin, 1, i + 1);
            spins_.append(spin);
        }
        root->addLayout(fields);

        auto* out = new QHBoxLayout;
        text_ = new QLineEdit;
        text_->setReadOnly(true);
        QFont font = text_->font();
        font.setFamily(QStringLiteral("Orbitron"));
        font.setPointSize(11);
        text_->setFont(font);
        auto* copy = new QPushButton(QStringLiteral("Copy"));
        connect(copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(text_->text()); });
        out->addWidget(text_, 1);
        out->addWidget(copy);
        root->addLayout(out);

        resize(360, 460);
        setColor(start);
    }

private:
    QColor color() const {
        QColor c = QColor::fromHsvF(static_cast<float>(h_), static_cast<float>(s_), static_cast<float>(v_));
        // (Python's round: half to even)
        c.setAlpha(static_cast<int>(std::nearbyint(a_ * 255)));
        return c;
    }

    void setColor(const QColor& c) {
        float h, s, v, a;
        c.getHsvF(&h, &s, &v, &a);
        // (a grey keeps the hue it had)
        if (h >= 0) {
            h_ = h;
        }
        s_ = s;
        v_ = v;
        a_ = a;
        refresh(c);
    }

    void refresh(std::optional<QColor> exact = std::nullopt) {
        const QColor c = exact ? *exact : color();
        updating_ = true;
        square_->setHsv(h_, s_, v_);
        hueBar_->setValue(h_);
        alphaBar_->setValue(a_, c);
        preview_->setColor(c);
        const int rgba[4] = {c.red(), c.green(), c.blue(), c.alpha()};
        for (int i = 0; i < 4; ++i) {
            spins_[i]->setValue(rgba[i]);
        }
        hexEdit_->setText(c.alpha() < 255 ? c.name(QColor::HexArgb) : c.name());
        text_->setText(QStringLiteral("QColor(%1,%2,%3,%4)").arg(rgba[0]).arg(rgba[1]).arg(rgba[2]).arg(rgba[3]));
        updating_ = false;
    }

    bool updating_ = false;
    double h_ = 0, s_ = 0, v_ = 1, a_ = 1;
    SVSquare* square_;
    Preview* preview_;
    Bar* hueBar_;
    Bar* alphaBar_;
    QLineEdit* hexEdit_;
    QList<QSpinBox*> spins_;
    QLineEdit* text_;
};

}  // namespace awo::colorpicker
