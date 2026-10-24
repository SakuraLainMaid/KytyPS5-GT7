#include "psHomeView.h"

#include "gameContent.h"
#include "psGlyphs.h"
#include "psTheme.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QGraphicsOpacityEffect>
#include <QDateTime>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <utility>

namespace {

// Sizes at a 720-pixel-high view; everything scales with the height.
constexpr float MarginX       = 64.0f;
constexpr float TopBarY       = 56.0f; // the baseline of the top bar's text
constexpr float TileRowY      = 108.0f;
constexpr float TileSize      = 92.0f;
constexpr float SelectedSize  = 128.0f;
constexpr float TileGap       = 14.0f;
constexpr float LibraryTop    = 168.0f; // the library grid's first row
constexpr float LibraryCell   = 132.0f;
constexpr float LibraryLabel  = 34.0f;  // the name under each icon
constexpr float LibraryGap    = 26.0f;
constexpr float ToolSize      = 38.0f;
constexpr float AvatarSize    = 34.0f;
constexpr float ButtonHeight  = 46.0f;
constexpr float OptionsWidth  = 400.0f;
constexpr float OptionHeight  = 52.0f;
constexpr int   RecentCount   = 10;
constexpr int   ResizeBorder  = 6;     // pixels
constexpr int   WindowButtonW = 46;    // pixels, as Windows draws them
constexpr int   WindowButtonH = 32;

float ViewScale(const QWidget& widget) {
	return std::clamp(static_cast<float>(widget.height()) / 720.0f, 0.75f, 1.6f);
}

int Px(const QWidget& widget, float size) {
	return static_cast<int>(std::lround(size * ViewScale(widget)));
}

QFont ScaledFont(const QWidget& widget, float size, int weight = QFont::Normal) {
	auto font = widget.font();
	font.setPixelSize(std::max(1, Px(widget, size)));
	font.setWeight(static_cast<QFont::Weight>(weight));
	return font;
}

QPixmap LoadImage(const QString& basedir, const QString& file) {
	QPixmap pixmap;
	const auto data = GameContent::ReadFile(basedir, file, GameContent::MaxImageSize);
	if (!data.isEmpty()) {
		pixmap.loadFromData(data);
	}
	return pixmap;
}

QSettings ViewSettings() {
	return QSettings(QSettings::UserScope, QStringLiteral("Kyty"), QStringLiteral("LauncherView"));
}

float Lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

QVariantAnimation* MakeAnimation(QObject* parent, int ms, QEasingCurve::Type curve,
                                 std::function<void(float)> apply) {
	auto* animation = new QVariantAnimation(parent);
	animation->setDuration(ms);
	animation->setEasingCurve(curve);
	animation->setStartValue(0.0f);
	animation->setEndValue(1.0f);
	QObject::connect(animation, &QVariantAnimation::valueChanged, parent,
	                 [apply = std::move(apply)](const QVariant& value) { apply(value.toFloat()); });
	return animation;
}

constexpr char ToolStyle[] = R"(
QToolButton { background: transparent; border: 0; }
QToolButton:hover { background: rgba(255,255,255,45); }
QToolButton:pressed { background: rgba(255,255,255,80); }
)";
constexpr char WindowButtonStyle[] = R"(
QToolButton { background: transparent; border: 0; border-radius: 0; }
QToolButton:hover { background: rgba(255,255,255,40); }
QToolButton:pressed { background: rgba(255,255,255,70); }
)";
constexpr char CloseButtonStyle[] = R"(
QToolButton { background: transparent; border: 0; border-radius: 0; }
QToolButton:hover { background: #e81123; }
QToolButton:pressed { background: #f1707a; }
)";
constexpr char SearchStyle[] = R"(
QLineEdit { background: rgba(255,255,255,40); color: #ffffff; border: 0; padding: 0 14px;
            selection-background-color: #3d8bfd; }
)";

// The top bar, from the right edge: the clock, the user name, the avatar, the tools.
struct TopBar {
	float clock_x;
	float name_x;
	QRect avatar;
	int   tools_right;
};

TopBar TopBarGeometry(const QWidget& widget, const QString& user_name) {
	const float         s = ViewScale(widget);
	const QFontMetricsF metrics(ScaledFont(widget, 17.0f));
	const auto          clock = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
	TopBar              bar {};
	bar.clock_x = static_cast<float>(widget.width()) - MarginX * s -
	              static_cast<float>(metrics.horizontalAdvance(clock));
	bar.name_x =
	    bar.clock_x - 24.0f * s - static_cast<float>(metrics.horizontalAdvance(user_name));
	const int avatar_size = Px(widget, AvatarSize);
	const int center_y    = Px(widget, TopBarY - 6.0f);
	bar.avatar = QRect(static_cast<int>(std::lround(bar.name_x - 10.0f * s)) - avatar_size,
	                   center_y - avatar_size / 2, avatar_size, avatar_size);
	bar.tools_right = bar.avatar.left() - Px(widget, 22.0f);
	return bar;
}

void PaintGlyphTile(QPainter& painter, const QRectF& rect, float radius, const QString& label,
                    const QFont& font) {
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(255, 255, 255, 38));
	painter.drawRoundedRect(rect, radius, radius);
	const int size = static_cast<int>(rect.width() * 0.36);
	PsGlyphIcon(PsGlyph::Grid, size, 0.06f)
	    .paint(&painter, QRect(static_cast<int>(rect.center().x()) - size / 2,
	                           static_cast<int>(rect.top() + rect.height() * 0.24), size, size));
	painter.setPen(Qt::white);
	painter.setFont(font);
	painter.drawText(rect.adjusted(4, rect.height() * 0.62, -4, -4),
	                 Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, label);
}

} // namespace

PsHomeView::PsHomeView(QWidget* parent): QWidget(parent) {
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent);

	m_select_animation = MakeAnimation(this, 240, QEasingCurve::OutCubic, [this](float v) {
		m_select_progress = v;
		update();
	});
	m_ring.animation = MakeAnimation(this, 200, QEasingCurve::OutCubic, [this](float v) {
		m_ring.progress = v;
		update();
	});
	m_option_glide.animation = MakeAnimation(this, 170, QEasingCurve::OutCubic, [this](float v) {
		m_option_glide.progress = v;
		update();
	});
	m_hero_animation = MakeAnimation(this, 420, QEasingCurve::InOutQuad, [this](float v) {
		m_hero_progress = v;
		update();
	});
	m_tab_animation = MakeAnimation(this, 260, QEasingCurve::OutCubic, [this](float v) {
		m_tab_progress = v;
		update();
	});
	m_options_animation = MakeAnimation(this, 260, QEasingCurve::OutCubic, [this](float v) {
		m_options_progress = m_options_open ? v : 1.0f - v;
		update();
	});
	m_scroll_animation = new QVariantAnimation(this);
	m_scroll_animation->setDuration(220);
	m_scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
	connect(m_scroll_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
		m_scroll = v.toFloat();
		update();
	});

	const std::array<std::pair<PsGlyph, QString>, 5> tools {{
	    {PsGlyph::Search, tr("Search")},
	    {PsGlyph::Refresh, tr("Rescan game folders")},
	    {PsGlyph::Gamepad, tr("Input mapping")},
	    {PsGlyph::Gear, tr("Global settings and game folders")},
	    {PsGlyph::List, tr("Game list")},
	}};
	for (const auto& [glyph, tip]: tools) {
		auto* tool = new QToolButton(this);
		tool->setToolTip(tip);
		tool->setCursor(Qt::PointingHandCursor);
		tool->setFocusPolicy(Qt::NoFocus);
		tool->setProperty("glyph", static_cast<int>(glyph));
		m_tools.push_back(tool);
	}
	connect(m_tools[0], &QToolButton::clicked, this, [this] { ShowSearch(!m_search->isVisible()); });
	connect(m_tools[1], &QToolButton::clicked, this, &PsHomeView::RefreshRequested);
	connect(m_tools[2], &QToolButton::clicked, this, &PsHomeView::InputMappingRequested);
	connect(m_tools[3], &QToolButton::clicked, this, &PsHomeView::GlobalSettingsRequested);
	connect(m_tools[4], &QToolButton::clicked, this, &PsHomeView::ListViewRequested);

	const std::array<std::pair<PsGlyph, QString>, 3> window_buttons {{
	    {PsGlyph::Minimize, tr("Minimize")},
	    {PsGlyph::Maximize, tr("Maximize")},
	    {PsGlyph::Close, tr("Close")},
	}};
	for (const auto& [glyph, tip]: window_buttons) {
		auto* button = new QToolButton(this);
		button->setToolTip(tip);
		button->setFocusPolicy(Qt::NoFocus);
		button->setFixedSize(WindowButtonW, WindowButtonH);
		button->setIcon(PsGlyphIcon(glyph, 20, 0.05f));
		button->setIconSize(QSize(20, 20));
		button->setStyleSheet(QString::fromLatin1(glyph == PsGlyph::Close ? CloseButtonStyle
		                                                                : WindowButtonStyle));
		button->hide();
		m_window_buttons.push_back(button);
	}
	connect(m_window_buttons[0], &QToolButton::clicked, this, &PsHomeView::MinimizeRequested);
	connect(m_window_buttons[1], &QToolButton::clicked, this, &PsHomeView::MaximizeRequested);
	connect(m_window_buttons[2], &QToolButton::clicked, this, &PsHomeView::CloseRequested);

	m_avatar_button = new QToolButton(this);
	m_avatar_button->setToolTip(tr("Choose your avatar"));
	m_avatar_button->setCursor(Qt::PointingHandCursor);
	m_avatar_button->setFocusPolicy(Qt::NoFocus);
	m_avatar_button->setStyleSheet(
	    QStringLiteral("QToolButton { background: transparent; border: 0; }"));
	connect(m_avatar_button, &QToolButton::clicked, this, &PsHomeView::ShowAvatarMenu);

	m_search = new QLineEdit(this);
	m_search->setPlaceholderText(tr("Search games"));
	m_search->setClearButtonEnabled(true);
	m_search->hide();
	connect(m_search, &QLineEdit::textChanged, this, &PsHomeView::SearchTextChanged);

	m_clock = new QTimer(this);
	m_clock->setInterval(15000);
	connect(m_clock, &QTimer::timeout, this, qOverload<>(&QWidget::update));
	m_clock->start();

	connect(&PsThemeNotifier::Instance(), &PsThemeNotifier::Changed, this,
	        qOverload<>(&QWidget::update));

	m_gamepad = new GamepadNavigator(this, this);
	// Queued: a page the pad opens runs its own event loop, and the pad's polling (whose signal
	// this is) must not be inside it, or its timer stops for as long as the page is open.
	connect(m_gamepad, &GamepadNavigator::Triggered, this, [this](GamepadNavigator::Action a) {
		if (!isVisible()) {
			return;
		}
		if (m_page != nullptr) {
			// A page is driven as the keyboard drives it.
			using A = GamepadNavigator::Action;
			int key = 0;
			switch (a) {
				case A::Up: key = Qt::Key_Up; break;
				case A::Down: key = Qt::Key_Down; break;
				case A::Left: key = Qt::Key_Left; break;
				case A::Right: key = Qt::Key_Right; break;
				case A::Accept: key = Qt::Key_Return; break;
				case A::Back: key = Qt::Key_Escape; break;
				case A::PreviousTab: key = Qt::Key_PageUp; break;
				case A::NextTab: key = Qt::Key_PageDown; break;
				case A::Search: key = Qt::Key_Y; break;
				case A::Options: key = Qt::Key_Menu; break;
				default: return;
			}
			QWidget* target = QApplication::activePopupWidget();
			if (target == nullptr) {
				target = QApplication::focusWidget();
				if (target == nullptr || !m_page->isAncestorOf(target)) {
					target = m_page;
				}
			}
			QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
			QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
			QApplication::sendEvent(target, &press);
			QApplication::sendEvent(target, &release);
			return;
		}
		Navigate(a);
	}, Qt::QueuedConnection);

	const auto avatar_path = ViewSettings().value(QStringLiteral("avatar")).toString();
	if (!avatar_path.isEmpty()) {
		m_avatar.load(avatar_path);
	}
	LayoutWidgets();
}

// Games and selection.

void PsHomeView::SetGames(std::vector<Game> games, const ConfigurationItem* selected) {
	m_games  = std::move(games);
	m_wanted = selected;
	RebuildTiles();
}

void PsHomeView::RebuildTiles() {
	std::vector<Tile> tiles;
	const auto        icon_of = [this](const QString& basedir) {
        for (const auto& old: m_tiles) {
            if (IsGame(old) && old.game.basedir == basedir) {
                return old.icon;
            }
        }
        return LoadImage(basedir, QStringLiteral("sce_sys/icon0.png"));
	};
	if (m_tab == Tab::Games) {
		// The console's home row: the welcome hub, the recently played games, the library.
		Tile welcome;
		welcome.welcome_tile = true;
		welcome.game.title   = tr("Welcome");
		tiles.push_back(welcome);
		// A game opened from the library comes first even when it has not been played.
		std::vector<const Game*> order;
		for (const auto& game: m_games) {
			order.push_back(&game);
		}
		std::stable_sort(order.begin(), order.end(), [this](const Game* a, const Game* b) {
			const bool a_wanted = a->item == m_wanted && a->last_played == 0;
			const bool b_wanted = b->item == m_wanted && b->last_played == 0;
			if (a_wanted != b_wanted) {
				return a_wanted;
			}
			return a->last_played > b->last_played;
		});
		for (const auto* game: order) {
			if (static_cast<int>(tiles.size()) == RecentCount + 1) {
				break;
			}
			tiles.push_back({*game, icon_of(game->basedir)});
		}
		if (!m_games.empty()) {
			Tile library;
			library.library_tile  = true;
			library.game.title    = tr("Game Library");
			library.game.subtitle = tr("%n game(s)", nullptr, static_cast<int>(m_games.size()));
			tiles.push_back(library);
		}
	} else {
		for (const auto& game: m_games) {
			tiles.push_back({game, icon_of(game.basedir)});
		}
	}
	// The welcome hub's storage card: the drive of the games.
	m_storage_free = m_storage_total = 0;
	if (!m_games.empty()) {
		const QStorageInfo storage(m_games.front().basedir);
		if (storage.isValid()) {
			m_storage_free  = storage.bytesAvailable();
			m_storage_total = storage.bytesTotal();
			m_storage_name  = storage.rootPath();
		}
	}
	m_tiles          = std::move(tiles);
	m_index          = -1;
	m_previous_index = -1;
	int wanted       = -1;
	for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
		if (IsGame(m_tiles[i]) && m_tiles[i].game.item == m_wanted) {
			wanted = i;
			break;
		}
	}
	if (wanted < 0) {
		// The first game, as the console selects the game played last.
		for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
			if (IsGame(m_tiles[i])) {
				wanted = i;
				break;
			}
		}
	}
	if (!m_tiles.empty()) {
		Select(std::max(0, wanted), true);
		m_scroll_animation->stop();
		m_scroll = ScrollTarget();
	} else {
		m_hero = m_previous_hero = QPixmap();
		m_hero_key.clear();
	}
	LayoutWidgets();
	update();
}

bool PsHomeView::IsGame(const Tile& tile) {
	return !tile.library_tile && !tile.welcome_tile;
}

// A game's logo: the picture chosen for it in its options (the packages carry none; the
// console's come from the store).
QPixmap PsHomeView::LogoOf(const Game& game) {
	if (game.logo.isEmpty()) {
		return {};
	}
	if (auto found = m_logos.find(game.logo); found != m_logos.end()) {
		return found.value();
	}
	QPixmap logo;
	logo.load(game.logo);
	m_logos.insert(game.logo, logo);
	return logo;
}

void PsHomeView::SetSelected(const ConfigurationItem* item) {
	m_wanted = item;
	for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
		if (IsGame(m_tiles[i]) && m_tiles[i].game.item == item) {
			Select(i, false);
			return;
		}
	}
}

const PsHomeView::Tile* PsHomeView::Current() const {
	return m_index >= 0 && m_index < static_cast<int>(m_tiles.size()) ? &m_tiles[m_index]
	                                                                   : nullptr;
}

void PsHomeView::Select(int index, bool notify) {
	if (m_tiles.empty()) {
		return;
	}
	index = std::clamp(index, 0, static_cast<int>(m_tiles.size()) - 1);
	if (index == m_index) {
		return;
	}
	m_previous_index = m_index;
	m_index          = index;
	const auto& tile = m_tiles[m_index];
	if (IsGame(tile)) {
		m_wanted = tile.game.item;
	}
	LoadHero();
	m_select_animation->stop();
	m_select_animation->start();
	m_scroll_animation->stop();
	m_scroll_animation->setStartValue(m_scroll);
	m_scroll_animation->setEndValue(ScrollTarget());
	m_scroll_animation->start();
	const auto buttons = VisibleButtons();
	m_button_focus =
	    std::clamp(m_button_focus, 0, std::max(0, static_cast<int>(buttons.size()) - 1));
	LayoutWidgets();
	update();
	if (notify && IsGame(tile)) {
		emit SelectionChanged(tile.game.item);
	}
}

// The Games tab scrolls sideways (the selected tile stays second from the left), the library
// down (the selected row stays in view).
float PsHomeView::ScrollTarget() const {
	const float s = ViewScale(*this);
	if (m_tab == Tab::Games) {
		return static_cast<float>(std::max(0, m_index - 1)) * (TileSize + TileGap) * s;
	}
	const int   row     = std::max(0, m_index) / LibraryColumns();
	const float row_h   = (LibraryCell + LibraryLabel + LibraryGap) * s;
	const float top     = LibraryTop * s;
	const float visible = static_cast<float>(height()) - top - 24.0f * s;
	const float row_top = static_cast<float>(row) * row_h;
	float       target  = m_scroll;
	if (row_top < target) {
		target = row_top;
	} else if (row_top + row_h > target + visible) {
		target = row_top + row_h - visible;
	}
	return std::max(0.0f, target);
}

int PsHomeView::LibraryColumns() const {
	const float s     = ViewScale(*this);
	const float space = static_cast<float>(width()) - 2.0f * MarginX * s + LibraryGap * s;
	return std::max(1, static_cast<int>(space / ((LibraryCell + LibraryGap) * s)));
}

void PsHomeView::SetTab(Tab tab) {
	if (tab == m_tab) {
		return;
	}
	m_tab  = tab;
	m_zone = Zone::Tiles;
	RebuildTiles();
	m_tab_animation->stop();
	m_tab_animation->start();
}

void PsHomeView::LoadHero() {
	const auto& tile = m_tiles[m_index];
	if (tile.library_tile) {
		return; // the library tile keeps the art behind it
	}
	const auto key = tile.welcome_tile ? QStringLiteral("welcome") : tile.game.basedir;
	if (key == m_hero_key) {
		return;
	}
	m_hero_key = key;
	QPixmap hero; // the welcome hub: the console's own background
	if (!tile.welcome_tile) {
		hero = LoadImage(m_hero_key, QStringLiteral("sce_sys/pic0.png"));
		if (hero.isNull()) {
			hero = LoadImage(m_hero_key, QStringLiteral("sce_sys/pic1.png"));
		}
	}
	m_previous_hero = m_hero;
	m_hero          = hero;
	m_hero_animation->stop();
	m_hero_animation->start();
}

void PsHomeView::SetUserName(const QString& name) {
	m_user_name = name;
	LayoutWidgets();
	update();
}

void PsHomeView::SetPlayEnabled(bool enabled) {
	m_play_enabled = enabled;
	update();
}

void PsHomeView::SetFrameless(bool frameless) {
	m_frameless = frameless;
	for (auto* button: m_window_buttons) {
		button->setVisible(frameless);
	}
	LayoutWidgets();
}

// Geometry.

QRectF PsHomeView::TabRect(Tab tab) const {
	const float         s = ViewScale(*this);
	const QFontMetricsF metrics(ScaledFont(*this, 20.0f, QFont::DemiBold));
	const float         games = static_cast<float>(metrics.horizontalAdvance(tr("Games")));
	const float library       = static_cast<float>(metrics.horizontalAdvance(tr("Library")));
	const float x = MarginX * s + (tab == Tab::Games ? 0.0f : games + 28.0f * s);
	return {x, (TopBarY - 24.0f) * s, tab == Tab::Games ? games : library, 32.0f * s};
}

QRectF PsHomeView::TileRect(int index) const {
	const float s = ViewScale(*this);
	if (m_tab == Tab::Games) {
		const auto weight = [this](int i) {
			if (i == m_index) {
				return m_select_progress;
			}
			return i == m_previous_index ? 1.0f - m_select_progress : 0.0f;
		};
		float x = MarginX * s - m_scroll;
		for (int i = 0; i < index; i++) {
			x += (Lerp(TileSize, SelectedSize, weight(i)) + TileGap) * s;
		}
		const float size = Lerp(TileSize, SelectedSize, weight(index)) * s;
		return {x, TileRowY * s, size, size};
	}
	// The library: a grid of the games' icons, their names below, as the console's.
	const int   columns = LibraryColumns();
	const int   column  = index % columns;
	const int   row     = index / columns;
	const float x       = (MarginX + static_cast<float>(column) * (LibraryCell + LibraryGap)) * s;
	const float y       = LibraryTop * s +
	                static_cast<float>(row) * (LibraryCell + LibraryLabel + LibraryGap) * s -
	                m_scroll;
	return {x, y, LibraryCell * s, LibraryCell * s};
}

int PsHomeView::TileAt(const QPointF& position) const {
	for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
		if (TileRect(i).contains(position)) {
			return i;
		}
	}
	return -1;
}

std::vector<PsHomeView::Button> PsHomeView::VisibleButtons() const {
	const auto* tile = Current();
	if (tile == nullptr || m_tab == Tab::Library || tile->welcome_tile) {
		return {};
	}
	if (tile->library_tile) {
		return {Button::Play};
	}
	std::vector<Button> buttons {Button::Play, Button::Settings};
	if (tile->game.has_trophies) {
		buttons.push_back(Button::Trophies);
	}
	buttons.push_back(Button::Options);
	return buttons;
}

// The details (title, subtitle, buttons) start here: below the row on the Games tab, beside the
// covers in the library.
// The selected game's logo (or title) sits in the lower left of the art, as on the console, with
// its buttons below.
static QPointF DetailsOrigin(const QWidget& widget) {
	const float s = ViewScale(widget);
	return {MarginX * s, std::max((TileRowY + SelectedSize + 80.0f) * s,
	                              static_cast<float>(widget.height()) * 0.5f)};
}

static float LogoHeight(const QWidget& widget) {
	return std::clamp(static_cast<float>(widget.height()) * 0.17f, 80.0f * ViewScale(widget),
	                  220.0f * ViewScale(widget));
}

QRectF PsHomeView::ButtonRect(Button button) const {
	const float s      = ViewScale(*this);
	const auto  origin = DetailsOrigin(*this);
	const float slide  = (1.0f - m_select_progress) * 14.0f * s;
	float       x      = static_cast<float>(origin.x());
	const float y = static_cast<float>(origin.y()) + LogoHeight(*this) + 62.0f * s + slide;
	const float height = ButtonHeight * s;
	for (const auto visible: VisibleButtons()) {
		float width = height;
		if (visible != Button::Options) {
			const bool play  = visible == Button::Play;
			const auto label = play ? (Current()->library_tile ? tr("Open") : tr("Play"))
			                   : visible == Button::Settings ? tr("Settings")
			                                                 : tr("Trophies");
			const QFontMetricsF metrics(
			    ScaledFont(*this, play ? 17.0f : 15.0f, play ? QFont::Bold : QFont::Normal));
			width = static_cast<float>(metrics.horizontalAdvance(label)) +
			        (play ? 96.0f : 44.0f) * s;
		}
		if (visible == button) {
			return {x, y, width, height};
		}
		x += width + 12.0f * s;
	}
	return {};
}

QRectF PsHomeView::OptionsPanelRect() const {
	const float s     = ViewScale(*this);
	const float width = std::min(OptionsWidth * s, static_cast<float>(this->width()) * 0.8f);
	return {static_cast<float>(this->width()) - width * m_options_progress, 0.0f, width,
	        static_cast<float>(height())};
}

QRectF PsHomeView::OptionRect(int index) const {
	const float s     = ViewScale(*this);
	const auto  panel = OptionsPanelRect();
	return {panel.left() + 16.0f * s, (124.0f + static_cast<float>(index) * OptionHeight) * s,
	        panel.width() - 32.0f * s, (OptionHeight - 6.0f) * s};
}

Qt::Edges PsHomeView::ResizeEdges(const QPointF& position) const {
	Qt::Edges edges;
	if (!m_frameless || window()->isMaximized()) {
		return edges;
	}
	if (position.x() < ResizeBorder) {
		edges |= Qt::LeftEdge;
	}
	if (position.x() > width() - ResizeBorder) {
		edges |= Qt::RightEdge;
	}
	if (position.y() < ResizeBorder) {
		edges |= Qt::TopEdge;
	}
	if (position.y() > height() - ResizeBorder) {
		edges |= Qt::BottomEdge;
	}
	return edges;
}

void PsHomeView::LayoutWidgets() {
	const auto bar       = TopBarGeometry(*this, m_user_name);
	const int  tool_size = Px(*this, ToolSize);
	const auto round     = QStringLiteral("QToolButton { border-radius: %1px; }").arg(tool_size / 2);
	int        right     = bar.tools_right;
	for (auto it = m_tools.rbegin(); it != m_tools.rend(); ++it) {
		auto* tool = *it;
		tool->setFixedSize(tool_size, tool_size);
		tool->setIcon(
		    PsGlyphIcon(static_cast<PsGlyph>(tool->property("glyph").toInt()), tool_size * 6 / 10));
		tool->setIconSize(QSize(tool_size * 6 / 10, tool_size * 6 / 10));
		tool->setStyleSheet(QString::fromLatin1(ToolStyle) + round);
		right -= tool_size;
		tool->move(right, bar.avatar.center().y() - tool_size / 2);
		right -= Px(*this, 6.0f);
	}
	const int search_width = Px(*this, 260.0f);
	m_search->setFixedSize(search_width, tool_size);
	m_search->setFont(ScaledFont(*this, 15.0f));
	m_search->setStyleSheet(QString::fromLatin1(SearchStyle) +
	                        QStringLiteral("QLineEdit { border-radius: %1px; }").arg(tool_size / 2));
	m_search->move(right - search_width, bar.avatar.center().y() - tool_size / 2);
	m_avatar_button->setGeometry(bar.avatar);
	UpdateAvatar();

	int x = width();
	for (auto it = m_window_buttons.rbegin(); it != m_window_buttons.rend(); ++it) {
		x -= WindowButtonW;
		(*it)->move(x, 0);
		(*it)->raise();
	}
	if (m_window_buttons.size() == 3) {
		const bool maximized = window()->isMaximized();
		m_window_buttons[1]->setIcon(PsGlyphIcon(maximized ? PsGlyph::Restore : PsGlyph::Maximize, 20, 0.05f));
		m_window_buttons[1]->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
	}
}

// The avatar.

void PsHomeView::UpdateAvatar() {
	const int     size       = std::max(1, m_avatar_button->width());
	constexpr int Oversample = 2;
	QPixmap       pixmap(size * Oversample, size * Oversample);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	const QRectF circle(0, 0, pixmap.width(), pixmap.height());
	QPainterPath clip;
	clip.addEllipse(circle);
	painter.setClipPath(clip);
	if (!m_avatar.isNull()) {
		const auto scaled = m_avatar.scaled(pixmap.size(), Qt::KeepAspectRatioByExpanding,
		                                    Qt::SmoothTransformation);
		painter.drawPixmap((pixmap.width() - scaled.width()) / 2,
		                   (pixmap.height() - scaled.height()) / 2, scaled);
	} else {
		// No picture chosen: the name's initial on the console's blue.
		QLinearGradient blue(0, 0, pixmap.width(), pixmap.height());
		blue.setColorAt(0.0, QColor(0, 112, 209));
		blue.setColorAt(1.0, QColor(0, 55, 145));
		painter.fillRect(circle, blue);
		auto font = this->font();
		font.setPixelSize(pixmap.height() / 2);
		font.setBold(true);
		painter.setFont(font);
		painter.setPen(Qt::white);
		painter.drawText(circle, Qt::AlignCenter,
		                 m_user_name.isEmpty() ? QStringLiteral("?") : m_user_name.left(1).toUpper());
	}
	painter.setClipping(false);
	painter.setPen(QPen(QColor(255, 255, 255, 200), 2.0 * Oversample));
	painter.setBrush(Qt::NoBrush);
	painter.drawEllipse(circle.adjusted(Oversample, Oversample, -Oversample, -Oversample));
	painter.end();
	pixmap.setDevicePixelRatio(Oversample);
	m_avatar_button->setIcon(QIcon(pixmap));
	m_avatar_button->setIconSize(QSize(size, size));
}

void PsHomeView::ShowAvatarMenu() {
	QMenu menu(this);
	auto* choose = menu.addAction(tr("Choose avatar..."));
	auto* remove = menu.addAction(tr("Remove avatar"));
	remove->setEnabled(!m_avatar.isNull());
	const auto* chosen =
	    menu.exec(m_avatar_button->mapToGlobal(QPoint(0, m_avatar_button->height() + 4)));
	if (chosen == choose) {
		const auto path = QFileDialog::getOpenFileName(
		    this, tr("Choose avatar"),
		    QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
		    tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
		QPixmap picture;
		if (path.isEmpty() || !picture.load(path)) {
			return;
		}
		m_avatar = picture;
		ViewSettings().setValue(QStringLiteral("avatar"), path);
	} else if (chosen == remove) {
		m_avatar = QPixmap();
		ViewSettings().remove(QStringLiteral("avatar"));
	}
	UpdateAvatar();
}

// Pages: the settings, the trophies and the input mapping, inside the view.

int PsHomeView::RunPage(QDialog* dialog) {
	if (m_page != nullptr) {
		return dialog->exec(); // a page from a page: a window of its own
	}
	CloseOptions();
	ShowSearch(false);
	m_page = dialog;
	dialog->setParent(this, Qt::Widget);
	dialog->setAttribute(Qt::WA_StyledBackground);
	dialog->setGeometry(rect());
	for (auto* tool: m_tools) {
		tool->hide();
	}
	m_avatar_button->hide();

	// In: from a little to the right, fading in, while the home screen dims; out the same way.
	auto* effect = new QGraphicsOpacityEffect(dialog);
	dialog->setGraphicsEffect(effect);
	const auto slide = [this, dialog, effect](float from, float to, int ms) {
		QVariantAnimation animation;
		animation.setDuration(ms);
		animation.setEasingCurve(to > from ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
		animation.setStartValue(from);
		animation.setEndValue(to);
		connect(&animation, &QVariantAnimation::valueChanged, dialog,
		        [this, dialog, effect](const QVariant& value) {
			        const float v = value.toFloat();
			        effect->setOpacity(v);
			        dialog->move(static_cast<int>((1.0f - v) * static_cast<float>(width()) * 0.06f), 0);
			        m_page_progress = v;
			        update();
		        });
		QEventLoop wait;
		connect(&animation, &QVariantAnimation::finished, &wait, &QEventLoop::quit);
		animation.start();
		wait.exec();
	};
	dialog->show();
	for (auto* button: m_window_buttons) {
		button->raise();
	}
	// The dialog's own focus (its first list), or the first widget that takes it.
	const auto can_focus = [dialog](QWidget* widget) {
		return widget != nullptr && widget != dialog && dialog->isAncestorOf(widget) && widget->isVisible() &&
		       widget->isEnabled() && (widget->focusPolicy() & Qt::TabFocus) != 0;
	};
	QWidget* focus = dialog->focusWidget();
	for (auto* next = dialog->nextInFocusChain(); !can_focus(focus) && next != dialog;
	     next = next->nextInFocusChain()) {
		focus = next;
	}
	if (can_focus(focus)) {
		focus->setFocus(Qt::OtherFocusReason);
	} else {
		dialog->setFocus();
	}
	int        result = QDialog::Rejected;
	QEventLoop loop;
	connect(dialog, &QDialog::finished, &loop, [&result, &loop](int code) {
		result = code;
		loop.quit();
	});
	slide(0.0f, 1.0f, 260);
	if (dialog->isVisible()) {
		loop.exec();
	} else {
		result = dialog->result(); // done before the slide ended
	}
	// done() hid it: shown again for the way out.
	dialog->show();
	slide(1.0f, 0.0f, 200);
	dialog->hide();
	dialog->setGraphicsEffect(nullptr);
	m_page          = nullptr;
	m_page_progress = 0.0f;
	for (auto* tool: m_tools) {
		tool->show();
	}
	m_avatar_button->show();
	setFocus();
	update();
	return result;
}

// The search field and the options panel.

void PsHomeView::ShowSearch(bool shown) {
	m_search->setVisible(shown);
	if (shown) {
		m_search->setFocus();
	} else {
		m_search->clear();
		setFocus();
	}
}

void PsHomeView::ShowOptions(const QString& title, const QList<QAction*>& actions) {
	m_option_glide.key = -1; // a new panel: its highlight starts where it is, not from the last one
	m_options.clear();
	for (auto* action: actions) {
		if (action->isSeparator() || !action->isVisible()) {
			continue;
		}
		m_options.push_back({action, action->text().remove(QLatin1Char('&')), action->isEnabled()});
	}
	if (m_options.empty()) {
		return;
	}
	m_options_title = title;
	m_option_focus  = 0;
	while (m_option_focus < static_cast<int>(m_options.size()) - 1 &&
	       !m_options[m_option_focus].enabled) {
		m_option_focus++;
	}
	m_options_open = true;
	// The top bar's widgets would sit over the panel.
	for (auto* tool: m_tools) {
		tool->hide();
	}
	m_avatar_button->hide();
	m_search->hide();
	m_options_animation->stop();
	m_options_animation->start();
	setFocus();
}

void PsHomeView::CloseOptions() {
	if (!m_options_open) {
		return;
	}
	m_options_open = false;
	m_options_animation->stop();
	m_options_animation->start();
	for (auto* tool: m_tools) {
		tool->show();
	}
	m_avatar_button->show();
}

// The library's selected game becomes the Games tab's first tile, ready to play.
void PsHomeView::OpenInGames() {
	const auto* tile = Current();
	if (tile == nullptr || !IsGame(*tile)) {
		return;
	}
	m_wanted = tile->game.item;
	SetTab(Tab::Games);
	m_zone         = Zone::Buttons;
	m_button_focus = 0;
	update();
}

// Input.

void PsHomeView::Activate(Button button) {
	const auto* tile = Current();
	if (tile == nullptr) {
		return;
	}
	if (tile->library_tile) {
		SetTab(Tab::Library);
		return;
	}
	switch (button) {
		case Button::Play:
			if (m_play_enabled) {
				emit PlayRequested();
			}
			break;
		case Button::Settings: emit SettingsRequested(); break;
		case Button::Trophies: emit TrophiesRequested(); break;
		case Button::Options: emit OptionsRequested(); break;
	}
}

void PsHomeView::Navigate(GamepadNavigator::Action action) {
	using A          = GamepadNavigator::Action;
	m_keyboard_focus = true;
	if (m_options_open) {
		const int  count = static_cast<int>(m_options.size());
		const auto step  = [&](int direction) {
            for (int i = m_option_focus + direction; i >= 0 && i < count; i += direction) {
                if (m_options[i].enabled) {
                    m_option_focus = i;
                    break;
                }
            }
		};
		switch (action) {
			case A::Up: step(-1); break;
			case A::Down: step(1); break;
			case A::Accept: {
				const auto chosen  = m_options[m_option_focus].action;
				const bool enabled = m_options[m_option_focus].enabled;
				CloseOptions();
				if (chosen != nullptr && enabled) {
					chosen->trigger();
				}
				break;
			}
			case A::Back:
			case A::Options:
			case A::Left: CloseOptions(); break;
			default: break;
		}
		update();
		return;
	}
	if (m_search->isVisible() && action == A::Back) {
		ShowSearch(false);
		return;
	}
	switch (action) {
		case A::PreviousTab: SetTab(Tab::Games); return;
		case A::NextTab: SetTab(Tab::Library); return;
		case A::Search: ShowSearch(true); return;
		case A::Options:
			if (Current() != nullptr && IsGame(*Current())) {
				emit OptionsRequested();
			}
			return;
		default: break;
	}
	if (m_tab == Tab::Library) {
		const int columns = LibraryColumns();
		switch (action) {
			case A::Left: Select(m_index - 1, true); break;
			case A::Right: Select(m_index + 1, true); break;
			case A::Up:
				if (m_index >= columns) {
					Select(m_index - columns, true);
				}
				break;
			case A::Down:
				if (m_index + columns < static_cast<int>(m_tiles.size())) {
					Select(m_index + columns, true);
				}
				break;
			case A::Accept: OpenInGames(); break;
			case A::Back: SetTab(Tab::Games); break;
			default: break;
		}
		update();
		return;
	}
	const auto buttons = VisibleButtons();
	if (m_zone == Zone::Tiles) {
		switch (action) {
			case A::Left: Select(m_index - 1, true); break;
			case A::Right: Select(m_index + 1, true); break;
			case A::Down:
				if (!buttons.empty()) {
					m_zone         = Zone::Buttons;
					m_button_focus = 0;
				}
				break;
			case A::Accept:
				if (!buttons.empty()) {
					Activate(buttons.front());
				}
				break;
			default: break;
		}
	} else {
		const int count = static_cast<int>(buttons.size());
		switch (action) {
			case A::Left: m_button_focus = std::max(0, m_button_focus - 1); break;
			case A::Right: m_button_focus = std::min(count - 1, m_button_focus + 1); break;
			case A::Up:
			case A::Back: m_zone = Zone::Tiles; break;
			case A::Accept:
				if (m_button_focus < count) {
					Activate(buttons[m_button_focus]);
				}
				break;
			default: break;
		}
	}
	update();
}

void PsHomeView::mousePressEvent(QMouseEvent* event) {
	setFocus();
	m_keyboard_focus = false;
	const auto position = event->position();
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	if (m_options_open) {
		const auto panel = OptionsPanelRect();
		if (!panel.contains(position)) {
			CloseOptions();
			return;
		}
		for (int i = 0; i < static_cast<int>(m_options.size()); i++) {
			if (OptionRect(i).contains(position) && m_options[i].enabled) {
				m_option_focus = i;
				Navigate(GamepadNavigator::Action::Accept);
				m_keyboard_focus = false;
				return;
			}
		}
		return;
	}
	if (const auto edges = ResizeEdges(position); edges && window()->windowHandle() != nullptr) {
		window()->windowHandle()->startSystemResize(edges);
		return;
	}
	for (const auto tab: {Tab::Games, Tab::Library}) {
		if (TabRect(tab).adjusted(-8, -6, 8, 6).contains(position)) {
			SetTab(tab);
			return;
		}
	}
	for (const auto button: VisibleButtons()) {
		if (ButtonRect(button).contains(position)) {
			m_zone = Zone::Buttons;
			const auto buttons = VisibleButtons();
			m_button_focus =
			    static_cast<int>(std::find(buttons.begin(), buttons.end(), button) - buttons.begin());
			Activate(button);
			update();
			return;
		}
	}
	if (const int index = TileAt(position); index >= 0) {
		m_zone = Zone::Tiles;
		if (index == m_index && m_tiles[index].library_tile) {
			SetTab(Tab::Library);
		} else if (index == m_index && m_tab == Tab::Library) {
			OpenInGames();
		} else {
			Select(index, true);
		}
		return;
	}
	// The empty top of the view moves a frameless window.
	if (m_frameless && position.y() < (TopBarY + 18.0f) * ViewScale(*this) &&
	    window()->windowHandle() != nullptr) {
		window()->windowHandle()->startSystemMove();
		return;
	}
	QWidget::mousePressEvent(event);
}

void PsHomeView::mouseMoveEvent(QMouseEvent* event) {
	const auto position = event->position();
	const auto edges    = ResizeEdges(position);
	if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge)) {
		setCursor(Qt::SizeFDiagCursor);
	} else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
		setCursor(Qt::SizeBDiagCursor);
	} else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
		setCursor(Qt::SizeHorCursor);
	} else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
		setCursor(Qt::SizeVerCursor);
	} else {
		unsetCursor();
	}
	int hover = -1;
	if (!m_options_open) {
		const auto buttons = VisibleButtons();
		for (int i = 0; i < static_cast<int>(buttons.size()); i++) {
			if (ButtonRect(buttons[i]).contains(position)) {
				hover = i;
			}
		}
	} else {
		for (int i = 0; i < static_cast<int>(m_options.size()); i++) {
			if (OptionRect(i).contains(position) && m_options[i].enabled && m_option_focus != i) {
				m_option_focus = i;
				update();
			}
		}
	}
	if (hover != m_hover_button) {
		m_hover_button = hover;
		update();
	}
	QWidget::mouseMoveEvent(event);
}

void PsHomeView::leaveEvent(QEvent* event) {
	m_hover_button = -1;
	update();
	QWidget::leaveEvent(event);
}

void PsHomeView::mouseDoubleClickEvent(QMouseEvent* event) {
	const auto position = event->position();
	if (const int index = TileAt(position); index >= 0 && index == m_index && !m_options_open) {
		if (m_tab == Tab::Library) {
			OpenInGames();
			return;
		}
		const auto buttons = VisibleButtons();
		if (!buttons.empty()) {
			Activate(buttons.front());
		}
		return;
	}
	if (m_frameless && position.y() < (TopBarY + 18.0f) * ViewScale(*this)) {
		emit MaximizeRequested();
		return;
	}
	QWidget::mouseDoubleClickEvent(event);
}

void PsHomeView::wheelEvent(QWheelEvent* event) {
	if (m_options_open) {
		event->accept();
		return;
	}
	const auto delta = event->angleDelta();
	const int  steps = (delta.y() != 0 ? -delta.y() : delta.x()) / 120;
	if (steps != 0) {
		Select(m_index + steps * (m_tab == Tab::Library ? LibraryColumns() : 1), true);
	}
	event->accept();
}

void PsHomeView::keyPressEvent(QKeyEvent* event) {
	using A = GamepadNavigator::Action;
	switch (event->key()) {
		case Qt::Key_Left: Navigate(A::Left); return;
		case Qt::Key_Right: Navigate(A::Right); return;
		case Qt::Key_Up: Navigate(A::Up); return;
		case Qt::Key_Down: Navigate(A::Down); return;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Navigate(A::Accept); return;
		case Qt::Key_Escape:
		case Qt::Key_Backspace: Navigate(A::Back); return;
		case Qt::Key_Menu:
		case Qt::Key_F10: Navigate(A::Options); return;
		case Qt::Key_PageUp:
		case Qt::Key_Q: Navigate(A::PreviousTab); return;
		case Qt::Key_PageDown:
		case Qt::Key_E: Navigate(A::NextTab); return;
		case Qt::Key_Slash: Navigate(A::Search); return;
		default: QWidget::keyPressEvent(event);
	}
}

void PsHomeView::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);
	if (m_page != nullptr) {
		m_page->resize(size());
	}
	m_scroll_animation->stop();
	if (m_tab == Tab::Library) {
		m_scroll = 0.0f;
	}
	m_scroll = ScrollTarget();
	LayoutWidgets();
}

// Painting.

void PsHomeView::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	const float  s = ViewScale(*this);
	const QRectF area(rect());

	// Background: the selected game's art, fading in over the one before; without art the
	// console's deep blue.
	// Under it all, the theme: its gradient, and its picture for a custom one.
	const auto      theme = PsTheme::Current();
	QLinearGradient background(0, 0, area.width(), area.height());
	background.setColorAt(0.0, theme.top);
	background.setColorAt(1.0, theme.bottom);
	painter.fillRect(area, background);
	if (const auto picture = theme.Picture(); !picture.isNull()) {
		const auto scaled =
		    picture.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		painter.drawPixmap((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled);
	}
	const auto draw_hero = [&](const QPixmap& hero, float opacity) {
		if (hero.isNull() || opacity <= 0.0f) {
			return;
		}
		const auto scaled =
		    hero.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		painter.setOpacity(opacity);
		painter.drawPixmap((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled);
		painter.setOpacity(1.0);
	};
	// Toward no art (the welcome hub), the art before fades out instead.
	draw_hero(m_previous_hero, m_hero.isNull() ? 1.0f - m_hero_progress : 1.0f);
	draw_hero(m_hero, m_hero_progress);
	QLinearGradient vertical(0, 0, 0, area.height());
	vertical.setColorAt(0.0, QColor(0, 0, 0, 130));
	vertical.setColorAt(0.45, QColor(0, 0, 0, 60));
	vertical.setColorAt(1.0, QColor(0, 0, 0, 215));
	painter.fillRect(area, vertical);
	QLinearGradient horizontal(0, 0, area.width(), 0);
	horizontal.setColorAt(0.0, QColor(0, 0, 0, 170));
	horizontal.setColorAt(0.65, QColor(0, 0, 0, 0));
	painter.fillRect(area, horizontal);
	if (m_tab == Tab::Library) {
		// The library over the theme, the art all but gone.
		auto tint = theme.bottom;
		tint.setAlpha(static_cast<int>(225.0f * m_tab_progress));
		painter.fillRect(area, tint);
	}

	// Top bar: the tabs on the left, the active one underlined; the user and clock on the right.
	for (const auto tab: {Tab::Games, Tab::Library}) {
		const auto rect   = TabRect(tab);
		const bool active = tab == m_tab;
		painter.setPen(active ? QColor(Qt::white) : QColor(255, 255, 255, 120));
		painter.setFont(ScaledFont(*this, 20.0f, active ? QFont::DemiBold : QFont::Normal));
		painter.drawText(QPointF(rect.left(), TopBarY * s),
		                 tab == Tab::Games ? tr("Games") : tr("Library"));
		if (active) {
			painter.fillRect(QRectF(rect.left(), (TopBarY + 9.0f) * s, rect.width(), 2.5f * s),
			                 Qt::white);
		}
	}
	const auto bar = TopBarGeometry(*this, m_user_name);
	painter.setFont(ScaledFont(*this, 17.0f));
	painter.setPen(Qt::white);
	painter.drawText(QPointF(bar.clock_x, TopBarY * s),
	                 QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
	if (!m_user_name.isEmpty()) {
		painter.setPen(QColor(255, 255, 255, 190));
		painter.drawText(QPointF(bar.name_x, TopBarY * s), m_user_name);
	}

	if (m_tiles.empty()) {
		painter.setPen(QColor(255, 255, 255, 200));
		painter.setFont(ScaledFont(*this, 20.0f));
		painter.drawText(area.adjusted(MarginX * s, 0, -MarginX * s, 0), Qt::AlignCenter,
		                 tr("No games yet. Add a game folder in the global settings."));
	} else if (m_tab == Tab::Games) {
		PaintGames(painter, m_tab_progress);
	} else {
		PaintLibrary(painter, m_tab_progress);
	}
	PaintFocusRing(painter);
	PaintOptions(painter);
	// Behind a page, the home screen dims as it slides in.
	if (m_page_progress > 0.0f) {
		painter.fillRect(rect(), QColor(0, 0, 0, static_cast<int>(170.0f * m_page_progress)));
	}
}

void PsHomeView::PaintGames(QPainter& painter, float opacity) {
	const float s      = ViewScale(*this);
	const float radius = 14.0f * s;
	const float rise   = (1.0f - opacity) * 20.0f * s;
	painter.save();
	painter.setOpacity(opacity);
	painter.translate(0, rise);
	for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
		const auto  tile = TileRect(i);
		const auto& data = m_tiles[i];
		if (tile.right() < 0 || tile.left() > width()) {
			continue;
		}
		if (data.library_tile || data.welcome_tile) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(255, 255, 255, data.welcome_tile ? 30 : 38));
			painter.drawRoundedRect(tile, radius, radius);
			const int size = static_cast<int>(tile.width() * 0.46);
			PsGlyphIcon(data.welcome_tile ? PsGlyph::Shapes : PsGlyph::Grid, size, 0.06f)
			    .paint(&painter, QRect(static_cast<int>(tile.center().x()) - size / 2,
			                           static_cast<int>(tile.center().y()) - size / 2, size, size));
		} else {
			QPainterPath path;
			path.addRoundedRect(tile, radius, radius);
			painter.save();
			painter.setClipPath(path);
			if (data.icon.isNull()) {
				painter.fillRect(tile, QColor(40, 48, 66));
				painter.setPen(QColor(255, 255, 255, 160));
				painter.setFont(ScaledFont(*this, 12.0f));
				painter.drawText(tile.adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap,
				                 data.game.title);
			} else {
				painter.setOpacity(opacity * (i == m_index ? 1.0f : 0.82f));
				painter.drawPixmap(tile.toRect(), data.icon);
			}
			painter.restore();
		}
	}
	// The selected tile's name beside it, under the small tiles, as the console shows it.
	if (const auto* current = Current(); current != nullptr) {
		const auto  selected = TileRect(m_index);
		float       x        = static_cast<float>(selected.right()) + 18.0f * s;
		const float baseline = (TileRowY + TileSize + 30.0f) * s;
		painter.setOpacity(opacity * m_select_progress);
		if (IsGame(*current)) {
			// A badge for the platform, as the console's "PS5".
			const QFont         badge_font = ScaledFont(*this, 12.0f, QFont::Black);
			const QFontMetricsF badge_metrics(badge_font);
			const QRectF badge(x, baseline - 18.0f * s,
			                   badge_metrics.horizontalAdvance(QStringLiteral("PS5")) + 14.0f * s,
			                   22.0f * s);
			painter.setPen(Qt::NoPen);
			painter.setBrush(Qt::white);
			painter.drawRoundedRect(badge, 3.0f * s, 3.0f * s);
			painter.setPen(QColor(11, 13, 18));
			painter.setFont(badge_font);
			painter.drawText(badge, Qt::AlignCenter, QStringLiteral("PS5"));
			x = static_cast<float>(badge.right()) + 10.0f * s;
		}
		painter.setPen(Qt::white);
		painter.setFont(ScaledFont(*this, 21.0f));
		painter.drawText(QPointF(x, baseline),
		                 QFontMetricsF(painter.font())
		                     .elidedText(current->game.title, Qt::ElideRight,
		                                 static_cast<float>(width()) - x - MarginX * s));
	}
	painter.restore();
	if (const auto* current = Current(); current != nullptr && current->welcome_tile) {
		PaintWelcome(painter, opacity);
	} else {
		PaintDetails(painter, QRectF(rect()), opacity);
	}
}

std::pair<QRectF, float> PsHomeView::GlideTo(Glide& glide, int key, const QRectF& target, float radius) {
	if (key != glide.key) {
		const bool first = glide.key < 0;
		glide.key        = key;
		glide.from         = first ? target : glide.shown;
		glide.from_radius  = first ? radius : glide.shown_radius;
		glide.progress     = first ? 1.0f : 0.0f;
		if (!first) {
			glide.animation->stop();
			glide.animation->start();
		}
	}
	const float t   = glide.progress;
	glide.shown     = QRectF(Lerp(static_cast<float>(glide.from.x()), static_cast<float>(target.x()), t),
	                         Lerp(static_cast<float>(glide.from.y()), static_cast<float>(target.y()), t),
	                         Lerp(static_cast<float>(glide.from.width()), static_cast<float>(target.width()), t),
	                         Lerp(static_cast<float>(glide.from.height()), static_cast<float>(target.height()), t));
	glide.shown_radius = Lerp(glide.from_radius, radius, t);
	return {glide.shown, glide.shown_radius};
}

// The focus frame: around the selected tile, or (from the keyboard or a pad) around the focused
// button, gliding between them; with a button framed, the tile keeps a fainter frame of its own.
void PsHomeView::PaintFocusRing(QPainter& painter) {
	if (m_index < 0 || m_index >= static_cast<int>(m_tiles.size())) {
		return;
	}
	const float s       = ViewScale(*this);
	const bool  library = m_tab == Tab::Library;
	painter.save();
	painter.setOpacity(m_tab_progress);
	painter.translate(0, (1.0f - m_tab_progress) * (library ? 24.0f : 20.0f) * s);
	painter.setBrush(Qt::NoBrush);

	auto        tile        = TileRect(m_index);
	const float tile_radius = (library ? 10.0f : 14.0f) * s;
	if (library) {
		const float grow = 0.06f * m_select_progress * static_cast<float>(tile.width()) / 2.0f;
		tile             = tile.adjusted(-grow, -grow, grow, grow);
	}
	const auto tile_ring = tile.adjusted(-4.0f * s, -4.0f * s, 4.0f * s, 4.0f * s);
	const auto buttons   = VisibleButtons();
	const bool on_button = m_zone == Zone::Buttons && m_keyboard_focus && m_button_focus >= 0 &&
	                       m_button_focus < static_cast<int>(buttons.size());
	if (m_zone == Zone::Buttons) {
		// The tile the buttons belong to.
		painter.setPen(QPen(QColor(255, 255, 255, 150), 3.0f * s));
		painter.drawRoundedRect(tile_ring, tile_radius + 4.0f * s, tile_radius + 4.0f * s);
	}
	if (m_zone == Zone::Buttons && !on_button) {
		painter.restore();
		return;
	}
	QRectF target;
	float  radius = 0.0f;
	int    key    = 0;
	float  pen    = 3.0f * s;
	if (on_button) {
		target = ButtonRect(buttons[m_button_focus]).adjusted(-5.0f * s, -5.0f * s, 5.0f * s, 5.0f * s);
		radius = static_cast<float>(target.height()) / 2.0f;
		key    = 1000000 + m_button_focus;
		pen    = 2.0f * s;
	} else {
		target = tile_ring;
		radius = tile_radius + 4.0f * s;
		key    = (library ? 100000 : 0) + m_index;
	}
	if (library) {
		painter.setClipRect(QRectF(0, (LibraryTop - 10.0f) * s, width(), height()));
	}
	const auto [frame, corner] = GlideTo(m_ring, key, target, radius);
	painter.setPen(QPen(Qt::white, pen));
	painter.drawRoundedRect(frame, corner, corner);
	painter.restore();
}

// The welcome hub: a greeting and cards, as the console's first tile shows.
void PsHomeView::PaintWelcome(QPainter& painter, float opacity) {
	const float s     = ViewScale(*this);
	const float alpha = opacity * m_select_progress;
	const float slide = (1.0f - m_select_progress) * 16.0f * s;
	const float top   = DetailsOrigin(*this).y() - 40.0f * s + slide;
	painter.save();
	painter.setOpacity(alpha);
	painter.setPen(Qt::white);
	painter.setFont(ScaledFont(*this, 30.0f, QFont::Light));
	painter.drawText(QPointF(MarginX * s, top),
	                 m_user_name.isEmpty() ? tr("Welcome back")
	                                       : tr("Welcome back, %1").arg(m_user_name));

	const float gap        = 18.0f * s;
	const float card_top   = top + 28.0f * s;
	const float card_h     = std::max(150.0f * s, static_cast<float>(height()) - card_top - 40.0f * s);
	const float card_w     = (static_cast<float>(width()) - 2.0f * MarginX * s - 2.0f * gap) / 3.0f;
	const auto  card_rect  = [&](int i) {
        return QRectF(MarginX * s + static_cast<float>(i) * (card_w + gap), card_top, card_w,
                      std::min(card_h, 230.0f * s));
	};
	const auto card = [&](int i, PsGlyph glyph, const QString& title) {
		const auto rect = card_rect(i);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(18, 21, 30, 215));
		painter.drawRoundedRect(rect, 14.0f * s, 14.0f * s);
		const int icon = static_cast<int>(22.0f * s);
		PsGlyphIcon(glyph, icon).paint(&painter, QRect(static_cast<int>(rect.left() + 20.0f * s),
		                                              static_cast<int>(rect.top() + 18.0f * s),
		                                              icon, icon));
		painter.setPen(Qt::white);
		painter.setFont(ScaledFont(*this, 16.0f, QFont::DemiBold));
		painter.drawText(QPointF(rect.left() + 20.0f * s + icon + 10.0f * s, rect.top() + 36.0f * s),
		                 title);
		return rect;
	};

	// Trophies: earned of every grade, over every game.
	{
		std::array<int, 4> earned {}, total {};
		for (const auto& game: m_games) {
			for (int g = 0; g < 4; g++) {
				earned[g] += game.trophies_earned[g];
				total[g] += game.trophies_total[g];
			}
		}
		const auto rect = card(0, PsGlyph::Trophy, tr("Trophies"));
		painter.setPen(QColor(255, 255, 255, 170));
		painter.setFont(ScaledFont(*this, 14.0f));
		const int sum = earned[0] + earned[1] + earned[2] + earned[3];
		painter.drawText(QRectF(rect.left(), rect.top() + 18.0f * s, rect.width() - 20.0f * s, 24.0f * s),
		                 Qt::AlignRight | Qt::AlignVCenter, tr("Total: %1").arg(sum));
		constexpr std::array<QRgb, 4> Colors {qRgb(161, 225, 245), qRgb(255, 207, 82),
		                                      qRgb(211, 220, 232), qRgb(211, 142, 91)};
		const float column = (rect.width() - 40.0f * s) / 4.0f;
		for (int g = 0; g < 4; g++) {
			const QPointF center(rect.left() + 20.0f * s + column * (static_cast<float>(g) + 0.5f),
			                     rect.top() + 92.0f * s);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(Colors[g]));
			painter.drawEllipse(center, 16.0f * s, 16.0f * s);
			painter.setPen(Qt::white);
			painter.setFont(ScaledFont(*this, 17.0f, QFont::DemiBold));
			painter.drawText(QRectF(center.x() - column / 2, center.y() + 24.0f * s, column, 24.0f * s),
			                 Qt::AlignCenter, QString::number(earned[g]));
			painter.setPen(QColor(255, 255, 255, 130));
			painter.setFont(ScaledFont(*this, 12.0f));
			painter.drawText(QRectF(center.x() - column / 2, center.y() + 48.0f * s, column, 20.0f * s),
			                 Qt::AlignCenter, tr("of %1").arg(total[g]));
		}
	}
	// Storage: the games' drive.
	{
		const auto rect = card(1, PsGlyph::Storage, tr("Storage"));
		const auto gib  = [](qint64 bytes) {
            return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1);
		};
		painter.setPen(QColor(255, 255, 255, 170));
		painter.setFont(ScaledFont(*this, 14.0f));
		painter.drawText(QPointF(rect.left() + 20.0f * s, rect.top() + 84.0f * s),
		                 tr("Free space on %1").arg(m_storage_name));
		painter.setPen(Qt::white);
		painter.setFont(ScaledFont(*this, 16.0f, QFont::DemiBold));
		painter.drawText(QRectF(rect.left(), rect.top() + 66.0f * s, rect.width() - 20.0f * s, 24.0f * s),
		                 Qt::AlignRight | Qt::AlignVCenter,
		                 m_storage_total > 0 ? tr("%1 GB").arg(gib(m_storage_free)) : tr("Unknown"));
		const QRectF bar(rect.left() + 20.0f * s, rect.top() + 104.0f * s, rect.width() - 40.0f * s,
		                 8.0f * s);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(255, 255, 255, 40));
		painter.drawRoundedRect(bar, 4.0f * s, 4.0f * s);
		if (m_storage_total > 0) {
			const double used = 1.0 - static_cast<double>(m_storage_free) /
			                              static_cast<double>(m_storage_total);
			painter.setBrush(PsTheme::Current().accent);
			painter.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * used, bar.height()),
			                        4.0f * s, 4.0f * s);
			painter.setPen(QColor(255, 255, 255, 130));
			painter.setFont(ScaledFont(*this, 12.0f));
			painter.drawText(QPointF(bar.left(), bar.bottom() + 22.0f * s),
			                 tr("%1 GB of %2 GB used")
			                     .arg(gib(m_storage_total - m_storage_free), gib(m_storage_total)));
		}
	}
	// Recently played.
	{
		const auto  rect = card(2, PsGlyph::Clock, tr("Recently played"));
		const Game* last = nullptr;
		for (const auto& game: m_games) {
			if (game.last_played > 0 && (last == nullptr || game.last_played > last->last_played)) {
				last = &game;
			}
		}
		painter.setPen(Qt::white);
		painter.setFont(ScaledFont(*this, 17.0f, QFont::DemiBold));
		const QRectF text(rect.left() + 20.0f * s, rect.top() + 62.0f * s, rect.width() - 40.0f * s,
		                  26.0f * s);
		painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
		                 last != nullptr ? QFontMetricsF(painter.font())
		                                       .elidedText(last->title, Qt::ElideRight, text.width())
		                                 : tr("Nothing yet"));
		painter.setPen(QColor(255, 255, 255, 150));
		painter.setFont(ScaledFont(*this, 13.0f));
		painter.drawText(QPointF(text.left(), text.bottom() + 20.0f * s),
		                 last != nullptr ? QDateTime::fromSecsSinceEpoch(last->last_played)
		                                       .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
		                                 : tr("Start a game from the row above."));
		painter.drawText(QPointF(text.left(), rect.bottom() - 18.0f * s),
		                 tr("Q / E or L1 / R1: switch tabs"));
	}
	painter.restore();
}

void PsHomeView::PaintLibrary(QPainter& painter, float opacity) {
	const float s      = ViewScale(*this);
	const float radius = 10.0f * s;
	painter.save();
	painter.setOpacity(opacity);
	painter.translate(0, (1.0f - opacity) * 24.0f * s);
	// The heading and the count, under the tabs.
	painter.setPen(Qt::white);
	painter.setFont(ScaledFont(*this, 26.0f, QFont::Bold));
	painter.drawText(QPointF(MarginX * s, (TopBarY + 58.0f) * s), tr("Game Library"));
	painter.setPen(QColor(255, 255, 255, 150));
	painter.setFont(ScaledFont(*this, 14.0f));
	painter.drawText(QPointF(MarginX * s, (TopBarY + 82.0f) * s),
	                 tr("Installed  \u00B7  %n game(s)", nullptr, static_cast<int>(m_tiles.size())));
	painter.setClipRect(QRectF(0, (LibraryTop - 10.0f) * s, width(), height()));
	const QFont name_font     = ScaledFont(*this, 13.0f);
	const QFont selected_font = ScaledFont(*this, 13.0f, QFont::DemiBold);
	for (int i = 0; i < static_cast<int>(m_tiles.size()); i++) {
		auto rect = TileRect(i);
		if (rect.bottom() + LibraryLabel * s < 0 || rect.top() > height()) {
			continue;
		}
		const bool selected = i == m_index;
		if (selected) {
			// The selected icon grows a little, as the console's do.
			const float grow = 0.06f * m_select_progress * rect.width() / 2.0f;
			rect             = rect.adjusted(-grow, -grow, grow, grow);
		}
		QPainterPath path;
		path.addRoundedRect(rect, radius, radius);
		painter.save();
		painter.setClipPath(path, Qt::IntersectClip);
		if (m_tiles[i].icon.isNull()) {
			painter.fillRect(rect, QColor(40, 48, 66));
			painter.setPen(QColor(255, 255, 255, 170));
			painter.setFont(name_font);
			painter.drawText(rect.adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
			                 m_tiles[i].game.title);
		} else {
			painter.drawPixmap(rect.toRect(), m_tiles[i].icon);
		}
		painter.restore();
		painter.setPen(selected ? QColor(Qt::white) : QColor(255, 255, 255, 170));
		painter.setFont(selected ? selected_font : name_font);
		const QRectF label(rect.left() - 6.0f * s, rect.bottom() + 8.0f * s, rect.width() + 12.0f * s,
		                   (LibraryLabel - 10.0f) * s);
		painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop,
		                 QFontMetricsF(painter.font())
		                     .elidedText(m_tiles[i].game.title, Qt::ElideRight, label.width()));
	}
	painter.restore();
}

void PsHomeView::PaintDetails(QPainter& painter, const QRectF& area, float opacity) {
	const auto* tile = Current();
	if (tile == nullptr) {
		return;
	}
	const float s      = ViewScale(*this);
	const auto  origin = DetailsOrigin(*this);
	const float slide  = (1.0f - m_select_progress) * 14.0f * s;
	const float alpha  = opacity * m_select_progress;
	painter.save();
	painter.setOpacity(alpha);

	// The logo, or the title written large where a logo would be.
	const float  logo_h = LogoHeight(*this);
	const QRectF logo_box(origin.x(), origin.y() + slide,
	                      std::min(area.width() * 0.38f, area.width() - origin.x() - MarginX * s),
	                      logo_h);
	const auto logo = IsGame(*tile) ? LogoOf(tile->game) : QPixmap();
	if (!logo.isNull()) {
		const auto scaled = logo.scaled(logo_box.size().toSize(), Qt::KeepAspectRatio,
		                                Qt::SmoothTransformation);
		painter.drawPixmap(QPointF(logo_box.left(), logo_box.bottom() - scaled.height()), scaled);
	} else {
		painter.setPen(Qt::white);
		auto font = ScaledFont(*this, 40.0f, QFont::Bold);
		painter.setFont(font);
		painter.drawText(logo_box, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap,
		                 tile->game.title);
	}
	painter.setPen(QColor(255, 255, 255, 170));
	painter.setFont(ScaledFont(*this, 15.0f));
	painter.drawText(QPointF(origin.x(), logo_box.bottom() + 30.0f * s), tile->game.subtitle);

	const auto buttons = VisibleButtons();
	for (int i = 0; i < static_cast<int>(buttons.size()); i++) {
		const auto button  = buttons[i];
		const auto rect    = ButtonRect(button);
		const bool hover   = i == m_hover_button;
		const bool focused = m_zone == Zone::Buttons && m_button_focus == i;
		const bool play    = button == Button::Play;
		const bool enabled = !play || m_play_enabled || tile->library_tile;
		QColor     fill    = play ? QColor(255, 255, 255, enabled ? 255 : 100)
		                          : QColor(255, 255, 255, hover || focused ? 75 : 42);
		if (play && hover && enabled) {
			fill = QColor(233, 237, 245);
		}
		painter.setPen(Qt::NoPen);
		painter.setBrush(fill);
		painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
		if (button == Button::Options) {
			const int size = static_cast<int>(rect.height() / 2);
			PsGlyphIcon(PsGlyph::More, size)
			    .paint(&painter, QRect(static_cast<int>(rect.center().x()) - size / 2,
			                           static_cast<int>(rect.center().y()) - size / 2, size, size));
			continue;
		}
		painter.setPen(play ? QColor(11, 13, 18, enabled ? 255 : 140) : QColor(Qt::white));
		painter.setFont(ScaledFont(*this, play ? 17.0f : 15.0f, play ? QFont::Bold : QFont::Normal));
		const auto label = play ? (tile->library_tile ? tr("Open") : tr("Play"))
		                   : button == Button::Settings ? tr("Settings")
		                                                : tr("Trophies");
		painter.drawText(rect, Qt::AlignCenter, label);
	}
	painter.restore();
}

void PsHomeView::PaintOptions(QPainter& painter) {
	if (m_options_progress <= 0.0f) {
		return;
	}
	const float s     = ViewScale(*this);
	const auto  panel = OptionsPanelRect();
	painter.save();
	painter.fillRect(rect(), QColor(0, 0, 0, static_cast<int>(120.0f * m_options_progress)));
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(22, 24, 31, 245));
	painter.drawRect(panel);
	painter.setOpacity(m_options_progress);
	painter.setPen(QColor(255, 255, 255, 150));
	painter.setFont(ScaledFont(*this, 14.0f));
	painter.drawText(QPointF(panel.left() + 32.0f * s, 76.0f * s), tr("Options"));
	painter.setPen(Qt::white);
	painter.setFont(ScaledFont(*this, 20.0f, QFont::DemiBold));
	painter.drawText(QRectF(panel.left() + 32.0f * s, 82.0f * s, panel.width() - 64.0f * s, 30.0f * s),
	                 Qt::AlignLeft | Qt::AlignVCenter,
	                 QFontMetricsF(painter.font())
	                     .elidedText(m_options_title, Qt::ElideRight, panel.width() - 64.0f * s));
	painter.setFont(ScaledFont(*this, 16.0f));
	if (m_option_focus >= 0 && m_option_focus < static_cast<int>(m_options.size())) {
		const auto [highlight, corner] =
		    GlideTo(m_option_glide, m_option_focus, OptionRect(m_option_focus), 8.0f * s);
		painter.setPen(Qt::NoPen);
		painter.setBrush(Qt::white);
		painter.drawRoundedRect(highlight, corner, corner);
	}
	for (int i = 0; i < static_cast<int>(m_options.size()); i++) {
		const auto rect    = OptionRect(i);
		const bool focused = i == m_option_focus;
		painter.setPen(focused                  ? QColor(11, 13, 18)
		               : m_options[i].enabled ? QColor(Qt::white)
		                                      : QColor(255, 255, 255, 80));
		painter.drawText(rect.adjusted(16.0f * s, 0, -16.0f * s, 0), Qt::AlignLeft | Qt::AlignVCenter,
		                 m_options[i].text);
	}
	painter.restore();
}
