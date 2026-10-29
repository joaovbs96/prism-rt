"""The prismrt API on the CPU: exceptions, zero-copy inputs and outputs,
names and params, from_dict/to_dict, vectors as tuples, enums as strings,
the logging bridge and several renderers in one process."""

from __future__ import annotations

import logging
import sys

import numpy as np
import pytest

import prismrt as prism
from conftest import cpu_session, quad_scene


def test_module(renderer):
    assert prism.__version__
    assert prism._prism.__file__.endswith(".pyd") or prism._prism.__file__.endswith(".so")
    devices = renderer.devices()
    assert devices and devices[-1].kind == prism.DeviceKind.Cpu
    assert renderer.color().working_space
    assert "Renderer(" in repr(renderer)


def test_exceptions_map_to_classes(renderer, tmp_path):
    assert issubclass(prism.InvalidArgumentError, prism.Error)
    assert issubclass(prism.InvalidArgumentError, ValueError)
    assert issubclass(prism.InvalidHandleError, ValueError)
    assert issubclass(prism.NotFoundError, LookupError)
    assert issubclass(prism.UnsupportedError, NotImplementedError)
    assert issubclass(prism.OutOfMemoryError, MemoryError)
    assert issubclass(prism.Error, RuntimeError)

    scene = quad_scene(renderer)
    # No committed revision: the C++ session throws InvalidArgument.
    session = cpu_session(renderer, scene)
    with pytest.raises(prism.InvalidArgumentError):
        session.render(allow_uncommitted=True)
    # A removed handle.
    light = scene["key"]
    scene.remove("key")
    with pytest.raises(prism.InvalidHandleError):
        scene._native.update_light(light, prism.LightDesc())
    # A missing file.
    with pytest.raises(prism.NotFoundError) as error:
        prism.gltf.load(scene, tmp_path / "missing.gltf")
    assert isinstance(error.value, LookupError)
    # A device this build lacks, without fallback (no GPU is opened).
    if not any(d.kind == prism.DeviceKind.Metal for d in renderer.devices()):
        with pytest.raises(prism.UnsupportedError):
            renderer.create_session(scene, device="metal", allow_fallback=False, camera="camera")
    # A cancelled commit.
    with pytest.raises(prism.CancelledError):
        scene.commit(lambda info: False)
    # Bad shapes and values name the field.
    with pytest.raises(prism.InvalidArgumentError, match="points: expected \\(N, 3\\)"):
        scene.add_mesh(np.zeros((4, 2), np.float32), indices=[0, 1, 2])
    with pytest.raises(prism.InvalidArgumentError, match="unknown value 'triangle'"):
        scene.add_light("triangle")
    with pytest.raises(TypeError, match="unexpected keyword argument 'colour'"):
        prism.LightDesc(colour=(1, 1, 1))


def test_zero_copy_mesh_inputs(renderer):
    scene = renderer.create_scene()
    points = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], np.float32)
    indices = np.array([[0, 1, 2], [0, 2, 3]], np.int32)
    base_points, base_indices = sys.getrefcount(points), sys.getrefcount(indices)

    # The native description views the arrays themselves.
    view = prism.buffer(points, np.float32, 3, "points")
    assert view.address == points.ctypes.data and view.count == 4 and view.components == 3 and view.owned
    desc = prism.MeshDesc(points=view, face_vertex_indices=prism.buffer(indices, np.int32, 1, "indices"))
    assert desc.points[0][1].address == points.ctypes.data
    assert desc.face_vertex_indices.address == indices.ctypes.data and desc.face_vertex_indices.count == 6
    mesh = scene._native.add_mesh(desc)
    del view, desc
    # The scene holds a reference to each array (no copy) until commit reads them.
    assert sys.getrefcount(points) > base_points and sys.getrefcount(indices) > base_indices

    # The Python layer passes the same memory down.
    handle = scene.add_mesh(points, indices=indices, name="tri")
    stored = scene._entries["tri"].desc
    assert stored.points[0][1].address == points.ctypes.data
    assert stored.face_vertex_indices.address == indices.ctypes.data
    # float64 points are accepted as they are; other dtypes and layouts are converted once.
    points64 = points.astype(np.float64)
    assert prism.buffer(points64, np.float32, 3, "points", (np.float32, np.float64)).address == points64.ctypes.data
    assert prism.buffer(indices.astype(np.int64), np.int32, 1, "indices").address != indices.ctypes.data
    strided = np.zeros((4, 6), np.float32)[:, ::2]
    assert prism.buffer(strided, np.float32, 3, "points").address != strided.ctypes.data
    # params read copies.
    copy = scene.params["tri.points"]
    assert np.array_equal(copy, points) and not np.shares_memory(copy, points)

    scene.add_instance(mesh)
    scene.add_instance(handle)
    result = scene.commit()
    assert result.meshes == 2 and result.triangles == 4
    # In-place changes after commit have no effect until update_mesh_points and a commit.
    points[:] += 10
    assert scene.pending_edits == 0


def test_images_are_zero_copy_views(renderer, output_dir):
    scene = quad_scene(renderer)
    scene.commit()
    session = cpu_session(renderer, scene, aovs=["color", "prim_id", "depth"])
    result = session.render()
    assert set(result.aovs) == {"color", "primId", "depth"}
    image = result.aovs["color"]
    assert image.shape == (24, 32, 3) and image.dtype == "float32"
    array = np.asarray(image)
    assert array.shape == (24, 32, 3) and array.dtype == np.float32
    assert array.ctypes.data == image.address and not array.flags.writeable
    assert np.asarray(image).ctypes.data == image.address
    dlpack = np.from_dlpack(image)
    assert dlpack.ctypes.data == image.address
    assert image.__dlpack_device__() == (1, 0)
    ids = np.asarray(result.aov("primId"))
    assert ids.dtype == np.int32 and ids[12, 16, 0] == 7 and ids[0, 0, 0] == -1
    # The views keep the pixels alive after the result and image are gone.
    expected = array.copy()
    del result, image
    assert np.array_equal(array, expected) and array.max() > 0
    # Image(array) copies; Image.write goes through prism_io.
    copied = prism.Image(expected)
    assert copied.address != expected.ctypes.data and np.array_equal(np.asarray(copied), expected)
    assert "32x24" in repr(copied)
    assert copied.write(str(output_dir / "api_image.exr"), renderer._native) == []
    assert (output_dir / "api_image.exr").stat().st_size > 0
    with pytest.raises(prism.InvalidArgumentError, match="pass the renderer"):
        copied.write(str(output_dir / "api_image_no_renderer.exr"))
    # Images from a session know their renderer.
    again = session.render().aovs["color"]
    again.write(str(output_dir / "api_image.png"))
    assert (output_dir / "api_image.png").stat().st_size > 0


def test_params_and_commit(renderer):
    scene = quad_scene(renderer)
    scene.commit()
    assert scene.pending_edits == 0
    params = scene.params
    assert "key.intensity" in params and "quad_instance.transform" in params and "sky.color" in params
    assert params["key.intensity"] == 10
    session = cpu_session(renderer, scene)
    before = np.asarray(session.render().aovs["color"]).mean()

    params["key.intensity"] = 40
    assert scene.pending_edits & int(prism.DirtyKind.Light)
    # A render with uncommitted edits raises (the C++ session would render the last revision).
    with pytest.raises(prism.InvalidArgumentError, match="edits since its last commit"):
        session.render()
    result = params.update()
    assert result.dirty & int(prism.DirtyKind.Light) and scene.pending_edits == 0
    brighter = np.asarray(session.render().aovs["color"]).mean()
    assert brighter > before

    params["quad_instance.transform"] = prism.translate(5, 0, 0)  # out of view
    assert scene.pending_edits == int(prism.DirtyKind.Placement)
    result = scene.commit()
    assert result.placements_updated + result.placements_attached >= 1
    ids_session = cpu_session(renderer, scene, aovs=["primId"])
    ids = np.asarray(ids_session.render().aovs["primId"])
    assert (ids == -1).all()

    with pytest.raises(KeyError):
        params["nothing.intensity"] = 1
    scene.update_light("key", color=[1.0, 0.5, 0.25])
    assert params["key.color"] == pytest.approx((1.0, 0.5, 0.25))
    # Per-call overrides leave the session's settings as they were.
    scene.commit()
    session.render(samples=2, seed=5)
    assert session.settings.samples == 8 and session.settings.seed == 0


def test_from_dict_and_to_dict(renderer):
    description = {
        # Instances first: references resolve whatever the order.
        "instances": {
            "floor": {"prototype": "floor_mesh", "materials": [{"ref": "red"}]},
            "ball": {"prototype": "balls", "transform": prism.translate(0, 0, 0.5), "materials": ["white"]},
        },
        "materials": {"red": {"diffuse": (0.8, 0.1, 0.1)}, "white": {"diffuse": [0.8, 0.8, 0.8]}},
        "meshes": {"floor_mesh": {"points": [[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]],
                             "indices": [[0, 1, 2], [0, 2, 3]]}},
        "spheres": {"balls": {"centers": np.zeros((1, 3), np.float32), "radii": np.array([0.4], np.float32)}},
        "lights": [{"name": "key", "shape": "rect", "width": 2, "height": 2, "intensity": 10,
                    "transform": prism.translate(0, 0, 3)}],
        "environments": {"sky": {"color": (0.05, 0.05, 0.05)}},
        "cameras": {"main": {"look_at": {"eye": (0, 0, 5), "target": (0, 0, 0), "vertical_fov": 40}}},
    }
    scene = prism.Scene.from_dict(renderer, description)
    assert set(scene.names) >= {"floor", "floor_mesh", "ball", "balls", "red", "white", "key", "sky", "main"}
    with pytest.raises(prism.InvalidArgumentError, match="already used"):
        scene.add_material(diffuse=0.5, name="floor")
    scene.commit()
    session = cpu_session(renderer, scene, camera="main")
    image = np.asarray(session.render().aovs["color"]).copy()
    assert image.max() > 0

    # Round trip: the same image.
    again = prism.Scene.from_dict(renderer, scene.to_dict())
    again.commit()
    image2 = np.asarray(cpu_session(renderer, again, camera="main").render().aovs["color"])
    assert np.array_equal(image, image2)

    bad = dict(description)
    bad["instances"] = {"ball": {"prototype": "balls", "materials": ["gld"]}}
    with pytest.raises(prism.NotFoundError, match=r"scene\.instances\['ball'\]\.materials\[0\]: no material named "
                                                  r"'gld'"):
        prism.Scene.from_dict(renderer, bad)
    with pytest.raises(prism.InvalidArgumentError, match="unknown section"):
        prism.Scene.from_dict(renderer, {"lamps": {}})
    with pytest.raises(prism.InvalidArgumentError, match=r"scene\.lights\[0\]: .*unknown value 'blob'"):
        prism.Scene.from_dict(renderer, {"lights": [{"shape": "blob"}]})
    with pytest.raises(ValueError, match=r"scene\.meshes\['m'\]: points"):
        prism.Scene.from_dict(renderer, {"meshes": {"m": {"points": [[0, 0]], "indices": [0]}}})


def test_vectors_matrices_and_enums(renderer):
    assert prism.translate(1, 2, 3)[3].tolist() == [1, 2, 3, 1]
    assert np.allclose(prism.translate((1, 2, 3)), prism.translate(1, 2, 3))
    m = prism.look_at((0, 0, 5), (0, 0, 0))
    assert m.shape == (4, 4) and np.allclose(m[2, :3], [0, 0, 1]) and np.allclose(m[3, :3], [0, 0, 5])
    assert np.allclose(prism.rotate((0, 0, 1), 90)[0, :3], [0, 1, 0])  # x -> y (row vectors)
    light = prism.LightDesc(color=[1, 0.5, 0.25], transform=[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [1, 2, 3, 1]])
    assert light.color == pytest.approx((1, 0.5, 0.25))
    (time, matrix), = light.transform
    assert time == 0 and isinstance(matrix, np.ndarray) and matrix[3].tolist() == [1, 2, 3, 1]
    light.transform = [(0.0, np.eye(4)), (1.0, prism.translate(1, 0, 0))]
    assert len(light.transform) == 2
    light.color = 0.5
    assert light.color == (0.5, 0.5, 0.5)
    settings = prism.RenderSettings(texture_filtering="bilinear", samples=4)
    assert settings.texture_filtering == prism.TextureFiltering.Bilinear and settings.samples == 4
    assert prism.EnvironmentDesc(kind="lat_long").kind == prism.EnvironmentKind.LatLong
    options = prism.SessionOptions(aovs=["color", "primvars:st", "P"])
    assert options.aovs == [prism.Aov.Color, prism.Aov.St, prism.Aov.Pworld]
    assert prism.aov_name("primId") == "primId" and prism.aov_from_name("nothing") is None
    assert "RenderSettings(samples=4" in repr(settings)
    camera = prism.Camera.look_at((0, 1, 5), (0, 0, 0), vertical_fov=40)
    assert camera.vertical_aperture == pytest.approx(2 * 50 * np.tan(np.radians(20)), rel=1e-6)


def test_logging_bridge(renderer, caplog):
    received = []
    custom = prism.Renderer(working_space="lin_rec709", disable_vulkan=True, log=lambda level, category, message:
                            received.append((level, category, message)))
    scene = quad_scene(custom)
    light = prism.LightShaping(active=True, ies_file="no_such_profile.ies")
    scene.update_light("key", shaping=light)
    scene.commit()
    assert any(category == "lights" and "no_such_profile.ies" in message for _, category, message in received)
    # The default: the Python logger "prismrt".
    with caplog.at_level(logging.WARNING, logger="prismrt"):
        scene2 = quad_scene(renderer)
        scene2.update_light("key", shaping=prism.LightShaping(active=True, ies_file="another_missing.ies"))
        scene2.commit()
    assert any("another_missing.ies" in record.getMessage() and record.name == "prismrt.lights"
               for record in caplog.records)


def test_two_renderers(renderer):
    other = prism.Renderer(working_space="ACEScg", log=False, disable_vulkan=True)
    assert other.color().working_space != renderer.color().working_space
    images = []
    for r in (renderer, other):
        scene = quad_scene(r)
        scene.commit()
        images.append(np.asarray(cpu_session(r, scene).render().aovs["color"]).copy())
    assert images[0].max() > 0 and images[1].max() > 0
    assert not np.array_equal(images[0], images[1])


def test_camera_transform_helper(renderer):
    scene = quad_scene(renderer)
    cam = scene.camera("camera")
    assert isinstance(cam.camera_to_world, list)            # the C++ form: (time, matrix) samples
    m = prism.Camera.transform(cam)
    assert m.shape == (4, 4) and np.allclose(m, prism.translate(0, 0, 4))
    assert np.allclose(prism.matrix_at(cam.camera_to_world), m) and np.allclose(prism.matrix_at(m), m)
    # params reads match how transforms are written (a 4x4)
    assert np.allclose(scene.params["camera.camera_to_world"], prism.translate(0, 0, 4))
    assert np.allclose(scene.params["quad_instance.transform"], np.eye(4))
    # several samples: a time picks the nearest, none is an error
    scene.update_camera("camera", camera_to_world=[(0.0, prism.translate(0, 0, 4)), (1.0, prism.translate(0, 0, 8))])
    cam = scene.camera("camera")
    assert np.allclose(prism.Camera.transform(cam, time=0.9), prism.translate(0, 0, 8))
    assert np.allclose(prism.Camera.transform(cam, time=0.1), prism.translate(0, 0, 4))
    with pytest.raises(ValueError, match="time samples"):
        prism.Camera.transform(cam)
    assert len(scene.params["camera.camera_to_world"]) == 2


def test_camera_depth_aov_string(renderer):
    scene = quad_scene(renderer)
    scene.commit()
    session = cpu_session(renderer, scene, aovs=["color", "cameraDepth", "depth"])
    result = session.render()
    assert {"cameraDepth", "depth"} <= set(result.aovs.keys())
    assert result.has("cameraDepth") and result.has(prism.Aov.CameraDepth)
    distance = np.asarray(result.aov("cameraDepth"))
    assert distance.shape[-1] == 1 and distance[12, 16, 0] == pytest.approx(4.0, abs=1e-3)   # the quad at z = 0
    assert prism.aov_from_name("cameraDepth") == prism.Aov.CameraDepth
    assert "view axis" in (prism.Aov.__doc__ or "")


def test_names_are_unique_across_kinds(renderer):
    scene = quad_scene(renderer)
    with pytest.raises(prism.InvalidArgumentError) as info:
        scene.add_mesh(np.zeros((3, 3), np.float32), indices=np.array([0, 1, 2], np.int32), name="red")
    message = str(info.value)
    assert "'red' is already used by a material" in message and "unique across kinds" in message
    assert "'red_mat'" in message
    with pytest.raises(prism.InvalidArgumentError, match="already used by an instance"):
        scene.add_material(diffuse=(1, 1, 1), name="quad_instance")
    with pytest.raises(prism.InvalidArgumentError, match="already used by a camera"):
        scene.add_light("point", name="camera")
