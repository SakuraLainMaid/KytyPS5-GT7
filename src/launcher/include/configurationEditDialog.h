#ifndef CONFIGURATION_EDIT_DIALOG_H
#define CONFIGURATION_EDIT_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

#include <vector>
class QByteArray;
class QGroupBox;
class QListWidget;
class QMoveEvent;
class QResizeEvent;
class QListWidget;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QSettings;
class QWidget;

class Configuration;
namespace Ui {
class ConfigurationEditDialog;
} // namespace Ui

class ConfigurationEditDialog: public QDialog {
	Q_OBJECT

public:
	explicit ConfigurationEditDialog(Configuration& info, QWidget* parent = nullptr);
	~ConfigurationEditDialog() override;

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

	void                      SetGlobalSettings(const QStringList& dirs);
	[[nodiscard]] QStringList GetGameDirectories() const;

signals:
	void PreviewControllerColor(const QString& color);
	void ImportGameSettings();
	void ExportGameSettings();

private:
	Ui::ConfigurationEditDialog* m_ui = nullptr;
	Configuration&               m_info;
	QGroupBox*                   m_game_dirs_group        = nullptr;
	QListWidget*                 m_game_dirs_list         = nullptr;
	QToolButton*                 m_remove_game_dir_button = nullptr;
	bool                         m_global_settings        = false;
	// The settings as the console's Settings app: a list of sections, one page shown at a time.
	QListWidget*                 m_home                   = nullptr; // the first level
	QListWidget*                 m_nav                    = nullptr; // the second level's list
	QStackedWidget*              m_levels                 = nullptr;
	class QLabel*                m_section_title          = nullptr;
	QStackedWidget*              m_pages                  = nullptr;
	std::vector<QGroupBox*>      m_page_groups;

protected:
	void Init(const Configuration& info);
	void InitGameDirectories();
	void AddGameDirectoryItem(const QString& dir);
	// The settings sit in a scroll area; the dialog takes the size they need, but no more than
	// the screen it is on has room for, and stays inside that screen.
	void       InitPages();
	QGroupBox* CreateThemesGroup();
	void UpdateNavigation();
	void FitToScreen(bool restored);

	void moveEvent(QMoveEvent* event) override;
	void reject() override;
	void keyPressEvent(QKeyEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

	static QByteArray g_last_geometry;

	/*slots:*/

	void save();
	void clear();
	void add_game_directory();
	void remove_selected_game_directories();
	void update_game_directory_buttons();
};

#endif // CONFIGURATION_EDIT_DIALOG_H
