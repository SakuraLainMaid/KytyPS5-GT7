#include "configurationEditDialog.h"

#include "common/emulatorConfig.h"
#include "configuration.h"
#include "mandatoryLineEdit.h"
#include "psGlyphs.h"
#include "psTheme.h"
#include <SDL3/SDL.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QTimer>
#include <QStringList>

#include <thread>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtAlgorithms>

#include <algorithm>
#include <memory>

#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#include <QVulkanWindow>
#endif

#include "ui_configuration_edit_dialog.h"

constexpr char SETTINGS_CFG_DIALOG[]               = "ConfigurationEditDialog";
constexpr char SETTINGS_CFG_LAST_GEOMETRY[]        = "geometry";
constexpr int  GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH = 400;

static void UpdateControllerColorButton(QPushButton* button, const QString& hex) {
	const QColor color(hex);
	if (!color.isValid()) {
		button->setProperty("controllerColor", QString {});
		button->setText(QObject::tr("Custom"));
		button->setStyleSheet({});
		return;
	}
	const auto normalized = color.name(QColor::HexRgb);
	button->setProperty("controllerColor", normalized);
	button->setText(normalized.toUpper());
	button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
	                          .arg(normalized, color.lightness() < 128 ? QStringLiteral("white")
	                                                                   : QStringLiteral("black")));
}

const QStringList CONSOLE_LANGUAGE_NAMES = {
    "Japanese",
    "English (United States)",
    "French (France)",
    "Spanish (Spain)",
    "German",
    "Italian",
    "Dutch",
    "Portuguese (Portugal)",
    "Russian",
    "Korean",
    "Chinese (Traditional)",
    "Chinese (Simplified)",
    "Finnish",
    "Swedish",
    "Danish",
    "Norwegian",
    "Polish",
    "Portuguese (Brazil)",
    "English (United Kingdom)",
    "Turkish",
    "Spanish (Latin America)",
    "Arabic",
    "French (Canada)",
    "Czech",
    "Hungarian",
    "Greek",
    "Romanian",
    "Thai",
    "Vietnamese",
    "Indonesian",
};

static QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

static QString GameDirectoryKey(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

ConfigurationEditDialog::ConfigurationEditDialog(Configuration& info, QWidget* parent)
    : QDialog(parent, Qt::WindowCloseButtonHint), m_ui(new Ui::ConfigurationEditDialog),
      m_info(info) {
	m_ui->setupUi(this);
	InitGameDirectories();
	InitPages();
	// Small screens: the scroll area takes what does not fit, so the dialog itself can be small.
	setMinimumSize(480, 320);
	m_ui->comboBox_gt7_render_patch->addItem(tr("Off"), QString {});
	m_ui->comboBox_gt7_render_patch->addItem(tr("1920x1080 (power saver profile)"),
	                                         QStringLiteral("1080p"));
	m_ui->comboBox_gt7_render_patch->addItem(tr("2560x1440"), QStringLiteral("1440p"));
	m_ui->comboBox_pipeline_mode->addItem(tr("Background (skip draws)"),
	                                      QStringLiteral("background"));
	m_ui->comboBox_pipeline_mode->addItem(tr("Background, complete frames"),
	                                      QStringLiteral("hold"));
	m_ui->comboBox_pipeline_mode->addItem(tr("On the GPU thread"), QStringLiteral("sync"));
	m_ui->comboBox_cpu_set->addItem(tr("Default"), QString {});
	m_ui->comboBox_cpu_set->addItem(tr("GPU thread on its own core"), QStringLiteral("gpucore"));
	m_ui->comboBox_cpu_set->addItem(tr("GPU thread on its own core, game on fast cores"),
	                                QStringLiteral("gpucoregame"));
	m_ui->controller_group->setVisible(false);
	m_ui->settings_button->setVisible(false);

	connect(m_ui->ok_button, &QPushButton::clicked, this, &ConfigurationEditDialog::save);
	// Enter (the pad's cross) acts on the focused row, never saves through a default button.
	for (auto* button: findChildren<QPushButton*>()) {
		button->setDefault(false);
		button->setAutoDefault(false);
	}
	for (auto* button: {m_ui->ok_button, m_ui->cancel_button, m_ui->clear_button, m_ui->settings_button}) {
		button->setFocusPolicy(Qt::NoFocus);
	}
	// Cancel closes the settings from either level (reject() steps back a level).
	connect(m_ui->cancel_button, &QPushButton::clicked, this, [this] { done(QDialog::Rejected); });
	connect(m_ui->clear_button, &QPushButton::clicked, this, &ConfigurationEditDialog::clear);
	connect(m_ui->button_controller_color, &QPushButton::clicked, this, [this]() {
		const auto current = m_ui->button_controller_color->property("controllerColor").toString();
		const auto initial =
		    current.isEmpty() ? QColor(QStringLiteral("#0070d1")) : QColor(current);
		const auto color = QColorDialog::getColor(initial, this, tr("DualSense lightbar color"));
		if (color.isValid()) {
			const auto hex = color.name(QColor::HexRgb);
			UpdateControllerColorButton(m_ui->button_controller_color, hex);
			emit PreviewControllerColor(hex);
		}
	});
	connect(m_ui->button_controller_color_reset, &QPushButton::clicked, this, [this]() {
		UpdateControllerColorButton(m_ui->button_controller_color, {});
		emit PreviewControllerColor({});
	});
	connect(m_ui->slider_controller_vibration, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_vibration_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->slider_controller_volume, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_volume_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->comboBox_shader_log_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_shader_log_folder->setEnabled(
		            log == Configuration::LogDirection::File);
	        });
	connect(m_ui->checkBox_cmd_dump, &QCheckBox::toggled, this,
	        [this](bool flag) { m_ui->lineEdit_cmd_dump_folder->setEnabled(flag); });
	connect(m_ui->comboBox_printf_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_printf_file->setEnabled(log == Configuration::LogDirection::File);
	        });

	setSizeGripEnabled(true);

	const bool restored = restoreGeometry(g_last_geometry);

	Init(info);
	UpdateNavigation();
	FitToScreen(restored);
}

// The console's Settings app: the sections in a list on the left, the chosen one's page on the
// right (each scrolls on its own), in its dark style.
constexpr char SettingsStyle[] = R"(
QDialog#ConfigurationEditDialog {
	background: qradialgradient(cx:0.12, cy:-0.05, radius:1.25, fx:0.12, fy:-0.05,
	                            stop:0 @light, stop:0.45 @top, stop:1 @bottom);
}
QLabel#settingsTitle { color: #e9eaee; font-size: 30px; font-weight: 300; }
QListWidget#settingsHome, QListWidget#settingsNav {
	background: transparent; border: 0; outline: 0; color: #d8dbe2; font-size: 17px;
}
QListWidget#settingsHome::item {
	border: 2px solid transparent; border-bottom: 1px solid rgba(255,255,255,28); padding-left: 18px;
}
QListWidget#settingsNav::item { border: 2px solid transparent; padding-left: 26px; }
QListWidget#settingsHome::item:selected, QListWidget#settingsNav::item:selected {
	background: transparent; color: #ffffff; /* the frame is a FocusRing that glides between items */
}
QListWidget#settingsHome::item:hover:!selected, QListWidget#settingsNav::item:hover:!selected {
	background: rgba(255,255,255,10);
}
QScrollArea#settingsPage, QWidget#settingsPageContent { background: transparent; }
QGroupBox { border: 0; margin: 0; padding: 0; background: transparent; color: #d8dbe2; }
QLabel { color: #d8dbe2; font-size: 15px; }
QWidget#settingsRow { border: 2px solid transparent; border-bottom: 1px solid rgba(255,255,255,28);
                      min-height: 30px; }
QWidget#settingsRow:hover { background: rgba(255,255,255,10); }
QWidget#settingsRow[focused="true"] { border-radius: 4px; }
QWidget#settingsRow QLabel { font-size: 16px; }
QLabel#settingsValue { color: rgba(216,219,226,150); }
QWidget#settingsRow QLabel#settingsValue:disabled { color: rgba(216,219,226,70); }
QLabel#settingsNote { color: rgba(216,219,226,150); padding: 12px 16px; }
QWidget#settingsRow QLabel:disabled { color: rgba(216,219,226,90); }
QLineEdit, QComboBox, QSpinBox { background: rgba(255,255,255,14); color: #e9eaee;
                                  border: 1px solid rgba(255,255,255,40); border-radius: 4px;
                                  padding: 6px 10px; min-height: 24px; font-size: 14px; }
QComboBox QAbstractItemView { background: #26272d; color: #e9eaee; selection-background-color: #3d8bfd; }
QListWidget { background: rgba(255,255,255,10); color: #e9eaee; border: 1px solid rgba(255,255,255,30);
              border-radius: 4px; }
QPushButton { background: rgba(255,255,255,22); color: #ffffff; border: 0; border-radius: 18px;
              padding: 8px 22px; font-size: 14px; }
QPushButton:hover { background: rgba(255,255,255,40); }
QPushButton#ok_button { background: #ffffff; color: #0b0d12; font-weight: 700; }
QPushButton#ok_button:hover { background: #e9edf5; }
)";

// The settings' style sheet in the theme's colours.
static QString SettingsStyleSheet(const PsTheme& theme) {
	return QString::fromLatin1(SettingsStyle)
	    .replace(QStringLiteral("@light"), theme.top.lighter(170).name())
	    .replace(QStringLiteral("@top"), theme.top.name())
	    .replace(QStringLiteral("@bottom"), theme.bottom.name());
}

// The launcher's own section: the theme of the home view and of these settings, applied as it is
// chosen (it is the launcher's, not a game's, so Save and Cancel leave it alone).
QGroupBox* ConfigurationEditDialog::CreateThemesGroup() {
	auto* group = new QGroupBox(tr("Themes"), this);
	auto* form  = new QFormLayout(group);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setVerticalSpacing(14);

	auto* start_view = new QComboBox(group);
	start_view->addItem(tr("Game list"), QStringLiteral("list"));
	start_view->addItem(tr("Home (console style)"), QStringLiteral("home"));
	start_view->setToolTip(tr("How the launcher opens. The button in its toolbar switches views at any time."));
	{
		const QSettings view_settings(QSettings::UserScope, QStringLiteral("Kyty"), QStringLiteral("LauncherView"));
		start_view->setCurrentIndex(
		    std::max(0, start_view->findData(view_settings.value(QStringLiteral("default_view"),
		                                                         QStringLiteral("list")))));
	}
	connect(start_view, &QComboBox::currentIndexChanged, group, [start_view] {
		QSettings(QSettings::UserScope, QStringLiteral("Kyty"), QStringLiteral("LauncherView"))
		    .setValue(QStringLiteral("default_view"), start_view->currentData());
	});
	form->addRow(tr("Default view:"), start_view);

	const auto current = PsTheme::Current();
	auto*      themes  = new QComboBox(group);
	for (const auto& id: PsTheme::PresetIds()) {
		themes->addItem(PsTheme::Preset(id).name, id);
	}
	themes->setCurrentIndex(std::max(0, themes->findData(current.id)));
	form->addRow(tr("Theme:"), themes);

	auto* picture_row    = new QHBoxLayout;
	auto* picture_label  = new QLabel(current.image.isEmpty() ? tr("No picture")
	                                                          : QFileInfo(current.image).fileName(),
	                                  group);
	auto* picture_button = new QPushButton(tr("Choose picture..."), group);
	picture_row->addWidget(picture_label, 1);
	picture_row->addWidget(picture_button);
	form->addRow(tr("Custom picture:"), picture_row);

	auto* accent_row    = new QHBoxLayout;
	auto* accent_button = new QPushButton(group);
	auto* accent_reset  = new QPushButton(tr("Theme's own"), group);
	accent_row->addWidget(accent_button);
	accent_row->addWidget(accent_reset);
	accent_row->addStretch(1);
	form->addRow(tr("Accent colour:"), accent_row);

	// The chosen picture and accent (an invalid colour: the theme's own).
	struct State {
		QString image;
		QColor  accent;
	};
	auto       state = std::make_shared<State>();
	state->image     = QSettings(QSettings::UserScope, QStringLiteral("Kyty"),
	                             QStringLiteral("LauncherView"))
	                   .value(QStringLiteral("theme/image"))
	                   .toString();
	if (const QColor accent(QSettings(QSettings::UserScope, QStringLiteral("Kyty"),
	                                  QStringLiteral("LauncherView"))
	                            .value(QStringLiteral("theme/accent"))
	                            .toString());
	    accent.isValid()) {
		state->accent = accent;
	}
	const auto show_accent = [accent_button, themes, state] {
		const auto color = state->accent.isValid()
		                       ? state->accent
		                       : PsTheme::Preset(themes->currentData().toString()).accent;
		accent_button->setText(color.name().toUpper());
		accent_button->setStyleSheet(
		    QStringLiteral("QPushButton { background: %1; color: %2; }")
		        .arg(color.name(), color.lightness() < 140 ? QStringLiteral("white")
		                                                   : QStringLiteral("black")));
	};
	const auto apply = [themes, state, show_accent] {
		PsTheme::Save(themes->currentData().toString(), state->image, state->accent);
		show_accent();
	};
	show_accent();
	connect(themes, &QComboBox::currentIndexChanged, group, apply);
	connect(picture_button, &QPushButton::clicked, group, [this, themes, picture_label, state, apply] {
		const auto path = QFileDialog::getOpenFileName(
		    this, tr("Theme picture"), {}, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
		if (path.isEmpty()) {
			return;
		}
		state->image = path;
		picture_label->setText(QFileInfo(path).fileName());
		const QSignalBlocker blocker(themes);
		themes->setCurrentIndex(themes->findData(QStringLiteral("custom")));
		apply();
	});
	connect(accent_button, &QPushButton::clicked, group, [this, themes, state, apply] {
		const auto color = QColorDialog::getColor(
		    state->accent.isValid() ? state->accent
		                            : PsTheme::Preset(themes->currentData().toString()).accent,
		    this, tr("Accent colour"));
		if (color.isValid()) {
			state->accent = color;
			apply();
		}
	});
	connect(accent_reset, &QPushButton::clicked, group, [state, apply] {
		state->accent = QColor();
		apply();
	});
	return group;
}

// Keys between the settings' two columns: Right from the list of sections into the page,
// Left from a switch in the page back to the list.
class SettingsColumnKeys final: public QObject {
public:
	SettingsColumnKeys(QListWidget* nav, QStackedWidget* pages, QObject* parent)
	    : QObject(parent), m_nav(nav), m_pages(pages) {}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() != QEvent::KeyPress) {
			return QObject::eventFilter(watched, event);
		}
		const int key = static_cast<QKeyEvent*>(event)->key();
		if (watched == m_nav && (key == Qt::Key_Right || key == Qt::Key_Return || key == Qt::Key_Enter)) {
			auto* page = m_pages->currentWidget();
			for (auto* widget: page->findChildren<QWidget*>()) {
				if ((widget->focusPolicy() & Qt::TabFocus) != 0 && widget->isVisibleTo(page) &&
				    widget->isEnabled() && qobject_cast<QCheckBox*>(widget) == nullptr) {
					widget->setFocus(Qt::TabFocusReason);
					return true;
				}
			}
		} else if (watched != m_nav && key == Qt::Key_Left &&
		           (qobject_cast<PsSwitch*>(watched) != nullptr || qobject_cast<PsChoiceRow*>(watched) != nullptr ||
		            qobject_cast<QLineEdit*>(watched) != nullptr)) {
			m_nav->setFocus(Qt::BacktabFocusReason);
			return true;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	QListWidget*    m_nav;
	QStackedWidget* m_pages;
};

void ConfigurationEditDialog::InitPages() {
	setStyleSheet(SettingsStyleSheet(PsTheme::Current()));
	connect(&PsThemeNotifier::Instance(), &PsThemeNotifier::Changed, this,
	        [this] { setStyleSheet(SettingsStyleSheet(PsTheme::Current())); });
	auto* themes_group = CreateThemesGroup();
	const std::vector<std::pair<QGroupBox*, PsGlyph>> groups {
	    {m_ui->user_profile_group, PsGlyph::User},     {m_ui->graphics_group, PsGlyph::Display},
	    {m_ui->console_group, PsGlyph::Console},        {m_ui->tuning_group, PsGlyph::Gauge},
	    {m_ui->audio_group, PsGlyph::Speaker},          {m_ui->controller_group, PsGlyph::Gamepad},
	    {m_game_dirs_group, PsGlyph::Folder},           {m_ui->notifications_group, PsGlyph::Bell},
	    {themes_group, PsGlyph::Image},                {m_ui->compatibility_group, PsGlyph::Puzzle},
	    {m_ui->debug_group, PsGlyph::Bug}};

	// The first level: the sections, each with its icon, in a list down the middle.
	m_home = new QListWidget(this);
	m_home->setObjectName(QStringLiteral("settingsHome"));
	m_home->setIconSize(QSize(26, 26));
	m_home->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// The second level: the sections down the left, the chosen one's page on the right.
	m_nav = new QListWidget(this);
	m_nav->setObjectName(QStringLiteral("settingsNav"));
	m_nav->setFixedWidth(300);
	m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_pages = new QStackedWidget(this);

	for (const auto& [group, glyph]: groups) {
		// The group leaves the form's grid for a page of its own.
		if (auto* parent_layout = group->parentWidget() != nullptr ? group->parentWidget()->layout()
		                                                          : nullptr;
		    parent_layout != nullptr) {
			parent_layout->removeWidget(group);
		}
		m_ui->sectionsLayout->removeWidget(group);
		m_ui->gridLayout->removeWidget(group);
		const auto title = group->title().replace(QStringLiteral("&&"), QStringLiteral("&"));
		group->setTitle({});

		auto* page = new PsScrollArea(m_pages);
		page->setObjectName(QStringLiteral("settingsPage"));
		page->setWidgetResizable(true);
		page->setFrameShape(QFrame::NoFrame);
		auto* content = new QWidget(page);
		content->setObjectName(QStringLiteral("settingsPageContent"));
		auto* layout = new QVBoxLayout(content);
		layout->setContentsMargins(36, 4, 36, 16);
		group->setParent(content);
		layout->addWidget(group);
		layout->addStretch(1);
		page->setWidget(content);
		m_pages->addWidget(page);

		auto* home_item = new QListWidgetItem(PsGlyphIcon(glyph, 26), title, m_home);
		home_item->setSizeHint(QSize(0, 64));
		auto* nav_item = new QListWidgetItem(title, m_nav);
		nav_item->setSizeHint(QSize(0, 62));
		m_page_groups.push_back(group);
	}
	// The console's list of rows in place of the form: switches, choices, fields.
	for (const auto& [group, glyph]: groups) {
		if (group != m_game_dirs_group) {
			BuildSettingsRows(group);
		}
	}
	auto* column_keys = new SettingsColumnKeys(m_nav, m_pages, this);
	m_nav->installEventFilter(column_keys);
	for (auto* widget: m_pages->findChildren<QWidget*>()) {
		if (qobject_cast<PsSwitch*>(widget) != nullptr || qobject_cast<PsChoiceRow*>(widget) != nullptr ||
		    qobject_cast<QLineEdit*>(widget) != nullptr) {
			widget->installEventFilter(column_keys);
		}
	}
	// The form's grid of sections is empty now.
	m_ui->gridLayout->removeItem(m_ui->sectionsLayout);
	delete m_ui->sectionsLayout;
	m_ui->sectionsLayout = nullptr;

	m_levels = new QStackedWidget(this);

	auto* home        = new QWidget(m_levels);
	auto* home_layout = new QVBoxLayout(home);
	home_layout->setContentsMargins(40, 20, 40, 0);
	auto* home_title = new QLabel(tr("Settings"), home);
	home_title->setObjectName(QStringLiteral("settingsTitle"));
	home_layout->addWidget(home_title);
	auto* home_row = new QHBoxLayout;
	home_row->addStretch(1);
	home_row->addWidget(m_home, 6);
	home_row->addStretch(1);
	home_layout->addSpacing(18);
	home_layout->addLayout(home_row, 1);
	m_levels->addWidget(home);

	auto* section        = new QWidget(m_levels);
	auto* section_layout = new QVBoxLayout(section);
	section_layout->setContentsMargins(40, 20, 40, 0);
	m_section_title = new QLabel(section);
	m_section_title->setObjectName(QStringLiteral("settingsTitle"));
	section_layout->addWidget(m_section_title);
	section_layout->addSpacing(18);
	auto* section_row = new QHBoxLayout;
	section_row->setSpacing(24);
	section_row->addWidget(m_nav);
	section_row->addWidget(m_pages, 1);
	section_layout->addLayout(section_row, 1);
	m_levels->addWidget(section);

	m_ui->gridLayout->addWidget(m_levels, 0, 0, 2, 2);

	const auto open = [this](QListWidgetItem* item) {
		if (item != nullptr) {
			m_nav->setCurrentRow(m_home->row(item));
			PsSwitchStack(m_levels, 1, true);
			m_nav->setFocus();
		}
	};
	connect(m_home, &QListWidget::itemClicked, this, open);
	connect(m_home, &QListWidget::itemActivated, this, open);
	connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row >= 0 && row != m_pages->currentIndex()) {
			m_pages->setCurrentIndex(row);
			if (auto* page = qobject_cast<PsScrollArea*>(m_pages->currentWidget()); page != nullptr) {
				page->JumpTo(0);
			}
			// The whole page at once, rising a little as it fades in: its contents, as the scroll
			// area itself neither fades (its viewport paints on its own) nor stays where it is put
			// (the stack lays it out).
			if (auto* page = qobject_cast<PsScrollArea*>(m_pages->currentWidget());
			    page != nullptr && page->widget() != nullptr) {
				PsAnimateIn(page->widget(), QPoint(0, 26), 320);
			}
			m_section_title->setText(m_nav->item(row)->text());
		} else if (row >= 0) {
			m_section_title->setText(m_nav->item(row)->text());
		}
	});
	PsAttachFocusRing(m_home);
	PsAttachFocusRing(m_nav);
	UpdateNavigation();
	m_home->setFocus();
}

// Shown (as a page or a window), the focus goes to the current level's list once the caller is
// done placing it.
void ConfigurationEditDialog::showEvent(QShowEvent* event) {
	QDialog::showEvent(event);
	QTimer::singleShot(0, this, [this] {
		if (m_levels == nullptr) {
			return;
		}
		if (m_levels->currentIndex() == 0) {
			m_home->setFocus(Qt::OtherFocusReason);
		} else if (auto* focus = QApplication::focusWidget(); focus == nullptr || !isAncestorOf(focus)) {
			m_nav->setFocus(Qt::OtherFocusReason);
		}
	});
}

// The menu key (the pad's Options) saves, as the Save button.
void ConfigurationEditDialog::keyPressEvent(QKeyEvent* event) {
	if (event->key() == Qt::Key_Menu) {
		m_ui->ok_button->click();
		return;
	}
	QDialog::keyPressEvent(event);
}

// Escape (or the gamepad's back) from a section goes back to the list of sections, as on the
// console; from the list it closes the settings.
void ConfigurationEditDialog::reject() {
	if (m_levels != nullptr && m_levels->currentIndex() == 1) {
		m_home->setCurrentRow(m_nav->currentRow());
		PsSwitchStack(m_levels, 0, false);
		m_home->setFocus();
		return;
	}
	QDialog::reject();
}

// A section whose group is hidden (the controller and the game folders outside the global
// settings) has no entry in the list.
void ConfigurationEditDialog::UpdateNavigation() {
	int first_shown = -1;
	for (int i = 0; i < static_cast<int>(m_page_groups.size()); i++) {
		const bool hidden = m_page_groups[i]->isHidden();
		m_nav->item(i)->setHidden(hidden);
		m_home->item(i)->setHidden(hidden);
		if (!hidden && first_shown < 0) {
			first_shown = i;
		}
	}
	for (auto* list: {m_nav, m_home}) {
		if ((list->currentRow() < 0 || list->item(list->currentRow())->isHidden()) &&
		    first_shown >= 0) {
			list->setCurrentRow(first_shown);
		}
	}
}

void ConfigurationEditDialog::FitToScreen(bool restored) {
	auto* where = screen();
	if (where == nullptr || m_pages == nullptr) {
		return;
	}
	layout()->activate();
	// Room for the window frame and title bar, which the geometry leaves out.
	const auto available = where->availableGeometry().adjusted(8, 40, -8, -8);
	auto       wanted    = QSize(1040, 700);
	if (restored) {
		wanted = size(); // The size the user left it at, within the screen below.
	}
	resize(wanted.boundedTo(available.size()).expandedTo(minimumSize()));
	auto frame = geometry();
	if (!available.contains(frame)) {
		frame.moveLeft(std::clamp(frame.left(), available.left(),
		                          std::max(available.left(), available.right() - frame.width())));
		frame.moveTop(std::clamp(frame.top(), available.top(),
		                         std::max(available.top(), available.bottom() - frame.height())));
		move(frame.topLeft());
	}
}

QByteArray ConfigurationEditDialog::g_last_geometry;

ConfigurationEditDialog::~ConfigurationEditDialog() {
	delete m_ui;
}

void ConfigurationEditDialog::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry);
	}

	s.endGroup();
}

void ConfigurationEditDialog::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	g_last_geometry = s.value(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry).toByteArray();

	s.endGroup();
}

template <class T>
static void ListInit(QComboBox* combo, T value) {
	combo->clear();
	combo->addItems(EnumToList<T>());
	combo->setCurrentText(EnumToText(value));
}

void ConfigurationEditDialog::Init(const Configuration& info) {
	m_ui->lineEdit_user_name->setMaxLength(static_cast<int>(Config::MAX_USER_NAME_LENGTH));
	m_ui->lineEdit_user_name->setText(info.user_name);
	m_ui->spinBox_user_id->setValue(info.user_id);
	UpdateControllerColorButton(m_ui->button_controller_color, info.controller.color);
	m_ui->slider_controller_vibration->setValue(info.controller.vibration_intensity);
	m_ui->slider_controller_volume->setValue(info.controller.speaker_volume);
	auto* microphone = m_ui->comboBox_audio_input_device;
	microphone->clear();
	microphone->addItem(tr("None"), QString {});
	microphone->setToolTip(tr("Microphone used by games. None supplies silence."));
	// The microphones, listed on a thread of their own: the audio subsystem sets up and tears down
	// COM on the thread it runs on, which would take the launcher's gamepad input with it.
	QStringList microphones;
	QString     audio_error;
	std::thread([&microphones, &audio_error] {
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			audio_error = QString::fromUtf8(SDL_GetError());
			return;
		}
		int                device_count = 0;
		SDL_AudioDeviceID* devices      = SDL_GetAudioRecordingDevices(&device_count);
		for (int i = 0; i < device_count; i++) {
			if (const auto* device = SDL_GetAudioDeviceName(devices[i]); device != nullptr) {
				microphones.append(QString::fromUtf8(device));
			}
		}
		SDL_free(devices);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}).join();
	for (const auto& name: microphones) {
		if (microphone->findData(name) < 0) {
			microphone->addItem(name, name);
		}
	}
	if (!audio_error.isEmpty()) {
		microphone->setToolTip(tr("Microphones could not be listed: %1").arg(audio_error));
	}
	if (microphone->findData(info.audio_input_device) < 0) {
		microphone->addItem(tr("%1 (unavailable)").arg(info.audio_input_device),
		                    info.audio_input_device);
	}
	microphone->setCurrentIndex(microphone->findData(info.audio_input_device));
	ListInit(m_ui->comboBox_screen_resolution, info.screen_resolution);
	ListInit(m_ui->comboBox_present_mode, info.present_mode);
	m_ui->comboBox_gpu->clear();
	m_ui->comboBox_gpu->addItem(tr("Auto"));
	// Keep Auto when Qt is built without Vulkan support.
#if QT_CONFIG(vulkan)
#if defined(__APPLE__)
	if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) {
		const auto base    = QCoreApplication::applicationDirPath();
		auto       library = base + "/libMoltenVK.dylib";
		if (!QFileInfo::exists(library)) {
			library = base + "/../Frameworks/libMoltenVK.dylib";
		}
		if (QFileInfo::exists(library)) {
			qputenv("QT_VULKAN_LIB", library.toUtf8());
		}
	}
#endif
	QVulkanInstance instance;
	instance.setApiVersion(QVersionNumber(1, 3, 0));
#if !defined(__APPLE__) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	instance.setFlags(QVulkanInstance::NoPortabilityDrivers);
#endif
	if (instance.create()) {
		QVulkanWindow window;
		window.setVulkanInstance(&instance);
		for (const auto& device: window.availablePhysicalDevices()) {
			m_ui->comboBox_gpu->addItem(QString::fromUtf8(device.deviceName));
		}
	}
#endif
	m_ui->comboBox_gpu->setCurrentIndex(
	    info.gpu_index >= 0 && info.gpu_index < m_ui->comboBox_gpu->count() - 1 ? info.gpu_index + 1
	                                                                            : 0);
	m_ui->checkBox_fullscreen->setChecked(info.fullscreen_enabled);
	m_ui->checkBox_hide_cursor->setChecked(info.hide_cursor_enabled);
	m_ui->checkBox_readback->setChecked(info.readback_linear_images);
	m_ui->checkBox_tessellation->setChecked(info.tessellation_enabled);
	m_ui->checkBox_trophy_notifications->setChecked(info.trophy_enabled);
	m_ui->checkBox_bindless->setChecked(info.bindless_images_enabled);
	m_ui->checkBox_ps5_pro->setChecked(info.ps5_pro_enabled);
	m_ui->checkBox_power_saver->setChecked(info.power_saver_enabled);
	m_ui->comboBox_gt7_render_patch->setCurrentIndex(
	    std::max(0, m_ui->comboBox_gt7_render_patch->findData(info.gt7_render_patch)));
	m_ui->checkBox_vma_small_blocks->setChecked(info.vma_small_blocks);
	m_ui->checkBox_low_vram_gc->setChecked(info.low_vram_gc);
	m_ui->checkBox_readahead->setChecked(info.readahead_enabled);
	m_ui->checkBox_long_fault_window->setChecked(info.long_fault_window);
	m_ui->comboBox_pipeline_mode->setCurrentIndex(
	    std::max(0, m_ui->comboBox_pipeline_mode->findData(info.pipeline_mode)));
	m_ui->spinBox_pipeline_threads->setValue(info.pipeline_threads);
	m_ui->comboBox_cpu_set->setCurrentIndex(
	    std::max(0, m_ui->comboBox_cpu_set->findData(info.cpu_set)));
	m_ui->spinBox_ray_loop_cap->setValue(info.ray_loop_cap);
	m_ui->checkBox_reflections_half_rate->setChecked(info.reflections_half_rate);
	m_ui->lineEdit_skip_shaders->setText(info.skip_shader_hashes);
	m_ui->spinBox_vblank_frequency->setValue(info.vblank_frequency);
	m_ui->comboBox_console_language->clear();
	m_ui->comboBox_console_language->addItems(CONSOLE_LANGUAGE_NAMES);
	m_ui->comboBox_console_language->setCurrentIndex(
	    info.console_language >= 0 && info.console_language < CONSOLE_LANGUAGE_NAMES.size()
	        ? info.console_language
	        : Configuration::DEFAULT_CONSOLE_LANGUAGE);
	m_ui->checkBox_shader_validation->setChecked(info.shader_validation_enabled);
	m_ui->checkBox_vulkan_validation->setChecked(info.vulkan_validation_enabled);
	m_ui->checkBox_renderdoc_capture->setChecked(info.renderdoc_enabled);
	m_ui->checkBox_amd_cpu->setChecked(info.amd_cpu_enabled);
#if defined(__APPLE__)
	m_ui->checkBox_amd_cpu->setVisible(false);
#endif
#if defined(_WIN32)
	m_ui->checkBox_red_zone_protection->setChecked(info.red_zone_protection_enabled);
#else
	m_ui->checkBox_red_zone_protection->setVisible(false);
#endif
	ListInit(m_ui->comboBox_shader_optimization_type, info.shader_optimization_type);
	ListInit(m_ui->comboBox_shader_log_direction, info.shader_log_direction);
	m_ui->lineEdit_shader_log_folder->setText(info.shader_log_folder);
	m_ui->lineEdit_shader_log_folder->setEnabled(info.shader_log_direction ==
	                                             Configuration::LogDirection::File);
	m_ui->checkBox_cmd_dump->setChecked(info.command_buffer_dump_enabled);
	m_ui->lineEdit_cmd_dump_folder->setText(info.command_buffer_dump_folder);
	m_ui->lineEdit_cmd_dump_folder->setEnabled(info.command_buffer_dump_enabled);
	ListInit(m_ui->comboBox_printf_direction, info.printf_direction);
	m_ui->lineEdit_printf_file->setText(info.printf_output_file);
	m_ui->lineEdit_printf_file->setEnabled(info.printf_direction ==
	                                       Configuration::LogDirection::File);
	m_ui->checkBox_profiler->setChecked(info.profiler_enabled);
}

void ConfigurationEditDialog::InitGameDirectories() {
	m_game_dirs_group = new QGroupBox(tr("Game folders"), this);

	auto* group_layout = new QVBoxLayout(m_game_dirs_group);
	group_layout->setContentsMargins(8, 8, 8, 8);
	group_layout->setSpacing(6);

	m_game_dirs_list = new QListWidget(m_game_dirs_group);
	m_game_dirs_list->setMinimumHeight(120);
	m_game_dirs_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	group_layout->addWidget(m_game_dirs_list);

	auto* button_layout = new QHBoxLayout;
	button_layout->setContentsMargins(0, 0, 0, 0);
	button_layout->setSpacing(4);

	auto* add_button = new QToolButton(m_game_dirs_group);
	add_button->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
	add_button->setToolTip(tr("Add game folder"));
	add_button->setAutoRaise(true);
	button_layout->addWidget(add_button);

	m_remove_game_dir_button = new QToolButton(m_game_dirs_group);
	m_remove_game_dir_button->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
	m_remove_game_dir_button->setToolTip(tr("Remove selected game folders"));
	m_remove_game_dir_button->setAutoRaise(true);
	button_layout->addWidget(m_remove_game_dir_button);

	button_layout->addStretch(1);
	group_layout->addLayout(button_layout);

	m_ui->gridLayout->addWidget(m_game_dirs_group, 1, 0, 1, 1);
	m_game_dirs_group->setVisible(false);

	connect(add_button, &QToolButton::clicked, this, &ConfigurationEditDialog::add_game_directory);
	connect(m_remove_game_dir_button, &QToolButton::clicked, this,
	        &ConfigurationEditDialog::remove_selected_game_directories);
	connect(m_game_dirs_list, &QListWidget::itemSelectionChanged, this,
	        &ConfigurationEditDialog::update_game_directory_buttons);

	update_game_directory_buttons();
}

void ConfigurationEditDialog::SetGlobalSettings(const QStringList& dirs) {
	m_global_settings = true;
	auto* menu        = new QMenu(m_ui->settings_button);
	menu->addAction(tr("Import..."), this, &ConfigurationEditDialog::ImportGameSettings);
	menu->addAction(tr("Export..."), this, &ConfigurationEditDialog::ExportGameSettings);
	m_ui->settings_button->setMenu(menu);
	m_ui->settings_button->setVisible(true);
	m_game_dirs_list->clear();
	m_game_dirs_group->setMinimumWidth(GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH);

	for (const auto& dir: dirs) {
		AddGameDirectoryItem(dir);
	}

	m_game_dirs_group->setVisible(true);
	m_ui->controller_group->setVisible(true);
	update_game_directory_buttons();
	UpdateNavigation();
	FitToScreen(false);
}

QStringList ConfigurationEditDialog::GetGameDirectories() const {
	QStringList dirs;
	if (!m_global_settings) {
		return dirs;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		dirs.append(item->text());
	}

	return dirs;
}

void ConfigurationEditDialog::AddGameDirectoryItem(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	const auto key        = GameDirectoryKey(normalized);
	if (normalized.isEmpty() || key.isEmpty()) {
		return;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		if (GameDirectoryKey(item->text()) == key) {
			return;
		}
	}

	auto* item = new QListWidgetItem(normalized, m_game_dirs_list);
	item->setToolTip(normalized);
}

void ConfigurationEditDialog::moveEvent(QMoveEvent* event) {
	QDialog::moveEvent(event);
	if (isWindow()) { // inside the home view it is a page, not a window to remember
		g_last_geometry = saveGeometry();
	}
}

void ConfigurationEditDialog::resizeEvent(QResizeEvent* event) {
	QDialog::resizeEvent(event);
	if (isWindow()) {
		g_last_geometry = saveGeometry();
	}
}

static void UpdateInfo(Configuration& info, Ui::ConfigurationEditDialog& ui, bool global_settings) {
	info.user_name = ui.lineEdit_user_name->text().trimmed();
	info.user_id   = ui.spinBox_user_id->value();
	info.audio_input_device = ui.comboBox_audio_input_device->currentData().toString();
	if (global_settings) {
		info.controller.color = ui.button_controller_color->property("controllerColor").toString();
		info.controller.vibration_intensity = ui.slider_controller_vibration->value();
		info.controller.speaker_volume      = ui.slider_controller_volume->value();
	}
	info.screen_resolution =
	    TextToEnum<Configuration::Resolution>(ui.comboBox_screen_resolution->currentText());
	info.present_mode =
	    TextToEnum<Configuration::PresentMode>(ui.comboBox_present_mode->currentText());
	info.gpu_index                 = ui.comboBox_gpu->currentIndex() - 1;
	info.fullscreen_enabled        = ui.checkBox_fullscreen->isChecked();
	info.hide_cursor_enabled       = ui.checkBox_hide_cursor->isChecked();
	info.readback_linear_images    = ui.checkBox_readback->isChecked();
	info.tessellation_enabled      = ui.checkBox_tessellation->isChecked();
	info.trophy_enabled            = ui.checkBox_trophy_notifications->isChecked();
	info.bindless_images_enabled   = ui.checkBox_bindless->isChecked();
	info.ps5_pro_enabled           = ui.checkBox_ps5_pro->isChecked();
	info.power_saver_enabled       = ui.checkBox_power_saver->isChecked();
	info.gt7_render_patch          = ui.comboBox_gt7_render_patch->currentData().toString();
	info.vma_small_blocks          = ui.checkBox_vma_small_blocks->isChecked();
	info.low_vram_gc               = ui.checkBox_low_vram_gc->isChecked();
	info.readahead_enabled         = ui.checkBox_readahead->isChecked();
	info.long_fault_window         = ui.checkBox_long_fault_window->isChecked();
	info.pipeline_mode             = ui.comboBox_pipeline_mode->currentData().toString();
	info.pipeline_threads          = ui.spinBox_pipeline_threads->value();
	info.cpu_set                   = ui.comboBox_cpu_set->currentData().toString();
	info.ray_loop_cap              = ui.spinBox_ray_loop_cap->value();
	info.reflections_half_rate     = ui.checkBox_reflections_half_rate->isChecked();
	info.skip_shader_hashes        = ui.lineEdit_skip_shaders->text().simplified().remove(' ');
	info.vblank_frequency          = ui.spinBox_vblank_frequency->value();
	info.console_language          = ui.comboBox_console_language->currentIndex();
	info.vulkan_validation_enabled = ui.checkBox_vulkan_validation->isChecked();
	info.shader_validation_enabled = ui.checkBox_shader_validation->isChecked();
	info.renderdoc_enabled         = ui.checkBox_renderdoc_capture->isChecked();
	info.amd_cpu_enabled           = ui.checkBox_amd_cpu->isChecked();
#if defined(_WIN32)
	info.red_zone_protection_enabled = ui.checkBox_red_zone_protection->isChecked();
#endif
	info.shader_optimization_type = TextToEnum<Configuration::ShaderOptimizationType>(
	    ui.comboBox_shader_optimization_type->currentText());
	info.shader_log_direction = TextToEnum<Configuration::LogDirection>(
	    ui.comboBox_shader_log_direction->currentText());
	info.shader_log_folder           = ui.lineEdit_shader_log_folder->text();
	info.command_buffer_dump_enabled = ui.checkBox_cmd_dump->isChecked();
	info.command_buffer_dump_folder  = ui.lineEdit_cmd_dump_folder->text();
	info.printf_direction =
	    TextToEnum<Configuration::LogDirection>(ui.comboBox_printf_direction->currentText());
	info.printf_output_file = ui.lineEdit_printf_file->text();
	info.profiler_enabled = ui.checkBox_profiler->isChecked();
}

void ConfigurationEditDialog::save() {
	if (MandatoryLineEdit::FindEmpty(this)) {
		QMessageBox::critical(this, tr("Save failed"), tr("Please fill all mandatory fields"));
		return;
	}

	const auto user_name = m_ui->lineEdit_user_name->text().trimmed();
	if (user_name.isEmpty() || user_name.toUtf8().size() > Config::MAX_USER_NAME_LENGTH) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User name must contain 1-16 UTF-8 bytes"));
		return;
	}
	m_ui->lineEdit_user_name->setText(user_name);
	if (!Config::IsConfiguredUserIdValid(m_ui->spinBox_user_id->value())) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User ID cannot be 254 (everyone) or 255 (system)"));
		return;
	}

	UpdateInfo(m_info, *m_ui, m_global_settings);

	emit accept();
}

void ConfigurationEditDialog::clear() {
	Configuration default_info;
	Init(default_info);

	if (m_global_settings) {
		emit PreviewControllerColor({});
		m_game_dirs_list->clear();
		update_game_directory_buttons();
	}
}

void ConfigurationEditDialog::add_game_directory() {
	QString start_dir = QDir::homePath();
	if (m_game_dirs_list->count() > 0) {
		start_dir = m_game_dirs_list->item(m_game_dirs_list->count() - 1)->text();
	}

#if defined(__APPLE__)
	// Use the native macOS picker to browse mounted volumes.
	const auto dir = QFileDialog::getExistingDirectory(this, tr("Select game folder"), start_dir);
	if (dir.isEmpty()) {
		return;
	}
	AddGameDirectoryItem(dir);
#else
	QFileDialog dialog(this, tr("Select game folders"), start_dir);
	dialog.setFileMode(QFileDialog::Directory);
	dialog.setOption(QFileDialog::ShowDirsOnly, true);
	dialog.setOption(QFileDialog::DontUseNativeDialog, true);
	dialog.setLabelText(QFileDialog::Accept, tr("Add"));

	auto* list_view = dialog.findChild<QListView*>(QStringLiteral("listView"));
	if (list_view != nullptr) {
		list_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}
	auto* tree_view = dialog.findChild<QTreeView*>(QStringLiteral("treeView"));
	if (tree_view != nullptr) {
		tree_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	for (const auto& dir: dialog.selectedFiles()) {
		AddGameDirectoryItem(dir);
	}
#endif

	update_game_directory_buttons();
}

void ConfigurationEditDialog::remove_selected_game_directories() {
	qDeleteAll(m_game_dirs_list->selectedItems());
	update_game_directory_buttons();
}

void ConfigurationEditDialog::update_game_directory_buttons() {
	m_remove_game_dir_button->setEnabled(!m_game_dirs_list->selectedItems().isEmpty());
}
