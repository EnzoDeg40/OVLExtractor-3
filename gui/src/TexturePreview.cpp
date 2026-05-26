#include "TexturePreview.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/extract/TextureExtractor.hpp"
#include "ovl/extract/IResourceExtractor.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ovlgui {

namespace {

std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Tiny reader for the uncompressed 32-bit BGRA TGAs that our TextureExtractor
// writes. We assume header type 2, 32 bpp, descriptor byte with top-left
// origin (0x28). Returns an empty QImage on any mismatch.
QImage load_bgra_tga(const std::filesystem::path& path) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray data = f.readAll();
    if (data.size() < 18) return {};

    const auto* p = reinterpret_cast<const std::uint8_t*>(data.constData());
    const std::uint8_t id_len   = p[0];
    const std::uint8_t cmap_t   = p[1];
    const std::uint8_t img_type = p[2];
    const int width   = static_cast<int>(p[12] | (p[13] << 8));
    const int height  = static_cast<int>(p[14] | (p[15] << 8));
    const std::uint8_t bpp      = p[16];
    const std::uint8_t desc     = p[17];

    if (img_type != 2 || cmap_t != 0 || bpp != 32 || width <= 0 || height <= 0) return {};
    const std::size_t pix_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const std::size_t need = 18u + id_len + pix_bytes;
    if (static_cast<std::size_t>(data.size()) < need) return {};

    const std::uint8_t* px = p + 18u + id_len;
    QImage img(width, height, QImage::Format_ARGB32);
    const bool top_origin = (desc & 0x20) != 0;
    for (int y = 0; y < height; ++y) {
        const int src_y = top_origin ? y : (height - 1 - y);
        const std::uint8_t* row = px + static_cast<std::size_t>(src_y) * static_cast<std::size_t>(width) * 4u;
        auto* dst = reinterpret_cast<std::uint8_t*>(img.scanLine(y));
        // BGRA -> Qt ARGB32 little-endian (B,G,R,A in memory) — passthrough.
        std::memcpy(dst, row, static_cast<std::size_t>(width) * 4u);
    }
    return img;
}

std::filesystem::path session_temp_dir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(base);
    dir.mkpath("ovlextract_gui");
    return std::filesystem::path(dir.absoluteFilePath("ovlextract_gui").toStdString());
}

}  // namespace

TexturePreview::TexturePreview(QWidget* parent)
    : QWidget(parent),
      image_(new QLabel(this)),
      status_(new QLabel(this)) {
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumSize(256, 256);
    image_->setStyleSheet("QLabel { background: #222; color: #888; }");
    image_->setText("Select a texture in the tree");

    status_->setStyleSheet("QLabel { color: #444; }");
    status_->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(image_, /*stretch=*/1);
    layout->addWidget(status_);
}

void TexturePreview::clear() {
    source_ = QPixmap();
    image_->setPixmap(QPixmap());
    image_->setText("Select a texture in the tree");
    status_->clear();
}

void TexturePreview::showMessage(const QString& msg) {
    source_ = QPixmap();
    image_->setPixmap(QPixmap());
    image_->setText(msg);
    status_->clear();
}

void TexturePreview::loadFromOvl(const ovl::OvlParser& parser, const std::string& symbol) {
    auto tmp = session_temp_dir();
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);

    ovl::ExtractContext ctx;
    ctx.output_dir = tmp;
    ctx.overwrite = true;
    ctx.log = [](std::string_view){};

    const std::string sym_lc = to_lower_copy(symbol);
    const bool ok = ovl::TextureExtractor::extract_symbol(parser, sym_lc, ctx);
    if (!ok) {
        showMessage("Could not extract texture '" + QString::fromStdString(symbol) + "'");
        return;
    }

    // The extractor strips the loader-tag suffix (e.g. "Dice:ftx" -> "Dice")
    // and writes "<base>.tga". We don't duplicate that logic here — instead
    // we pick the newest .tga in the temp dir (the one we just wrote).
    std::filesystem::path best;
    std::filesystem::file_time_type best_time{};
    for (auto& e : std::filesystem::directory_iterator(tmp, ec)) {
        if (ec || !e.is_regular_file()) continue;
        auto name = e.path().filename().string();
        if (name.size() < 4) continue;
        if (name.compare(name.size() - 4, 4, ".tga") != 0) continue;
        auto t = std::filesystem::last_write_time(e.path(), ec);
        if (ec) continue;
        if (best.empty() || t > best_time) {
            best = e.path();
            best_time = t;
        }
    }
    if (best.empty()) {
        showMessage("'" + QString::fromStdString(symbol) +
                    "' extracted (raw block only — format not previewable yet)");
        return;
    }

    setImage(best, symbol);
}

void TexturePreview::setImage(const std::filesystem::path& tga_path,
                              const std::string& symbol) {
    QImage img = load_bgra_tga(tga_path);
    if (img.isNull()) {
        showMessage("Failed to load preview from " +
                    QString::fromStdString(tga_path.filename().string()));
        return;
    }
    source_ = QPixmap::fromImage(img);
    image_->setText(QString());
    rescale();
    status_->setText(QString::fromStdString(symbol) + "  —  " +
                     QString::number(img.width()) + " × " +
                     QString::number(img.height()) + " px  ·  " +
                     QString::fromStdString(tga_path.filename().string()));
}

void TexturePreview::rescale() {
    if (source_.isNull()) return;
    QPixmap scaled = source_.scaled(image_->size(), Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
    image_->setPixmap(scaled);
}

void TexturePreview::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    rescale();
}

}  // namespace ovlgui
