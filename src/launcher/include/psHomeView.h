#ifndef PS_HOME_VIEW_H
#define PS_HOME_VIEW_H

#include "gamepadNavigator.h"

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <array>
#include <utility>
#include <vector>

class ConfigurationItem;
class QAction;
class QDialog;
class QLineEdit;
class QTimer;
class QToolButton;
class QVariantAnimation;

// The game list as a console-style home screen, after the PS5's: over the selected game's art,
// a Games tab with the recently played games in a row (and a tile to the library) and a Library
// tab with every game in a grid of icons; the selected game's title and Play,
// Settings, Trophies and options buttons; an options panel that slides in from the right; the
// launcher's tools, the user's avatar, the clock and (frameless) the window's buttons in the top
// bar. Mouse, keyboard and gamepads drive it. It shows the items of the list widget's tree and
// selects, runs and edits them through it (ConfigurationListWidget).
class PsHomeView final: public QWidget {
	Q_OBJECT

public:
	struct Game {
		ConfigurationItem* item = nullptr;
		QString            title;
		QString            subtitle;
		QString            basedir;
		bool               has_trophies = false;
		qint64             last_played  = 0; // seconds since 1970; 0: never
		QString            logo;             // a picture file chosen for the game, or none
		// Trophies earned and in all, by grade: platinum, gold, silver, bronze.
		std::array<int, 4> trophies_earned {};
		std::array<int, 4> trophies_total {};
	};

	explicit PsHomeView(QWidget* parent = nullptr);

	void SetGames(std::vector<Game> games, const ConfigurationItem* selected);
	void SetSelected(const ConfigurationItem* item);
	void SetUserName(const QString& name);
	void SetPlayEnabled(bool enabled);
	// The window has no frame: the top bar shows its buttons, and the view moves and resizes it.
	void SetFrameless(bool frameless);
	// The options panel, with these actions (a game's menu); the view triggers the one chosen.
	void ShowOptions(const QString& title, const QList<QAction*>& actions);
	// `dialog` as a page of the view: it slides in over the home screen, runs until it is done,
	// and slides out; returns its result. The dialog stays the caller's.
	int RunPage(QDialog* dialog);

signals:
	void SelectionChanged(ConfigurationItem* item);
	void PlayRequested();
	void SettingsRequested();
	void TrophiesRequested();
	void OptionsRequested();
	void SearchTextChanged(const QString& text);
	void RefreshRequested();
	void InputMappingRequested();
	void GlobalSettingsRequested();
	void ListViewRequested();
	void MinimizeRequested();
	void MaximizeRequested();
	void CloseRequested();

protected:
	void paintEvent(QPaintEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void leaveEvent(QEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;

private:
	enum class Tab { Games, Library };
	// Where the keyboard or gamepad focus is: the tiles (or covers), or the buttons below them.
	enum class Zone { Tiles, Buttons };
	enum class Button { Play, Settings, Trophies, Options };

	struct Tile {
		Game    game;
		QPixmap icon;
		bool    library_tile = false; // the Games tab's last tile, to the library
		bool    welcome_tile = false; // the Games tab's first tile, the welcome hub
	};
	struct Option {
		QPointer<QAction> action;
		QString           text;
		bool              enabled = true;
	};
	// A frame that glides from what had the focus to what has it now: from where it was drawn
	// last to the new target, which may itself still be moving (a tile growing, the row scrolling).
	struct Glide {
		int                key = -1; // what it frames; -1: nothing yet
		QRectF             from;
		float              from_radius = 0.0f;
		QRectF             shown;
		float              shown_radius = 0.0f;
		float              progress     = 1.0f;
		QVariantAnimation* animation    = nullptr;
	};
	// The frame for `key` at `target` (with corners of `radius`), as far as it has glided there.
	std::pair<QRectF, float> GlideTo(Glide& glide, int key, const QRectF& target, float radius);
	void   PaintFocusRing(QPainter& painter);

	void   RebuildTiles();
	void   Select(int index, bool notify);
	void   SetTab(Tab tab);
	void   LoadHero();
	void   LayoutWidgets();
	void   ShowSearch(bool shown);
	void   ShowAvatarMenu();
	void   UpdateAvatar();
	void   CloseOptions();
	void   OpenInGames();
	[[nodiscard]] float ScrollTarget() const;
	[[nodiscard]] int   LibraryColumns() const;
	void   Navigate(GamepadNavigator::Action action);
	void   Activate(Button button);
	[[nodiscard]] std::vector<Button> VisibleButtons() const;
	[[nodiscard]] QRectF  ButtonRect(Button button) const;
	[[nodiscard]] QRectF  TileRect(int index) const;
	[[nodiscard]] QRectF  OptionsPanelRect() const;
	[[nodiscard]] QRectF  OptionRect(int index) const;
	[[nodiscard]] QRectF  TabRect(Tab tab) const;
	[[nodiscard]] int     TileAt(const QPointF& position) const;
	[[nodiscard]] Qt::Edges ResizeEdges(const QPointF& position) const;
	[[nodiscard]] const Tile* Current() const;
	void   PaintGames(QPainter& painter, float opacity);
	void   PaintLibrary(QPainter& painter, float opacity);
	void   PaintDetails(QPainter& painter, const QRectF& area, float opacity);
	void   PaintOptions(QPainter& painter);
	void   PaintWelcome(QPainter& painter, float opacity);
	[[nodiscard]] static bool IsGame(const Tile& tile);
	[[nodiscard]] QPixmap     LogoOf(const Game& game);

	std::vector<Game>  m_games;   // every game, in the tree's order
	std::vector<Tile>  m_tiles;   // the current tab's tiles
	Tab                m_tab   = Tab::Games;
	Zone               m_zone  = Zone::Tiles;
	int                m_index = -1;
	int                m_previous_index = -1;
	int                m_button_focus   = 0;
	int                m_hover_button   = -1;
	bool               m_keyboard_focus = false; // draw the focus ring (after keys or a pad)
	const ConfigurationItem* m_wanted = nullptr; // the selection to keep across rebuilds

	// Animations, 0 to 1: the selection (tile growth, details sliding in), the background's
	// cross-fade, the tab change and the options panel.
	QVariantAnimation* m_select_animation  = nullptr;
	QVariantAnimation* m_hero_animation    = nullptr;
	QVariantAnimation* m_tab_animation     = nullptr;
	QVariantAnimation* m_options_animation = nullptr;
	QVariantAnimation* m_scroll_animation  = nullptr;
	float              m_select_progress  = 1.0f;
	float              m_hero_progress    = 1.0f;
	float              m_tab_progress     = 1.0f;
	float              m_options_progress = 0.0f;
	float              m_scroll           = 0.0f;

	QPixmap            m_hero;
	QPixmap            m_previous_hero;
	QString            m_hero_key;
	QString            m_user_name;
	QPixmap            m_avatar; // the user's picture, empty for the initial
	QHash<QString, QPixmap> m_logos;
	qint64             m_storage_free  = 0;
	qint64             m_storage_total = 0;
	QString            m_storage_name;
	bool               m_play_enabled = true;
	bool               m_frameless    = false;

	Glide               m_ring;        // the focus frame: a tile, or a button under it
	Glide               m_option_glide; // the options panel's highlight
	std::vector<Option> m_options;
	QString             m_options_title;
	bool                m_options_open  = false;
	int                 m_option_focus  = 0;

	std::vector<QToolButton*> m_tools;          // search, refresh, input mapping, settings, list
	std::vector<QToolButton*> m_window_buttons; // minimize, maximize, close
	QToolButton*              m_avatar_button = nullptr;
	QLineEdit*                m_search        = nullptr;
	QTimer*                   m_clock         = nullptr;
	GamepadNavigator*         m_gamepad       = nullptr;
	QPointer<QDialog>         m_page;               // the page shown over the view, if any
	float                     m_page_progress = 0.0f; // the page sliding in, 0 to 1
};

#endif // PS_HOME_VIEW_H
