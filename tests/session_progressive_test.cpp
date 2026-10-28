// The progressive session (include/prism/session.h: start, pause, resume,
// cancel, restart, wait, status, aov, pick) on the CPU: AOVs polled while it
// renders, pause and resume, cancellation, a restart after a settings change
// and after a commit, picking, sessions of one scene sharing its render lock
// without deadlock, and errors on misuse. Links the core only.
//   prism_session_progressive_test
#include <prism/renderer.h>
#include <prism/session.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
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

// Fails the test (instead of hanging ctest) when f does not return in time.
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

// Polls until the condition holds (or fails after the timeout).
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

// A red 2x2 quad at z = 0 (prim id 7), a constant environment and a sphere
// light; a camera at z = 4 looking down -Z whose view is wider than the quad.
struct TestScene {
    prism::Scene scene;
    prism::CameraId camera;
    prism::InstanceId quad;
};
TestScene build(prism::Renderer const& renderer)
{
    static const std::vector<float> points{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const std::vector<int32_t> counts{4}, indices{0, 1, 2, 3};
    TestScene test{renderer.createScene(), {}, {}};
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
    instance.instanceId = 3;
    instance.userData = 42;
    test.quad = scene.addInstance(instance);
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
    scene.commit();
    return test;
}

prism::SessionOptions sessionOptions(prism::CameraId camera, int samples)
{
    prism::SessionOptions options;
    options.device = prism::DevicePreference::Cpu;
    options.width = 32;
    options.height = 24;
    options.camera = camera;
    options.aovs = {prism::Aov::Color, prism::Aov::Albedo, prism::Aov::Depth, prism::Aov::PrimId,
                    prism::Aov::ElementId, prism::Aov::InstanceId};
    options.settings.samples = samples;
    options.settings.adaptiveThreshold = 0.0f;
    return options;
}

double mean(prism::Image const& image)
{
    double sum = 0;
    for (size_t i = 0; i < image.valueCount(); ++i) sum += image.floats()[i];
    return image.valueCount() ? sum / double(image.valueCount()) : 0.0;
}

bool sameInts(prism::Image const& a, prism::Image const& b)
{
    if (a.empty() || b.empty() || a.valueCount() != b.valueCount()) return false;
    for (size_t i = 0; i < a.valueCount(); ++i)
        if (a.ints()[i] != b.ints()[i]) return false;
    return true;
}

bool sameFloats(prism::Image const& a, prism::Image const& b)
{
    if (a.empty() || b.empty() || a.valueCount() != b.valueCount()) return false;
    for (size_t i = 0; i < a.valueCount(); ++i)
        if (a.floats()[i] != b.floats()[i]) return false;
    return true;
}

// Start, poll AOVs while it runs, converge; the images agree with the batch
// render (geometric AOVs exactly, the colour within noise).
void progressiveRender(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    prism::Session session = renderer.createSession(test.scene, sessionOptions(test.camera, 256));
    check(session.status().state == prism::SessionState::Idle, "a new session is idle");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { session.pick(1, 1); }), "pick before start throws");
    int calls = 0;
    bool sawDone = false;
    note("start");
    session.start([&](prism::ProgressInfo const& info) {
        ++calls;
        sawDone |= info.phase == "done" && info.fraction == 1.0;
        return true;
    });
    check(eventually([&] { return !session.aov(prism::Aov::Color).empty(); }), "the colour is published while running");
    note("published");
    bool polled = false;
    for (int i = 0; i < 2000 && session.status().state == prism::SessionState::Running; ++i) {
        prism::Image color = session.aov(prism::Aov::Color);
        const auto status = session.status();
        polled |= !color.empty() && status.samples > 0 && status.samples < 256;
        check(color.empty() || (color.width() == 32 && color.height() == 24 && color.channels() == 3),
              "a polled colour has the image's size");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    note("polled");
    prism::SessionStatus status;
    within(120, "wait for convergence", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Converged && status.samples == 256 && status.fraction == 1.0 &&
              status.accumulations == 1 && status.revision == 1,
          "the progressive render converges with all samples");
    check(calls > 0 && sawDone, "the progress callback is called, last with done");
    (void)polled;

    note("converged");
    // The batch render of the same view (render() is allowed once converged).
    prism::SessionOptions batchOptions = sessionOptions(test.camera, 256);
    prism::Session batch = renderer.createSession(test.scene, batchOptions);
    const prism::RenderResult result = batch.render();
    for (prism::Aov aov : {prism::Aov::PrimId, prism::Aov::ElementId, prism::Aov::InstanceId})
        check(sameInts(session.aov(aov), result.aov(aov)), "id AOV " + std::string(prism::AovName(aov)) +
                                                                " matches the batch render");
    check(sameFloats(session.aov(prism::Aov::Depth), result.aov(prism::Aov::Depth)), "depth matches the batch render");
    const double progressive = mean(session.aov(prism::Aov::Color)), reference = mean(result.aov(prism::Aov::Color));
    check(std::abs(progressive - reference) <= 0.05 * reference,
          "the colour agrees with the batch render (" + std::to_string(progressive) + " vs " +
              std::to_string(reference) + ")");
    check(mean(session.aov(prism::Aov::Albedo)) > 0.0, "the albedo is rendered");
    check(throws(prism::ErrorCode::NotFound, [&] { session.aov(prism::Aov::Normal); }),
          "an AOV the session does not render throws NotFound");

    // Pick: the quad's centre and a corner that sees the environment.
    const prism::Pick hit = session.pick(16, 12);
    check(hit.hit && hit.primId == 7 && hit.instanceId == 3 && hit.userData == 42 && hit.instance == test.quad &&
              hit.elementId == 0 && std::abs(hit.position.z()) < 1e-4f && hit.depth > 0.0f && hit.depth < 1.0f,
          "pick at the centre finds the quad");
    const prism::Pick miss = session.pick(0, 0);
    check(!miss.hit && miss.primId == -1 && miss.depth == 1.0f, "pick at a corner misses");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { session.pick(32, 0); }), "pick outside the image throws");

    // Restart after a settings change: a new accumulation that converges to
    // another image (one bounce less light).
    prism::Image before = session.aov(prism::Aov::Color);
    prism::RenderSettings settings = session.options().settings;
    settings.maxDepth = 1;
    session.setSettings(settings);
    within(120, "wait after a settings change", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Converged && status.accumulations == 2 && status.samples == 256,
          "a settings change restarts the accumulation");
    check(!sameFloats(before, session.aov(prism::Aov::Color)), "the restarted accumulation renders the new settings");
    // restart(): the same image again, from sample 0.
    prism::Image depth1 = session.aov(prism::Aov::Color);
    session.restart();
    within(120, "wait after restart()", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Converged && status.accumulations == 3, "restart() starts over");
    check(sameFloats(depth1, session.aov(prism::Aov::Color)), "a restarted accumulation renders the same image");

    // A commit restarts a converged render with the new revision.
    test.scene.setTransform(test.quad, prism::Constant(translate(0.5, 0, 0)));
    test.scene.commit();
    check(eventually([&] { return session.status().revision == 2; }), "a converged session takes a new commit");
    within(120, "wait after a commit", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Converged && status.revision == 2, "the new revision converges");
    check(session.pick(25, 12).hit && !session.pick(10, 12).hit, "pick sees the moved quad");
}

// Pause and resume, cancel, and errors.
void control(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    prism::Session session = renderer.createSession(test.scene, sessionOptions(test.camera, 1 << 20));
    note("control: start");
    session.start();
    check(eventually([&] { return session.status().samples >= 2; }), "passes complete");
    note("control: passes");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { session.render(); }),
          "render() throws while a progressive render runs");
    session.pause();
    note("control: pause");
    prism::SessionStatus status;
    within(60, "wait for pause", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Paused, "pause() pauses");
    const uint64_t paused = status.samples;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(session.status().samples == paused && session.status().state == prism::SessionState::Paused,
          "a paused render renders no passes");
    session.resume();
    note("control: resume");
    check(eventually([&] { return session.status().samples > paused; }), "resume() continues the accumulation");
    check(session.status().accumulations == 1, "pause and resume keep the accumulation");
    // A second start() while running only replaces the callback.
    session.start();
    check(session.status().accumulations == 1, "start() on a running session does not restart");
    note("control: cancel");
    session.cancel();
    within(60, "wait for cancel", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Cancelled, "cancel() stops the render");
    // Batch rendering works again after the cancel.
    prism::RenderSettings small = session.options().settings;
    small.samples = 4;
    session.setSettings(small);
    check(session.render().has(prism::Aov::Color), "render() works after a cancel");

    // The callback cancels.
    session.setSettings(sessionOptions(test.camera, 1 << 20).settings);
    int calls = 0;
    session.start([&](prism::ProgressInfo const&) { return ++calls < 3; });
    within(60, "wait for a callback cancel", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Cancelled && calls == 3, "a false callback cancels");
    // A throwing callback fails the render; wait() rethrows.
    session.start([&](prism::ProgressInfo const&) -> bool { throw std::runtime_error("callback error"); });
    bool rethrown = false;
    within(60, "wait for a failure", [&] {
        try {
            session.wait();
        } catch (std::runtime_error const& error) {
            rethrown = std::string(error.what()) == "callback error";
        }
    });
    check(rethrown && session.status().state == prism::SessionState::Failed &&
              session.status().error == "callback error",
          "a callback's exception fails the render and wait() rethrows it");

    // Misuse.
    prism::SessionOptions noCamera = sessionOptions(test.camera, 4);
    noCamera.camera = {};
    prism::Session bad = renderer.createSession(test.scene, noCamera);
    check(throws(prism::ErrorCode::InvalidArgument, [&] { bad.start(); }), "start without a camera throws");
    prism::Scene empty = renderer.createScene();
    prism::SessionOptions described = sessionOptions(test.camera, 4);
    described.camera = {};
    described.cameraDesc = test.scene.camera(test.camera);
    prism::Session uncommitted = renderer.createSession(empty, described);
    check(throws(prism::ErrorCode::InvalidArgument, [&] { uncommitted.start(); }),
          "start before a commit throws");
    // Destroying a running session stops its thread.
    within(60, "destroy a running session", [&] {
        prism::Session running = renderer.createSession(test.scene, sessionOptions(test.camera, 1 << 20));
        running.start();
        eventually([&] { return running.status().samples >= 1; });
    });
}

// Sessions of one scene share its render lock: a progressive session yields
// to another session's batch render and to commits, with other settings.
void sharedScene(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    prism::Session progressive = renderer.createSession(test.scene, sessionOptions(test.camera, 1 << 20));
    prism::SessionOptions otherOptions = sessionOptions(test.camera, 16);
    otherOptions.settings.maxDepth = 1;
    prism::Session other = renderer.createSession(test.scene, otherOptions);
    const prism::RenderResult alone = other.render();
    progressive.start();
    check(eventually([&] { return progressive.status().samples >= 1; }), "the progressive session renders");
    for (int i = 0; i < 3; ++i) {
        prism::RenderResult result;
        within(120, "a batch render beside a progressive one", [&] { result = other.render(); });
        check(sameFloats(result.aov(prism::Aov::Color), alone.aov(prism::Aov::Color)),
              "a batch render beside a progressive session keeps its own settings");
    }
    const uint64_t before = progressive.status().samples;
    check(eventually([&] { return progressive.status().samples > before; }),
          "the progressive session continues after the batch renders");
    within(120, "commits beside a progressive render", [&] {
        for (int i = 0; i < 3; ++i) {
            test.scene.setTransform(test.quad, prism::Constant(translate(0.1 * (i + 1), 0, 0)));
            test.scene.commit();
        }
    });
    check(eventually([&] { return progressive.status().revision == 4; }),
          "the progressive session takes the new revision");
    // Two progressive sessions of one scene alternate.
    prism::Session second = renderer.createSession(test.scene, otherOptions);
    second.start();
    prism::SessionStatus status;
    within(120, "two progressive sessions", [&] { status = second.wait(); });
    check(status.state == prism::SessionState::Converged, "a second progressive session converges beside the first");
    const uint64_t first = progressive.status().samples;
    check(eventually([&] { return progressive.status().samples > first; }), "the first keeps rendering");
    progressive.cancel();
    within(60, "cancel", [&] { progressive.wait(); });
}

// Render progress timing: the estimate is -1 until two passes completed, then
// finite and nonnegative, and 0 once converged; the elapsed time never goes
// back. No timings are asserted (a busy machine).
void timing(prism::Renderer const& renderer)
{
    TestScene test = build(renderer);
    prism::SessionOptions options = sessionOptions(test.camera, 256);
    options.width = 64;
    options.height = 48;
    prism::Session session = renderer.createSession(test.scene, options);
    bool callbackValid = true, sawDone = false;
    session.start([&](prism::ProgressInfo const& info) {
        callbackValid &= info.estimatedSecondsRemaining == -1.0 || info.estimatedSecondsRemaining >= 0.0;
        callbackValid &= info.elapsedSeconds >= 0.0 && info.samplesPerSecond >= 0.0;
        if (info.phase == "done") sawDone = info.estimatedSecondsRemaining == 0.0;
        return true;
    });
    bool unknownEarly = true, sawFinite = false, monotonic = true;
    double lastElapsed = 0.0;
    for (;;) {
        const prism::SessionStatus status = session.status();
        if (status.state != prism::SessionState::Running) break;
        if (status.samples < 2) unknownEarly &= status.estimatedSecondsRemaining == -1.0;
        if (status.samples >= 2 && status.samples < 256 && std::isfinite(status.estimatedSecondsRemaining) &&
            status.estimatedSecondsRemaining >= 0.0)
            sawFinite = true;
        monotonic &= status.elapsedSeconds >= lastElapsed;
        lastElapsed = status.elapsedSeconds;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    prism::SessionStatus status;
    within(120, "wait for convergence (timing)", [&] { status = session.wait(); });
    check(status.state == prism::SessionState::Converged, "the timed render converges");
    check(unknownEarly, "the estimate is unknown (-1) before two passes");
    check(sawFinite, "the estimate is finite and nonnegative while refining");
    check(monotonic && status.elapsedSeconds >= lastElapsed && status.elapsedSeconds > 0.0,
          "the elapsed time grows");
    check(status.estimatedSecondsRemaining == 0.0 && status.samplesPerSecond > 0.0,
          "a converged render has no time left and a sample rate");
    check(callbackValid && sawDone, "the progress callback reports the timing, 0 left when done");
    // A restart starts the timing over.
    session.restart();
    within(120, "wait after restart (timing)", [&] { status = session.wait(); });
    check(status.accumulations == 2 && status.estimatedSecondsRemaining == 0.0,
          "the restarted render completes with no time left");
}

} // namespace

int main()
{
    try {
        prism::RendererOptions options = prism::RendererOptions::FromEnvironment();
        options.color = {};
        options.color.workingSpace = "lin_rec709";
        prism::Renderer renderer(options);
        note("progressive render");
        progressiveRender(renderer);
        note("control");
        control(renderer);
        note("shared scene");
        sharedScene(renderer);
        note("timing");
        timing(renderer);
    } catch (std::exception const& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "progressive session tests passed\n";
    return 0;
}
