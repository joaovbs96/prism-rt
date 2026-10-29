// Incremental Scene::commit (include/prism/scene.h): each dirty kind rebuilds
// only what it should (CommitResult counters), the spare revision is updated
// in place only when nothing holds it, edits then a render match a full
// rebuild bit for bit, a progressive render sees one revision at a time while
// commits run, a stress of rapid edits, and no deadlock between commits,
// batch renders and progressive sessions of one scene. CPU, the core only.
//   prism_scene_incremental_test
#include <prism/renderer.h>
#include <prism/scene.h>
#include <prism/session.h>

#include "../src/scene.h"
#include "../src/scene/scene_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <random>
#include <string>
#include <thread>

namespace {

int failures = 0;
void note(std::string const& message) { std::cout << message << std::endl; }
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

template <class F>
void within(double seconds, std::string const& what, F&& f)
{
    auto job = std::async(std::launch::async, std::forward<F>(f));
    if (job.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) {
        std::cerr << "FAILED: " << what << " did not finish within " << seconds << " s (deadlock?)\n";
        std::_Exit(1);
    }
    job.get();
}

bool eventually(std::function<bool()> const& condition, double seconds = 60.0)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

prism::Matrix4d translate(double x, double y, double z)
{
    prism::Matrix4d m;
    m[3][0] = x; m[3][1] = y; m[3][2] = z;
    return m;
}

prism::Matrix4d scaled(double s, double x, double y, double z)
{
    prism::Matrix4d m = translate(x, y, z);
    m[0][0] = m[1][1] = m[2][2] = s;
    return m;
}

// A quad at z = 0 facing +Z of half size `h`: a polygon mesh or a triangle list.
prism::MeshDesc quad(bool triangles, float h)
{
    auto points = std::make_shared<std::vector<float>>(std::vector<float>{-h, -h, 0, h, -h, 0, h, h, 0, -h, h, 0});
    static const std::vector<int32_t> counts{4}, polygon{0, 1, 2, 3}, list{0, 1, 2, 0, 2, 3};
    prism::MeshDesc mesh;
    prism::BufferView p = prism::BufferView::Of(*points, 3, prism::DataType::Float32);
    p.owner = points;  // the description outlives this function
    mesh.points = prism::Constant(p);
    if (triangles) {
        mesh.faceVertexIndices = prism::BufferView::Of(list, 1, prism::DataType::Int32);
    } else {
        mesh.faceVertexCounts = prism::BufferView::Of(counts, 1, prism::DataType::Int32);
        mesh.faceVertexIndices = prism::BufferView::Of(polygon, 1, prism::DataType::Int32);
    }
    return mesh;
}

prism::SpheresDesc spheres(float radius)
{
    static const std::vector<float> centers{0, 0, 0};
    auto radii = std::make_shared<std::vector<float>>(std::vector<float>{radius});
    prism::SpheresDesc desc;
    desc.centers = prism::Constant(prism::BufferView::Of(centers, 3, prism::DataType::Float32));
    desc.radii = prism::BufferView::Of(*radii, 1, prism::DataType::Float32);
    desc.radii.owner = radii;
    return desc;
}

// ── A scene's state, built fresh or reached by edits.
struct State {
    prism::Matrix4d single = translate(-2.5, 1, 0);  // the polygon quad's only instance (world space)
    prism::Matrix4d tri1 = translate(0, 1, 0), tri2 = translate(2.5, 1, 0);  // two instances of the triangle list
    prism::Matrix4d sphere = scaled(0.8, 0, -1.5, 0);
    bool tri2Visible = true;
    int singleMaterial = 0;    // 0 red, 1 green
    int tri1Material = 1;      // 0 red, 1 green, 2 glow
    Color3 red{0.8f, 0.1f, 0.1f};
    float triSize = 1.0f;      // the triangle list's half size
    float sphereRadius = 1.0f;
    float lightIntensity = 10.0f;
    Color3 environment{0.25f, 0.5f, 0.75f};
    bool extra = false;        // a second instance of the polygon quad
};

struct Built {
    prism::Scene scene;
    prism::MeshId polygon, triangles;
    prism::SpheresId sphere;
    prism::MaterialId red, green, glow;
    prism::InstanceId single, tri1, tri2, ball, extra;
    prism::LightId light;
    prism::EnvironmentId environment;
    prism::CameraId camera;
};

prism::MaterialDesc diffuse(Color3 color)
{
    prism::MaterialDesc desc;
    desc.color = color;
    return desc;
}

prism::LightDesc light(float intensity)
{
    prism::LightDesc desc;
    desc.transform = prism::Constant(translate(0, 0, 4));
    desc.intensity = intensity;
    return desc;
}

prism::EnvironmentDesc environment(Color3 color)
{
    prism::EnvironmentDesc desc;
    desc.kind = prism::EnvironmentDesc::Kind::Constant;
    desc.color = color;
    return desc;
}

prism::InstanceDesc instance(prism::MeshId mesh, prism::Matrix4d const& world, prism::MaterialId material, int primId)
{
    prism::InstanceDesc desc;
    desc.prototype = mesh;
    desc.transform = prism::Constant(world);
    desc.materials = {material};
    desc.primId = primId;
    return desc;
}

Built build(prism::Renderer const& renderer, State const& s)
{
    Built b{renderer.createScene()};
    prism::Scene& scene = b.scene;
    b.red = scene.addMaterial(diffuse(s.red));
    b.green = scene.addMaterial(diffuse(Color3(0.1f, 0.7f, 0.2f)));
    prism::MaterialDesc glow;
    glow.kind = prism::MaterialDesc::Kind::Emissive;
    glow.color = Color3(2.0f, 1.5f, 0.5f);
    b.glow = scene.addMaterial(glow);
    auto material = [&](int i) { return i == 0 ? b.red : i == 1 ? b.green : b.glow; };
    b.polygon = scene.addMesh(quad(false, 1.0f));
    b.triangles = scene.addMesh(quad(true, s.triSize));
    b.sphere = scene.addSpheres(spheres(s.sphereRadius));
    b.single = scene.addInstance(instance(b.polygon, s.single, material(s.singleMaterial), 1));
    b.tri1 = scene.addInstance(instance(b.triangles, s.tri1, material(s.tri1Material), 2));
    prism::InstanceDesc second = instance(b.triangles, s.tri2, b.red, 3);
    second.visible = s.tri2Visible;
    b.tri2 = scene.addInstance(second);
    prism::InstanceDesc ball;
    ball.prototype = b.sphere;
    ball.transform = prism::Constant(s.sphere);
    ball.materials = {b.green};
    ball.primId = 4;
    b.ball = scene.addInstance(ball);
    if (s.extra) b.extra = scene.addInstance(instance(b.polygon, translate(-2.5, -1.5, 0), b.red, 5));
    b.light = scene.addLight(light(s.lightIntensity));
    b.environment = scene.addEnvironment(environment(s.environment));
    prism::CameraDesc camera;
    camera.cameraToWorld = prism::Constant(translate(0, 0, 8));
    camera.focalLength = 20;
    camera.nearClip = 0.1f;
    camera.farClip = 100.0f;
    b.camera = scene.addCamera(camera);
    return b;
}

bool same(Color3 const& a, Color3 const& b) { return a.x() == b.x() && a.y() == b.y() && a.z() == b.z(); }

// The edits from `a` to `z` (z differs in a few fields).
void apply(Built& b, State const& a, State const& z)
{
    prism::Scene& scene = b.scene;
    auto material = [&](int i) { return i == 0 ? b.red : i == 1 ? b.green : b.glow; };
    if (!(a.single == z.single)) scene.setTransform(b.single, prism::Constant(z.single));
    if (!(a.tri1 == z.tri1)) scene.setTransform(b.tri1, prism::Constant(z.tri1));
    if (!(a.tri2 == z.tri2)) scene.setTransform(b.tri2, prism::Constant(z.tri2));
    if (!(a.sphere == z.sphere)) scene.setTransform(b.ball, prism::Constant(z.sphere));
    if (a.tri2Visible != z.tri2Visible) scene.setVisible(b.tri2, z.tri2Visible);
    if (a.singleMaterial != z.singleMaterial) scene.setMaterials(b.single, {material(z.singleMaterial)});
    if (a.tri1Material != z.tri1Material) scene.setMaterials(b.tri1, {material(z.tri1Material)});
    if (!same(a.red, z.red)) scene.updateMaterial(b.red, diffuse(z.red));
    if (a.triSize != z.triSize) scene.updateMesh(b.triangles, quad(true, z.triSize));
    if (a.sphereRadius != z.sphereRadius) scene.updateSpheres(b.sphere, spheres(z.sphereRadius));
    if (a.lightIntensity != z.lightIntensity) scene.updateLight(b.light, light(z.lightIntensity));
    if (!same(a.environment, z.environment)) scene.updateEnvironment(b.environment, environment(z.environment));
    if (a.extra != z.extra) {
        if (z.extra) b.extra = scene.addInstance(instance(b.polygon, translate(-2.5, -1.5, 0), b.red, 5));
        else scene.remove(b.extra);
    }
}

prism::SessionOptions batchOptions(prism::CameraId camera)
{
    prism::SessionOptions options;
    options.device = prism::DevicePreference::Cpu;
    options.width = 48;
    options.height = 32;
    options.camera = camera;
    options.aovs = {prism::Aov::Color, prism::Aov::Depth, prism::Aov::PrimId, prism::Aov::InstanceId};
    options.settings.samples = 4;
    options.settings.adaptiveThreshold = 0.0f;
    return options;
}

bool sameImage(prism::Image const& a, prism::Image const& b)
{
    if (a.empty() || b.empty() || a.valueCount() != b.valueCount() || a.type() != b.type()) return false;
    if (a.type() == prism::DataType::Int32) {
        for (size_t i = 0; i < a.valueCount(); ++i)
            if (a.ints()[i] != b.ints()[i]) return false;
        return true;
    }
    return std::memcmp(a.floats(), b.floats(), a.valueCount() * sizeof(float)) == 0;
}

bool sameRender(prism::RenderResult const& a, prism::RenderResult const& b)
{
    bool same = true;
    for (auto aov : {prism::Aov::Color, prism::Aov::Depth, prism::Aov::PrimId, prism::Aov::InstanceId})
        if (!sameImage(a.aov(aov), b.aov(aov))) {
            same = false;
            prism::Image const &x = a.aov(aov), &y = b.aov(aov);
            size_t differing = 0, first = ~size_t(0);
            double largest = 0;
            for (size_t i = 0; i < x.valueCount() && i < y.valueCount(); ++i) {
                const double u = x.type() == prism::DataType::Int32 ? x.ints()[i] : x.floats()[i];
                const double v = y.type() == prism::DataType::Int32 ? y.ints()[i] : y.floats()[i];
                if (u != v) {
                    ++differing;
                    if (first == ~size_t(0)) first = i;
                    largest = std::max(largest, std::abs(u - v));
                }
            }
            if (aov == prism::Aov::PrimId && std::getenv("PRISM_TEST_DUMP")) {
                for (unsigned row = 0; row < x.height(); ++row) {
                    std::string line;
                    for (unsigned c = 0; c < x.width(); ++c) line += char('0' + 1 + x.ints()[row * x.width() + c]);
                    line += "   ";
                    for (unsigned c = 0; c < y.width(); ++c) line += char('0' + 1 + y.ints()[row * y.width() + c]);
                    std::cerr << line << '\n';
                }
            }
            std::cerr << "  " << prism::AovName(aov) << ": " << differing << " values differ (first " << first
                      << ", largest " << largest << ")" << std::endl;
        }
    return same;
}

prism::RenderResult render(prism::Renderer const& renderer, Built const& b)
{
    prism::Session session = renderer.createSession(b.scene, batchOptions(b.camera));
    return session.render();
}

std::string describe(prism::CommitResult const& r)
{
    return "dirty " + std::to_string(r.dirty) + ", compiled " + std::to_string(r.materialsCompiled) + ", converted " +
           std::to_string(r.meshesConverted) + ", reused " + std::to_string(r.meshesReused) + ", attached " +
           std::to_string(r.placementsAttached) + ", updated " + std::to_string(r.placementsUpdated) +
           (r.accelerationShared ? ", shared" : "") + (r.revisionReused ? ", in place" : "") +
           (r.lightingRebuilt ? ", lighting" : "");
}

// Each step: edits, a commit and its counters, and the render against a
// scene built fresh in the new state.
void dirtyKinds(prism::Renderer const& renderer)
{
    State state;
    {
        // Two fresh scenes of one state render alike (the comparison's basis).
        Built a = build(renderer, state), b = build(renderer, state);
        a.scene.commit();
        b.scene.commit();
        check(sameRender(render(renderer, a), render(renderer, b)), "two fresh scenes render alike");
        check(sameRender(render(renderer, a), render(renderer, a)), "a scene renders alike twice");
    }
    Built incremental = build(renderer, state);
    prism::CommitResult r = incremental.scene.commit();
    check(r.revision == 1 && r.dirty == 63 && r.meshesConverted == 2 && r.meshesReused == 0 &&
              r.placementsAttached == 4 && r.materialsCompiled == 3 && r.lightingRebuilt,
          "the first commit builds everything (" + describe(r) + ")");
    check(r.instances == 4 && r.meshes == 2, "four placements of two meshes");
    r = incremental.scene.commit();
    check(r.revision == 1 && r.dirty == 0 && incremental.scene.revision() == 1,
          "a commit without edits publishes nothing");

    struct Step {
        std::string name;
        std::function<void(State&)> edit;
        std::function<bool(prism::CommitResult const&)> expect;
    };
    using DK = prism::DirtyKind;
    auto only = [](uint32_t bits) { return bits; };
    std::vector<Step> steps{
        {"move an instance (a new revision: no spare yet)", [](State& s) { s.tri1 = translate(0.3, 1.2, 0); },
         [&](auto const& r) {
             return r.dirty == only(uint32_t(DK::Placement)) && r.meshesConverted == 0 && r.meshesReused == 2 &&
                    !r.revisionReused && r.placementsAttached == 4 && !r.lightingRebuilt && r.materialsCompiled == 0;
         }},
        {"move it again (the spare, in place)", [](State& s) { s.tri1 = translate(0.5, 1.1, 0); },
         [&](auto const& r) {
             return r.meshesConverted == 0 && r.revisionReused && r.placementsAttached == 0 && r.placementsUpdated == 1;
         }},
        {"a light", [](State& s) { s.lightIntensity = 20.0f; },
         [&](auto const& r) {
             return r.dirty == only(uint32_t(DK::Light)) && r.accelerationShared && r.meshesConverted == 0 &&
                    r.placementsAttached == 0 && r.lightingRebuilt;
         }},
        {"an environment", [](State& s) { s.environment = Color3(0.5f, 0.4f, 0.3f); },
         [&](auto const& r) { return r.dirty == only(uint32_t(DK::Environment)) && r.accelerationShared; }},
        {"hide an instance", [](State& s) { s.tri2Visible = false; },
         [&](auto const& r) { return r.meshesConverted == 0 && r.instances == 3; }},
        {"hide it in place", [](State& s) { s.sphere = scaled(0.8, 0.2, -1.5, 0); },
         [&](auto const& r) { return r.meshesConverted == 0 && r.revisionReused && r.instances == 3; }},
        {"show it and change materials", [](State& s) {
             s.tri2Visible = true;
             s.singleMaterial = 1;
         },
         [&](auto const& r) {
             return r.meshesConverted == 0 && r.materialsCompiled == 0 && r.instances == 4 &&
                    r.dirty == (DK::Placement | DK::Material);
         }},
        {"an emissive material", [](State& s) { s.tri1Material = 2; },
         // The spare is two commits behind: it also takes the previous
         // step's edits (tri2 shown, the single quad's material).
         [&](auto const& r) {
             return r.meshesConverted == 0 && r.revisionReused && r.placementsUpdated == 3 && r.placementsAttached == 1;
         }},
        {"recompile a material", [](State& s) { s.red = Color3(0.6f, 0.2f, 0.1f); },
         [&](auto const& r) { return r.materialsCompiled == 1 && r.meshesConverted == 0; }},
        {"a mesh's points", [](State& s) { s.triSize = 0.7f; },
         [&](auto const& r) {
             return r.dirty == only(uint32_t(DK::Prototype)) && r.meshesConverted == 1 && r.meshesReused == 1;
         }},
        {"move a world-space mesh", [](State& s) { s.single = translate(-2.7, 1.1, 0); },
         [&](auto const& r) { return r.meshesConverted == 1 && r.meshesReused == 1; }},
        {"add an instance", [](State& s) { s.extra = true; },
         [&](auto const& r) {
             return r.dirty == only(uint32_t(DK::Structure)) && r.meshesConverted == 1 && !r.revisionReused &&
                    r.instances == 5;
         }},
        {"remove it", [](State& s) { s.extra = false; },
         // The spare predates the added instance: the same placements again.
         [&](auto const& r) { return r.instances == 4 && r.revisionReused && r.meshesConverted == 0; }},
        {"a sphere's radius", [](State& s) { s.sphereRadius = 0.6f; },
         [&](auto const& r) { return r.meshesConverted == 0 && r.dirty == only(uint32_t(DK::Prototype)); }},
    };
    for (Step const& step : steps) {
        State next = state;
        step.edit(next);
        apply(incremental, state, next);
        state = next;
        const prism::CommitResult result = incremental.scene.commit();
        check(step.expect(result), step.name + ": unexpected work (" + describe(result) + ")");
        Built fresh = build(renderer, state);
        fresh.scene.commit();
        check(sameRender(render(renderer, incremental), render(renderer, fresh)),
              step.name + ": the render differs from a full rebuild");
    }
}

// A revision held by a session (here: SceneAccess::committedRevision, as the
// sessions hold it) is never updated in place.
void pinnedSpare(prism::Renderer const& renderer)
{
    State state;
    Built b = build(renderer, state);
    b.scene.commit();
    prism::detail::CommittedRevision pinned;
    {
        prism::detail::RenderLock lock(b.scene);
        pinned = prism::detail::SceneAccess::committedRevision(b.scene);
    }
    b.scene.setTransform(b.tri1, prism::Constant(translate(0.1, 1, 0)));
    b.scene.commit();  // revision 1 (pinned) becomes the spare
    b.scene.setTransform(b.tri1, prism::Constant(translate(0.2, 1, 0)));
    prism::CommitResult r = b.scene.commit();
    check(!r.revisionReused, "a revision a session holds is not updated in place");
    check(pinned.manager->placement_transforms.size() == 3 && pinned.revision == 1,
          "the held revision keeps its placements");
    const RayHit hit = pinned.manager->cast_ray(initialize_ray(Float3(0, 1, 5), Float3(0, 0, -1)));
    check(hit.geomid != int(RTC_INVALID_GEOMETRY_ID), "the held revision still renders its own placement");
    pinned = {};
    b.scene.setTransform(b.tri1, prism::Constant(translate(0.3, 1, 0)));
    r = b.scene.commit();  // the spare: revision 2, which nothing holds
    check(r.revisionReused, "an unheld spare is updated in place (" + describe(r) + ")");
    b.scene.setTransform(b.tri1, prism::Constant(translate(0.4, 1, 0)));
    r = b.scene.commit();
    check(r.revisionReused, "and again, alternating banks (" + describe(r) + ")");
}

// A progressive render while another thread commits: every published image
// is of one revision (both quads always share a material and an offset), and
// the final image equals a fresh scene's.
void consistentRevision(prism::Renderer const& renderer)
{
    State state;
    Built b = build(renderer, state);
    b.scene.commit();
    prism::SessionOptions options = batchOptions(b.camera);
    options.aovs = {prism::Aov::Color, prism::Aov::Albedo, prism::Aov::PrimId};
    options.settings.samples = 1 << 20;
    options.progressive.adaptivePreview = false;
    options.progressive.refinementDelayMs = 0;
    prism::Session session = renderer.createSession(b.scene, options);
    session.start();
    check(eventually([&] { return !session.aov(prism::Aov::Albedo).empty(); }), "the progressive render publishes");
    std::atomic<bool> done{false};
    std::atomic<int> commits{0};
    std::thread editor([&] {
        for (int i = 0; i < 60; ++i) {
            const bool odd = i % 2 == 1;
            // Both triangle-list instances take the same material and offset.
            b.scene.setMaterials(b.tri1, {odd ? b.red : b.green});
            b.scene.setMaterials(b.tri2, {odd ? b.red : b.green});
            const double offset = 0.25 * (i % 4);
            b.scene.setTransform(b.tri1, prism::Constant(translate(0 + offset, 1, 0)));
            b.scene.setTransform(b.tri2, prism::Constant(translate(2.5 + offset, 1, 0)));
            if (i % 7 == 3) b.scene.updateLight(b.light, light(10.0f + float(i)));
            b.scene.commit();
            ++commits;
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 3));
        }
        done = true;
    });
    int images = 0, mixed = 0;
    within(300, "commits beside a progressive render", [&] {
        while (!done) {
            const prism::Image albedo = session.aov(prism::Aov::Albedo);
            const prism::Image ids = session.aov(prism::Aov::PrimId);
            if (albedo.empty() || ids.empty() || albedo.pixelCount() != ids.pixelCount()) continue;
            // The albedo of prim 2's pixels against prim 3's.
            float a2 = -1, a3 = -1;
            for (size_t p = 0; p < ids.pixelCount(); ++p) {
                const int id = ids.ints()[p];
                if (id == 2 && a2 < 0) a2 = albedo.floats()[p * albedo.channels()];
                if (id == 3 && a3 < 0) a3 = albedo.floats()[p * albedo.channels()];
            }
            if (a2 >= 0 && a3 >= 0) {
                ++images;
                // Images published mid-accumulation may pair an id image and
                // an albedo of different passes; only flag clear mixes.
                if (std::abs(a2 - a3) > 0.3f) ++mixed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    editor.join();
    note("  " + std::to_string(images) + " images checked (" + std::to_string(mixed) + " pairs across revisions), " +
         std::to_string(commits.load()) + " commits");
    check(images > 0, "images were published while committing");
    // aov() of two AOVs is two calls: a new revision can land between
    // them. Accept a few such pairs, never most.
    check(mixed * 10 <= images, "images mix revisions (" + std::to_string(mixed) + " of " + std::to_string(images) + ")");
    session.cancel();
    session.wait();
}

// Rapid random edits of every kind with a progressive session running; at
// the end the incremental scene renders as a fresh one.
void stress(prism::Renderer const& renderer)
{
    State state;
    Built b = build(renderer, state);
    b.scene.commit();
    prism::SessionOptions options = batchOptions(b.camera);
    options.settings.samples = 1 << 20;
    prism::Session progressive = renderer.createSession(b.scene, options);
    progressive.start();
    std::mt19937 random(12345);
    auto pick = [&](int n) { return int(random() % unsigned(n)); };
    size_t reused = 0, shared = 0;
    within(600, "rapid edits", [&] {
        for (int i = 0; i < 150; ++i) {
            State next = state;
            const int edits = 1 + pick(3);
            for (int e = 0; e < edits; ++e) {
                switch (pick(10)) {
                case 0: next.tri1 = translate(0.1 * pick(10), 1, 0); break;
                case 1: next.tri2 = translate(2.5, 0.1 * pick(10), 0); break;
                case 2: next.tri2Visible = !next.tri2Visible; break;
                case 3: next.singleMaterial = pick(2); break;
                case 4: next.tri1Material = pick(3); break;
                case 5: next.lightIntensity = 5.0f + float(pick(10)); break;
                case 6: next.environment = Color3(0.1f * float(pick(5)), 0.3f, 0.5f); break;
                case 7: next.extra = !next.extra; break;
                case 8: next.single = translate(-2.5 - 0.1 * pick(5), 1, 0); break;
                case 9: next.triSize = 0.6f + 0.1f * float(pick(5)); break;
                }
            }
            apply(b, state, next);
            state = next;
            const prism::CommitResult r = b.scene.commit();
            reused += r.revisionReused;
            shared += r.accelerationShared;
        }
    });
    note("  " + std::to_string(reused) + " in place, " + std::to_string(shared) + " shared acceleration");
    progressive.cancel();
    progressive.wait();
    check(reused > 0, "rapid edits reuse the spare revision");
    Built fresh = build(renderer, state);
    fresh.scene.commit();
    check(sameRender(render(renderer, b), render(renderer, fresh)), "after rapid edits the render matches a full rebuild");
}

// Commits, batch renders and two progressive sessions of one scene at once.
void noDeadlock(prism::Renderer const& renderer)
{
    State state;
    Built b = build(renderer, state);
    b.scene.commit();
    prism::SessionOptions options = batchOptions(b.camera);
    options.settings.samples = 1 << 20;
    prism::Session first = renderer.createSession(b.scene, options);
    prism::SessionOptions other = options;
    other.settings.maxDepth = 3;
    prism::Session second = renderer.createSession(b.scene, other);
    prism::Session batch = renderer.createSession(b.scene, batchOptions(b.camera));
    first.start();
    second.start();
    within(600, "commits, batch renders and progressive sessions together", [&] {
        std::thread renders([&] {
            for (int i = 0; i < 6; ++i) (void)batch.render();
        });
        for (int i = 0; i < 40; ++i) {
            b.scene.setTransform(b.tri1, prism::Constant(translate(0.05 * i, 1, 0)));
            if (i % 5 == 0) b.scene.updateEnvironment(b.environment, environment(Color3(0.1f * float(i % 4), 0.2f, 0.3f)));
            b.scene.commit();
        }
        renders.join();
        check(eventually([&] { return first.status().revision == b.scene.revision(); }),
              "the first progressive session takes the last revision");
        check(eventually([&] { return second.status().revision == b.scene.revision(); }),
              "the second progressive session takes the last revision");
        first.cancel();
        second.cancel();
        first.wait();
        second.wait();
    });
}

} // namespace

int main(int argc, char** argv)
{
    try {
        prism::RendererOptions options = prism::RendererOptions::FromEnvironment();
        options.color = {};
        options.color.workingSpace = "lin_rec709";
        prism::Renderer renderer(options);
        const std::string only = argc > 1 ? argv[1] : "";
        auto run = [&](const char* name, void (*f)(prism::Renderer const&)) {
            if (!only.empty() && only != name) return;
            note(name);
            f(renderer);
        };
        run("dirty-kinds", dirtyKinds);
        run("pinned-spare", pinnedSpare);
        run("consistent-revision", consistentRevision);
        run("stress", stress);
        run("no-deadlock", noDeadlock);
    } catch (std::exception const& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "incremental commit tests passed\n";
    return 0;
}
