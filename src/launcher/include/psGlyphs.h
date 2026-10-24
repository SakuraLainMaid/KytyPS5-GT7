#ifndef PS_GLYPHS_H
#define PS_GLYPHS_H

#include <QIcon>

// White outline icons in the console's style, drawn rather than shipped, for the home view and
// the settings.
enum class PsGlyph {
	Search,
	Refresh,
	Gamepad,
	Gear,
	List,
	More,
	Grid,
	Minimize,
	Maximize,
	Restore,
	Close,
	Shapes, // the four face-button shapes, the welcome hub's tile
	Trophy,
	Storage,
	Clock,
	User,
	Display,
	Console,
	Gauge,
	Speaker,
	Bell,
	Puzzle,
	Bug,
	Folder,
	Image,
};

// `stroke` is the line width as a fraction of `size`.
[[nodiscard]] QIcon PsGlyphIcon(PsGlyph glyph, int size, float stroke = 0.075f);

#endif // PS_GLYPHS_H
