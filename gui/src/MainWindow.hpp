#pragma once

#include "OvlInventory.hpp"
#include "ovl/OvlParser.hpp"

#include <QMainWindow>
#include <filesystem>
#include <memory>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace ovlgui {

class TexturePreview;
class MeshViewer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openOvl(const QString& path);

private slots:
    void onOpenTriggered();
    void onTreeSelection();
    void onExtractAll();
    void onPlaySound();

private:
    void buildUi();
    void rebuildTree();
    void selectPage(int page);
    void showDetails(const ResourceItem& item);

    // Right-pane stacked pages.
    enum Page { PageEmpty = 0, PageDetails = 1, PageTexture = 2, PageMesh = 3 };

    std::unique_ptr<ovl::OvlParser> parser_;
    std::filesystem::path           ovl_path_;
    OvlInventory                    inventory_;

    QTreeWidget*    tree_   = nullptr;
    QStackedWidget* stack_  = nullptr;
    QLabel*         empty_  = nullptr;
    QTextEdit*      details_ = nullptr;
    QPushButton*    play_   = nullptr;
    TexturePreview* texture_ = nullptr;
    MeshViewer*     mesh_    = nullptr;

    // Current selection (used by Play and Extract Selected).
    bool         have_selection_ = false;
    ResourceItem current_;
};

}  // namespace ovlgui
