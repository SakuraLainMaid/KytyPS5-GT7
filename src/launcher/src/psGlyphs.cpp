#include "psGlyphs.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <numbers>

QIcon PsGlyphIcon(PsGlyph glyph, int size, float stroke) {
	constexpr int Oversample = 2;
	const int     px         = std::max(1, size) * Oversample;
	QPixmap       pixmap(px, px);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(Qt::white, px * stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.setBrush(Qt::NoBrush);
	const auto p = [px](double x, double y) { return QPointF(x * px, y * px); };
	const auto r = [px](double v) { return px * v; };
	switch (glyph) {
		case PsGlyph::Search:
			painter.drawEllipse(p(0.42, 0.42), r(0.24), r(0.24));
			painter.drawLine(p(0.60, 0.60), p(0.82, 0.82));
			break;
		case PsGlyph::Refresh: {
			painter.drawArc(QRectF(p(0.2, 0.2), p(0.8, 0.8)), 60 * 16, 290 * 16);
			const QPointF tip = p(0.65, 0.24);
			painter.drawLine(tip, tip + QPointF(r(0.16), r(0.02)));
			painter.drawLine(tip, tip + QPointF(r(0.03), r(0.16)));
			break;
		}
		case PsGlyph::Gamepad: {
			QPainterPath body;
			body.addRoundedRect(QRectF(p(0.12, 0.32), p(0.88, 0.72)), r(0.18), r(0.18));
			painter.drawPath(body);
			painter.drawLine(p(0.27, 0.52), p(0.41, 0.52));
			painter.drawLine(p(0.34, 0.45), p(0.34, 0.59));
			painter.setBrush(Qt::white);
			painter.drawEllipse(p(0.64, 0.47), r(0.025), r(0.025));
			painter.drawEllipse(p(0.72, 0.56), r(0.025), r(0.025));
			break;
		}
		case PsGlyph::Gear:
			painter.drawEllipse(p(0.5, 0.5), r(0.12), r(0.12));
			painter.drawEllipse(p(0.5, 0.5), r(0.25), r(0.25));
			for (int i = 0; i < 8; i++) {
				const double  angle = i * std::numbers::pi / 4.0;
				const QPointF dir(std::cos(angle), std::sin(angle));
				painter.drawLine(p(0.5, 0.5) + dir * r(0.25), p(0.5, 0.5) + dir * r(0.36));
			}
			break;
		case PsGlyph::List:
			for (const double y: {0.30, 0.50, 0.70}) {
				painter.drawLine(p(0.22, y), p(0.78, y));
			}
			break;
		case PsGlyph::More:
			painter.setBrush(Qt::white);
			for (const double x: {0.28, 0.50, 0.72}) {
				painter.drawEllipse(p(x, 0.5), r(0.05), r(0.05));
			}
			break;
		case PsGlyph::Grid:
			for (const double x: {0.22, 0.54}) {
				for (const double y: {0.22, 0.54}) {
					painter.drawRoundedRect(QRectF(p(x, y), QSizeF(r(0.24), r(0.24))), r(0.04), r(0.04));
				}
			}
			break;
		case PsGlyph::Minimize: painter.drawLine(p(0.3, 0.5), p(0.7, 0.5)); break;
		case PsGlyph::Maximize: painter.drawRect(QRectF(p(0.3, 0.3), p(0.7, 0.7))); break;
		case PsGlyph::Restore:
			painter.drawRect(QRectF(p(0.3, 0.38), p(0.62, 0.7)));
			painter.drawPolyline(QPolygonF(
			    {p(0.38, 0.38), p(0.38, 0.3), p(0.7, 0.3), p(0.7, 0.62), p(0.62, 0.62)}));
			break;
		case PsGlyph::Close:
			painter.drawLine(p(0.32, 0.32), p(0.68, 0.68));
			painter.drawLine(p(0.68, 0.32), p(0.32, 0.68));
			break;
		case PsGlyph::Shapes:
			painter.drawRect(QRectF(p(0.14, 0.14), p(0.42, 0.42)));
			painter.drawPolygon(QPolygonF({p(0.72, 0.13), p(0.87, 0.42), p(0.57, 0.42)}));
			painter.drawLine(p(0.16, 0.58), p(0.40, 0.84));
			painter.drawLine(p(0.40, 0.58), p(0.16, 0.84));
			painter.drawEllipse(p(0.72, 0.71), r(0.14), r(0.14));
			break;
		case PsGlyph::Trophy:
			painter.drawPolyline(QPolygonF({p(0.30, 0.18), p(0.30, 0.38)}));
			painter.drawArc(QRectF(p(0.30, 0.02), p(0.70, 0.58)), 180 * 16, 180 * 16);
			painter.drawLine(p(0.30, 0.18), p(0.70, 0.18));
			painter.drawLine(p(0.70, 0.18), p(0.70, 0.38));
			painter.drawArc(QRectF(p(0.14, 0.20), p(0.32, 0.40)), 90 * 16, 180 * 16);
			painter.drawArc(QRectF(p(0.68, 0.20), p(0.86, 0.40)), -90 * 16, 180 * 16);
			painter.drawLine(p(0.50, 0.58), p(0.50, 0.72));
			painter.drawLine(p(0.34, 0.80), p(0.66, 0.80));
			break;
		case PsGlyph::Storage:
			painter.drawEllipse(QRectF(p(0.22, 0.18), p(0.78, 0.34)));
			painter.drawLine(p(0.22, 0.26), p(0.22, 0.74));
			painter.drawLine(p(0.78, 0.26), p(0.78, 0.74));
			painter.drawArc(QRectF(p(0.22, 0.66), p(0.78, 0.82)), 180 * 16, 180 * 16);
			break;
		case PsGlyph::Clock:
			painter.drawEllipse(p(0.5, 0.5), r(0.32), r(0.32));
			painter.drawLine(p(0.5, 0.5), p(0.5, 0.30));
			painter.drawLine(p(0.5, 0.5), p(0.64, 0.58));
			break;
		case PsGlyph::User:
			painter.drawEllipse(p(0.5, 0.36), r(0.15), r(0.15));
			painter.drawArc(QRectF(p(0.22, 0.56), p(0.78, 1.04)), 0, 180 * 16);
			break;
		case PsGlyph::Display:
			painter.drawRoundedRect(QRectF(p(0.14, 0.22), p(0.86, 0.68)), r(0.04), r(0.04));
			painter.drawLine(p(0.50, 0.68), p(0.50, 0.80));
			painter.drawLine(p(0.34, 0.80), p(0.66, 0.80));
			break;
		case PsGlyph::Console:
			painter.drawPolygon(QPolygonF({p(0.5, 0.14), p(0.84, 0.32), p(0.5, 0.50), p(0.16, 0.32)}));
			painter.drawPolyline(QPolygonF({p(0.16, 0.32), p(0.16, 0.68), p(0.5, 0.86), p(0.84, 0.68),
			                                p(0.84, 0.32)}));
			painter.drawLine(p(0.5, 0.50), p(0.5, 0.86));
			break;
		case PsGlyph::Gauge:
			painter.drawArc(QRectF(p(0.14, 0.20), p(0.86, 0.92)), 0, 180 * 16);
			painter.drawLine(p(0.5, 0.56), p(0.68, 0.36));
			painter.setBrush(Qt::white);
			painter.drawEllipse(p(0.5, 0.56), r(0.04), r(0.04));
			break;
		case PsGlyph::Speaker:
			painter.drawPolygon(QPolygonF({p(0.14, 0.40), p(0.30, 0.40), p(0.48, 0.24), p(0.48, 0.76),
			                               p(0.30, 0.60), p(0.14, 0.60)}));
			painter.drawArc(QRectF(p(0.46, 0.34), p(0.66, 0.66)), -60 * 16, 120 * 16);
			painter.drawArc(QRectF(p(0.46, 0.22), p(0.82, 0.78)), -60 * 16, 120 * 16);
			break;
		case PsGlyph::Bell:
			painter.drawArc(QRectF(p(0.26, 0.18), p(0.74, 0.70)), 0, 180 * 16);
			painter.drawLine(p(0.26, 0.44), p(0.24, 0.68));
			painter.drawLine(p(0.74, 0.44), p(0.76, 0.68));
			painter.drawLine(p(0.18, 0.70), p(0.82, 0.70));
			painter.drawArc(QRectF(p(0.42, 0.68), p(0.58, 0.84)), 180 * 16, 180 * 16);
			break;
		case PsGlyph::Puzzle:
			painter.drawRoundedRect(QRectF(p(0.18, 0.30), p(0.70, 0.82)), r(0.05), r(0.05));
			painter.drawEllipse(p(0.44, 0.24), r(0.08), r(0.08));
			painter.drawEllipse(p(0.76, 0.56), r(0.08), r(0.08));
			break;
		case PsGlyph::Bug:
			painter.drawEllipse(QRectF(p(0.32, 0.30), p(0.68, 0.82)));
			painter.drawLine(p(0.32, 0.50), p(0.16, 0.42));
			painter.drawLine(p(0.68, 0.50), p(0.84, 0.42));
			painter.drawLine(p(0.32, 0.66), p(0.16, 0.74));
			painter.drawLine(p(0.68, 0.66), p(0.84, 0.74));
			painter.drawLine(p(0.42, 0.30), p(0.34, 0.16));
			painter.drawLine(p(0.58, 0.30), p(0.66, 0.16));
			break;
		case PsGlyph::Folder:
			painter.drawPolygon(QPolygonF({p(0.14, 0.26), p(0.40, 0.26), p(0.48, 0.34), p(0.86, 0.34),
			                               p(0.86, 0.76), p(0.14, 0.76)}));
			break;
		case PsGlyph::Image:
			painter.drawRoundedRect(QRectF(p(0.14, 0.20), p(0.86, 0.80)), r(0.05), r(0.05));
			painter.drawPolyline(QPolygonF({p(0.20, 0.72), p(0.42, 0.48), p(0.58, 0.62), p(0.68, 0.52),
			                                p(0.82, 0.70)}));
			painter.drawEllipse(p(0.66, 0.36), r(0.06), r(0.06));
			break;
	}
	painter.end();
	pixmap.setDevicePixelRatio(Oversample);
	return QIcon(pixmap);
}
