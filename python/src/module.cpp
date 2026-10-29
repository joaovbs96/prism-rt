// prismrt._prism: nanobind bindings of the public C++ API (include/prism/*.h)
// over the shared prism.dll, with the glTF, OBJ and image-writing modules
// (docs/python.md; docs/sdk_design.md section 6). A thin layer: one class per
// C++ class or struct, snake_case names; the Pythonic API (keyword
// arguments, names, params, from_dict) is the pure-Python package around it.
//
// GIL (section 6.6): released in every call that can take long or wait for a
// render thread; callbacks take it on the C++ threads they run on. Blocking
// calls (commit, render, wait) wait in 100 ms slices and turn Ctrl-C into a
// cancel.

#include "common.h"

#include <nanobind/ndarray.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/variant.h>
#include <nanobind/stl/vector.h>

#include <prism/gltf.h>
#include <prism/io.h>
#include <prism/obj.h>
#include <prism/renderer.h>

#include <cctype>
#include <cstring>
#include <sstream>

namespace nb = nanobind;
using namespace nb::literals;

// ── Casters: Float3 (Color3) as a 3-tuple, Matrix4d as a NumPy 4x4 array.
// Both accept any sequence (lists, tuples, NumPy arrays); a number is a
// Float3 of three equal components.
namespace nanobind::detail {

inline bool ReadDouble(PyObject* o, double& out)
{
    out = PyFloat_AsDouble(o);
    if (out == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    return true;
}

// `count` numbers from a sequence.
inline bool ReadNumbers(PyObject* o, Py_ssize_t count, double* out)
{
    if (PyUnicode_Check(o) || PyBytes_Check(o) || !PySequence_Check(o)) return false;
    PyObject* seq = PySequence_Fast(o, "");
    if (!seq) {
        PyErr_Clear();
        return false;
    }
    bool ok = PySequence_Fast_GET_SIZE(seq) == count;
    for (Py_ssize_t i = 0; ok && i < count; ++i) ok = ReadDouble(PySequence_Fast_GET_ITEM(seq, i), out[i]);
    Py_DECREF(seq);
    return ok;
}

template <>
struct type_caster<Float3> {
    NB_TYPE_CASTER(Float3, const_name("tuple[float, float, float]"))
    bool from_python(handle src, uint8_t, cleanup_list*) noexcept
    {
        double v[3];
        if (ReadNumbers(src.ptr(), 3, v)) {
            value = Float3(float(v[0]), float(v[1]), float(v[2]));
            return true;
        }
        if (PyNumber_Check(src.ptr()) && !PySequence_Check(src.ptr()) && ReadDouble(src.ptr(), v[0])) {
            value = Float3(float(v[0]));
            return true;
        }
        return false;
    }
    static handle from_cpp(Float3 const& v, rv_policy, cleanup_list*) noexcept
    {
        return nb::make_tuple(double(v.x()), double(v.y()), double(v.z())).release();
    }
};

template <>
struct type_caster<prism::Matrix4d> {
    NB_TYPE_CASTER(prism::Matrix4d, const_name("numpy.typing.ArrayLike"))
    bool from_python(handle src, uint8_t, cleanup_list*) noexcept
    {
        PyObject* o = src.ptr();
        if (PyUnicode_Check(o) || !PySequence_Check(o)) return false;
        PyObject* rows = PySequence_Fast(o, "");
        if (!rows) {
            PyErr_Clear();
            return false;
        }
        bool ok = PySequence_Fast_GET_SIZE(rows) == 4;
        for (Py_ssize_t r = 0; ok && r < 4; ++r) ok = ReadNumbers(PySequence_Fast_GET_ITEM(rows, r), 4, value.m[r]);
        Py_DECREF(rows);
        return ok;
    }
    static handle from_cpp(prism::Matrix4d const& m, rv_policy, cleanup_list*) noexcept
    {
        try {
            auto* data = new double[16];
            std::memcpy(data, &m.m[0][0], sizeof(double) * 16);
            capsule owner(data, [](void* p) noexcept { delete[] static_cast<double*>(p); });
            ndarray<numpy, double, nb::shape<4, 4>> array(data, {4, 4}, owner);
            return nb::cast(array, rv_policy::reference).release();
        } catch (...) {
            return handle();
        }
    }
};

} // namespace nanobind::detail

namespace prism::python {
namespace {

// ── Errors: prismrt.Error and its subclasses live in prismrt/errors.py,
// which hands them over at import (_set_error_classes).
nb::object& ErrorClasses()
{
    static nb::object* classes = new nb::object();  // leaked: outlives finalization
    return *classes;
}

const char* ErrorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::InvalidHandle: return "InvalidHandle";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::DeviceLost: return "DeviceLost";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Internal: return "Internal";
    }
    return "Internal";
}

void RaiseError(ErrorCode code, const char* message)
{
    nb::object& classes = ErrorClasses();
    if (classes.is_valid()) {
        PyObject* cls = PyDict_GetItemString(classes.ptr(), ErrorCodeName(code));
        if (cls) {
            PyErr_SetString(cls, message);
            return;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, message);
}

[[noreturn]] void Throw(ErrorCode code, std::string const& message) { throw prism::Error(code, message); }

// ── Enums from enum members or strings ("color", "lat_long", "Rect",
// "primvars:st"): case and underscores do not matter.
std::string Normalized(std::string_view name)
{
    std::string out;
    for (char c : name)
        if (c != '_' && c != '-' && c != ' ') out += char(std::tolower((unsigned char)c));
    return out;
}

template <class E>
E EnumFrom(nb::handle value, const char* what)
{
    nb::handle type = nb::type<E>();
    if (nb::isinstance(value, type)) return nb::cast<E>(value);
    if constexpr (std::is_same_v<E, Aov>) {
        if (nb::isinstance<nb::str>(value)) {
            if (auto aov = AovFromName(nb::cast<std::string>(value))) return *aov;
        }
    }
    if (nb::isinstance<nb::str>(value)) {
        const std::string wanted = Normalized(nb::cast<std::string>(value));
        // __members__ is a mappingproxy (enum.Enum), not a dict.
        nb::object items = type.attr("__members__").attr("items")();
        std::string names;
        for (nb::handle item : items) {
            const std::string text = nb::cast<std::string>(item[0]);
            if (Normalized(text) == wanted) return nb::cast<E>(item[1]);
            names += (names.empty() ? "" : ", ") + text;
        }
        Throw(ErrorCode::InvalidArgument, std::string(what) + ": unknown value '" + nb::cast<std::string>(value) +
                                              "' (expected one of " + names + ")");
    }
    Throw(ErrorCode::InvalidArgument, std::string(what) + ": expected a " +
                                          nb::cast<std::string>(type.attr("__name__")) + " or its name");
}

// ── Time samples: a value, or a list of (time, value) pairs.
template <class T>
Sampled<T> SampledFrom(nb::handle value, const char* what)
{
    T single;
    if (nb::try_cast<T>(value, single)) return Constant(std::move(single));
    Sampled<T> samples;
    if (!nb::isinstance<nb::str>(value) && PySequence_Check(value.ptr())) {
        for (nb::handle item : nb::borrow<nb::sequence>(value)) {
            nb::tuple pair;
            if (!nb::try_cast<nb::tuple>(item, pair) || nb::len(pair) != 2)
                Throw(ErrorCode::InvalidArgument,
                      std::string(what) + ": expected a value or a list of (time, value) pairs");
            TimeSample<T> sample;
            sample.time = nb::cast<double>(pair[0]);
            if (!nb::try_cast<T>(pair[1], sample.value))
                Throw(ErrorCode::InvalidArgument, std::string(what) + ": a time sample's value has the wrong type");
            samples.push_back(std::move(sample));
        }
        if (!samples.empty()) return samples;
    }
    Throw(ErrorCode::InvalidArgument, std::string(what) + ": expected a value or a list of (time, value) pairs");
}

template <class T>
nb::list SampledTo(Sampled<T> const& samples)
{
    nb::list out;
    for (auto const& sample : samples) out.append(nb::make_tuple(sample.time, sample.value));
    return out;
}

// ── Buffers: a BufferView over a NumPy (or any DLPack / buffer-protocol)
// array without a copy; the view holds a reference to the array.
using InputArray = nb::ndarray<nb::ro, nb::c_contig, nb::device::cpu>;

DataType DataTypeOf(nb::dlpack::dtype dtype, std::string const& what)
{
    using nb::dlpack::dtype_code;
    if (dtype.lanes == 1) {
        if (dtype.code == uint8_t(dtype_code::Float) && dtype.bits == 32) return DataType::Float32;
        if (dtype.code == uint8_t(dtype_code::Float) && dtype.bits == 64) return DataType::Float64;
        if (dtype.code == uint8_t(dtype_code::Int) && dtype.bits == 32) return DataType::Int32;
        if (dtype.code == uint8_t(dtype_code::UInt) && dtype.bits == 32) return DataType::UInt32;
        if (dtype.code == uint8_t(dtype_code::UInt) && dtype.bits == 8) return DataType::UInt8;
    }
    Throw(ErrorCode::InvalidArgument,
          what + ": unsupported dtype (expected float32, float64, int32, uint32 or uint8)");
}

// The keep-alive of a view: a copy of the ndarray (a reference to the
// caller's array), released with the GIL.
std::shared_ptr<const void> KeepAlive(InputArray const& array)
{
    auto* held = new InputArray(array);
    return std::shared_ptr<const void>(array.data(), [held](const void*) {
        if (!PythonAlive()) return;  // leak during finalization
        nb::gil_scoped_acquire gil;
        delete held;
    });
}

BufferView MakeBuffer(InputArray array, uint32_t components, std::string const& what)
{
    BufferView view;
    view.type = DataTypeOf(array.dtype(), what);
    size_t values = array.size();
    if (components == 0) components = array.ndim() >= 2 ? uint32_t(values / std::max<size_t>(array.shape(0), 1)) : 1;
    if (array.ndim() == 0) values = 1;
    if (components == 0 || values % components != 0)
        Throw(ErrorCode::InvalidArgument, what + ": " + std::to_string(values) + " values do not make elements of " +
                                              std::to_string(components) + " components");
    view.components = components;
    view.count = values / components;
    view.data = array.data();
    if (view.count) view.owner = KeepAlive(array);
    return view;
}

const char* DataTypeName(DataType type)
{
    switch (type) {
    case DataType::Float32: return "float32";
    case DataType::Float64: return "float64";
    case DataType::Int32: return "int32";
    case DataType::UInt32: return "uint32";
    case DataType::UInt8: return "uint8";
    }
    return "?";
}

// ── Images out: a read-only NumPy view (or DLPack tensor) that keeps a
// shared copy of the Image (the pixels are not copied).
template <class Framework>
nb::object ImageArray(Image const& image)
{
    if (image.empty()) Throw(ErrorCode::InvalidArgument, "the image is empty");
    auto* keep = new Image(image);
    nb::capsule owner(keep, [](void* p) noexcept { delete static_cast<Image*>(p); });
    const size_t shape[3] = {image.height(), image.width(), image.channels()};
    const nb::dlpack::dtype dtype = image.type() == DataType::Int32 ? nb::dtype<int32_t>() : nb::dtype<float>();
    nb::ndarray<Framework, nb::ro> array(image.data(), 3, shape, owner, nullptr, dtype);
    return nb::cast(array, nb::rv_policy::reference);
}

Image ImageFromArray(InputArray array)
{
    if (array.ndim() != 2 && array.ndim() != 3)
        Throw(ErrorCode::InvalidArgument, "Image: expected a (height, width) or (height, width, channels) array");
    const auto dtype = array.dtype();
    DataType type;
    if (dtype == nb::dtype<float>()) type = DataType::Float32;
    else if (dtype == nb::dtype<int32_t>()) type = DataType::Int32;
    else Throw(ErrorCode::InvalidArgument, "Image: expected float32 or int32 values (convert with astype)");
    const unsigned channels = array.ndim() == 3 ? unsigned(array.shape(2)) : 1u;
    Image image(unsigned(array.shape(1)), unsigned(array.shape(0)), channels, type);
    std::memcpy(image.data(), array.data(), image.valueCount() * 4);
    return image;
}

// ── Kwargs constructors and a generic repr for the bound structs.
template <class T>
struct Struct {
    nb::class_<T> cls;
    std::vector<const char*> names;

    Struct(nb::handle scope, const char* name, const char* doc = "") : cls(scope, name, doc) {}

    template <class M>
    Struct& rw(const char* name, M T::*member, const char* doc = "")
    {
        cls.def_rw(name, member, doc);
        names.push_back(name);
        return *this;
    }
    template <class M>
    Struct& ro(const char* name, M T::*member, const char* doc = "")
    {
        cls.def_ro(name, member, doc);
        names.push_back(name);
        return *this;
    }
    // An enum field that also takes the value's name.
    template <class E>
    Struct& en(const char* name, E T::*member, const char* doc = "")
    {
        cls.def_prop_rw(
            name, [member](T const& self) { return self.*member; },
            [member, name](T& self, nb::handle value) { self.*member = EnumFrom<E>(value, name); }, doc);
        names.push_back(name);
        return *this;
    }
    template <class M>
    Struct& transform(const char* name, Sampled<M> T::*member, const char* doc = "")
    {
        cls.def_prop_rw(
            name, [member](T const& self) { return SampledTo(self.*member); },
            [member, name](T& self, nb::handle value) { self.*member = SampledFrom<M>(value, name); }, doc);
        names.push_back(name);
        return *this;
    }

    // T(**fields) and a repr of the fields.
    Struct& finish(bool kwargsInit = true)
    {
        if (kwargsInit)
            cls.def("__init__", [](T* self, nb::kwargs kwargs) {
                nb::object temporary = nb::cast(T(), nb::rv_policy::move);
                for (auto [key, value] : kwargs) {
                    const std::string field = nb::cast<std::string>(key);
                    nb::object fields = nb::type<T>().attr("_fields");
                    if (!PySequence_Contains(fields.ptr(), key.ptr()))
                        throw nb::type_error((nb::cast<std::string>(nb::type<T>().attr("__name__")) +
                                              "() got an unexpected keyword argument '" + field + "'")
                                                 .c_str());
                    nb::setattr(temporary, key, value);
                }
                new (self) T(nb::cast<T const&>(temporary));
            });
        nb::list fields;
        for (const char* name : names) fields.append(nb::str(name));
        cls.attr("_fields") = nb::tuple(fields);
        cls.def("__repr__", [](nb::handle self) {
            std::string out = nb::cast<std::string>(self.type().attr("__name__")) + "(";
            bool first = true;
            for (nb::handle name : self.type().attr("_fields")) {
                out += (first ? "" : ", ") + nb::cast<std::string>(name) + "=" +
                       nb::cast<std::string>(nb::repr(nb::getattr(self, name)));
                first = false;
            }
            return out + ")";
        });
        cls.def("__copy__", [](T const& self) { return T(self); });
        cls.def("__deepcopy__", [](T const& self, nb::handle) { return T(self); }, "memo"_a);
        return *this;
    }
};

template <class Tag>
void BindHandle(nb::module_& m, const char* name)
{
    using H = Handle<Tag>;
    nb::class_<H>(m, name, "A handle of a scene object (0: none).")
        .def(nb::init<>())
        .def("__init__", [](H* self, uint64_t value) { new (self) H{value}; }, "value"_a)
        .def_rw("value", &H::v)
        .def("__bool__", [](H const& h) { return bool(h); })
        .def("__eq__", [](H const& a, nb::handle b) {
            H other;
            return nb::try_cast<H>(b, other) && a == other;
        })
        .def("__hash__", [](H const& h) { return std::hash<uint64_t>()(h.v); })
        .def("__repr__", [name](H const& h) { return std::string(name) + "(" + std::to_string(h.v) + ")"; });
}

std::vector<Aov> AovsFrom(nb::handle value)
{
    std::vector<Aov> aovs;
    if (nb::isinstance<nb::str>(value)) {
        aovs.push_back(EnumFrom<Aov>(value, "aovs"));
        return aovs;
    }
    for (nb::handle item : nb::borrow<nb::sequence>(value)) aovs.push_back(EnumFrom<Aov>(item, "aovs"));
    return aovs;
}

nb::dict AovDict(RenderResult const& result, nb::handle renderer)
{
    nb::dict out;
    for (auto const& [aov, image] : result.aovs) {
        nb::object object = nb::cast(image);
        if (renderer.is_valid() && !renderer.is_none()) nb::setattr(object, "_renderer", renderer);
        out[nb::str(std::string(AovName(aov)).c_str())] = object;
    }
    return out;
}

// prism::io::Write for one image (Image.write).
std::vector<std::string> WriteImage(Image const& image, std::string const& path, PyRenderer& renderer, Aov aov)
{
    RenderResult result;
    result.aovs[aov] = image;
    result.width = image.width();
    result.height = image.height();
    result.dataWindow = Window{0, 0, image.width(), image.height()};
    result.traced = true;
    std::vector<std::string> warnings;
    io::Write(path, result, renderer.get(), {}, &warnings);
    return warnings;
}

PyRenderer& RendererOf(nb::handle object, const char* what)
{
    PyRenderer* renderer = nullptr;
    if (!object.is_none()) {
        nb::handle native = nb::hasattr(object, "_native") ? nb::handle(object.attr("_native")) : object;
        nb::try_cast<PyRenderer*>(native, renderer);
    }
    if (!renderer) Throw(ErrorCode::InvalidArgument, std::string(what) + ": pass the renderer (renderer=...)");
    return *renderer;
}

} // namespace

void BindModule(nb::module_& m)
{
    m.doc() = "PrismRT's public C++ API (include/prism/*.h); use the prismrt package.";

    nb::register_exception_translator([](std::exception_ptr const& p, void*) {
        try {
            std::rethrow_exception(p);
        } catch (prism::Error const& error) {
            RaiseError(error.code(), error.what());
        }
    });
    m.def("_set_error_classes", [](nb::dict classes) { ErrorClasses() = classes; },
          "Hands over the exception classes by code name (prismrt/errors.py).");

    // ── Enums (the C++ names; anywhere one is taken, its name as a string is too).
    nb::enum_<ErrorCode>(m, "ErrorCode")
        .value("InvalidArgument", ErrorCode::InvalidArgument)
        .value("InvalidHandle", ErrorCode::InvalidHandle)
        .value("NotFound", ErrorCode::NotFound)
        .value("Unsupported", ErrorCode::Unsupported)
        .value("DeviceLost", ErrorCode::DeviceLost)
        .value("OutOfMemory", ErrorCode::OutOfMemory)
        .value("Cancelled", ErrorCode::Cancelled)
        .value("Internal", ErrorCode::Internal);
    nb::enum_<DataType>(m, "DataType")
        .value("Float32", DataType::Float32)
        .value("Float64", DataType::Float64)
        .value("Int32", DataType::Int32)
        .value("UInt32", DataType::UInt32)
        .value("UInt8", DataType::UInt8);
    nb::enum_<Interpolation>(m, "Interpolation")
        .value("Constant", Interpolation::Constant)
        .value("Uniform", Interpolation::Uniform)
        .value("Varying", Interpolation::Varying)
        .value("Vertex", Interpolation::Vertex)
        .value("FaceVarying", Interpolation::FaceVarying)
        .value("Instance", Interpolation::Instance);
    nb::enum_<DeviceKind>(m, "DeviceKind")
        .value("Cpu", DeviceKind::Cpu)
        .value("Vulkan", DeviceKind::Vulkan)
        .value("Metal", DeviceKind::Metal);
    nb::enum_<DevicePreference>(m, "DevicePreference")
        .value("Auto", DevicePreference::Auto)
        .value("Cpu", DevicePreference::Cpu)
        .value("Vulkan", DevicePreference::Vulkan)
        .value("Metal", DevicePreference::Metal);
    nb::enum_<ConformPolicy>(m, "ConformPolicy")
        .value("ExpandAperture", ConformPolicy::ExpandAperture)
        .value("CropAperture", ConformPolicy::CropAperture)
        .value("AdjustApertureWidth", ConformPolicy::AdjustApertureWidth)
        .value("AdjustApertureHeight", ConformPolicy::AdjustApertureHeight)
        .value("AdjustPixelAspectRatio", ConformPolicy::AdjustPixelAspectRatio);
    nb::enum_<TextureFiltering>(m, "TextureFiltering")
        .value("Filtered", TextureFiltering::Filtered)
        .value("Bilinear", TextureFiltering::Bilinear);
    nb::enum_<DenoiseQuality>(m, "DenoiseQuality")
        .value("Fast", DenoiseQuality::Fast)
        .value("Balanced", DenoiseQuality::Balanced)
        .value("High", DenoiseQuality::High);
    nb::enum_<Aov>(m, "Aov",
                   "A render output (AOV). Hydra tokens (\"color\", \"cameraDepth\", \"primId\", ...) are accepted "
                   "wherever an Aov is. Depth is normalised device depth, about 1.0 everywhere in typical scenes: it "
                   "is not a distance. For the distance along the view axis use CameraDepth (\"cameraDepth\").")
        .value("Color", Aov::Color)
        .value("Depth", Aov::Depth,
                "Normalised device depth (token \"depth\"), about 1.0 for typical scenes: not a distance; see CameraDepth.")
        .value("CameraDepth", Aov::CameraDepth,
                "Distance along the view axis, in scene units (token \"cameraDepth\").")
        .value("Normal", Aov::Normal)
        .value("Neye", Aov::Neye)
        .value("Peye", Aov::Peye)
        .value("Pworld", Aov::Pworld)
        .value("St", Aov::St)
        .value("PrimId", Aov::PrimId)
        .value("ElementId", Aov::ElementId)
        .value("InstanceId", Aov::InstanceId)
        .value("Albedo", Aov::Albedo)
        .value("Diffuse", Aov::Diffuse)
        .value("Specular", Aov::Specular)
        .value("Emission", Aov::Emission)
        .value("Denoised", Aov::Denoised);
    nb::enum_<SessionState>(m, "SessionState")
        .value("Idle", SessionState::Idle)
        .value("Running", SessionState::Running)
        .value("Paused", SessionState::Paused)
        .value("Converged", SessionState::Converged)
        .value("Cancelled", SessionState::Cancelled)
        .value("Failed", SessionState::Failed);
    nb::enum_<DiagnosticLevel>(m, "DiagnosticLevel")
        .value("Info", DiagnosticLevel::Info)
        .value("Warning", DiagnosticLevel::Warning)
        .value("Error", DiagnosticLevel::Error);
    nb::enum_<LogLevel>(m, "LogLevel")
        .value("Debug", LogLevel::Debug)
        .value("Info", LogLevel::Info)
        .value("Warning", LogLevel::Warning)
        .value("Error", LogLevel::Error);
    nb::enum_<DirtyKind>(m, "DirtyKind", nb::is_flag(), nb::is_arithmetic())
        .value("Placement", DirtyKind::Placement)
        .value("Prototype", DirtyKind::Prototype)
        .value("Material", DirtyKind::Material)
        .value("Light", DirtyKind::Light)
        .value("Environment", DirtyKind::Environment)
        .value("Structure", DirtyKind::Structure);
    nb::enum_<LightDesc::Shape>(m, "LightShape")
        .value("Sphere", LightDesc::Shape::Sphere)
        .value("Rect", LightDesc::Shape::Rect)
        .value("Disk", LightDesc::Shape::Disk)
        .value("Cylinder", LightDesc::Shape::Cylinder)
        .value("Distant", LightDesc::Shape::Distant)
        .value("Point", LightDesc::Shape::Point);
    nb::enum_<EnvironmentDesc::Kind>(m, "EnvironmentKind")
        .value("Dome", EnvironmentDesc::Kind::Dome)
        .value("LatLong", EnvironmentDesc::Kind::LatLong)
        .value("Constant", EnvironmentDesc::Kind::Constant);
    nb::enum_<MaterialDesc::Kind>(m, "MaterialKind")
        .value("MaterialXFile", MaterialDesc::Kind::MaterialXFile)
        .value("MaterialXString", MaterialDesc::Kind::MaterialXString)
        .value("Diffuse", MaterialDesc::Kind::Diffuse)
        .value("Emissive", MaterialDesc::Kind::Emissive);
    nb::enum_<CameraDesc::Projection>(m, "Projection")
        .value("Perspective", CameraDesc::Projection::Perspective)
        .value("Orthographic", CameraDesc::Projection::Orthographic);
    nb::enum_<SubdivisionDesc::Scheme>(m, "SubdivisionScheme")
        .value("None_", SubdivisionDesc::Scheme::None)
        .value("CatmullClark", SubdivisionDesc::Scheme::CatmullClark)
        .value("Loop", SubdivisionDesc::Scheme::Loop)
        .value("Bilinear", SubdivisionDesc::Scheme::Bilinear);
    nb::enum_<MaterialOptions::CompileMode>(m, "CompileMode")
        .value("Auto", MaterialOptions::CompileMode::Auto)
        .value("Class", MaterialOptions::CompileMode::Class);

    m.def("aov_name", [](nb::handle aov) { return std::string(AovName(EnumFrom<Aov>(aov, "aov"))); }, "aov"_a,
          "The Hydra AOV token of an AOV ('color', 'primvars:st', ...).");
    m.def("aov_from_name", [](std::string const& name) { return AovFromName(name); }, "name"_a,
          "The AOV of a Hydra AOV token, or None.");

    BindHandle<MeshTag>(m, "MeshId");
    BindHandle<SpheresTag>(m, "SpheresId");
    BindHandle<InstanceTag>(m, "InstanceId");
    BindHandle<MaterialTag>(m, "MaterialId");
    BindHandle<LightTag>(m, "LightId");
    BindHandle<EnvironmentTag>(m, "EnvironmentId");
    BindHandle<CameraTag>(m, "CameraId");

    // ── BufferView
    nb::class_<BufferView>(m, "BufferView",
                           "A typed view of an array: `count` elements of `components` scalars. Made from a "
                           "C-contiguous CPU array without a copy; the view keeps the array alive.")
        .def(nb::init<>())
        .def("__init__",
             [](BufferView* self, InputArray array, uint32_t components, std::string const& name) {
                 new (self) BufferView(MakeBuffer(std::move(array), components, name.empty() ? "array" : name));
             },
             "array"_a, "components"_a = 0, "name"_a = "")
        .def_ro("count", &BufferView::count)
        .def_ro("components", &BufferView::components)
        .def_ro("type", &BufferView::type)
        .def_ro("stride_bytes", &BufferView::strideBytes)
        .def_prop_ro("address", [](BufferView const& v) { return uintptr_t(v.data); },
                     "The address of the first element (the array's own memory for a zero-copy view).")
        .def_prop_ro("nbytes", [](BufferView const& v) { return v.count * v.elementBytes(); })
        .def_prop_ro("owned", [](BufferView const& v) { return bool(v.owner); },
                     "The view keeps its memory alive.")
        .def("__len__", [](BufferView const& v) { return v.count; })
        .def("__repr__", [](BufferView const& v) {
            std::ostringstream out;
            out << "BufferView(count=" << v.count << ", components=" << v.components << ", type="
                << DataTypeName(v.type) << ")";
            return out.str();
        });

    // ── Descriptions
    Struct<Primvar>(m, "Primvar", "Named data for MDL scene-data lookups (MaterialX geompropvalue).")
        .rw("name", &Primvar::name)
        .en("interpolation", &Primvar::interpolation)
        .rw("data", &Primvar::data)
        .finish();
    Struct<SubdivisionDesc>(m, "SubdivisionDesc")
        .en("scheme", &SubdivisionDesc::scheme)
        .rw("level", &SubdivisionDesc::level)
        .rw("crease_indices", &SubdivisionDesc::creaseIndices)
        .rw("crease_lengths", &SubdivisionDesc::creaseLengths)
        .rw("crease_sharpness", &SubdivisionDesc::creaseSharpness)
        .rw("corner_indices", &SubdivisionDesc::cornerIndices)
        .rw("corner_sharpness", &SubdivisionDesc::cornerSharpness)
        .rw("hole_indices", &SubdivisionDesc::holeIndices)
        .rw("interpolate_boundary", &SubdivisionDesc::interpolateBoundary)
        .rw("face_varying_linear_interpolation", &SubdivisionDesc::faceVaryingLinearInterpolation)
        .rw("triangle_subdivision_rule", &SubdivisionDesc::triangleSubdivisionRule)
        .finish();
    Struct<MeshDesc>(m, "MeshDesc", "A mesh in its own space: a triangle list or a polygon mesh (prism/scene.h).")
        .transform("points", &MeshDesc::points)
        .rw("face_vertex_counts", &MeshDesc::faceVertexCounts)
        .rw("face_vertex_indices", &MeshDesc::faceVertexIndices)
        .rw("normals", &MeshDesc::normals)
        .en("normals_interpolation", &MeshDesc::normalsInterpolation)
        .rw("uvs", &MeshDesc::uvs)
        .en("uvs_interpolation", &MeshDesc::uvsInterpolation)
        .rw("tangents", &MeshDesc::tangents)
        .rw("primvars", &MeshDesc::primvars)
        .rw("velocities", &MeshDesc::velocities)
        .rw("accelerations", &MeshDesc::accelerations)
        .rw("subdivision", &MeshDesc::subdivision)
        .rw("left_handed", &MeshDesc::leftHanded)
        .rw("face_material_slots", &MeshDesc::faceMaterialSlots)
        .rw("double_sided", &MeshDesc::doubleSided)
        .rw("single_sided_faces", &MeshDesc::singleSidedFaces)
        .rw("instanced", &MeshDesc::instanced)
        .finish();
    Struct<SpheresDesc>(m, "SpheresDesc", "Analytic spheres in their own space.")
        .transform("centers", &SpheresDesc::centers)
        .rw("radii", &SpheresDesc::radii)
        .finish();
    Struct<InstanceDesc>(m, "InstanceDesc", "A placement of a mesh or of spheres.")
        .rw("prototype", &InstanceDesc::prototype)
        .transform("transform", &InstanceDesc::transform, "Local to world (row vectors, translation in row 3).")
        .rw("materials", &InstanceDesc::materials)
        .rw("primvars", &InstanceDesc::primvars)
        .rw("visible", &InstanceDesc::visible)
        .rw("light_link_categories", &InstanceDesc::lightLinkCategories)
        .rw("shadow_link_categories", &InstanceDesc::shadowLinkCategories)
        .rw("prim_id", &InstanceDesc::primId)
        .rw("instance_id", &InstanceDesc::instanceId)
        .rw("user_data", &InstanceDesc::userData)
        .finish();
    Struct<LightDesc::Shaping>(m, "LightShaping", "UsdLuxShapingAPI.")
        .rw("active", &LightDesc::Shaping::active)
        .rw("focus", &LightDesc::Shaping::focus)
        .rw("focus_tint", &LightDesc::Shaping::focusTint)
        .rw("cone_angle", &LightDesc::Shaping::coneAngle)
        .rw("cone_softness", &LightDesc::Shaping::coneSoftness)
        .rw("ies_file", &LightDesc::Shaping::iesFile)
        .rw("ies_angle_scale", &LightDesc::Shaping::iesAngleScale)
        .rw("ies_normalize", &LightDesc::Shaping::iesNormalize)
        .finish();
    Struct<LightDesc>(m, "LightDesc", "A UsdLux analytic light; `color` is linear Rec.709.")
        .en("shape", &LightDesc::shape)
        .transform("transform", &LightDesc::transform)
        .rw("color", &LightDesc::color)
        .rw("intensity", &LightDesc::intensity)
        .rw("exposure", &LightDesc::exposure)
        .rw("diffuse", &LightDesc::diffuse)
        .rw("specular", &LightDesc::specular)
        .rw("normalize", &LightDesc::normalize)
        .rw("camera_visible", &LightDesc::cameraVisible)
        .rw("radius", &LightDesc::radius)
        .rw("width", &LightDesc::width)
        .rw("height", &LightDesc::height)
        .rw("length", &LightDesc::length)
        .rw("angle", &LightDesc::angle)
        .rw("treat_as_point", &LightDesc::treatAsPoint)
        .rw("shaping", &LightDesc::shaping)
        .rw("texture", &LightDesc::texture)
        .rw("texture_color_space", &LightDesc::textureColorSpace)
        .rw("light_link_category", &LightDesc::lightLinkCategory)
        .rw("shadow_link_category", &LightDesc::shadowLinkCategory)
        .finish();
    Struct<EnvironmentDesc>(m, "EnvironmentDesc", "A dome, a lat-long image or a constant radiance.")
        .en("kind", &EnvironmentDesc::kind)
        .rw("texture", &EnvironmentDesc::texture)
        .rw("texture_color_space", &EnvironmentDesc::textureColorSpace)
        .rw("color", &EnvironmentDesc::color)
        .rw("color_space", &EnvironmentDesc::colorSpace)
        .rw("intensity", &EnvironmentDesc::intensity)
        .rw("exposure", &EnvironmentDesc::exposure)
        .rw("diffuse", &EnvironmentDesc::diffuse)
        .rw("specular", &EnvironmentDesc::specular)
        .transform("transform", &EnvironmentDesc::transform)
        .finish();
    Struct<CameraDesc>(m, "CameraDesc", "A USD-style camera (lens values in tenths of the scene unit).")
        .transform("camera_to_world", &CameraDesc::cameraToWorld)
        .en("projection", &CameraDesc::projection)
        .rw("focal_length", &CameraDesc::focalLength)
        .rw("horizontal_aperture", &CameraDesc::horizontalAperture)
        .rw("vertical_aperture", &CameraDesc::verticalAperture)
        .rw("horizontal_aperture_offset", &CameraDesc::horizontalApertureOffset)
        .rw("vertical_aperture_offset", &CameraDesc::verticalApertureOffset)
        .rw("near_clip", &CameraDesc::nearClip)
        .rw("far_clip", &CameraDesc::farClip)
        .rw("f_stop", &CameraDesc::fStop)
        .rw("focus_distance", &CameraDesc::focusDistance)
        .rw("exposure_scale", &CameraDesc::exposureScale)
        .finish();
    Struct<MaterialDesc>(m, "MaterialDesc", "A MaterialX document (file or string), a Diffuse or an Emissive.")
        .en("kind", &MaterialDesc::kind)
        .rw("path", &MaterialDesc::path)
        .rw("document", &MaterialDesc::document)
        .rw("material_name", &MaterialDesc::materialName)
        .rw("search_path", &MaterialDesc::searchPath)
        .rw("color", &MaterialDesc::color)
        .rw("opacity", &MaterialDesc::opacity)
        .rw("intensity", &MaterialDesc::intensity)
        .rw("display_color_primvar", &MaterialDesc::displayColorPrimvar)
        .rw("display_opacity_primvar", &MaterialDesc::displayOpacityPrimvar)
        .rw("name", &MaterialDesc::name)
        .finish();
    Struct<SceneOptions>(m, "SceneOptions")
        .rw("subdivision_level", &SceneOptions::subdivisionLevel)
        .rw("subdivision_max_faces", &SceneOptions::subdivisionMaxFaces)
        .rw("subdivision_budget_faces", &SceneOptions::subdivisionBudgetFaces)
        .rw("displacement_level", &SceneOptions::displacementLevel)
        .rw("displacement_max_faces", &SceneOptions::displacementMaxFaces)
        .rw("default_dome", &SceneOptions::defaultDome)
        .rw("default_dome_texture", &SceneOptions::defaultDomeTexture)
        .rw("time_codes_per_second", &SceneOptions::timeCodesPerSecond)
        .finish();
    Struct<Diagnostic>(m, "Diagnostic")
        .ro("level", &Diagnostic::level)
        .ro("category", &Diagnostic::category)
        .ro("message", &Diagnostic::message)
        .finish(false);
    Struct<CommitResult>(m, "CommitResult")
        .ro("revision", &CommitResult::revision)
        .ro("meshes", &CommitResult::meshes)
        .ro("instances", &CommitResult::instances)
        .ro("triangles", &CommitResult::triangles)
        .ro("materials", &CommitResult::materials)
        .ro("lights", &CommitResult::lights)
        .ro("seconds", &CommitResult::seconds)
        .ro("dirty", &CommitResult::dirty)
        .ro("materials_compiled", &CommitResult::materialsCompiled)
        .ro("meshes_converted", &CommitResult::meshesConverted)
        .ro("meshes_reused", &CommitResult::meshesReused)
        .ro("placements_attached", &CommitResult::placementsAttached)
        .ro("placements_updated", &CommitResult::placementsUpdated)
        .ro("acceleration_shared", &CommitResult::accelerationShared)
        .ro("revision_reused", &CommitResult::revisionReused)
        .ro("lighting_rebuilt", &CommitResult::lightingRebuilt)
        .finish(false);
    Struct<ProgressInfo>(m, "ProgressInfo", "Progress of a commit or a render (passed to progress callbacks).")
        .ro("phase", &ProgressInfo::phase)
        .ro("detail", &ProgressInfo::detail)
        .ro("fraction", &ProgressInfo::fraction)
        .ro("done", &ProgressInfo::done)
        .ro("total", &ProgressInfo::total)
        .ro("samples", &ProgressInfo::samples)
        .ro("target_samples", &ProgressInfo::targetSamples)
        .ro("active_fraction", &ProgressInfo::activeFraction)
        .ro("elapsed_seconds", &ProgressInfo::elapsedSeconds)
        .ro("samples_per_second", &ProgressInfo::samplesPerSecond)
        .ro("estimated_seconds_remaining", &ProgressInfo::estimatedSecondsRemaining)
        .finish(false);

    // ── Render settings and session options
    Struct<NeeSettings>(m, "NeeSettings")
        .rw("probability_min", &NeeSettings::probabilityMin)
        .rw("from_depth", &NeeSettings::fromDepth)
        .rw("spatial_lights", &NeeSettings::spatialLights)
        .finish();
    Struct<DenoiseSettings>(m, "DenoiseSettings")
        .en("quality", &DenoiseSettings::quality)
        .rw("hdr", &DenoiseSettings::hdr)
        .rw("prefilter", &DenoiseSettings::prefilter)
        .finish();
    Struct<Shutter>(m, "Shutter").rw("open", &Shutter::open).rw("close", &Shutter::close).finish();
    Struct<RenderSettings>(m, "RenderSettings", "Sampling and integrator settings of a session.")
        .rw("samples", &RenderSettings::samples)
        .rw("adaptive_threshold", &RenderSettings::adaptiveThreshold)
        .rw("adaptive_min_samples", &RenderSettings::adaptiveMinSamples)
        .rw("time_limit_seconds", &RenderSettings::timeLimitSeconds)
        .rw("max_depth", &RenderSettings::maxDepth)
        .rw("roulette_depth", &RenderSettings::rouletteDepth)
        .rw("seed", &RenderSettings::seed)
        .rw("sample_clamp", &RenderSettings::sampleClamp)
        .rw("exposure", &RenderSettings::exposure)
        .rw("environment_mis", &RenderSettings::environmentMis)
        .en("texture_filtering", &RenderSettings::textureFiltering)
        .rw("honor_double_sided", &RenderSettings::honorDoubleSided)
        .rw("nee", &RenderSettings::nee)
        .rw("texture_streaming", &RenderSettings::textureStreaming)
        .rw("gpu_texture_budget_mib", &RenderSettings::gpuTextureBudgetMiB)
        .rw("debug_contributions", &RenderSettings::debugContributions)
        .rw("shutter", &RenderSettings::shutter)
        .finish();
    Struct<Window>(m, "Window", "A rectangle of pixels, top row first.")
        .rw("x", &Window::x)
        .rw("y", &Window::y)
        .rw("width", &Window::width)
        .rw("height", &Window::height)
        .finish();
    Struct<ProgressiveOptions>(m, "ProgressiveOptions")
        .rw("adaptive_preview", &ProgressiveOptions::adaptivePreview)
        .rw("preview_target_ms", &ProgressiveOptions::previewTargetMs)
        .rw("preview_max_dimension", &ProgressiveOptions::previewMaxDimension)
        .rw("refinement_delay_ms", &ProgressiveOptions::refinementDelayMs)
        .rw("denoise_interval", &ProgressiveOptions::denoiseInterval)
        .rw("gpu_preparation_preview", &ProgressiveOptions::gpuPreparationPreview)
        .rw("gpu_preparation_preview_max_mib", &ProgressiveOptions::gpuPreparationPreviewMaxMiB)
        .finish();
    {
        Struct<SessionOptions> options(m, "SessionOptions");
        options.en("device", &SessionOptions::device)
            .rw("allow_fallback", &SessionOptions::allowFallback)
            .rw("width", &SessionOptions::width)
            .rw("height", &SessionOptions::height)
            .rw("data_window", &SessionOptions::dataWindow)
            .rw("pixel_aspect_ratio", &SessionOptions::pixelAspectRatio)
            .en("conform_policy", &SessionOptions::conformPolicy)
            .rw("camera", &SessionOptions::camera)
            .rw("camera_desc", &SessionOptions::cameraDesc);
        options.cls.def_prop_rw(
            "aovs", [](SessionOptions const& o) { return o.aovs; },
            [](SessionOptions& o, nb::handle value) { o.aovs = AovsFrom(value); });
        options.names.push_back("aovs");
        options.rw("settings", &SessionOptions::settings)
            .rw("denoise", &SessionOptions::denoise)
            .rw("denoised_color", &SessionOptions::denoisedColor)
            .rw("progressive", &SessionOptions::progressive)
            .finish();
    }

    // ── Renderer options
    Struct<ColorOptions>(m, "ColorOptions")
        .rw("config", &ColorOptions::config)
        .rw("working_space", &ColorOptions::workingSpace)
        .rw("display", &ColorOptions::display)
        .rw("view", &ColorOptions::view)
        .rw("rendering_color_space", &ColorOptions::renderingColorSpace)
        .finish();
    Struct<MaterialOptions>(m, "MaterialOptions")
        .rw("materialx_libraries", &MaterialOptions::materialxLibraries)
        .rw("materialx_mdl_path", &MaterialOptions::materialxMdlPath)
        .rw("sss_fallback", &MaterialOptions::sssFallback)
        .en("compile_mode", &MaterialOptions::compileMode)
        .rw("prewarm", &MaterialOptions::prewarm)
        .rw("parallel_texture_preparation", &MaterialOptions::parallelTexturePreparation)
        .rw("generation_cache", &MaterialOptions::generationCache)
        .rw("mdl_resource_mode", &MaterialOptions::mdlResourceMode)
        .finish();
    Struct<Limits>(m, "Limits")
        .rw("material_threads", &Limits::materialThreads)
        .rw("texture_threads", &Limits::textureThreads)
        .rw("material_cache_mib", &Limits::materialCacheMiB)
        .rw("preparation_mib", &Limits::preparationMiB)
        .rw("texture_prepare_mib", &Limits::texturePrepareMiB)
        .rw("disk_cache_gib", &Limits::diskCacheGiB)
        .rw("texture_cache_mib", &Limits::textureCacheMiB)
        .rw("texture_open_files", &Limits::textureOpenFiles)
        .rw("texture_writer_threads", &Limits::textureWriterThreads)
        .finish();
    Struct<TextureOptions>(m, "TextureOptions")
        .rw("compression", &TextureOptions::compression)
        .rw("background_writes", &TextureOptions::backgroundWrites)
        .finish();
    Struct<DeviceOptions>(m, "DeviceOptions")
        .rw("disable_vulkan", &DeviceOptions::disableVulkan)
        .rw("disable_metal", &DeviceOptions::disableMetal)
        .rw("initial_texture_budget_mib", &DeviceOptions::initialTextureBudgetMiB)
        .finish();
    {
        Struct<RendererOptions> options(m, "RendererOptions",
                                        "Options of a renderer; from_environment() reads OCIO and PRISM_*.");
        options.rw("color", &RendererOptions::color)
            .rw("materials", &RendererOptions::materials)
            .rw("limits", &RendererOptions::limits)
            .rw("textures", &RendererOptions::textures)
            .rw("devices", &RendererOptions::devices)
            .en("log_level", &RendererOptions::logLevel)
            .rw("debug_categories", &RendererOptions::debugCategories)
            .rw("cache_directory", &RendererOptions::cacheDirectory)
            .finish();
        options.cls.def_static("from_environment", &RendererOptions::FromEnvironment,
                               nb::call_guard<nb::gil_scoped_release>());
    }
    m.def("default_cache_directory", &DefaultCacheDirectory);

    // ── Renderer information
    Struct<DeviceInfo>(m, "DeviceInfo")
        .ro("kind", &DeviceInfo::kind)
        .ro("name", &DeviceInfo::name)
        .ro("hardware_ray_tracing", &DeviceInfo::hardwareRayTracing)
        .finish(false);
    Struct<ColorInfo>(m, "ColorInfo")
        .ro("config", &ColorInfo::config)
        .ro("config_name", &ColorInfo::configName)
        .ro("working_space", &ColorInfo::workingSpace)
        .ro("display", &ColorInfo::display)
        .ro("view", &ColorInfo::view)
        .finish(false);
    Struct<Capabilities>(m, "Capabilities")
        .ro("denoising", &Capabilities::denoising)
        .ro("denoising_note", &Capabilities::denoisingNote)
        .ro("motion_blur", &Capabilities::motionBlur)
        .finish(false);
    Struct<RendererStats>(m, "RendererStats")
        .ro("texture_prepare_calls", &RendererStats::texturePrepareCalls)
        .ro("texture_memory_cache_hits", &RendererStats::textureMemoryCacheHits)
        .ro("texture_disk_cache_hits", &RendererStats::textureDiskCacheHits)
        .ro("texture_source_bytes_hashed", &RendererStats::textureSourceBytesHashed)
        .ro("texture_preparation_nanoseconds", &RendererStats::texturePreparationNanoseconds)
        .finish(false);

    // ── Images and results
    nb::class_<Image>(m, "Image", nb::dynamic_attr(),
                      "Host pixels (height x width x channels, top row first) with shared ownership. "
                      "np.asarray(image) and DLPack consumers see the pixels without a copy (read-only).")
        .def(nb::init<>())
        .def("__init__", [](Image* self, InputArray array) { new (self) Image(ImageFromArray(std::move(array))); },
             "array"_a, "A copy of a float32 or int32 (height, width[, channels]) array.")
        .def_prop_ro("width", &Image::width)
        .def_prop_ro("height", &Image::height)
        .def_prop_ro("channels", &Image::channels)
        .def_prop_ro("type", &Image::type)
        .def_prop_ro("shape",
                     [](Image const& i) { return nb::make_tuple(i.height(), i.width(), i.channels()); })
        .def_prop_ro("dtype", [](Image const& i) { return i.type() == DataType::Int32 ? "int32" : "float32"; })
        .def_prop_ro("address", [](Image const& i) { return uintptr_t(i.data()); })
        .def_prop_ro("empty", &Image::empty)
        .def("numpy", [](Image const& i) { return ImageArray<nb::numpy>(i); },
             "A read-only NumPy view of the pixels (no copy).")
        .def(
            "__array__",
            [](Image const& i, nb::handle dtype, nb::handle copy) {
                nb::object array = ImageArray<nb::numpy>(i);
                if (!dtype.is_none()) array = array.attr("astype")(dtype);
                else if (!copy.is_none() && nb::cast<bool>(copy)) array = array.attr("copy")();
                return array;
            },
            "dtype"_a = nb::none(), "copy"_a = nb::none())
        .def(
            "__dlpack__",
            [](Image const& i, nb::kwargs) {
                // A framework-less ndarray casts to the DLPack capsule itself.
                return ImageArray<nb::no_framework>(i);
            },
            "kwargs"_a)
        .def("__dlpack_device__", [](Image const&) { return nb::make_tuple(1, 0); })  // kDLCPU
        .def(
            "write",
            [](nb::handle self, std::string const& path, nb::handle renderer, nb::handle aov) {
                Image const& image = nb::cast<Image const&>(self);
                if (renderer.is_none() && nb::hasattr(self, "_renderer")) renderer = self.attr("_renderer");
                PyRenderer& r = RendererOf(renderer, "Image.write");
                const Aov which = aov.is_none() ? (image.type() == DataType::Int32 ? Aov::PrimId : Aov::Color)
                                                : EnumFrom<Aov>(aov, "aov");
                nb::gil_scoped_release release;
                return WriteImage(image, path, r, which);
            },
            "path"_a, "renderer"_a = nb::none(), "aov"_a = nb::none(),
            "Writes the image through prism_io: EXR linear in the working space, other formats through the "
            "renderer's display and view. Returns the warnings.")
        .def("__repr__", [](Image const& i) {
            std::ostringstream out;
            if (i.empty()) return std::string("Image(empty)");
            out << "Image(" << i.width() << "x" << i.height() << ", " << i.channels() << " channel"
                << (i.channels() == 1 ? "" : "s") << ", " << (i.type() == DataType::Int32 ? "int32" : "float32")
                << ")";
            return out.str();
        });

    nb::class_<RenderResult>(m, "RenderResult", nb::dynamic_attr())
        .def_prop_ro("aovs",
                     [](nb::handle self) {
                         nb::handle renderer = nb::hasattr(self, "_renderer") ? nb::handle(self.attr("_renderer"))
                                                                              : nb::handle();
                         return AovDict(nb::cast<RenderResult const&>(self), renderer);
                     },
                     "The rendered AOVs by Hydra token ('color', 'albedo', ...): dataWindow-sized images.")
        .def_ro("width", &RenderResult::width)
        .def_ro("height", &RenderResult::height)
        .def_ro("data_window", &RenderResult::dataWindow)
        .def_ro("traced", &RenderResult::traced)
        .def_ro("mean_samples", &RenderResult::meanSamples)
        .def_ro("fraction_at_max_samples", &RenderResult::fractionAtMaxSamples)
        .def_ro("time_limit_reached", &RenderResult::timeLimitReached)
        .def_ro("render_seconds", &RenderResult::renderSeconds)
        .def_ro("denoise_milliseconds", &RenderResult::denoiseMilliseconds)
        .def_ro("denoiser", &RenderResult::denoiser)
        .def("has", [](RenderResult const& r, nb::handle aov) { return r.has(EnumFrom<Aov>(aov, "aov")); }, "aov"_a)
        .def(
            "aov",
            [](nb::handle self, nb::handle aov) {
                nb::object image = nb::cast(nb::cast<RenderResult const&>(self).aov(EnumFrom<Aov>(aov, "aov")));
                if (nb::hasattr(self, "_renderer")) nb::setattr(image, "_renderer", self.attr("_renderer"));
                return image;
            },
            "aov"_a)
        .def(
            "write",
            [](RenderResult const& result, std::string const& path, nb::handle renderer, nb::handle layers,
               bool multiPart) {
                PyRenderer& r = RendererOf(renderer, "RenderResult.write");
                io::WriteOptions options;
                options.multiPart = multiPart;
                if (!layers.is_none())
                    for (nb::handle layer : nb::borrow<nb::sequence>(layers)) {
                        io::Layer l;
                        l.aov = EnumFrom<Aov>(layer, "layers");
                        options.layers.push_back(l);
                    }
                std::vector<std::string> warnings;
                nb::gil_scoped_release release;
                io::Write(path, result, r.get(), options, &warnings);
                return warnings;
            },
            "path"_a, "renderer"_a, "layers"_a = nb::none(), "multi_part"_a = false,
            "Writes the result through prism_io (EXR: every layer; others: the colour). Returns the warnings.")
        .def("__repr__", [](RenderResult const& r) {
            std::ostringstream out;
            out << "RenderResult(" << r.dataWindow.width << "x" << r.dataWindow.height << ", aovs=[";
            bool first = true;
            for (auto const& [aov, image] : r.aovs) {
                out << (first ? "" : ", ") << "'" << AovName(aov) << "'";
                first = false;
            }
            out << "], mean_samples=" << r.meanSamples << ")";
            return out.str();
        });
    m.def("is_multi_channel_format", &io::IsMultiChannelFormat, "path"_a,
          "True for formats written as linear floats with every layer (EXR).");

    Struct<Pick>(m, "Pick")
        .ro("hit", &Pick::hit)
        .ro("instance", &Pick::instance)
        .ro("prim_id", &Pick::primId)
        .ro("instance_id", &Pick::instanceId)
        .ro("element_id", &Pick::elementId)
        .ro("user_data", &Pick::userData)
        .ro("position", &Pick::position)
        .ro("normal", &Pick::normal)
        .ro("depth", &Pick::depth)
        .finish(false);
    Struct<SessionStatus>(m, "SessionStatus")
        .ro("state", &SessionStatus::state)
        .ro("samples", &SessionStatus::samples)
        .ro("target_samples", &SessionStatus::targetSamples)
        .ro("fraction", &SessionStatus::fraction)
        .ro("active_fraction", &SessionStatus::activeFraction)
        .ro("time_limit_reached", &SessionStatus::timeLimitReached)
        .ro("elapsed_seconds", &SessionStatus::elapsedSeconds)
        .ro("samples_per_second", &SessionStatus::samplesPerSecond)
        .ro("estimated_seconds_remaining", &SessionStatus::estimatedSecondsRemaining)
        .ro("accumulations", &SessionStatus::accumulations)
        .ro("revision", &SessionStatus::revision)
        .ro("error", &SessionStatus::error)
        .finish(false);

    // ── Renderer, Scene, Session
    nb::class_<PyRenderer>(m, "Renderer",
                           "A renderer context: colour management, materials, textures, Embree, logging. "
                           "Several renderers per process are allowed.")
        .def(
            "__init__",
            [](PyRenderer* self, std::optional<RendererOptions> options, nb::object log) {
                RendererOptions o = options ? *options : RendererOptions();
                if (!options) {
                    nb::gil_scoped_release release;
                    o = RendererOptions::FromEnvironment();
                }
                SharedObject sink;
                if (!log.is_none()) {
                    sink = Share(log);
                    // On any thread; through the GIL, never while finalizing.
                    o.log = [sink](LogLevel level, std::string_view category, std::string_view message) {
                        if (!PythonAlive()) return;
                        nb::gil_scoped_acquire gil;
                        try {
                            (*sink)(level, nb::str(category.data(), category.size()),
                                    nb::str(message.data(), message.size()));
                        } catch (nb::python_error& e) {
                            e.discard_as_unraisable("prismrt log callback");
                        }
                    };
                }
                new (self) PyRenderer();
                self->log = sink;
                nb::gil_scoped_release release;
                self->renderer.emplace(o);
            },
            "options"_a = nb::none(), "log"_a = nb::none(),
            "A renderer with `options` (default: RendererOptions.from_environment()); `log(level, category, "
            "message)` receives its diagnostics on any thread.")
        .def_prop_ro("options", [](PyRenderer& r) { return r.get().options(); })
        .def("color", [](PyRenderer& r) { return r.get().color(); }, nb::call_guard<nb::gil_scoped_release>())
        .def("devices", [](PyRenderer& r) { return r.get().devices(); }, nb::call_guard<nb::gil_scoped_release>())
        .def("capabilities", [](PyRenderer& r) { return r.get().capabilities(); },
             nb::call_guard<nb::gil_scoped_release>())
        .def("stats", [](PyRenderer& r) { return r.get().stats(); }, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "create_scene",
            [](PyRenderer& r, SceneOptions const& options) {
                auto scene = std::make_unique<PyScene>();
                nb::gil_scoped_release release;
                scene->scene.emplace(r.get().createScene(options));
                return scene.release();
            },
            "options"_a = SceneOptions(), nb::rv_policy::take_ownership)
        .def(
            "create_session",
            [](PyRenderer& r, PyScene& scene, SessionOptions const& options) {
                auto session = std::make_unique<PySession>();
                nb::gil_scoped_release release;
                session->session.emplace(r.get().createSession(scene.get(), options));
                return session.release();
            },
            "scene"_a, "options"_a = SessionOptions(), nb::rv_policy::take_ownership);

    nb::class_<PyScene>(m, "Scene",
                        "A scene of a renderer: edits are recorded and published by commit(). One writer at a time.")
        .def("add_mesh", [](PyScene& s, MeshDesc const& d) { return s.get().addMesh(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_mesh", [](PyScene& s, MeshId id, MeshDesc const& d) { s.get().updateMesh(id, d); }, "id"_a,
             "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "update_mesh_points",
            [](PyScene& s, MeshId id, nb::handle points) {
                Sampled<BufferView> samples = SampledFrom<BufferView>(points, "points");
                nb::gil_scoped_release release;
                s.get().updateMeshPoints(id, samples);
            },
            "id"_a, "points"_a)
        .def("add_spheres", [](PyScene& s, SpheresDesc const& d) { return s.get().addSpheres(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_spheres", [](PyScene& s, SpheresId id, SpheresDesc const& d) { s.get().updateSpheres(id, d); },
             "id"_a, "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("add_instance", [](PyScene& s, InstanceDesc const& d) { return s.get().addInstance(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_instance",
             [](PyScene& s, InstanceId id, InstanceDesc const& d) { s.get().updateInstance(id, d); }, "id"_a,
             "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "set_transform",
            [](PyScene& s, InstanceId id, nb::handle transform) {
                Sampled<Matrix4d> samples = SampledFrom<Matrix4d>(transform, "transform");
                nb::gil_scoped_release release;
                s.get().setTransform(id, samples);
            },
            "id"_a, "transform"_a)
        .def("set_visible", [](PyScene& s, InstanceId id, bool visible) { s.get().setVisible(id, visible); }, "id"_a,
             "visible"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("set_materials",
             [](PyScene& s, InstanceId id, std::vector<MaterialId> const& m) { s.get().setMaterials(id, m); },
             "id"_a, "materials"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("add_material", [](PyScene& s, MaterialDesc const& d) { return s.get().addMaterial(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_material",
             [](PyScene& s, MaterialId id, MaterialDesc const& d) { s.get().updateMaterial(id, d); }, "id"_a,
             "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("add_light", [](PyScene& s, LightDesc const& d) { return s.get().addLight(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_light", [](PyScene& s, LightId id, LightDesc const& d) { s.get().updateLight(id, d); }, "id"_a,
             "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("add_environment", [](PyScene& s, EnvironmentDesc const& d) { return s.get().addEnvironment(d); },
             "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("update_environment",
             [](PyScene& s, EnvironmentId id, EnvironmentDesc const& d) { s.get().updateEnvironment(id, d); },
             "id"_a, "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("add_camera", [](PyScene& s, CameraDesc const& d) { return s.get().addCamera(d); }, "desc"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("update_camera", [](PyScene& s, CameraId id, CameraDesc const& d) { s.get().updateCamera(id, d); },
             "id"_a, "desc"_a, nb::call_guard<nb::gil_scoped_release>())
        .def("camera", [](PyScene& s, CameraId id) { return s.get().camera(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, MeshId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, SpheresId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, InstanceId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, MaterialId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, LightId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, EnvironmentId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("remove", [](PyScene& s, CameraId id) { s.get().remove(id); }, "id"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def(
            "commit",
            [](PyScene& s, nb::object progress) {
                auto error = std::make_shared<CallbackError>();
                auto stop = std::make_shared<std::atomic<bool>>(false);
                prism::ProgressCallback callback = ProgressFromPython(std::move(progress), error, stop);
                prism::Scene scene = s.get();
                try {
                    return RunInterruptible<CommitResult>([scene, callback]() mutable { return scene.commit(callback); },
                                                          [stop] { stop->store(true); });
                } catch (prism::Error const& e) {
                    if (e.code() == ErrorCode::Cancelled)
                        if (auto pending = error->take()) std::rethrow_exception(pending);
                    throw;
                }
            },
            "progress"_a = nb::none(),
            "Publishes the edits as a new revision (blocking, without the GIL). `progress(info)` returning "
            "False, or Ctrl-C, cancels; the edits then stay pending.")
        .def_prop_ro("revision", [](PyScene& s) { return s.get().revision(); })
        .def_prop_ro("pending_edits", [](PyScene& s) { return s.get().pendingEdits(); },
                     "The DirtyKind bits of the edits since the last commit (0: none).")
        .def("diagnostics", [](PyScene& s) { return s.get().diagnostics(); })
        .def_prop_ro("options", [](PyScene& s) { return s.get().options(); });

    nb::class_<PySession>(m, "Session", "A device, render settings and a camera rendering a scene's revisions.")
        .def_prop_ro("device", [](PySession& s) { return s.get().device(); })
        .def_prop_ro("device_note", [](PySession& s) { return s.get().deviceNote(); })
        .def_prop_ro("options", [](PySession& s) { return s.get().options(); })
        .def("set_settings", [](PySession& s, RenderSettings const& r) { s.get().setSettings(r); }, "settings"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("set_camera", [](PySession& s, CameraId id) { s.get().setCamera(id); }, "camera"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def("set_camera", [](PySession& s, CameraDesc const& d) { s.get().setCamera(d); }, "camera"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def(
            "resize",
            [](PySession& s, unsigned width, unsigned height, std::optional<Window> window, float pixelAspectRatio,
               nb::handle policy) {
                const ConformPolicy p = EnumFrom<ConformPolicy>(policy, "conform_policy");
                nb::gil_scoped_release release;
                s.get().resize(width, height, window, pixelAspectRatio, p);
            },
            "width"_a, "height"_a, "data_window"_a = nb::none(), "pixel_aspect_ratio"_a = 1.0f,
            "conform_policy"_a = ConformPolicy::ExpandAperture)
        .def(
            "set_aovs",
            [](PySession& s, nb::handle aovs) {
                std::vector<Aov> list = AovsFrom(aovs);
                nb::gil_scoped_release release;
                s.get().setAovs(list);
            },
            "aovs"_a)
        .def("set_denoise",
             [](PySession& s, std::optional<DenoiseSettings> d, bool denoisedColor) {
                 s.get().setDenoise(d, denoisedColor);
             },
             "denoise"_a, "denoised_color"_a = false, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "prepare",
            [](PySession& s) {
                prism::Session session = s.get();
                RunInterruptible<void>([session]() mutable { session.prepare(); }, {});
            },
            "Device setup for the scene's committed revision (optional: render() does it).")
        .def(
            "render",
            [](nb::handle self, nb::object progress) {
                PySession& s = nb::cast<PySession&>(self);
                auto error = std::make_shared<CallbackError>();
                prism::ProgressCallback callback = ProgressFromPython(std::move(progress), error);
                prism::Session session = s.get();
                try {
                    RenderResult result = RunInterruptible<RenderResult>(
                        [session, callback]() mutable { return session.render(callback); },
                        [session]() mutable { session.cancel(); });
                    return nb::cast(std::move(result));
                } catch (prism::Error const& e) {
                    if (e.code() == ErrorCode::Cancelled)
                        if (auto pending = error->take()) std::rethrow_exception(pending);
                    throw;
                }
            },
            "progress"_a = nb::none(),
            "Renders the scene's last committed revision to completion (blocking, without the GIL). "
            "`progress(info)` runs on a C++ thread; False or Ctrl-C cancels (CancelledError / KeyboardInterrupt).")
        .def("cancel", [](PySession& s) { s.get().cancel(); }, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "start",
            [](PySession& s, nb::object progress) {
                s.progressiveError->take();
                prism::ProgressCallback callback = ProgressFromPython(std::move(progress), s.progressiveError);
                nb::gil_scoped_release release;
                s.get().start(callback);
            },
            "progress"_a = nb::none(),
            "Starts progressive rendering on the session's thread; returns at once.")
        .def("pause", [](PySession& s) { s.get().pause(); }, nb::call_guard<nb::gil_scoped_release>())
        .def("resume", [](PySession& s) { s.get().resume(); }, nb::call_guard<nb::gil_scoped_release>())
        .def("restart", [](PySession& s) { s.get().restart(); }, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "wait",
            [](PySession& s, double timeout) {
                // 100 ms slices with signal checks (Ctrl-C raises KeyboardInterrupt;
                // the render keeps running).
                const auto start = std::chrono::steady_clock::now();
                prism::Session session = s.get();
                for (;;) {
                    double slice = 0.1;
                    if (timeout >= 0) {
                        const double left =
                            timeout - std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                        slice = std::max(0.0, std::min(slice, left));
                    }
                    SessionStatus status;
                    {
                        nb::gil_scoped_release release;
                        status = session.wait(slice);
                    }
                    if (PyErr_CheckSignals() != 0) throw nb::python_error();
                    if (status.state != SessionState::Running) return status;
                    if (timeout >= 0 &&
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= timeout)
                        return status;
                }
            },
            "timeout"_a = -1.0,
            "Waits until the progressive render is not running or the timeout (seconds; negative: none) passed; "
            "rethrows a failed render's error.")
        .def("status", [](PySession& s) { return s.get().status(); }, nb::call_guard<nb::gil_scoped_release>())
        .def(
            "aov",
            [](PySession& s, nb::handle aov) {
                const Aov which = EnumFrom<Aov>(aov, "aov");
                nb::gil_scoped_release release;
                return s.get().aov(which);
            },
            "aov"_a = "color", "The progressive render's last published pass of an AOV (a snapshot).")
        .def("pick", [](PySession& s, unsigned x, unsigned y) { return s.get().pick(x, y); }, "x"_a, "y"_a,
             nb::call_guard<nb::gil_scoped_release>())
        .def(
            "_take_callback_error",
            [](PySession& s) {
                if (auto error = s.progressiveError->take()) std::rethrow_exception(error);
            },
            "Raises the exception a progressive callback raised (which cancelled the render), once.");

    // ── glTF and OBJ
    Struct<gltf::LoadResult>(m, "GltfLoadResult")
        .ro("mesh", &gltf::LoadResult::mesh)
        .ro("instance", &gltf::LoadResult::instance)
        .ro("triangles", &gltf::LoadResult::triangles)
        .ro("materials", &gltf::LoadResult::materials)
        .ro("failed_materials", &gltf::LoadResult::failedMaterials)
        .finish(false);
    Struct<obj::LoadResult>(m, "ObjLoadResult")
        .ro("mesh", &obj::LoadResult::mesh)
        .ro("instance", &obj::LoadResult::instance)
        .ro("triangles", &obj::LoadResult::triangles)
        .ro("materials", &obj::LoadResult::materials)
        .ro("failed_materials", &obj::LoadResult::failedMaterials)
        .finish(false);
    m.def("load_gltf", [](PyScene& s, std::filesystem::path const& path) { return gltf::Load(s.get(), path); },
          "scene"_a, "path"_a, nb::call_guard<nb::gil_scoped_release>(),
          "Adds a .gltf/.glb file's default scene: one mesh, one instance and its materials.");
    m.def("load_obj", [](PyScene& s, std::filesystem::path const& path) { return obj::Load(s.get(), path); },
          "scene"_a, "path"_a, nb::call_guard<nb::gil_scoped_release>(),
          "Adds an OBJ file: one mesh, one instance and its MTL materials.");
}

} // namespace prism::python

NB_MODULE(_prism, m)
{
    // Objects the interpreter still holds at exit are released by the process.
    nb::set_leak_warnings(false);
    prism::python::BindModule(m);
}
