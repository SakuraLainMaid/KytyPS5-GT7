#ifndef GAMEPAD_NAVIGATOR_H
#define GAMEPAD_NAVIGATOR_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <SDL3/SDL_gamepad.h>

// Gamepads (XInput, DirectInput, DualSense and others SDL knows) as navigation input for the
// launcher's home view: the d-pad and the left stick move, with key-repeat; the face, shoulder
// and menu buttons act. It reads the pads' state rather than SDL's event queue, which the
// controller lightbar preview drains, and only while `window` is the active window, so a game
// started from the launcher does not also drive it.
class GamepadNavigator final: public QObject {
	Q_OBJECT

public:
	enum class Action { Up, Down, Left, Right, Accept, Back, Options, PreviousTab, NextTab, Search };
	Q_ENUM(Action)

	explicit GamepadNavigator(QWidget* window, QObject* parent = nullptr);
	~GamepadNavigator() override;

signals:
	void Triggered(GamepadNavigator::Action action);

private:
	void Poll();
	void RefreshDevices();

	QWidget*                            m_window = nullptr;
	QTimer                              m_timer;
	QElapsedTimer                       m_since_refresh;
	QHash<SDL_JoystickID, SDL_Gamepad*> m_gamepads;
	bool                                m_initialized = false;
	// The buttons held at the last poll, and when each direction repeats next.
	uint32_t                            m_held = 0;
	qint64                              m_repeat_at[4] {};
	QElapsedTimer                       m_clock;
};

#endif // GAMEPAD_NAVIGATOR_H
