#include "MainWindow.hpp"

#include "MeshViewer.hpp"
#include "TexturePreview.hpp"

#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/AtlasExtractor.hpp"
#include "ovl/extract/DumpExtractor.hpp"
#include "ovl/extract/IResourceExtractor.hpp"
#include "ovl/extract/ModelExtractor.hpp"
#include "ovl/extract/SoundExtractor.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <sstream>

namespace ovlgui {

namespace {

QString human_side(ovl::OvlSide s) {
    return s == ovl::OvlSide::Common ? "common" : "unique";
}

std::string strip_tag(const std::string& sym) {
    auto p = sym.rfind(':');
    return p == std::string::npos ? sym : sym.substr(0, p);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle("OVL Extractor");
    resize(1100, 700);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* file_menu = menuBar()->addMenu("&File");
    auto* open_act = file_menu->addAction("&Open OVL...");
    open_act->setShortcut(QKeySequence::Open);
    connect(open_act, &QAction::triggered, this, &MainWindow::onOpenTriggered);

    auto* extract_act = file_menu->addAction("&Extract all to folder...");
    extract_act->setShortcut(QKeySequence("Ctrl+E"));
    connect(extract_act, &QAction::triggered, this, &MainWindow::onExtractAll);

    file_menu->addSeparator();
    auto* quit_act = file_menu->addAction("&Quit");
    quit_act->setShortcut(QKeySequence::Quit);
    connect(quit_act, &QAction::triggered, qApp, &QApplication::quit);

    auto* tb = addToolBar("Main");
    tb->setMovable(false);
    tb->addAction(open_act);
    tb->addAction(extract_act);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    tree_ = new QTreeWidget(splitter);
    tree_->setHeaderLabels({"Resource", "Tag", "Side"});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree_->setMinimumWidth(320);
    connect(tree_, &QTreeWidget::itemSelectionChanged,
            this, &MainWindow::onTreeSelection);

    stack_ = new QStackedWidget(splitter);

    // Page 0: empty welcome.
    empty_ = new QLabel("Open an OVL file (File → Open or ⌘O).");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setStyleSheet("QLabel { color: #666; font-size: 14px; }");
    stack_->addWidget(empty_);

    // Page 1: details panel with optional "Play" button.
    {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(8, 8, 8, 8);
        details_ = new QTextEdit;
        details_->setReadOnly(true);
        details_->setStyleSheet("QTextEdit { font-family: ui-monospace, Menlo, monospace; }");
        v->addWidget(details_, 1);
        play_ = new QPushButton("Play / Open externally");
        play_->setVisible(false);
        connect(play_, &QPushButton::clicked, this, &MainWindow::onPlaySound);
        v->addWidget(play_);
        stack_->addWidget(w);
    }

    // Page 2: texture preview.
    texture_ = new TexturePreview;
    stack_->addWidget(texture_);

    // Page 3: 3D mesh viewer.
    mesh_ = new MeshViewer;
    stack_->addWidget(mesh_);

    splitter->addWidget(tree_);
    splitter->addWidget(stack_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({340, 760});

    setCentralWidget(splitter);
    statusBar()->showMessage("Ready — open an .ovl file");
    selectPage(PageEmpty);
}

bool MainWindow::openOvl(const QString& path) {
    auto p = std::make_unique<ovl::OvlParser>();
    try {
        p->parse(path.toStdString());
    } catch (const ovl::OvlError& e) {
        QMessageBox::critical(this, "Parse error", e.what());
        return false;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Parse error", e.what());
        return false;
    }

    parser_    = std::move(p);
    ovl_path_  = std::filesystem::path(path.toStdString());
    inventory_ = OvlInventory::build(*parser_);
    have_selection_ = false;
    rebuildTree();
    selectPage(PageEmpty);
    setWindowTitle(QString("OVL Extractor — %1")
                       .arg(QString::fromStdString(ovl_path_.filename().string())));
    statusBar()->showMessage(
        QString("%1  ·  %2 resources  ·  sides: %3")
            .arg(QString::fromStdString(ovl_path_.string()))
            .arg(inventory_.total_resources())
            .arg(inventory_.has_unique ? "common+unique" : "common only"));
    return true;
}

void MainWindow::rebuildTree() {
    tree_->clear();
    for (std::size_t ci = 0; ci < static_cast<std::size_t>(Category::_Count); ++ci) {
        auto c = static_cast<Category>(ci);
        const auto& items = inventory_.by_category[ci];
        if (items.empty()) continue;
        auto* root = new QTreeWidgetItem(tree_);
        root->setText(0, QString("%1  (%2)")
                             .arg(category_label(c)).arg(items.size()));
        root->setExpanded(true);
        QFont f = root->font(0);
        f.setBold(true);
        root->setFont(0, f);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            auto* leaf = new QTreeWidgetItem(root);
            std::string display = strip_tag(it.symbol);
            if (display.empty()) display = "(unnamed)";
            leaf->setText(0, QString::fromStdString(display));
            leaf->setText(1, QString::fromStdString(it.tag));
            leaf->setText(2, human_side(it.side));
            leaf->setData(0, Qt::UserRole, static_cast<int>(ci));
            leaf->setData(0, Qt::UserRole + 1, static_cast<int>(i));
        }
    }
}

void MainWindow::selectPage(int page) {
    stack_->setCurrentIndex(page);
}

void MainWindow::onTreeSelection() {
    auto items = tree_->selectedItems();
    if (items.empty()) { have_selection_ = false; selectPage(PageEmpty); return; }
    QTreeWidgetItem* it = items.first();
    QVariant v = it->data(0, Qt::UserRole);
    if (!v.isValid()) {
        // Category header selected — show a summary.
        have_selection_ = false;
        selectPage(PageEmpty);
        return;
    }
    int ci = v.toInt();
    int ri = it->data(0, Qt::UserRole + 1).toInt();
    if (ci < 0 || ci >= static_cast<int>(Category::_Count)) return;
    const auto& vec = inventory_.by_category[ci];
    if (ri < 0 || ri >= static_cast<int>(vec.size())) return;
    current_ = vec[ri];
    have_selection_ = true;

    showDetails(current_);

    switch (current_.category) {
        case Category::Texture:
            texture_->loadFromOvl(*parser_, current_.symbol);
            selectPage(PageTexture);
            break;
        case Category::Model:
            if (mesh_->loadFromOvl(*parser_, ovl_path_, current_.symbol)) {
                selectPage(PageMesh);
            } else {
                details_->append("\n[mesh] could not extract / load this model "
                                 "(possibly mms — positions still WIP).");
                play_->setVisible(false);
                selectPage(PageDetails);
            }
            break;
        case Category::Sound:
            play_->setVisible(true);
            play_->setText("▶  Play (open externally)");
            selectPage(PageDetails);
            break;
        default:
            play_->setVisible(false);
            selectPage(PageDetails);
            break;
    }
}

void MainWindow::showDetails(const ResourceItem& item) {
    std::ostringstream os;
    os << "Symbol     : " << item.symbol << "\n"
       << "Loader tag : " << item.tag << "\n"
       << "Loader name: " << item.loader_name << "\n"
       << "Side       : " << (item.side == ovl::OvlSide::Common ? "common" : "unique") << "\n"
       << "Category   : " << category_label(item.category) << "\n";
    details_->setPlainText(QString::fromStdString(os.str()));
    play_->setVisible(false);
}

void MainWindow::onOpenTriggered() {
    QString start = QString::fromStdString(ovl_path_.parent_path().string());
    if (start.isEmpty()) {
        start = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    QString path = QFileDialog::getOpenFileName(
        this, "Open OVL", start,
        "OVL files (*.ovl *.common.ovl *.unique.ovl);;All files (*)");
    if (path.isEmpty()) return;
    openOvl(path);
}

void MainWindow::onExtractAll() {
    if (!parser_) {
        QMessageBox::information(this, "Extract", "Open an OVL first.");
        return;
    }
    QString dir = QFileDialog::getExistingDirectory(
        this, "Extract to folder",
        QString::fromStdString(ovl_path_.parent_path().string()));
    if (dir.isEmpty()) return;

    std::filesystem::path out_root = std::filesystem::path(dir.toStdString()) /
                                     ovl_path_.stem().string();
    std::error_code ec;
    std::filesystem::create_directories(out_root, ec);

    ovl::ExtractContext ctx;
    ctx.output_dir = out_root;
    ctx.overwrite  = true;
    ctx.log = [](std::string_view){};

    ovl::ExtractResult totals{};
    auto run = [&](auto& extractor) {
        auto r = extractor.extract(*parser_, ctx);
        totals.files_written += r.files_written;
        totals.errors        += r.errors;
    };

    ovl::SoundExtractor snd;   run(snd);
    ovl::TextureExtractor tex; run(tex);
    ovl::AtlasExtractor atl;   run(atl);
    ovl::ModelExtractor mdl;   run(mdl);

    QMessageBox::information(
        this, "Extract",
        QString("Wrote %1 file(s) to %2  (%3 error(s))")
            .arg(totals.files_written)
            .arg(QString::fromStdString(out_root.string()))
            .arg(totals.errors));
}

void MainWindow::onPlaySound() {
    if (!have_selection_ || current_.category != Category::Sound) return;
    if (!parser_) return;

    QString tmp_root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(tmp_root);
    dir.mkpath("ovlextract_gui/sounds");
    std::filesystem::path out = std::filesystem::path(
        dir.absoluteFilePath("ovlextract_gui/sounds").toStdString());

    ovl::ExtractContext ctx;
    ctx.output_dir = out;
    ctx.overwrite  = true;
    ctx.log = [](std::string_view){};

    ovl::SoundExtractor snd;
    auto before = std::filesystem::file_time_type{};
    (void)snd.extract(*parser_, ctx);  // extracts all .wav; cheap (one OVL)

    // Pick the .wav matching our symbol's basename.
    std::string base = strip_tag(current_.symbol);
    // Match SoundExtractor's sanitisation (it strips problematic chars too).
    std::string candidate = base;
    for (auto& c : candidate) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    auto wav = out / (candidate + ".wav");
    if (!std::filesystem::exists(wav)) {
        // Fallback: newest .wav in the directory.
        std::error_code ec;
        std::filesystem::path best;
        std::filesystem::file_time_type best_time = before;
        for (auto& e : std::filesystem::directory_iterator(out, ec)) {
            if (ec || !e.is_regular_file()) continue;
            auto name = e.path().filename().string();
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
            auto t = std::filesystem::last_write_time(e.path(), ec);
            if (ec) continue;
            if (best.empty() || t > best_time) { best = e.path(); best_time = t; }
        }
        wav = best;
    }
    if (wav.empty() || !std::filesystem::exists(wav)) {
        QMessageBox::warning(this, "Sound", "Could not extract this sound.");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(wav.string())));
}

}  // namespace ovlgui
