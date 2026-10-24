#include "trophyViewerDialog.h"

#include "common/archive.h"
#include "common/trophies.h"
#include "configuration.h"
#include "gameContent.h"
#include "psTheme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QRegularExpression>
#include <QApplication>
#include <QKeyEvent>
#include <QScrollArea>
#include <QTabBar>
#include <QScrollBar>
#include <QStyle>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace {

static const QRegularExpression TrophyFilePattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));

static QStringList FindTrophyFiles(const Configuration* info) {
	if (info == nullptr || info->basedir.isEmpty()) {
		return {};
	}
	QStringList files;
	for (const auto& file: GameContent::ListFiles(
	         info->basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		if (TrophyFilePattern.match(QFileInfo(file).fileName()).hasMatch()) {
			files.append(file);
		}
	}
	files.sort();
	return files;
}

static QString GradeToText(int grade) {
	switch (grade) {
		case 1: return QObject::tr("Platinum");
		case 2: return QObject::tr("Gold");
		case 3: return QObject::tr("Silver");
		case 4: return QObject::tr("Bronze");
		default: return {};
	}
}

// The colours of the grades, as the in-game toast draws them.
static QString GradeColor(int grade) {
	switch (grade) {
		case 1: return QStringLiteral("#a1e1f5");
		case 2: return QStringLiteral("#ffcf52");
		case 3: return QStringLiteral("#d3dce8");
		case 4: return QStringLiteral("#d38e5b");
		default: return QStringLiteral("#9aa3b5");
	}
}

// Progress counts trophies by grade, as the console does (a platinum weighs most).
static int GradePoints(int grade) {
	switch (grade) {
		case 1: return 300;
		case 2: return 90;
		case 3: return 30;
		case 4: return 15;
		default: return 0;
	}
}

constexpr char StyleSheet[] = R"(
QDialog, QScrollArea, QScrollArea > QWidget > QWidget { background: #10131a; color: #e8ebf2; }
QTabWidget::pane { border: 0; }
QTabBar::tab { background: #1a1f2b; color: #9aa3b5; padding: 6px 16px; border-radius: 4px; margin: 2px; }
QTabBar::tab:selected { background: #2a3142; color: #ffffff; }
QFrame#card { background: #1a1f2b; border: 2px solid transparent; border-radius: 10px; }
QFrame#card[earned="true"] { background: #1e2533; }
QLabel#title { font-size: 22px; font-weight: 600; }
QLabel#name { font-size: 15px; font-weight: 600; }
QLabel#secondary { color: #9aa3b5; }
QProgressBar { background: #2a3142; border: 0; border-radius: 4px; height: 8px; text-align: right; }
QProgressBar::chunk { background: #3d8bfd; border-radius: 4px; }
QComboBox { background: #1a1f2b; color: #e8ebf2; border: 1px solid #2a3142; border-radius: 4px; padding: 3px 8px; }
)";

struct Entry {
	const Common::Trophies::Trophy* trophy = nullptr;
	bool                            earned = false;
	int64_t                         time   = 0; // 0: none recorded
	QString                         group;
};

enum class Filter { All, Earned, NotEarned };
enum class Sort { Default, Grade, Recent };

static QWidget* MakeCard(const Entry& entry, QWidget* parent) {
	const auto& trophy = *entry.trophy;
	// A hidden trophy not earned yet shows nothing of itself, as on the console.
	const bool concealed = trophy.hidden && !entry.earned;

	auto* card = new QFrame(parent);
	card->setObjectName(QStringLiteral("card"));
	card->setFocusPolicy(Qt::StrongFocus);
	card->setProperty("earned", entry.earned);
	auto* layout = new QHBoxLayout(card);
	layout->setContentsMargins(12, 10, 16, 10);
	layout->setSpacing(14);

	auto*       icon = new QLabel(card);
	constexpr int IconSize = 72;
	icon->setFixedSize(IconSize, IconSize);
	icon->setAlignment(Qt::AlignCenter);
	QPixmap pixmap;
	if (!concealed && pixmap.loadFromData(reinterpret_cast<const uchar*>(trophy.icon_png.data()),
	                                      static_cast<uint>(trophy.icon_png.size()))) {
		if (!entry.earned) {
			pixmap = QPixmap::fromImage(pixmap.toImage().convertToFormat(QImage::Format_Grayscale8));
		}
		icon->setPixmap(
		    pixmap.scaled(IconSize, IconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	} else {
		icon->setText(QStringLiteral("?"));
		icon->setStyleSheet(QStringLiteral(
		    "background: #2a3142; border-radius: 8px; font-size: 30px; color: #6b7487;"));
	}
	layout->addWidget(icon);

	auto* text = new QVBoxLayout;
	text->setSpacing(2);
	const auto name =
	    concealed ? QObject::tr("Hidden trophy")
	    : trophy.name.empty() ? QObject::tr("Trophy %1").arg(trophy.id)
	                          : QString::fromStdString(trophy.name);
	auto* name_label = new QLabel(name, card);
	name_label->setObjectName(QStringLiteral("name"));
	name_label->setWordWrap(true);
	text->addWidget(name_label);
	auto* description =
	    new QLabel(concealed ? QObject::tr("Keep playing to reveal this trophy.")
	                         : QString::fromStdString(trophy.description),
	               card);
	description->setObjectName(QStringLiteral("secondary"));
	description->setWordWrap(true);
	text->addWidget(description);
	if (!entry.group.isEmpty()) {
		auto* group = new QLabel(entry.group, card);
		group->setObjectName(QStringLiteral("secondary"));
		text->addWidget(group);
	}
	text->addStretch(1);
	layout->addLayout(text, 1);

	auto* side = new QVBoxLayout;
	side->setSpacing(4);
	auto* grade = new QLabel(QStringLiteral("● %1").arg(GradeToText(trophy.grade)), card);
	grade->setAlignment(Qt::AlignRight);
	grade->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(GradeColor(trophy.grade)));
	side->addWidget(grade);
	QString status;
	if (!entry.earned) {
		status = QObject::tr("Not earned");
	} else if (entry.time > 0) {
		status = QObject::tr("Earned %1").arg(QDateTime::fromSecsSinceEpoch(entry.time)
		                                          .toLocalTime()
		                                          .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
	} else {
		status = QObject::tr("Earned");
	}
	auto* status_label = new QLabel(status, card);
	status_label->setObjectName(QStringLiteral("secondary"));
	status_label->setAlignment(Qt::AlignRight);
	side->addWidget(status_label);
	side->addStretch(1);
	layout->addLayout(side);

	if (!concealed && trophy.has_reward && !trophy.reward.empty()) {
		card->setToolTip(QObject::tr("Reward: %1").arg(QString::fromStdString(trophy.reward)));
	}
	return card;
}

// One trophy package: a header with the progress and the counts by grade, a filter and an
// order, and the trophies as cards.
class PackagePage final: public QWidget {
public:
	PackagePage(Common::Trophies::Package package, const std::set<int>& unlocked,
	            const std::map<int, int64_t>& times, const QString& fallback_title,
	            QWidget* parent)
	    : QWidget(parent), m_package(std::move(package)) {
		for (const auto& [id, trophy]: m_package.trophies) {
			Entry entry;
			entry.trophy = &trophy;
			entry.earned = unlocked.contains(id);
			if (const auto found = times.find(id); found != times.end()) {
				entry.time = found->second;
			}
			if (m_package.groups.size() > 1) {
				if (const auto group = m_package.groups.find(trophy.group_id);
				    group != m_package.groups.end()) {
					entry.group = QString::fromStdString(group->second);
				}
			}
			m_entries.push_back(entry);
		}

		auto* layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 8, 4, 4);
		layout->setSpacing(10);

		auto* title = new QLabel(m_package.title.empty() ? fallback_title
		                                                 : QString::fromStdString(m_package.title),
		                         this);
		title->setObjectName(QStringLiteral("title"));
		title->setWordWrap(true);
		layout->addWidget(title);

		int earned_points = 0, all_points = 0, earned = 0;
		std::array<int, 5> grade_earned {}, grade_all {};
		for (const auto& entry: m_entries) {
			const auto grade = std::clamp(entry.trophy->grade, 0, 4);
			all_points += GradePoints(grade);
			grade_all[grade]++;
			if (entry.earned) {
				earned_points += GradePoints(grade);
				grade_earned[grade]++;
				earned++;
			}
		}
		const int percent = all_points > 0 ? earned_points * 100 / all_points : 0;

		auto* progress_row = new QHBoxLayout;
		auto* progress     = new QProgressBar(this);
		progress->setRange(0, 100);
		progress->setValue(percent);
		progress->setTextVisible(false);
		progress->setFixedHeight(8);
		progress_row->addWidget(progress, 1);
		auto* percent_label = new QLabel(QStringLiteral("%1%").arg(percent), this);
		percent_label->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
		progress_row->addWidget(percent_label);
		layout->addLayout(progress_row);

		auto* counts = new QHBoxLayout;
		counts->setSpacing(18);
		auto* total = new QLabel(tr("%1 / %2 trophies").arg(earned).arg(m_entries.size()), this);
		total->setObjectName(QStringLiteral("secondary"));
		counts->addWidget(total);
		for (int grade = 1; grade <= 4; grade++) {
			if (grade_all[grade] == 0) {
				continue;
			}
			auto* count = new QLabel(QStringLiteral("● %1/%2")
			                             .arg(grade_earned[grade])
			                             .arg(grade_all[grade]),
			                         this);
			count->setToolTip(GradeToText(grade));
			count->setStyleSheet(
			    QStringLiteral("color: %1; font-weight: 600;").arg(GradeColor(grade)));
			counts->addWidget(count);
		}
		counts->addStretch(1);
		m_filter = new QComboBox(this);
		m_filter->addItems({tr("All"), tr("Earned"), tr("Not earned")});
		counts->addWidget(m_filter);
		m_sort = new QComboBox(this);
		m_sort->addItems({tr("Default order"), tr("Grade"), tr("Recently earned")});
		// The cards keep the focus; left and right change the filter, Y the order.
		m_filter->setFocusPolicy(Qt::NoFocus);
		m_sort->setFocusPolicy(Qt::NoFocus);
		m_filter->setToolTip(tr("Left / Right"));
		m_sort->setToolTip(tr("Y"));
		counts->addWidget(m_sort);
		layout->addLayout(counts);

		auto* scroll = new PsScrollArea(this);
		m_scroll     = scroll;
		scroll->SetRingRadius(10.0); // the cards' corners
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		m_list = new QWidget(scroll);
		m_list_layout = new QVBoxLayout(m_list);
		m_list_layout->setContentsMargins(0, 0, 6, 0);
		m_list_layout->setSpacing(8);
		scroll->setWidget(m_list);
		layout->addWidget(scroll, 1);

		connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { Fill(); }, Qt::QueuedConnection);
		connect(m_sort, &QComboBox::currentIndexChanged, this, [this] { Fill(); }, Qt::QueuedConnection);
		Fill();
	}

	// The first card takes the focus (when the page is shown, or the list filled again).
	void FocusFirst() {
		if (!m_cards.empty()) {
			m_cards.front()->setFocus(Qt::OtherFocusReason);
		}
	}

protected:
	void showEvent(QShowEvent* event) override {
		QWidget::showEvent(event);
		FocusFirst();
	}

	bool eventFilter(QObject* watched, QEvent* event) override {
		auto* card = qobject_cast<QWidget*>(watched);
		const auto at = std::find(m_cards.begin(), m_cards.end(), card);
		if (at == m_cards.end()) {
			return QWidget::eventFilter(watched, event);
		}
		if (event->type() == QEvent::FocusIn) {
			m_scroll->FocusRow(card);
			m_scroll->EnsureVisibleSmooth(card, 12);
		} else if (event->type() == QEvent::FocusOut) {
			m_scroll->UnfocusRow(card);
		} else if (event->type() == QEvent::KeyPress) {
			const int key  = static_cast<QKeyEvent*>(event)->key();
			const int step = key == Qt::Key_Down ? 1 : key == Qt::Key_Up ? -1 : 0;
			if (step != 0) {
				const auto index = std::clamp<std::ptrdiff_t>((at - m_cards.begin()) + step, 0,
				                                              static_cast<std::ptrdiff_t>(m_cards.size()) - 1);
				m_cards[index]->setFocus(step > 0 ? Qt::TabFocusReason : Qt::BacktabFocusReason);
				return true;
			}
			if (key == Qt::Key_Left || key == Qt::Key_Right) {
				const int count = m_filter->count();
				m_filter->setCurrentIndex((m_filter->currentIndex() + (key == Qt::Key_Right ? 1 : count - 1)) % count);
				return true;
			}
			if (key == Qt::Key_Y) {
				m_sort->setCurrentIndex((m_sort->currentIndex() + 1) % m_sort->count());
				return true;
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private:
	void Fill() {
		const bool had_focus = m_list->isAncestorOf(QApplication::focusWidget());
		m_cards.clear();
		while (auto* item = m_list_layout->takeAt(0)) {
			if (auto* widget = item->widget(); widget != nullptr) {
				widget->hide();
				widget->deleteLater(); // it may be inside an event of its own
			}
			delete item;
		}
		const auto filter = static_cast<Filter>(m_filter->currentIndex());
		const auto sort   = static_cast<Sort>(m_sort->currentIndex());
		std::vector<const Entry*> shown;
		for (const auto& entry: m_entries) {
			if ((filter == Filter::Earned && !entry.earned) ||
			    (filter == Filter::NotEarned && entry.earned)) {
				continue;
			}
			shown.push_back(&entry);
		}
		if (sort == Sort::Grade) {
			std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) {
				return std::clamp(a->trophy->grade, 1, 5) < std::clamp(b->trophy->grade, 1, 5);
			});
		} else if (sort == Sort::Recent) {
			// Earned first, newest first; then those earned at an unknown time; then the rest.
			std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) {
				if (a->earned != b->earned) {
					return a->earned;
				}
				return a->time > b->time;
			});
		}
		for (const auto* entry: shown) {
			auto* card = MakeCard(*entry, m_list);
			card->installEventFilter(this);
			m_cards.push_back(card);
			m_list_layout->addWidget(card);
		}
		if (shown.empty()) {
			auto* none = new QLabel(tr("No trophies here."), m_list);
			none->setObjectName(QStringLiteral("secondary"));
			none->setAlignment(Qt::AlignCenter);
			m_list_layout->addWidget(none);
		}
		m_list_layout->addStretch(1);
		m_scroll->JumpTo(0); // a glide still going would take the list back down
		if (had_focus || isVisible()) {
			FocusFirst();
		}
	}

	Common::Trophies::Package m_package;
	std::vector<Entry>        m_entries;
	QComboBox*                m_filter      = nullptr;
	QComboBox*                m_sort        = nullptr;
	QWidget*                  m_list        = nullptr;
	QVBoxLayout*              m_list_layout = nullptr;
	PsScrollArea*             m_scroll      = nullptr;
	std::vector<QWidget*>     m_cards;
};

} // namespace

TrophyViewerDialog::TrophyViewerDialog(QWidget* parent): QDialog(parent) {
	setWindowTitle(tr("Trophies"));
	// Over the theme's colours, as the rest of the launcher.
	const auto theme = PsTheme::Current();
	setStyleSheet(QString::fromLatin1(StyleSheet) +
	              QStringLiteral("QDialog { background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
	                             "stop:0 %1, stop:1 %2); }")
	                  .arg(theme.top.name(), theme.bottom.name()));
	setAttribute(Qt::WA_StyledBackground);
	resize(820, 680);

	auto* layout = new QVBoxLayout(this);

	m_tabs = new QTabWidget(this);
	m_tabs->setFocusPolicy(Qt::NoFocus);
	m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus); // the cards keep the focus; L1 / R1 change tabs
	m_tabs->setDocumentMode(true);
	layout->addWidget(m_tabs, 1);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

bool TrophyViewerDialog::HasTrophyData(const Configuration* info) {
	return !FindTrophyFiles(info).isEmpty();
}

std::pair<std::array<int, 4>, std::array<int, 4>>
TrophyViewerDialog::Count(const Configuration& info, const QString& runtime_directory) {
	std::array<int, 4> earned {}, total {};
	const auto         reader = Common::OpenArchive(GameContent::ToPath(info.basedir));
	for (const auto& file: FindTrophyFiles(&info)) {
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		const auto label = TrophyFilePattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
		const auto unlocked = Common::Trophies::LoadUnlocks(Common::Trophies::UnlocksPath(
		    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id, label));
		for (const auto& [id, trophy]: package.trophies) {
			if (trophy.grade < 1 || trophy.grade > 4) {
				continue;
			}
			total[trophy.grade - 1]++;
			if (unlocked.contains(id)) {
				earned[trophy.grade - 1]++;
			}
		}
	}
	return {earned, total};
}

void TrophyViewerDialog::keyPressEvent(QKeyEvent* event) {
	if (m_tabs != nullptr && m_tabs->count() > 1 &&
	    (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
		const int count = m_tabs->count();
		m_tabs->setCurrentIndex((m_tabs->currentIndex() + (event->key() == Qt::Key_PageDown ? 1 : count - 1)) %
		                        count);
		return;
	}
	QDialog::keyPressEvent(event);
}

void TrophyViewerDialog::ShowForGame(const Configuration* info, const QString& runtime_directory,
                                     QWidget* parent, const std::function<int(QDialog&)>& exec) {
	if (info == nullptr) {
		return;
	}

	TrophyViewerDialog dlg(parent);
	if (!info->name.isEmpty()) {
		dlg.setWindowTitle(tr("Trophies - %1").arg(info->name));
	}

	QString error;
	if (!dlg.LoadGame(*info, runtime_directory, error)) {
		QMessageBox::warning(parent, tr("Trophies"), error);
		return;
	}

	if (exec) {
		exec(dlg);
	} else {
		dlg.exec();
	}
}

bool TrophyViewerDialog::LoadGame(const Configuration& info, const QString& runtime_directory,
                                  QString& error) {
	const auto reader       = Common::OpenArchive(GameContent::ToPath(info.basedir));
	const auto trophy_files = FindTrophyFiles(&info);
	if (trophy_files.isEmpty()) {
		error = tr("No trophy package found in %1.")
		            .arg(QString::fromLatin1(Common::Trophies::PackageDirectory));
		return false;
	}
	QStringList errors;
	for (const auto& file: trophy_files) {
		auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		if (package.trophies.empty()) {
			errors.append(tr("Could not read trophy package %1.").arg(QFileInfo(file).fileName()));
			continue;
		}
		const auto label = TrophyFilePattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
		const auto unlocks_path = Common::Trophies::UnlocksPath(
		    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id,
		    label);
		const auto unlocked = Common::Trophies::LoadUnlocks(unlocks_path);
		const auto times    = Common::Trophies::LoadUnlockTimes(unlocks_path);
		auto*      page = new PackagePage(std::move(package), unlocked, times, info.name, m_tabs);
		m_tabs->addTab(page, tr("Trophy set %1").arg(label));
	}
	if (m_tabs->count() == 0) {
		error = errors.join(QLatin1Char('\n'));
		return false;
	}
	// One package (the usual case): no tab bar.
	m_tabs->tabBar()->setVisible(m_tabs->count() > 1);
	return true;
}
