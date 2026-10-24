#include "gamepadNavigator.h"

#include <QApplication>
#include <QDebug>
#include <QWidget>

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <array>
#include <utility>

namespace {

bool IsForeground([[maybe_unused]] const QWidget* window) {
#ifdef _WIN32
	DWORD process = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process);
	return process == GetCurrentProcessId();
#else
	return window->isActiveWindow();
#endif
}

constexpr int    PollMs        = 16;
constexpr qint64 RepeatDelayMs = 380;
constexpr qint64 RepeatRateMs  = 110;
constexpr int    StickDeadzone = 16000; // of 32767

// Bits of the combined state of every pad: the four directions first (they repeat).
enum Bit : uint32_t {
	Up          = 1u << 0,
	Down        = 1u << 1,
	Left        = 1u << 2,
	Right       = 1u << 3,
	Accept      = 1u << 4,
	Back        = 1u << 5,
	Options     = 1u << 6,
	PreviousTab = 1u << 7,
	NextTab     = 1u << 8,
	Search      = 1u << 9,
};

} // namespace

GamepadNavigator::GamepadNavigator(QWidget* window, QObject* parent)
    : QObject(parent), m_window(window) {
	m_initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	if (!m_initialized) {
		qWarning() << "Gamepad navigation unavailable:" << SDL_GetError();
		return;
	}
	m_clock.start();
	m_since_refresh.start();
	RefreshDevices();
	m_timer.setInterval(PollMs);
	connect(&m_timer, &QTimer::timeout, this, &GamepadNavigator::Poll);
	m_timer.start();
}

GamepadNavigator::~GamepadNavigator() {
	for (auto* gamepad: m_gamepads) {
		SDL_CloseGamepad(gamepad);
	}
	if (m_initialized) {
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	}
}

void GamepadNavigator::RefreshDevices() {
	int   count = 0;
	auto* ids   = SDL_GetGamepads(&count);
	QHash<SDL_JoystickID, SDL_Gamepad*> found;
	for (int i = 0; i < count; i++) {
		if (auto* open = m_gamepads.take(ids[i]); open != nullptr) {
			found.insert(ids[i], open);
		} else if (auto* gamepad = SDL_OpenGamepad(ids[i]); gamepad != nullptr) {
			found.insert(ids[i], gamepad);
		}
	}
	SDL_free(ids);
	for (auto* gone: m_gamepads) {
		SDL_CloseGamepad(gone);
	}
	m_gamepads = std::move(found);
}

void GamepadNavigator::Poll() {
	if (m_since_refresh.elapsed() > 1000) {
		RefreshDevices();
		m_since_refresh.restart();
	}
	SDL_UpdateGamepads();
	uint32_t state = 0;
	for (auto* gamepad: m_gamepads) {
		const auto button = [gamepad](SDL_GamepadButton b) {
			return SDL_GetGamepadButton(gamepad, b);
		};
		const auto x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
		const auto y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
		state |= (button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -StickDeadzone) ? Up : 0u;
		state |= (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > StickDeadzone) ? Down : 0u;
		state |= (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -StickDeadzone) ? Left : 0u;
		state |= (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > StickDeadzone) ? Right : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_SOUTH) ? Accept : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_EAST) ? Back : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_START) ? Options : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? PreviousTab : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? NextTab : 0u;
		state |= button(SDL_GAMEPAD_BUTTON_NORTH) ? Search : 0u;
	}
	// Only the launcher's own window takes the input (not a game it started, not another app).
	// A popup of the window (a list of choices) counts as the window.
	const auto* popup  = QApplication::activePopupWidget();
	const bool  active = m_window != nullptr &&
	                    IsForeground(m_window) &&
	                    (m_window->isActiveWindow() ||
	                     (popup != nullptr && popup->parentWidget() != nullptr &&
	                      popup->parentWidget()->window() == m_window->window()));
	const auto pressed = state & ~m_held;
	m_held             = state;
	if (!active) {
		return;
	}
	const auto now = m_clock.elapsed();
	constexpr std::array<Action, 4> Directions {Action::Up, Action::Down, Action::Left,
	                                            Action::Right};
	for (int i = 0; i < 4; i++) {
		const uint32_t bit = 1u << i;
		if ((pressed & bit) != 0) {
			emit Triggered(Directions[i]);
			m_repeat_at[i] = now + RepeatDelayMs;
		} else if ((state & bit) != 0 && now >= m_repeat_at[i]) {
			emit Triggered(Directions[i]);
			m_repeat_at[i] = now + RepeatRateMs;
		}
	}
	const std::array<std::pair<uint32_t, Action>, 6> buttons {{{Accept, Action::Accept},
	                                                           {Back, Action::Back},
	                                                           {Options, Action::Options},
	                                                           {PreviousTab, Action::PreviousTab},
	                                                           {NextTab, Action::NextTab},
	                                                           {Search, Action::Search}}};
	for (const auto& [bit, action]: buttons) {
		if ((pressed & bit) != 0) {
			emit Triggered(action);
		}
	}
}
