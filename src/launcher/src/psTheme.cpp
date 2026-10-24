#include "psTheme.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QVariantAnimation>
#include <QListWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <vector>

namespace {

QSettings ViewSettings() {
	return QSettings(QSettings::UserScope, QStringLiteral("Kyty"), QStringLiteral("LauncherView"));
}

struct Preset {
	const char* id;
	const char* name;
	QRgb        top;
	QRgb        bottom;
	QRgb        accent;
};

constexpr Preset Presets[] = {
    {"default", QT_TRANSLATE_NOOP("PsTheme", "Default (PS5 blue)"), qRgb(12, 30, 68), qRgb(4, 8, 20),
     qRgb(61, 139, 253)},
    {"graphite", QT_TRANSLATE_NOOP("PsTheme", "Graphite"), qRgb(74, 74, 82), qRgb(18, 19, 24),
     qRgb(200, 205, 215)},
    {"midnight", QT_TRANSLATE_NOOP("PsTheme", "Midnight"), qRgb(28, 18, 60), qRgb(6, 4, 16),
     qRgb(150, 110, 255)},
    {"sunset", QT_TRANSLATE_NOOP("PsTheme", "Sunset"), qRgb(110, 40, 30), qRgb(24, 8, 14),
     qRgb(255, 140, 70)},
    {"forest", QT_TRANSLATE_NOOP("PsTheme", "Forest"), qRgb(20, 70, 50), qRgb(4, 16, 12),
     qRgb(70, 210, 140)},
    {"custom", QT_TRANSLATE_NOOP("PsTheme", "Custom picture"), qRgb(30, 30, 36), qRgb(8, 8, 12),
     qRgb(61, 139, 253)},
};

} // namespace

QStringList PsTheme::PresetIds() {
	QStringList ids;
	for (const auto& preset: Presets) {
		ids.append(QString::fromLatin1(preset.id));
	}
	return ids;
}

PsTheme PsTheme::Preset(const QString& id) {
	for (const auto& preset: Presets) {
		if (id == QLatin1String(preset.id)) {
			return {QString::fromLatin1(preset.id), QCoreApplication::translate("PsTheme", preset.name),
			        QColor(preset.top), QColor(preset.bottom), QColor(preset.accent), {}};
		}
	}
	return Preset(QStringLiteral("default"));
}

PsTheme PsTheme::Current() {
	const auto settings = ViewSettings();
	auto theme = Preset(settings.value(QStringLiteral("theme/id"), QStringLiteral("default")).toString());
	if (theme.id == QLatin1String("custom")) {
		theme.image = settings.value(QStringLiteral("theme/image")).toString();
	}
	if (const QColor accent(settings.value(QStringLiteral("theme/accent")).toString()); accent.isValid()) {
		theme.accent = accent;
	}
	return theme;
}

void PsTheme::Save(const QString& id, const QString& image, const QColor& accent) {
	auto settings = ViewSettings();
	settings.setValue(QStringLiteral("theme/id"), id);
	settings.setValue(QStringLiteral("theme/image"), image);
	if (accent.isValid()) {
		settings.setValue(QStringLiteral("theme/accent"), accent.name());
	} else {
		settings.remove(QStringLiteral("theme/accent"));
	}
	emit PsThemeNotifier::Instance().Changed();
}

QPixmap PsTheme::Picture() const {
	static QHash<QString, QPixmap> cache;
	if (image.isEmpty()) {
		return {};
	}
	if (auto found = cache.find(image); found != cache.end()) {
		return found.value();
	}
	QPixmap picture;
	picture.load(image);
	cache.insert(image, picture);
	return picture;
}

PsThemeNotifier& PsThemeNotifier::Instance() {
	static PsThemeNotifier notifier;
	return notifier;
}

// The switch.

PsSwitch::PsSwitch(QWidget* parent): QAbstractButton(parent) {
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::StrongFocus);
	m_animation = new QVariantAnimation(this);
	m_animation->setDuration(160);
	m_animation->setEasingCurve(QEasingCurve::OutCubic);
	connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
		m_position = value.toFloat();
		update();
	});
	connect(&PsThemeNotifier::Instance(), &PsThemeNotifier::Changed, this,
	        qOverload<>(&QWidget::update));
}

QSize PsSwitch::sizeHint() const {
	return {50, 26};
}

void PsSwitch::checkStateSet() {
	// Set from outside (the check box it mirrors): no animation.
	m_animation->stop();
	m_position = isChecked() ? 1.0f : 0.0f;
	update();
}

void PsSwitch::nextCheckState() {
	QAbstractButton::nextCheckState();
	Animate();
}

void PsSwitch::keyPressEvent(QKeyEvent* event) {
	// Up and down (the keyboard's or a gamepad's) go from row to row; Enter toggles, as Space.
	switch (event->key()) {
		case Qt::Key_Up: focusNextPrevChild(false); return;
		case Qt::Key_Down: focusNextPrevChild(true); return;
		case Qt::Key_Return:
		case Qt::Key_Enter: click(); return;
		default: QAbstractButton::keyPressEvent(event);
	}
}

void PsSwitch::Animate() {
	m_animation->stop();
	m_animation->setStartValue(m_position);
	m_animation->setEndValue(isChecked() ? 1.0f : 0.0f);
	m_animation->start();
}

void PsSwitch::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
	const qreal  radius = track.height() / 2;
	const auto   accent = PsTheme::Current().accent;
	QColor       off(255, 255, 255, 46);
	QColor       on = accent;
	on.setAlpha(230);
	// The track blends from off to on with the knob.
	const auto blend = [](const QColor& a, const QColor& b, float t) {
		return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
		                        a.greenF() + (b.greenF() - a.greenF()) * t,
		                        a.blueF() + (b.blueF() - a.blueF()) * t,
		                        a.alphaF() + (b.alphaF() - a.alphaF()) * t);
	};
	painter.setOpacity(isEnabled() ? 1.0 : 0.4);
	painter.setPen(QPen(QColor(255, 255, 255, 70), 1.0));
	painter.setBrush(blend(off, on, m_position));
	painter.drawRoundedRect(track, radius, radius);
	const qreal knob   = track.height() - 6;
	const qreal left   = track.left() + 3;
	const qreal right  = track.right() - 3 - knob;
	const QRectF circle(left + (right - left) * m_position, track.top() + 3, knob, knob);
	painter.setPen(Qt::NoPen);
	painter.setBrush(Qt::white);
	painter.drawEllipse(circle);
	if (hasFocus()) {
		painter.setPen(QPen(QColor(255, 255, 255, 180), 1.5));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(track.adjusted(-0.5, -0.5, 0.5, 0.5), radius, radius);
	}
}

// The rows: the settings as the console lists them, one per line, each with a line under it and
// an outline while it has the focus.

namespace {

// The smooth scroll area a row sits in, if any.
PsScrollArea* ScrollAreaOf(QWidget* widget) {
	for (auto* parent = widget->parentWidget(); parent != nullptr; parent = parent->parentWidget()) {
		if (auto* area = qobject_cast<PsScrollArea*>(parent); area != nullptr) {
			return area;
		}
	}
	return nullptr;
}

QWidget* NewRow(QWidget* parent, const QString& tooltip) {
	auto* row = new QWidget(parent);
	row->setObjectName(QStringLiteral("settingsRow"));
	row->setAttribute(Qt::WA_StyledBackground);
	row->setToolTip(tooltip);
	return row;
}

void SetRowFocused(QWidget* row, bool focused) {
	row->setProperty("focused", focused);
	row->style()->unpolish(row);
	row->style()->polish(row);
	row->update();
	if (auto* area = ScrollAreaOf(row); area != nullptr) {
		if (focused) {
			area->FocusRow(row);
			area->EnsureVisibleSmooth(row);
		} else {
			area->UnfocusRow(row);
		}
	}
}

// The outline itself: a rounded frame with a faint fill, drawn over the row.
class FocusRing final: public QWidget {
public:
	explicit FocusRing(QWidget* parent): QWidget(parent) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
		hide();
	}

	void SetAlpha(double alpha) {
		m_alpha = alpha;
		update();
	}

	void SetRadius(qreal radius) {
		m_radius = radius;
		update();
	}

protected:
	void paintEvent(QPaintEvent* /*event*/) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(QColor(255, 255, 255, static_cast<int>(160 * m_alpha)), 2));
		painter.setBrush(QColor(255, 255, 255, static_cast<int>(18 * m_alpha)));
		painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), m_radius, m_radius);
	}

private:
	double m_alpha  = 0.0;
	qreal  m_radius = 5.0;
};

// Keeps a row in step with the widget that owns its value (`owner`: a check box, a combo box, a
// field): visibility, enabled state, tooltip; outlines the row while `focus` has the focus; makes
// a click on the row `activate` it.
class RowSync final: public QObject {
public:
	RowSync(QWidget* owner, QWidget* row, QWidget* focus, std::function<void()> activate)
	    : QObject(row), m_owner(owner), m_row(row), m_focus(focus), m_activate(std::move(activate)) {
		owner->installEventFilter(this);
		row->installEventFilter(this);
		if (focus != nullptr && focus != owner) {
			focus->installEventFilter(this);
		}
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (watched == m_owner) {
			switch (event->type()) {
				case QEvent::HideToParent: m_row->hide(); break;
				case QEvent::ShowToParent: m_row->show(); break;
				case QEvent::EnabledChange:
					for (auto* child: m_row->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
						if (child != m_owner) {
							child->setEnabled(m_owner->isEnabled());
						}
					}
					break;
				case QEvent::ToolTipChange: m_row->setToolTip(m_owner->toolTip()); break;
				default: break;
			}
		}
		if (watched == m_focus && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
			SetRowFocused(m_row, event->type() == QEvent::FocusIn);
		}
		if (watched == m_row && m_activate && m_owner->isEnabled() &&
		    (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) &&
		    static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
			// The press is taken so that the row gets the release.
			if (event->type() == QEvent::MouseButtonRelease) {
				if (m_focus != nullptr) {
					m_focus->setFocus(Qt::MouseFocusReason);
				}
				m_activate();
			}
			return true;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	QWidget*              m_owner;
	QWidget*              m_row;
	QWidget*              m_focus;
	std::function<void()> m_activate;
};

QString RowText(QString text) {
	text.remove(QLatin1Char('&'));
	while (text.endsWith(QLatin1Char(':')) || text.endsWith(QChar(0xFF1A))) {
		text.chop(1);
	}
	return text.trimmed();
}

// The owner widget, kept in the row without size, so code that shows, hides or enables it still
// works.
void KeepOwner(QWidget* owner, QWidget* row, QBoxLayout* layout) {
	const bool hidden = owner->isHidden();
	owner->setParent(row);
	owner->setFixedSize(0, 0);
	owner->setFocusPolicy(Qt::NoFocus); // the row (or its switch) takes the focus
	layout->addWidget(owner);
	owner->setVisible(!hidden);
	row->setVisible(!hidden);
}

QWidget* CreateSwitchRow(QCheckBox* check_box, QWidget* parent) {
	auto* row    = NewRow(parent, check_box->toolTip());
	auto* layout = new QHBoxLayout(row);
	layout->setContentsMargins(16, 13, 16, 13);
	auto* label = new QLabel(RowText(check_box->text()), row);
	label->setWordWrap(true);
	auto* toggle = new PsSwitch(row);
	toggle->setChecked(check_box->isChecked());
	toggle->setEnabled(check_box->isEnabled());
	label->setEnabled(check_box->isEnabled());
	layout->addWidget(label, 1);
	layout->addWidget(toggle);
	KeepOwner(check_box, row, layout);
	QObject::connect(toggle, &PsSwitch::toggled, check_box, &QCheckBox::setChecked);
	QObject::connect(check_box, &QCheckBox::toggled, toggle, [toggle](bool on) {
		if (toggle->isChecked() != on) {
			toggle->setChecked(on);
		}
	});
	new RowSync(check_box, row, toggle, [toggle] { toggle->click(); });
	return row;
}

class FieldKeys final: public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() != QEvent::KeyPress) {
			return QObject::eventFilter(watched, event);
		}
		auto*     widget = static_cast<QWidget*>(watched);
		const int key    = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Up || key == Qt::Key_Down) {
			auto* next = key == Qt::Key_Down ? widget->nextInFocusChain() : widget->previousInFocusChain();
			while (next != nullptr && next != widget &&
			       ((next->focusPolicy() & Qt::TabFocus) == 0 || !next->isVisible() || !next->isEnabled())) {
				next = key == Qt::Key_Down ? next->nextInFocusChain() : next->previousInFocusChain();
			}
			if (next != nullptr && next != widget) {
				next->setFocus(key == Qt::Key_Down ? Qt::TabFocusReason : Qt::BacktabFocusReason);
			}
			return true;
		}
		if (auto* spin = qobject_cast<QAbstractSpinBox*>(watched);
		    spin != nullptr && (key == Qt::Key_Left || key == Qt::Key_Right)) {
			spin->stepBy(key == Qt::Key_Right ? 1 : -1);
			return true;
		}
		return QObject::eventFilter(watched, event);
	}
};

QWidget* CreateFieldRow(const QString& text, QWidget* field, QWidget* parent) {
	auto* row    = NewRow(parent, field->toolTip());
	auto* layout = new QHBoxLayout(row);
	layout->setContentsMargins(16, 9, 16, 9);
	auto* label = new QLabel(text, row);
	label->setWordWrap(true);
	layout->addWidget(label, 1);
	const bool hidden = field->isHidden();
	field->setParent(row);
	field->setMinimumWidth(280);
	field->setMaximumWidth(420);
	layout->addWidget(field, 1);
	field->setVisible(!hidden);
	row->setVisible(!hidden);
	label->setEnabled(field->isEnabled());
	auto* keys = new FieldKeys(row);
	QWidget* focus = field;
	if (qobject_cast<QLineEdit*>(field) != nullptr || qobject_cast<QAbstractSpinBox*>(field) != nullptr) {
		field->installEventFilter(keys);
	} else {
		// A layout of controls: its first focusable one stands for the row.
		for (auto* child: field->findChildren<QWidget*>()) {
			if ((child->focusPolicy() & Qt::TabFocus) != 0) {
				focus = focus == field ? child : focus;
				child->installEventFilter(keys);
			}
		}
	}
	new RowSync(field, row, focus, {});
	return row;
}

} // namespace

// The choice list.

PsChoicePopup::PsChoicePopup(QComboBox* combo, QWidget* anchor)
    : QWidget(anchor, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
      m_combo(combo) {
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_DeleteOnClose);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	for (int i = 0; i < combo->count(); i++) {
		m_items.append(combo->itemText(i));
	}
	m_focus = std::max(0, combo->currentIndex());
	QFont item_font = font();
	item_font.setPixelSize(17);
	setFont(item_font);
	const QFontMetrics metrics(item_font);
	int width = 220;
	for (const auto& item: m_items) {
		width = std::max(width, metrics.horizontalAdvance(item) + 96);
	}
	const int visible = std::min(static_cast<int>(m_items.size()), MaxVisible);
	resize(std::min(width, 520), visible * ItemHeight + 2 * Padding);
	m_first = std::clamp(m_focus - visible / 2, 0, std::max(0, static_cast<int>(m_items.size()) - visible));

	// Beside the row's right end, the current choice level with the row, kept on the screen.
	const auto right  = anchor->mapToGlobal(QPoint(anchor->width() - 8, anchor->height() / 2));
	QPoint     origin = QPoint(right.x() - this->width(),
	                           right.y() - Padding - (m_focus - m_first) * ItemHeight - ItemHeight / 2);
	if (const auto* screen = anchor->screen(); screen != nullptr) {
		const auto area = screen->availableGeometry();
		origin.setX(std::clamp(origin.x(), area.left(), std::max(area.left(), area.right() - this->width())));
		origin.setY(std::clamp(origin.y(), area.top(), std::max(area.top(), area.bottom() - height())));
	}
	move(origin);

	m_animation = new QVariantAnimation(this);
	m_animation->setDuration(170);
	m_animation->setEasingCurve(QEasingCurve::OutCubic);
	m_animation->setStartValue(0.0);
	m_animation->setEndValue(1.0);
	const auto final_position = pos();
	connect(m_animation, &QVariantAnimation::valueChanged, this, [this, final_position](const QVariant& value) {
		const double v = value.toDouble();
		setWindowOpacity(v);
		move(final_position + QPoint(0, static_cast<int>((1.0 - v) * -10.0)));
	});
	setWindowOpacity(0.0);
}

void PsChoicePopup::Open() {
	show();
	setFocus();
	m_animation->start();
}

void PsChoicePopup::Choose(int index) {
	if (index >= 0 && index < m_items.size() && m_combo != nullptr) {
		m_combo->setCurrentIndex(index);
	}
	close();
}

int PsChoicePopup::ItemAt(const QPoint& position) const {
	const int row = (position.y() - Padding) / ItemHeight;
	const int index = m_first + row;
	return position.y() >= Padding && index >= 0 && index < m_items.size() ? index : -1;
}

void PsChoicePopup::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
	painter.setBrush(QColor(38, 39, 46, 248));
	painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
	const int visible = std::min(static_cast<int>(m_items.size()), MaxVisible);
	const int current = m_combo != nullptr ? m_combo->currentIndex() : -1;
	for (int row = 0; row < visible; row++) {
		const int   index = m_first + row;
		const QRect item(Padding, Padding + row * ItemHeight, width() - 2 * Padding, ItemHeight);
		if (index == m_focus) {
			painter.setPen(QPen(QColor(255, 255, 255, 170), 2));
			painter.setBrush(QColor(255, 255, 255, 22));
			painter.drawRoundedRect(QRectF(item).adjusted(2, 2, -2, -2), 4, 4);
		}
		if (index == current) {
			// The console's check mark beside the current choice.
			painter.setPen(QPen(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			const QPointF c(item.left() + 30, item.center().y());
			painter.drawPolyline(QPolygonF({c + QPointF(-7, 0), c + QPointF(-2, 5), c + QPointF(8, -6)}));
		}
		painter.setPen(QColor(235, 237, 242));
		painter.drawText(item.adjusted(56, 0, -16, 0), Qt::AlignLeft | Qt::AlignVCenter, m_items[index]);
	}
	if (m_items.size() > visible) {
		// Where the visible part sits in the whole list.
		const qreal track = height() - 2 * Padding;
		const qreal knob  = track * visible / m_items.size();
		const qreal top   = Padding + track * m_first / m_items.size();
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(255, 255, 255, 70));
		painter.drawRoundedRect(QRectF(width() - 7, top, 3, knob), 1.5, 1.5);
	}
}

void PsChoicePopup::MoveFocus(int step) {
	const int count   = static_cast<int>(m_items.size());
	const int visible = std::min(count, MaxVisible);
	m_focus           = std::clamp(m_focus + step, 0, count - 1);
	if (m_focus < m_first) {
		m_first = m_focus;
	} else if (m_focus >= m_first + visible) {
		m_first = m_focus - visible + 1;
	}
	update();
}

void PsChoicePopup::keyPressEvent(QKeyEvent* event) {
	switch (event->key()) {
		case Qt::Key_Up: MoveFocus(-1); return;
		case Qt::Key_Down: MoveFocus(1); return;
		case Qt::Key_PageUp: MoveFocus(-MaxVisible); return;
		case Qt::Key_PageDown: MoveFocus(MaxVisible); return;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Choose(m_focus); return;
		case Qt::Key_Escape:
		case Qt::Key_Left:
		case Qt::Key_Backspace: close(); return;
		default: QWidget::keyPressEvent(event);
	}
}

void PsChoicePopup::mouseMoveEvent(QMouseEvent* event) {
	if (const int index = ItemAt(event->position().toPoint()); index >= 0 && index != m_focus) {
		m_focus = index;
		update();
	}
}

void PsChoicePopup::mouseReleaseEvent(QMouseEvent* event) {
	if (!rect().contains(event->position().toPoint())) {
		close();
		return;
	}
	Choose(ItemAt(event->position().toPoint()));
}

void PsChoicePopup::wheelEvent(QWheelEvent* event) {
	const int steps = -event->angleDelta().y() / 120;
	const int count = static_cast<int>(m_items.size());
	const int visible = std::min(count, MaxVisible);
	m_first = std::clamp(m_first + steps, 0, std::max(0, count - visible));
	m_focus = std::clamp(m_focus, m_first, m_first + visible - 1);
	update();
}

// The choice row.

PsChoiceRow::PsChoiceRow(const QString& text, QComboBox* combo, QWidget* parent)
    : QWidget(parent), m_combo(combo) {
	setObjectName(QStringLiteral("settingsRow"));
	setAttribute(Qt::WA_StyledBackground);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(Qt::PointingHandCursor);
	setToolTip(combo->toolTip());
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 15, 16, 15);
	m_label = new QLabel(text, this);
	m_label->setWordWrap(true);
	m_value = new QLabel(this);
	m_value->setObjectName(QStringLiteral("settingsValue"));
	m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	layout->addWidget(m_label, 1);
	layout->addWidget(m_value);
	KeepOwner(combo, this, layout);
	const auto show_value = [this] { m_value->setText(m_combo->currentText()); };
	show_value();
	connect(combo, &QComboBox::currentIndexChanged, this, show_value);
	connect(combo, &QComboBox::currentTextChanged, this, show_value);
	new RowSync(combo, this, this, [this] { OpenChoices(); });
}

void PsChoiceRow::OpenChoices() {
	if (!m_combo->isEnabled() || m_combo->count() == 0) {
		return;
	}
	auto* popup = new PsChoicePopup(m_combo, this);
	connect(popup, &QObject::destroyed, this, [this] { setFocus(Qt::OtherFocusReason); });
	popup->Open();
}

void PsChoiceRow::keyPressEvent(QKeyEvent* event) {
	switch (event->key()) {
		case Qt::Key_Up: focusNextPrevChild(false); return;
		case Qt::Key_Down: focusNextPrevChild(true); return;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space:
		case Qt::Key_Right: OpenChoices(); return;
		default: QWidget::keyPressEvent(event);
	}
}

// The smooth scroll area.

PsScrollArea::PsScrollArea(QWidget* parent): QScrollArea(parent) {
	m_animation = new QVariantAnimation(this);
	m_animation->setDuration(220);
	m_animation->setEasingCurve(QEasingCurve::OutCubic);
	connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
		verticalScrollBar()->setValue(value.toInt());
	});
}

void PsScrollArea::ScrollTo(int value) {
	auto* bar = verticalScrollBar();
	value     = std::clamp(value, bar->minimum(), bar->maximum());
	m_target  = value;
	m_animation->stop();
	m_animation->setStartValue(bar->value());
	m_animation->setEndValue(value);
	m_animation->start();
}

void PsScrollArea::EnsureVisibleSmooth(QWidget* child, int margin) {
	if (widget() == nullptr || !widget()->isAncestorOf(child)) {
		return;
	}
	const auto top    = child->mapTo(widget(), QPoint(0, 0)).y();
	const auto bottom = top + child->height();
	const int  view   = viewport()->height();
	const int  value  = m_animation->state() == QAbstractAnimation::Running ? m_target
	                                                                          : verticalScrollBar()->value();
	if (top - margin < value) {
		ScrollTo(top - margin);
	} else if (bottom + margin > value + view) {
		ScrollTo(bottom + margin - view);
	}
}

QRect PsScrollArea::RingTarget() const {
	if (m_ring_row == nullptr || widget() == nullptr) {
		return {};
	}
	return QRect(m_ring_row->mapTo(widget(), QPoint(0, 0)), m_ring_row->size());
}

void PsScrollArea::FocusRow(QWidget* row) {
	if (widget() == nullptr || !widget()->isAncestorOf(row)) {
		return;
	}
	if (m_ring == nullptr || m_ring->parentWidget() != widget()) {
		delete m_ring;
		m_ring      = new FocusRing(widget());
		static_cast<FocusRing*>(m_ring)->SetRadius(m_ring_radius);
		m_ring_move = new QVariantAnimation(m_ring);
		m_ring_move->setDuration(170);
		m_ring_move->setEasingCurve(QEasingCurve::OutCubic);
		connect(m_ring_move, &QVariantAnimation::valueChanged, m_ring,
		        [this](const QVariant& value) { m_ring->setGeometry(value.toRect()); });
		m_ring_fade = new QVariantAnimation(m_ring);
		m_ring_fade->setDuration(150);
		connect(m_ring_fade, &QVariantAnimation::valueChanged, m_ring, [this](const QVariant& value) {
			static_cast<FocusRing*>(m_ring)->SetAlpha(value.toDouble());
		});
		connect(m_ring_fade, &QVariantAnimation::finished, m_ring, [this] {
			if (m_ring_fade->endValue().toDouble() == 0.0) {
				m_ring->hide();
			}
		});
	}
	if (m_ring_row != nullptr && m_ring_row != row) {
		m_ring_row->removeEventFilter(this);
	}
	m_ring_row = row;
	row->installEventFilter(this);
	const auto target = RingTarget();
	m_ring->raise();
	const bool shown = m_ring->isVisible() && m_ring_fade->endValue().toDouble() > 0.0;
	if (shown) {
		// From where it is (or is going) to the row.
		m_ring_move->stop();
		m_ring_move->setStartValue(m_ring->geometry());
		m_ring_move->setEndValue(target);
		m_ring_move->start();
	} else {
		m_ring_move->stop();
		m_ring->setGeometry(target);
		m_ring->show();
	}
	if (m_ring_fade->endValue().toDouble() != 1.0 || m_ring_fade->state() != QAbstractAnimation::Running) {
		const double from = m_ring_fade->state() == QAbstractAnimation::Running
		                        ? m_ring_fade->currentValue().toDouble()
		                        : (shown ? 1.0 : 0.0);
		m_ring_fade->stop();
		m_ring_fade->setStartValue(from);
		m_ring_fade->setEndValue(1.0);
		m_ring_fade->start();
	}
}

void PsScrollArea::UnfocusRow(QWidget* row) {
	if (m_ring == nullptr || row != m_ring_row) {
		return;
	}
	// The focus may be on its way to another row (which then takes the ring along): only once it
	// has settled elsewhere does the ring fade out.
	QTimer::singleShot(0, this, [this, row = QPointer<QWidget>(row)] {
		if (m_ring == nullptr || row != m_ring_row || row == nullptr ||
		    row->property("focused").toBool() || row->hasFocus()) {
			return;
		}
		m_ring_fade->stop();
		m_ring_fade->setStartValue(m_ring_fade->currentValue().isValid() ? m_ring_fade->currentValue() : 1.0);
		m_ring_fade->setEndValue(0.0);
		m_ring_fade->start();
	});
}

bool PsScrollArea::eventFilter(QObject* watched, QEvent* event) {
	if (watched == m_ring_row && m_ring != nullptr) {
		switch (event->type()) {
			case QEvent::Move:
			case QEvent::Resize:
				if (m_ring_move->state() != QAbstractAnimation::Running) {
					m_ring->setGeometry(RingTarget());
				} else {
					m_ring_move->setEndValue(RingTarget());
				}
				break;
			case QEvent::Hide: m_ring->hide(); break;
			default: break;
		}
	}
	return QScrollArea::eventFilter(watched, event);
}

void PsScrollArea::wheelEvent(QWheelEvent* event) {
	const int base = m_animation->state() == QAbstractAnimation::Running ? m_target
	                                                                     : verticalScrollBar()->value();
	ScrollTo(base - event->angleDelta().y());
	event->accept();
}

// The settings' groups as rows.

void BuildSettingsRows(QGroupBox* group) {
	auto* old = group->layout();
	if (old == nullptr) {
		return;
	}
	// The entries in reading order: a label and its field, or a field (or layout) alone.
	struct Entry {
		QWidget* label  = nullptr;
		QWidget* widget = nullptr;
		QLayout* layout = nullptr;
	};
	std::vector<Entry> entries;
	const auto         entry_of = [](QLayoutItem* item) {
        Entry entry;
        if (item != nullptr) {
            entry.widget = item->widget();
            entry.layout = item->widget() == nullptr ? item->layout() : nullptr;
        }
        return entry;
	};
	if (auto* form = qobject_cast<QFormLayout*>(old); form != nullptr) {
		for (int row = 0; row < form->rowCount(); row++) {
			if (auto* spanning = form->itemAt(row, QFormLayout::SpanningRole); spanning != nullptr) {
				entries.push_back(entry_of(spanning));
				continue;
			}
			auto entry = entry_of(form->itemAt(row, QFormLayout::FieldRole));
			if (auto* label = form->itemAt(row, QFormLayout::LabelRole); label != nullptr) {
				entry.label = label->widget();
			}
			if (entry.widget != nullptr || entry.layout != nullptr) {
				entries.push_back(entry);
			}
		}
	} else if (auto* grid = qobject_cast<QGridLayout*>(old); grid != nullptr) {
		for (int row = 0; row < grid->rowCount(); row++) {
			QWidget* pending_label = nullptr;
			for (int column = 0; column < grid->columnCount(); column++) {
				auto* item = grid->itemAtPosition(row, column);
				if (item == nullptr) {
					continue;
				}
				int r = 0, c = 0, rs = 0, cs = 0;
				grid->getItemPosition(grid->indexOf(item), &r, &c, &rs, &cs);
				if (r != row || c != column) {
					continue; // a spanning item, met again
				}
				if (auto* label = qobject_cast<QLabel*>(item->widget());
				    label != nullptr && pending_label == nullptr &&
				    column + cs < grid->columnCount() && grid->itemAtPosition(row, column + cs) != nullptr) {
					pending_label = label;
					continue;
				}
				auto entry  = entry_of(item);
				entry.label = pending_label;
				pending_label = nullptr;
				entries.push_back(entry);
			}
		}
	} else {
		for (int i = 0; i < old->count(); i++) {
			auto entry = entry_of(old->itemAt(i));
			if (entry.widget != nullptr || entry.layout != nullptr) {
				entries.push_back(entry);
			}
		}
	}
	// The old layout lets go of its items; the widgets stay the group's.
	while (auto* item = old->takeAt(0)) {
		if (item->layout() != nullptr && item->widget() == nullptr) {
			item->layout()->setParent(nullptr);
			continue; // the layout is its own item, kept for its row
		}
		delete item;
	}
	delete old;

	auto* layout = new QVBoxLayout(group);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	for (const auto& entry: entries) {
		const auto text = entry.label != nullptr
		                      ? RowText(qobject_cast<QLabel*>(entry.label) != nullptr
		                                    ? qobject_cast<QLabel*>(entry.label)->text()
		                                    : QString())
		                      : QString();
		if (entry.label != nullptr) {
			entry.label->hide();
		}
		QWidget* field = entry.widget;
		if (field == nullptr && entry.layout != nullptr) {
			field = new QWidget(group);
			field->setLayout(entry.layout);
			entry.layout->setContentsMargins(0, 0, 0, 0);
		}
		if (field == nullptr) {
			continue;
		}
		if (auto* check_box = qobject_cast<QCheckBox*>(field); check_box != nullptr) {
			layout->addWidget(CreateSwitchRow(check_box, group));
		} else if (auto* combo = qobject_cast<QComboBox*>(field); combo != nullptr) {
			layout->addWidget(new PsChoiceRow(text, combo, group));
		} else if (qobject_cast<QLabel*>(field) != nullptr && text.isEmpty()) {
			field->setParent(group);
			field->setObjectName(QStringLiteral("settingsNote"));
			layout->addWidget(field); // a note, as the console's grey text above rows
		} else if (text.isEmpty()) {
			layout->addWidget(field); // a list, a button row: as it is
		} else {
			layout->addWidget(CreateFieldRow(text, field, group));
		}
	}
}

namespace {

// A picture of `widget` as it draws itself, on transparency: QWidget::grab() fills in the
// window's background colour first, which over the settings' gradient reads as a dimming.
QPixmap GrabClear(QWidget* widget) {
	const qreal ratio = widget->devicePixelRatioF();
	QPixmap     shot(widget->size() * ratio);
	shot.setDevicePixelRatio(ratio);
	shot.fill(Qt::transparent);
	widget->render(&shot, QPoint(), QRegion(), QWidget::DrawChildren);
	return shot;
}

// A widget coming in, drawn as a picture of itself over its parent while the widget itself is
// kept invisible: an opacity effect on the live widget renders all of it again for every frame,
// which on a large high-DPI page leaves only a frame or two of the animation.
class AnimateInShot final: public QWidget {
public:
	AnimateInShot(QWidget* host, QWidget* target, QPixmap shot, QPoint offset, int ms, int delay)
	    : QWidget(host), m_target(target), m_shot(std::move(shot)), m_offset(offset), m_ms(ms),
	      m_delay(delay) {
		setObjectName(QStringLiteral("psAnimateInShot"));
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setGeometry(host->rect());
		raise();
		show();
		auto* animation = new QVariantAnimation(this);
		animation->setDuration(delay + ms);
		animation->setStartValue(0.0);
		animation->setEndValue(static_cast<double>(delay + ms));
		connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
			const double t = std::clamp((value.toDouble() - m_delay) / m_ms, 0.0, 1.0);
			m_progress     = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t);
			update();
		});
		connect(animation, &QVariantAnimation::finished, this, &AnimateInShot::Finish);
		animation->start();
	}

	[[nodiscard]] QWidget* Target() const { return m_target; }

	// The widget shows itself again, and the picture goes.
	void Finish() {
		if (m_finished) {
			return;
		}
		m_finished = true;
		if (m_target != nullptr) {
			m_target->setGraphicsEffect(nullptr);
		}
		hide();
		deleteLater();
	}

protected:
	void paintEvent(QPaintEvent* /*event*/) override {
		if (m_target == nullptr) {
			return;
		}
		QPainter painter(this);
		painter.setOpacity(m_progress);
		const QPointF at = QPointF(m_target->pos()) + QPointF(m_offset) * (1.0 - m_progress);
		painter.drawPixmap(at, m_shot);
	}

private:
	QPointer<QWidget> m_target;
	QPixmap           m_shot;
	QPoint            m_offset;
	int               m_ms;
	int               m_delay;
	double            m_progress = 0.0;
	bool              m_finished = false;
};

} // namespace

void PsAnimateIn(QWidget* widget, QPoint offset, int ms, int delay) {
	auto* host = widget->parentWidget();
	if (host == nullptr) {
		return;
	}
	// One still going for this widget ends first.
	for (auto* child: host->findChildren<QWidget*>(QStringLiteral("psAnimateInShot"),
	                                                      Qt::FindDirectChildrenOnly)) {
		if (auto* running = dynamic_cast<AnimateInShot*>(child); running != nullptr && running->Target() == widget) {
			running->Finish();
		}
	}
	// Laid out as it will be shown, then pictured once.
	QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	auto shot = GrabClear(widget);
	// The live widget stays where it is, invisible (an effect at nothing draws nothing), until the
	// picture has come in.
	auto* hidden = new QGraphicsOpacityEffect(widget);
	hidden->setOpacity(0.0);
	widget->setGraphicsEffect(hidden);
	new AnimateInShot(host, widget, std::move(shot), offset, ms, delay);
}

namespace {

// Keeps a FocusRing on a list's current item.
class ListFocusRing final: public QObject {
public:
	explicit ListFocusRing(QListWidget* list): QObject(list), m_list(list) {
		m_ring = new FocusRing(list->viewport());
		m_move = new QVariantAnimation(this);
		m_move->setDuration(190);
		m_move->setEasingCurve(QEasingCurve::OutCubic);
		connect(m_move, &QVariantAnimation::valueChanged, m_ring,
		        [this](const QVariant& value) { m_ring->setGeometry(value.toRect()); });
		m_fade = new QVariantAnimation(this);
		m_fade->setDuration(160);
		connect(m_fade, &QVariantAnimation::valueChanged, m_ring,
		        [this](const QVariant& value) { m_ring->SetAlpha(value.toDouble()); });
		connect(list, &QListWidget::currentRowChanged, this, [this] { Follow(true); });
		connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { Follow(false); });
		list->installEventFilter(this);
		list->viewport()->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		switch (event->type()) {
			case QEvent::FocusIn:
			case QEvent::FocusOut:
				if (watched == m_list) {
					Fade(m_list->hasFocus() ? 1.0 : 0.45);
				}
				break;
			case QEvent::Show:
			case QEvent::Resize:
			case QEvent::LayoutRequest: QTimer::singleShot(0, this, [this] { Follow(false); }); break;
			default: break;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	void Follow(bool animate) {
		auto* item = m_list->currentItem();
		if (item == nullptr || item->isHidden()) {
			m_ring->hide();
			return;
		}
		const auto target = m_list->visualItemRect(item).adjusted(1, 1, -1, -1);
		if (animate && m_ring->isVisible()) {
			m_move->stop();
			m_move->setStartValue(m_ring->geometry());
			m_move->setEndValue(target);
			m_move->start();
		} else {
			if (m_move->state() == QAbstractAnimation::Running) {
				m_move->setEndValue(target);
			} else {
				m_ring->setGeometry(target);
			}
			if (!m_ring->isVisible()) {
				m_ring->show();
				m_ring->raise();
				Fade(m_list->hasFocus() ? 1.0 : 0.45);
			}
		}
	}

	void Fade(double to) {
		const double from = m_fade->currentValue().isValid() ? m_fade->currentValue().toDouble() : 0.0;
		m_fade->stop();
		m_fade->setStartValue(from);
		m_fade->setEndValue(to);
		m_fade->start();
	}

	QListWidget*       m_list;
	FocusRing*         m_ring = nullptr;
	QVariantAnimation* m_move = nullptr;
	QVariantAnimation* m_fade = nullptr;
};

// The screen left behind in a stack's transition, drawn over the stack as it goes.
class TransitionShot final: public QWidget {
public:
	TransitionShot(QStackedWidget* stack, QPixmap shot, bool forward)
	    : QWidget(stack), m_shot(std::move(shot)), m_forward(forward) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setGeometry(stack->rect());
		raise();
		show();
		auto* animation = new QVariantAnimation(this);
		animation->setDuration(forward ? 260 : 300);
		animation->setEasingCurve(QEasingCurve::OutCubic);
		animation->setStartValue(0.0);
		animation->setEndValue(1.0);
		connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
			m_progress = value.toDouble();
			update();
		});
		connect(animation, &QVariantAnimation::finished, this, &QObject::deleteLater);
		animation->start();
	}

protected:
	void paintEvent(QPaintEvent* /*event*/) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.setOpacity(1.0 - m_progress);
		const QSizeF size = QSizeF(m_shot.size()) / m_shot.devicePixelRatio();
		if (m_forward) {
			// Growing towards the viewer as it fades.
			const double scale = 1.0 + 0.06 * m_progress;
			painter.translate(width() / 2.0, height() / 2.0);
			painter.scale(scale, scale);
			painter.translate(-size.width() / 2.0, -size.height() / 2.0);
			painter.drawPixmap(QPointF(0, 0), m_shot);
		} else {
			painter.drawPixmap(QPointF(90.0 * m_progress, 0), m_shot);
		}
	}

private:
	QPixmap m_shot;
	bool    m_forward;
	double  m_progress = 0.0;
};

} // namespace

void PsAttachFocusRing(QListWidget* list) {
	new ListFocusRing(list);
}

void PsSwitchStack(QStackedWidget* stack, int index, bool forward) {
	if (index == stack->currentIndex() || stack->currentWidget() == nullptr) {
		stack->setCurrentIndex(index);
		return;
	}
	auto shot = GrabClear(stack->currentWidget());
	stack->setCurrentIndex(index);
	new TransitionShot(stack, std::move(shot), forward);
	// The next screen comes in as the last one goes, a beat behind it.
	PsAnimateIn(stack->currentWidget(), QPoint(forward ? 70 : -70, 0), 320, forward ? 70 : 40);
}
