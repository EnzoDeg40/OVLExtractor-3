#pragma once

#include <QPixmap>
#include <QWidget>
#include <filesystem>
#include <string>

class QLabel;

namespace ovl { class OvlParser; }

namespace ovlgui {

// Right-pane widget that decodes one texture from an open OVL and shows
// the resulting image. We extract to an in-process temp dir (via
// TextureExtractor::extract_symbol) and load the .tga with a tiny built-in
// reader — Qt's qtga plugin ships in qtimageformats which is optional.
class TexturePreview : public QWidget {
    Q_OBJECT
public:
    explicit TexturePreview(QWidget* parent = nullptr);

    void clear();
    void showMessage(const QString& msg);

    // Extract the linked file `symbol` from `parser` to a temp dir and show
    // the resulting image. On failure (no .tga produced, decode failed) we
    // fall back to a status message and the original symbol/dimensions.
    void loadFromOvl(const ovl::OvlParser& parser, const std::string& symbol);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void setImage(const std::filesystem::path& tga_path,
                  const std::string& symbol);
    void rescale();

    QLabel* image_;
    QLabel* status_;
    QPixmap source_;
};

}  // namespace ovlgui
