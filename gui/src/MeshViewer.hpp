#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QVector3D>
#include <filesystem>
#include <string>
#include <vector>

namespace ovl { class OvlParser; }

namespace ovlgui {

// Lightweight 3D viewer for the .obj files our ModelExtractor produces.
// Renders flat-lit triangles. No materials, no textures (yet) — the goal is
// to confirm the mesh decoded coherently. Mouse drag rotates the view,
// the scroll wheel zooms.
class MeshViewer : public QOpenGLWidget,
                   protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit MeshViewer(QWidget* parent = nullptr);
    ~MeshViewer() override;

    // Extract every shs in `parser` to a per-OVL temp dir (lazily, once per
    // `ovl_path`), then load the .obj that matches `symbol` and display it.
    bool loadFromOvl(const ovl::OvlParser& parser,
                     const std::filesystem::path& ovl_path,
                     const std::string& symbol);

    // Reset to the empty placeholder.
    void clear();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    bool loadObj(const std::filesystem::path& path);
    void uploadMesh();
    void resetView();

    // Vertex stream uploaded to the GPU. Positions are computed from the
    // raw OBJ verts; normals are computed per face and averaged per vertex
    // because RCT3 .obj output doesn't always include vn lines.
    struct Vertex {
        float pos[3];
        float nrm[3];
    };

    std::vector<Vertex>        verts_;
    std::vector<std::uint32_t> indices_;
    bool                       has_mesh_ = false;
    bool                       gl_ready_ = false;

    // View state.
    QVector3D model_center_{0, 0, 0};
    float     model_radius_ = 1.0f;
    float     yaw_   = 0.5f;   // radians
    float     pitch_ = 0.4f;
    float     dist_  = 3.0f;
    QPoint    last_mouse_;

    // GL resources.
    QOpenGLShaderProgram      program_;
    QOpenGLVertexArrayObject  vao_;
    QOpenGLBuffer             vbo_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer             ibo_{QOpenGLBuffer::IndexBuffer};
    int                       u_mvp_  = -1;
    int                       u_normal_ = -1;
    int                       u_color_  = -1;

    // Cache: skip the ModelExtractor pass when re-clicking on a different
    // symbol of the same OVL we already extracted.
    std::filesystem::path     extracted_for_;
    std::filesystem::path     extract_dir_;
};

}  // namespace ovlgui
