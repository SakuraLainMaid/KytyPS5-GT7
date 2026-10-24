#ifndef TROPHY_VIEWER_DIALOG_H
#define TROPHY_VIEWER_DIALOG_H

#include <QDialog>

#include <array>
#include <functional>
#include <utility>

class QKeyEvent;
class QTabWidget;
class QWidget;

class Configuration;
class TrophyViewerDialog: public QDialog {
public:
	explicit TrophyViewerDialog(QWidget* parent = nullptr);

	static bool HasTrophyData(const Configuration* info);
	// The game's trophies by grade (platinum, gold, silver, bronze): earned, and in all.
	static std::pair<std::array<int, 4>, std::array<int, 4>> Count(const Configuration& info,
	                                                               const QString& runtime_directory);
	// `exec` runs the dialog (its default: QDialog::exec()).
	static void ShowForGame(const Configuration* info, const QString& runtime_directory,
	                        QWidget* parent, const std::function<int(QDialog&)>& exec = {});

private:
	bool LoadGame(const Configuration& info, const QString& runtime_directory, QString& error);

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	QTabWidget* m_tabs = nullptr;
};

#endif // TROPHY_VIEWER_DIALOG_H
