#ifndef CONFIGURATION_LIST_WIDGET_H
#define CONFIGURATION_LIST_WIDGET_H

#include "configuration.h"

#include <QHash>
#include <QMap>
#include <QMargins>

#include <array>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class ConfigurationItem;
class CompatibilityDatabase;
class PsHomeView;
class QDialog;
class QMenu;
class QStackedWidget;
class QTimer;
class QToolButton;
class QEvent;
class QTreeWidgetItem;
class QPoint;

namespace Ui {
class ConfigurationListWidget;
} // namespace Ui

class ConfigurationListWidget: public QWidget {
	Q_OBJECT

public:
	explicit ConfigurationListWidget(QWidget* parent = nullptr);
	~ConfigurationListWidget() override;

	void SetRunEnabled(bool flag);
	void SetRuntimeDirectory(const QString& directory) { m_runtime_directory = directory; }

	[[nodiscard]] const ConfigurationItem* GetSelectedItem() const { return m_selected_item; }
	ConfigurationItem*                     GetSelectedItem() { return m_selected_item; }

	[[nodiscard]] const QString& GetSettingsFile() const { return m_settings_file; }
	[[nodiscard]] const QString& GetGlobalControllerColor() const {
		return m_global_info.controller.color;
	}
	[[nodiscard]] std::unique_ptr<Configuration>
	CreateConfiguration(const ConfigurationItem& item) const;

	[[nodiscard]] bool IsHomeView() const;

	bool EnsureGameDirectory();
	void ScanGameDirectory();
	void ViewTrophies();

signals:

	void Run();
	void Select();
	// The home view fills the window: the main dialog hides its status lines around it.
	void HomeViewChanged(bool home);
	void PreviewControllerColor(const QString& color);

protected:
	void changeEvent(QEvent* event) override;

public slots:
	void WriteSettings();
	void ReadSettings();

protected slots:

	void edit_configuration();
	void delete_configuartion();
	void edit_global_settings();
	void edit_input_mapping();
	void list_itemDoubleClicked(QTreeWidgetItem* witem, int column);
	void show_context_menu(const QPoint& pos);
	void open_game_folder();
	void remove_save_data();
	void filter_configurations(const QString& text);

private:
	void               SelectItem(QTreeWidgetItem* witem);
	void               ApplyCompatibility();
	void               UpdateToolbarIcons();
	void               ImportGameSettings(QWidget* parent);
	void               ExportGameSettings(QWidget* parent) const;
	[[nodiscard]] bool HasValidGameDirectory() const;
	// The console-style home view, an alternative to the tree (PsHomeView).
	void InitHomeView();
	void SetHomeView(bool home);
	void RefreshHomeView();
	void ShowItemMenu(ConfigurationItem* item);
	// A dialog, modally: inside the home view (sliding in over it) when that is shown, as its own
	// window otherwise. Returns its result, as QDialog::exec().
	int  ExecDialog(QDialog& dialog);

	ConfigurationItem*            m_selected_item = nullptr;
	bool                          m_run_enabled   = true;
	Ui::ConfigurationListWidget*  m_ui            = nullptr;
	QString                       m_settings_file;
	QString                       m_runtime_directory;
	QStringList                   m_game_dirs;
	Configuration                 m_global_info;
	QMap<QString, Configuration*> m_custom_infos;
	CompatibilityDatabase*        m_compatibility = nullptr;
	PsHomeView*                   m_home          = nullptr;
	QStackedWidget*               m_view_stack    = nullptr;
	QToolButton*                  m_view_button   = nullptr;
	QTimer*                       m_home_refresh  = nullptr;
	// The list's margins and spacing, which the edge-to-edge home view sets to nothing.
	std::array<QMargins, 3>       m_list_margins;
	// The last game menu, kept while the home view's options panel shows its actions.
	std::unique_ptr<QMenu>        m_item_menu;
	// The home view's trophy counts by game path, read again after a game has run.
	QHash<QString, std::pair<std::array<int, 4>, std::array<int, 4>>> m_trophy_counts;
	int                           m_list_spacing = 0;
};

#endif // CONFIGURATION_LIST_WIDGET_H
