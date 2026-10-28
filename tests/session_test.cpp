// The public session API (include/prism/session.h) and prism_io
// (include/prism/io.h): a scene from arrays rendered on the CPU into a
// RenderResult, its AOVs, exposure, crops, cancellation from the progress
// callback and through cancel(), commit progress, files, and errors on
// misuse. Links the core and prism_io only (no USD, glTF or OBJ module).
//   prism_session_test <output directory>
#include <prism/io.h>
#include <prism/renderer.h>
#include <prism/session.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool throws(prism::ErrorCode code, std::function<void()> const& f)
{
    try {
        f();
    } catch (prism::Error const& error) {
        if (error.code() != code) std::cerr << "  (threw code " << int(error.code()) << ": " << error.what() << ")\n";
        return error.code() == code;
    } catch (std::exception const& error) {
        std::cerr << "  (threw " << error.what() << ")\n";
        return false;
    }
    return false;
}

prism::Matrix4d translate(double x, double y, double z)
{
    prism::Matrix4d m;
    m[3][0] = x; m[3][1] = y; m[3][2] = z;
    return m;
}

prism::RendererOptions options(std::string workingSpace = "lin_rec709")
{
    prism::RendererOptions result = prism::RendererOptions::FromEnvironment();
    result.color = {};
    result.color.workingSpace = std::move(workingSpace);
    return result;
}

// A red 2x2 quad at z = 0 (prim id 7), a constant environment and a sphere
// light; a camera at z = 4 looking down -Z whose view is wider than the quad.
struct TestScene {
    prism::Scene scene;
    prism::CameraId camera;
};
TestScene build(prism::Renderer const& renderer)
{
    static const std::vector<float> points{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const std::vector<int32_t> counts{4}, indices{0, 1, 2, 3};
    TestScene test{renderer.createScene(), {}};
    prism::Scene& scene = test.scene;
    prism::MaterialDesc red;
    red.color = Color3(0.8f, 0.1f, 0.1f);
    prism::MeshDesc mesh;
    mesh.points = prism::Constant(prism::BufferView::Of(points, 3, prism::DataType::Float32));
    mesh.faceVertexCounts = prism::BufferView::Of(counts, 1, prism::DataType::Int32);
    mesh.faceVertexIndices = prism::BufferView::Of(indices, 1, prism::DataType::Int32);
    prism::InstanceDesc instance;
    instance.prototype = scene.addMesh(mesh);
    instance.materials = {scene.addMaterial(red)};
    instance.primId = 7;
    scene.addInstance(instance);
    prism::EnvironmentDesc environment;
    environment.kind = prism::EnvironmentDesc::Kind::Constant;
    environment.color = Color3(0.25f, 0.5f, 0.75f);
    scene.addEnvironment(environment);
    prism::LightDesc light;
    light.transform = prism::Constant(translate(0, 0, 3));
    light.intensity = 10;
    scene.addLight(light);
    prism::CameraDesc camera;
    camera.cameraToWorld = prism::Constant(translate(0, 0, 4));
    camera.focalLength = 20;
    camera.nearClip = 0.1f;
    camera.farClip = 100.0f;
    test.camera = scene.addCamera(camera);
    return test;
}

prism::SessionOptions sessionOptions(prism::CameraId camera)
{
    prism::SessionOptions options;
    options.device = prism::DevicePreference::Cpu;
    options.width = 32;
    options.height = 24;
    options.camera = camera;
    options.aovs = {prism::Aov::Color, prism::Aov::Albedo, prism::Aov::Depth, prism::Aov::Normal,
                    prism::Aov::PrimId, prism::Aov::ElementId, prism::Aov::InstanceId, prism::Aov::St};
    options.settings.samples = 8;
    options.settings.adaptiveThreshold = 0.0f;
    return options;
}

float const* at(prism::Image const& image, unsigned x, unsigned y)
{
    return image.floats() + (size_t(y) * image.width() + x) * image.channels();
}
int32_t idAt(prism::Image const& image, unsigned x, unsigned y) { return image.ints()[size_t(y) * image.width() + x]; }

void renderAndAovs(prism::Renderer const& renderer, std::filesystem::path const& output)
{
    TestScene test = build(renderer);
    prism::Scene& scene = test.scene;
    // Misuse before the first commit.
    prism::Session session = renderer.createSession(scene, sessionOptions(test.camera));
    check(session.device().kind == prism::DeviceKind::Cpu && session.deviceNote().empty(), "a CPU session");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { session.render(); }), "render before commit throws");

    // Commit progress: each phase, materials counted; false cancels.
    std::vector<std::string> phases;
    check(throws(prism::ErrorCode::Cancelled, [&] {
              scene.commit([&](prism::ProgressInfo const& info) { return info.phase != "geometry"; });
          }) && scene.revision() == 0,
          "a commit cancelled by its callback throws Cancelled and publishes nothing");
    size_t materials = 0;
    scene.commit([&](prism::ProgressInfo const& info) {
        phases.push_back(info.phase);
        if (info.phase == "materials") materials = info.total;
        return true;
    });
    check(scene.revision() == 1 && materials == 1 && phases.front() == "materials" && phases.back() == "acceleration",
          "commit reports its phases and materials");

    int calls = 0;
    bool sawRendering = false, sawDone = false;
    const prism::RenderResult result = session.render([&](prism::ProgressInfo const& info) {
        ++calls;
        sawRendering |= info.phase == "rendering";
        sawDone |= info.phase == "done" && info.fraction == 1.0;
        return true;
    });
    check(calls > 0 && sawDone, "the progress callback is called, last with done");
    (void)sawRendering;
    check(result.width == 32 && result.height == 24 && result.dataWindow == prism::Window{0, 0, 32, 24},
          "the result covers the image");
    check(result.traced && result.meanSamples == 8.0 && result.fractionAtMaxSamples == 1.0 && !result.timeLimitReached,
          "every pixel got its samples");
    for (prism::Aov aov : sessionOptions(test.camera).aovs)
        check(result.has(aov) && result.aov(aov).width() == 32 && result.aov(aov).height() == 24,
              "AOV " + std::string(prism::AovName(aov)) + " is present at the image size");
    check(!result.has(prism::Aov::Denoised) &&
              throws(prism::ErrorCode::NotFound, [&] { result.aov(prism::Aov::Denoised); }),
          "an AOV not asked for is absent");
    prism::Image const& color = result.aov(prism::Aov::Color);
    check(color.channels() == 3 && color.type() == prism::DataType::Float32, "colour is RGB floats");
    float const* centre = at(color, 16, 12);
    check(centre[0] > centre[2] && centre[0] > 0.0f, "the centre sees the red quad");
    float const* corner = at(color, 0, 0);
    check(corner[2] > corner[0], "the corner sees the blue environment");
    check(idAt(result.aov(prism::Aov::PrimId), 16, 12) == 7 && idAt(result.aov(prism::Aov::PrimId), 0, 0) == -1,
          "primId: the instance's at the centre, -1 on a miss");
    check(idAt(result.aov(prism::Aov::ElementId), 16, 12) == 0 && idAt(result.aov(prism::Aov::InstanceId), 16, 12) == -1,
          "elementId is the quad's face; instanceId its -1");
    check(result.aov(prism::Aov::PrimId).type() == prism::DataType::Int32, "id AOVs are Int32");
    check(std::abs(at(result.aov(prism::Aov::Depth), 16, 12)[0] - 1.0f) > 1e-3f &&
              at(result.aov(prism::Aov::Depth), 0, 0)[0] == 1.0f,
          "depth: below 1 on the quad, 1 on a miss");
    check(std::abs(at(result.aov(prism::Aov::Normal), 16, 12)[2] - 1.0f) < 1e-5f, "the quad's normal faces +Z");
    float const* albedo = at(result.aov(prism::Aov::Albedo), 16, 12);
    check(std::abs(albedo[0] - 0.8f) < 1e-4f && std::abs(albedo[1] - 0.1f) < 1e-4f, "the albedo is the quad's red");

    // Deterministic, and exposure in stops scales the radiance, not the albedo.
    const prism::RenderResult again = session.render();
    check(std::memcmp(again.aov(prism::Aov::Color).data(), color.data(), color.valueCount() * 4) == 0,
          "the same settings render the same pixels");
    prism::RenderSettings brighter = session.options().settings;
    brighter.exposure = 1.0f;
    session.setSettings(brighter);
    const prism::RenderResult exposed = session.render();
    bool doubled = true;
    for (size_t i = 0; i < color.valueCount(); ++i)
        doubled = doubled && exposed.aov(prism::Aov::Color).floats()[i] == 2.0f * color.floats()[i];
    check(doubled, "exposure 1 doubles the colour exactly");
    check(std::memcmp(exposed.aov(prism::Aov::Albedo).data(), result.aov(prism::Aov::Albedo).data(),
                      color.valueCount() * 4) == 0,
          "exposure leaves the albedo");
    session.setSettings(sessionOptions(test.camera).settings);

    // A crop renders the data window's pixels of the same image.
    session.resize(32, 24, prism::Window{8, 6, 16, 12});
    const prism::RenderResult crop = session.render();
    check(crop.aov(prism::Aov::Color).width() == 16 && crop.aov(prism::Aov::Color).height() == 12 &&
              crop.dataWindow == prism::Window{8, 6, 16, 12} && crop.width == 32,
          "a crop renders its data window");
    bool sameDepth = true;
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 16; ++x)
            sameDepth = sameDepth && std::abs(at(crop.aov(prism::Aov::Depth), x, y)[0] -
                                              at(result.aov(prism::Aov::Depth), x + 8, y + 6)[0]) < 1e-5f;
    check(sameDepth, "the crop's pixels are the full image's");
    check(throws(prism::ErrorCode::InvalidArgument, [&] {
              session.resize(32, 24, prism::Window{30, 0, 8, 8});
              session.render();
          }),
          "a data window outside the image throws");
    session.resize(32, 24);

    // Files through prism_io: an EXR with every AOV, an 8-bit PNG.
    std::filesystem::create_directories(output);
    std::vector<std::string> warnings;
    prism::io::Write((output / "session.exr").string(), result, renderer, {}, &warnings);
    prism::io::Write((output / "session.png").string(), result, renderer, {}, &warnings);
    check(std::filesystem::file_size(output / "session.exr") > 0 && std::filesystem::file_size(output / "session.png") > 0,
          "prism_io writes the EXR and the PNG");
    check(!warnings.empty(), "the PNG leaves out the other AOVs with a warning");
    prism::io::WriteOptions colorOnly;
    colorOnly.layers = {{prism::Aov::Color, "color", 4}};
    prism::io::Write((output / "session_rgba.exr").string(), result, renderer, colorOnly);
    check(prism::io::IsMultiChannelFormat("a.EXR") && !prism::io::IsMultiChannelFormat("a.png"), "EXR is multi-channel");
    prism::RenderResult empty;
    check(throws(prism::ErrorCode::InvalidArgument, [&] { prism::io::Write((output / "none.exr").string(), empty, renderer); }),
          "writing an empty result throws");
}

void cancellation(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    test.scene.commit();
    prism::SessionOptions options = sessionOptions(test.camera);
    options.aovs = {prism::Aov::Color};
    options.width = 256;
    options.height = 256;
    options.settings.samples = 1 << 20;  // hours, unless cancelled
    prism::Session session = renderer.createSession(test.scene, options);
    using Clock = std::chrono::steady_clock;

    // The callback returns false once the samples run.
    auto start = Clock::now();
    check(throws(prism::ErrorCode::Cancelled, [&] {
              session.render([](prism::ProgressInfo const& info) { return !(info.phase == "rendering" && info.elapsedSeconds > 0.5); });
          }),
          "a false progress callback cancels the render");
    check(std::chrono::duration<double>(Clock::now() - start).count() < 60.0, "the callback stops it early");

    // cancel() from another thread (here the callback's).
    start = Clock::now();
    check(throws(prism::ErrorCode::Cancelled, [&] {
              session.render([&](prism::ProgressInfo const& info) {
                  if (info.phase == "rendering" && info.elapsedSeconds > 0.5) session.cancel();
                  return true;
              });
          }),
          "cancel() cancels the render");
    check(std::chrono::duration<double>(Clock::now() - start).count() < 60.0, "cancel() stops it early");

    // A callback that throws cancels and its exception reaches the caller.
    check(throws(prism::ErrorCode::InvalidHandle, [&] {
              session.render([](prism::ProgressInfo const&) -> bool {
                  throw prism::Error(prism::ErrorCode::InvalidHandle, "from the callback");
              });
          }),
          "a callback's exception cancels and is rethrown");

    // The session renders again afterwards.
    prism::RenderSettings quick = options.settings;
    quick.samples = 2;
    session.setSettings(quick);
    session.resize(16, 16);
    check(session.render().aov(prism::Aov::Color).width() == 16, "a cancelled session renders again");
}

bool Same(prism::RenderResult const& a, prism::RenderResult const& b)
{
    prism::Image const& x = a.aov(prism::Aov::Color);
    prism::Image const& y = b.aov(prism::Aov::Color);
    return x.valueCount() == y.valueCount() && std::memcmp(x.data(), y.data(), x.valueCount() * 4) == 0;
}

// Two sessions on one scene with different values of a setting the scene
// holds (maxDepth): each renders with its own, also after the other, and
// concurrent renders serialize on the scene.
void sharedScene(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    test.scene.commit();
    prism::SessionOptions shallow = sessionOptions(test.camera);
    shallow.aovs = {prism::Aov::Color};
    shallow.settings.maxDepth = 1;
    prism::SessionOptions deep = shallow;
    deep.settings.maxDepth = 8;
    prism::Session a = renderer.createSession(test.scene, shallow);
    prism::Session b = renderer.createSession(test.scene, deep);
    const prism::RenderResult first = a.render();
    const prism::RenderResult other = b.render();
    const prism::RenderResult again = a.render();
    check(!Same(first, other), "sessions with different depths render different images");
    check(Same(first, again), "a session renders with its own settings after another session's render");
    check(Same(other, b.render()), "and so does the other");

    // Concurrent renders of both sessions: serialized, each with its settings.
    bool ok[2] = {true, true};
    std::thread threads[2];
    for (int t = 0; t < 2; ++t)
        threads[t] = std::thread([&, t] {
            try {
                for (int i = 0; i < 3; ++i)
                    ok[t] = ok[t] && Same(t == 0 ? a.render() : b.render(), t == 0 ? first : other);
            } catch (...) {
                ok[t] = false;
            }
        });
    for (auto& thread : threads) thread.join();
    check(ok[0] && ok[1], "concurrent renders of one scene's sessions keep their settings");
}

void misuse(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    test.scene.commit();
    prism::SessionOptions options = sessionOptions(test.camera);
    options.settings.samples = 0;
    check(throws(prism::ErrorCode::InvalidArgument, [&] { renderer.createSession(test.scene, options); }),
          "zero samples throw");
    options = sessionOptions({});
    prism::Session noCamera = renderer.createSession(test.scene, options);
    check(throws(prism::ErrorCode::InvalidArgument, [&] { noCamera.render(); }), "a session without a camera throws");
    check(throws(prism::ErrorCode::InvalidHandle, [&] { noCamera.setCamera(prism::CameraId{12345}); }),
          "an invalid camera handle throws");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { noCamera.setDenoise(std::nullopt, true); }),
          "denoisedColor without denoising throws");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { noCamera.setAovs({}); noCamera.setCamera(test.camera); noCamera.render(); }),
          "a session without AOVs throws");
    prism::Renderer other(::options());
    check(throws(prism::ErrorCode::InvalidArgument, [&] { other.createSession(test.scene); }),
          "a scene of another renderer throws");
    check(prism::AovName(prism::Aov::St) == "primvars:st" && prism::AovFromName("primId") == prism::Aov::PrimId &&
              prism::AovFromName("P") == prism::Aov::Pworld && !prism::AovFromName("nothing"),
          "AOV names are Hydra's tokens");
    check(throws(prism::ErrorCode::InvalidArgument, [] { prism::Image(2, 2, 1, prism::DataType::Int32).floats(); }),
          "typed access to the other type throws");
    check(renderer.capabilities().motionBlur == false && !renderer.devices().empty() &&
              renderer.devices().back().kind == prism::DeviceKind::Cpu,
          "capabilities and devices (the CPU last)");
    check(renderer.color().workingSpace.find("709") != std::string::npos, "the renderer's colour");
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : std::filesystem::temp_directory_path() / "prism_session_test";
    try {
        const prism::Renderer renderer(options());
        renderAndAovs(renderer, output);
        cancellation(renderer);
        sharedScene(renderer);
        misuse(renderer);
        // Several renderers in one process, in other working spaces.
        const prism::Renderer acescg(options("ACEScg"));
        TestScene test = build(acescg);
        test.scene.commit();
        check(acescg.createSession(test.scene, sessionOptions(test.camera)).render().has(prism::Aov::Color),
              "a second renderer renders");
    } catch (std::exception const& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "prism_session_test passed\n";
    return 0;
}
