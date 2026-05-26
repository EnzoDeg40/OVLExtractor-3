#include "MeshViewer.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/extract/ModelExtractor.hpp"
#include "ovl/extract/IResourceExtractor.hpp"

#include <QDir>
#include <QFile>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLShader>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ovlgui {

namespace {

// Mirror of ModelExtractor::sanitize — keep in sync if extract changes it.
std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

std::string strip_tag(const std::string& sym) {
    auto p = sym.rfind(':');
    return p == std::string::npos ? sym : sym.substr(0, p);
}

std::filesystem::path session_temp_root() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(base);
    dir.mkpath("ovlextract_gui/meshes");
    return std::filesystem::path(
        dir.absoluteFilePath("ovlextract_gui/meshes").toStdString());
}

constexpr const char* kVertSrc = R"(#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_nrm;
uniform mat4 u_mvp;
uniform mat3 u_normal;
out vec3 v_nrm;
void main() {
    v_nrm = u_normal * a_nrm;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

constexpr const char* kFragSrc = R"(#version 330 core
in vec3 v_nrm;
uniform vec3 u_color;
out vec4 fragColor;
void main() {
    vec3 L = normalize(vec3(0.5, 0.8, 0.4));
    float ndotl = max(dot(normalize(v_nrm), L), 0.0);
    vec3 c = u_color * (0.25 + 0.75 * ndotl);
    fragColor = vec4(c, 1.0);
}
)";

}  // namespace

MeshViewer::MeshViewer(QWidget* parent) : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    setFormat(fmt);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
}

MeshViewer::~MeshViewer() {
    if (gl_ready_) {
        makeCurrent();
        vbo_.destroy();
        ibo_.destroy();
        vao_.destroy();
        doneCurrent();
    }
}

void MeshViewer::clear() {
    has_mesh_ = false;
    verts_.clear();
    indices_.clear();
    if (gl_ready_) update();
}

bool MeshViewer::loadFromOvl(const ovl::OvlParser& parser,
                             const std::filesystem::path& ovl_path,
                             const std::string& symbol) {
    if (extracted_for_ != ovl_path) {
        // Stable per-OVL subdir under the session temp root so re-opens of
        // the same OVL are free. Use a name derived from the stem.
        auto stem = ovl_path.stem().string();
        if (stem.size() > 80) stem.resize(80);
        extract_dir_ = session_temp_root() / sanitize(stem);
        std::error_code ec;
        std::filesystem::create_directories(extract_dir_, ec);

        ovl::ExtractContext ctx;
        ctx.output_dir = extract_dir_;
        ctx.overwrite = true;
        ctx.log = [](std::string_view){};

        ovl::ModelExtractor m;
        (void)m.extract(parser, ctx);
        extracted_for_ = ovl_path;
    }

    std::string base = sanitize(strip_tag(symbol));
    auto obj = extract_dir_ / (base + ".obj");
    if (!std::filesystem::exists(obj)) {
        clear();
        return false;
    }
    return loadObj(obj);
}

bool MeshViewer::loadObj(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::vector<QVector3D> positions;
    struct FaceIdx { int v; };  // we only need vertex indices
    std::vector<std::array<int, 3>> tris;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string kw;
        ss >> kw;
        if (kw == "v") {
            float x, y, z;
            if (ss >> x >> y >> z) positions.emplace_back(x, y, z);
        } else if (kw == "f") {
            std::vector<int> face_v;
            std::string tok;
            while (ss >> tok) {
                int vi = 0;
                // formats: "v", "v/vt", "v/vt/vn", "v//vn"
                auto slash = tok.find('/');
                std::string vs = (slash == std::string::npos) ? tok : tok.substr(0, slash);
                try { vi = std::stoi(vs); } catch (...) { vi = 0; }
                if (vi < 0) vi = static_cast<int>(positions.size()) + vi + 1;  // negative refs
                face_v.push_back(vi);
            }
            if (face_v.size() >= 3) {
                // Fan triangulation for N-gons.
                for (std::size_t i = 1; i + 1 < face_v.size(); ++i) {
                    tris.push_back({face_v[0], face_v[i], face_v[i + 1]});
                }
            }
        }
        // Ignore vn / vt / g / usemtl / mtllib / o — we recompute normals.
    }

    if (positions.empty() || tris.empty()) return false;

    // Compute per-vertex normals by face-area-weighted accumulation.
    std::vector<QVector3D> normals(positions.size(), QVector3D{0, 0, 0});
    for (const auto& t : tris) {
        int a = t[0] - 1, b = t[1] - 1, c = t[2] - 1;
        if (a < 0 || b < 0 || c < 0) continue;
        if (a >= static_cast<int>(positions.size()) ||
            b >= static_cast<int>(positions.size()) ||
            c >= static_cast<int>(positions.size())) continue;
        QVector3D e1 = positions[b] - positions[a];
        QVector3D e2 = positions[c] - positions[a];
        QVector3D n = QVector3D::crossProduct(e1, e2);  // length = 2*area
        normals[a] += n;
        normals[b] += n;
        normals[c] += n;
    }

    verts_.clear();
    verts_.reserve(positions.size());
    QVector3D bb_min = positions[0], bb_max = positions[0];
    for (std::size_t i = 0; i < positions.size(); ++i) {
        QVector3D n = normals[i];
        if (n.lengthSquared() > 0) n.normalize(); else n = QVector3D{0, 1, 0};
        Vertex v;
        v.pos[0] = positions[i].x(); v.pos[1] = positions[i].y(); v.pos[2] = positions[i].z();
        v.nrm[0] = n.x();           v.nrm[1] = n.y();           v.nrm[2] = n.z();
        verts_.push_back(v);
        bb_min.setX(std::min(bb_min.x(), positions[i].x()));
        bb_min.setY(std::min(bb_min.y(), positions[i].y()));
        bb_min.setZ(std::min(bb_min.z(), positions[i].z()));
        bb_max.setX(std::max(bb_max.x(), positions[i].x()));
        bb_max.setY(std::max(bb_max.y(), positions[i].y()));
        bb_max.setZ(std::max(bb_max.z(), positions[i].z()));
    }

    indices_.clear();
    indices_.reserve(tris.size() * 3);
    for (const auto& t : tris) {
        int a = t[0] - 1, b = t[1] - 1, c = t[2] - 1;
        if (a < 0 || b < 0 || c < 0) continue;
        if (a >= static_cast<int>(verts_.size()) ||
            b >= static_cast<int>(verts_.size()) ||
            c >= static_cast<int>(verts_.size())) continue;
        indices_.push_back(static_cast<std::uint32_t>(a));
        indices_.push_back(static_cast<std::uint32_t>(b));
        indices_.push_back(static_cast<std::uint32_t>(c));
    }

    model_center_ = (bb_min + bb_max) * 0.5f;
    model_radius_ = std::max((bb_max - bb_min).length() * 0.5f, 0.001f);
    resetView();
    has_mesh_ = true;

    if (gl_ready_) {
        makeCurrent();
        uploadMesh();
        doneCurrent();
        update();
    }
    return true;
}

void MeshViewer::resetView() {
    yaw_   = 0.6f;
    pitch_ = 0.4f;
    dist_  = model_radius_ * 2.8f;
}

void MeshViewer::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc);
    program_.link();
    u_mvp_    = program_.uniformLocation("u_mvp");
    u_normal_ = program_.uniformLocation("u_normal");
    u_color_  = program_.uniformLocation("u_color");

    vao_.create();
    vbo_.create();
    ibo_.create();
    gl_ready_ = true;

    if (has_mesh_) uploadMesh();
}

void MeshViewer::uploadMesh() {
    if (verts_.empty() || indices_.empty()) return;
    vao_.bind();

    vbo_.bind();
    vbo_.allocate(verts_.data(),
                  static_cast<int>(verts_.size() * sizeof(Vertex)));

    program_.enableAttributeArray(0);
    program_.setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(Vertex));
    program_.enableAttributeArray(1);
    program_.setAttributeBuffer(1, GL_FLOAT, offsetof(Vertex, nrm), 3, sizeof(Vertex));

    ibo_.bind();
    ibo_.allocate(indices_.data(),
                  static_cast<int>(indices_.size() * sizeof(std::uint32_t)));

    vao_.release();
    vbo_.release();
    ibo_.release();
}

void MeshViewer::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void MeshViewer::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!has_mesh_ || verts_.empty() || indices_.empty()) return;

    const float aspect = (height() > 0) ? float(width()) / float(height()) : 1.0f;
    QMatrix4x4 proj;
    proj.perspective(45.0f, aspect, model_radius_ * 0.01f, model_radius_ * 100.0f);

    QMatrix4x4 view;
    float cy = std::cos(yaw_),   sy = std::sin(yaw_);
    float cp = std::cos(pitch_), sp = std::sin(pitch_);
    QVector3D eye = QVector3D(cy * cp, sp, sy * cp) * dist_ + model_center_;
    view.lookAt(eye, model_center_, QVector3D(0, 1, 0));

    QMatrix4x4 mvp = proj * view;
    QMatrix3x3 nrm = view.normalMatrix();

    program_.bind();
    program_.setUniformValue(u_mvp_, mvp);
    program_.setUniformValue(u_normal_, nrm);
    program_.setUniformValue(u_color_, QVector3D(0.78f, 0.82f, 0.88f));

    vao_.bind();
    glDrawElements(GL_TRIANGLES, static_cast<int>(indices_.size()), GL_UNSIGNED_INT, nullptr);
    vao_.release();
    program_.release();
}

void MeshViewer::mousePressEvent(QMouseEvent* e) {
    last_mouse_ = e->pos();
}

void MeshViewer::mouseMoveEvent(QMouseEvent* e) {
    QPoint d = e->pos() - last_mouse_;
    last_mouse_ = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        yaw_   += d.x() * 0.01f;
        pitch_ += d.y() * 0.01f;
        const float lim = 1.55f;
        if (pitch_ >  lim) pitch_ =  lim;
        if (pitch_ < -lim) pitch_ = -lim;
        update();
    }
}

void MeshViewer::wheelEvent(QWheelEvent* e) {
    const float steps = e->angleDelta().y() / 120.0f;
    dist_ *= std::pow(0.9f, steps);
    if (dist_ < model_radius_ * 0.2f) dist_ = model_radius_ * 0.2f;
    if (dist_ > model_radius_ * 50.0f) dist_ = model_radius_ * 50.0f;
    update();
}

}  // namespace ovlgui
