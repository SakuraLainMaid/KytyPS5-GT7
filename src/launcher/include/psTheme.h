#ifndef PS_THEME_H
#define PS_THEME_H

#include <QAbstractButton>
#include <QColor>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QStackedWidget;
class QVariantAnimation;

// The launcher's look in the console's style: a theme (the background behind the home view's
// welcome hub, the library and the settings, and an accent colour), chosen among presets or
// made of the user's own picture, kept in the launcher's settings.
struct PsTheme {
	QString id;
	QString name;
	QColor  top;    // the background gradient, top left
	QColor  bottom; // and bottom right
	QColor  accent;
	QString image;  // a picture of the user's (the "custom" theme), or empty

	[[nodiscard]] static QStringList PresetIds();
	[[nodiscard]] static PsTheme     Preset(const QString& id);
	// The theme chosen (with the custom picture and accent, when set).
	[[nodiscard]] static PsTheme     Current();
	static void                      Save(const QString& id, const QString& image, const QColor& accent);
	// The picture of a custom theme, loaded once.
	[[nodiscard]] QPixmap            Picture() const;
};

// Tells the views that the theme changed (they repaint with PsTheme::Current()).
class PsThemeNotifier final: public QObject {
	Q_OBJECT

public:
	static PsThemeNotifier& Instance();

signals:
	void Changed();
};

// The console's on/off switch: a pill whose knob slides across, the track lit in the theme's
// accent when on.
class PsSwitch final: public QAbstractButton {
	Q_OBJECT

public:
	explicit PsSwitch(QWidget* parent = nullptr);

	[[nodiscard]] QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent* event) override;
	void checkStateSet() override;
	void nextCheckState() override;
	void keyPressEvent(QKeyEvent* event) override;

private:
	void Animate();

	QVariantAnimation* m_animation = nullptr;
	float              m_position  = 0.0f; // the knob, 0 (off) to 1 (on)
};

// The console's list of choices for a setting: a panel beside the row, the current choice ticked,
// the focused one outlined; up and down move, Enter picks, Escape closes.
class PsChoicePopup final: public QWidget {
	Q_OBJECT

public:
	PsChoicePopup(QComboBox* combo, QWidget* anchor);
	void Open();

protected:
	void paintEvent(QPaintEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	static constexpr int MaxVisible = 9;
	static constexpr int ItemHeight = 54;
	static constexpr int Padding    = 6;

	void Choose(int index);
	void MoveFocus(int step);
	[[nodiscard]] int ItemAt(const QPoint& position) const;

	QPointer<QComboBox> m_combo;
	QStringList         m_items;
	int                 m_focus = 0;
	int                 m_first = 0; // the first item shown
	QVariantAnimation*  m_animation = nullptr;
};

// A setting with choices as the console shows it: its name on the left, the current choice on the
// right in grey; Enter or a click opens a PsChoicePopup. The combo box stays the value's owner.
class PsChoiceRow final: public QWidget {
	Q_OBJECT

public:
	PsChoiceRow(const QString& text, QComboBox* combo, QWidget* parent);

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	void OpenChoices();

	QComboBox* m_combo = nullptr;
	QLabel*    m_label = nullptr;
	QLabel*    m_value = nullptr;
};

// A scroll area that glides: the wheel and EnsureVisibleSmooth() animate the scroll bar.
class PsScrollArea final: public QScrollArea {
	Q_OBJECT

public:
	explicit PsScrollArea(QWidget* parent = nullptr);

	void ScrollTo(int value);
	// At once, stopping a glide.
	void JumpTo(int value);
	void EnsureVisibleSmooth(QWidget* child, int margin = 24);
	// The page's focus outline: it glides to `row` (or fades in on it), and fades out once no
	// row has the focus.
	void FocusRow(QWidget* row);
	void UnfocusRow(QWidget* row);
	// The outline's corners (to match what it frames).
	void SetRingRadius(qreal radius) { m_ring_radius = radius; }

protected:
	void wheelEvent(QWheelEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	[[nodiscard]] QRect RingTarget() const;

	QVariantAnimation* m_animation = nullptr;
	int                m_target    = 0;
	QWidget*           m_ring      = nullptr;
	qreal              m_ring_radius = 5.0;
	QPointer<QWidget>  m_ring_row;
	QVariantAnimation* m_ring_move = nullptr;
	QVariantAnimation* m_ring_fade = nullptr;
};

// Lays a settings group out again as the console's list: one row per setting, check boxes as
// switch rows, combo boxes as choice rows, other fields beside their names. The original widgets
// keep the values (hidden in the rows), so code that reads, sets, shows or enables them works on.
void BuildSettingsRows(QGroupBox* group);

// Slides `widget` in from `offset` (relative to where it is) while it fades in, after `delay`
// milliseconds (hidden until then).
void PsAnimateIn(QWidget* widget, QPoint offset, int ms = 220, int delay = 0);

// A list's current item outlined by a frame that glides from item to item (fainter while the
// list does not have the focus); the list's own selection is left unframed by the style sheet.
void PsAttachFocusRing(QListWidget* list);

// Switches `stack` to `index` with a transition: forward, the screen left behind grows a little
// as it fades away while the next comes in from the right; back, the screen left slides off to
// the right while the earlier one comes back from the left.
void PsSwitchStack(QStackedWidget* stack, int index, bool forward);

#endif // PS_THEME_H
