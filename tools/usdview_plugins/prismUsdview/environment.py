"""Prism > Environment...: an HDR dome light authored in usdview's session layer.

The pure functions work on a Usd.Stage and only ever write to its session layer,
never to the asset's layers. The dialog (EnvironmentDialog) is the usdview UI.

The environment is a UsdLux.DomeLight at /__PrismEnvironment with
inputs:texture:file, inputs:texture:format = latlong, inputs:intensity,
inputs:exposure and one rotate xformOp about the stage's up axis. It is a light
of the stage, so PrismRT stops using usdview's default dome light while it
exists; removing it brings the default dome back.

A stage that has dome lights of its own would get both lit twice (PrismRT
combines several domes), so by default applying an environment deactivates those
domes in the session layer (`active = false`) and removing it clears that
override again. The prim records the paths it deactivated in its customData.
"""

import glob
import os

from pxr import Plug, Sdf, Tf, Usd, UsdGeom, UsdLux, Vt

ENVIRONMENT_PATH = Sdf.Path("/__PrismEnvironment")
_DEACTIVATED_KEY = "prismDeactivatedDomes"
EXTENSIONS = (".hdr", ".exr")
ROOTS_ENV = "PRISM_USDVIEW_ENVIRONMENT_DIR"


def _session_edit(stage):
    return Usd.EditContext(stage, stage.GetSessionLayer())


def stage_dome_lights(stage):
    """Active dome lights of the composed stage, except the Prism environment."""
    domes = []
    for prim in stage.Traverse():
        if prim.GetPath() == ENVIRONMENT_PATH:
            continue
        if prim.IsA(UsdLux.DomeLight):
            domes.append(prim.GetPath())
    return domes


def environment_prim(stage):
    prim = stage.GetPrimAtPath(ENVIRONMENT_PATH)
    return prim if prim and prim.IsValid() else None


def current_settings(stage):
    """(texture path, intensity, exposure, rotation degrees) of the active
    environment, or None when there is none."""
    prim = environment_prim(stage)
    if prim is None:
        return None
    light = UsdLux.DomeLight(prim)
    texture = light.GetTextureFileAttr().Get()
    rotation = 0.0
    for op in UsdGeom.Xformable(prim).GetOrderedXformOps():
        if op.GetOpType() in (UsdGeom.XformOp.TypeRotateY, UsdGeom.XformOp.TypeRotateZ):
            rotation = float(op.Get() or 0.0)
    return (texture.path if texture else "",
            float(light.GetIntensityAttr().Get()),
            float(light.GetExposureAttr().Get()), rotation)


def _up_axis_op(xformable, up_axis):
    if up_axis == UsdGeom.Tokens.z:
        return xformable.AddRotateZOp()
    return xformable.AddRotateYOp()


def set_environment(stage, texture, intensity=1.0, exposure=0.0, rotation=0.0,
                    replace_stage_domes=True):
    """Author (or update) the environment dome in the session layer.

    Returns the paths of the stage's own dome lights that were deactivated.
    """
    with _session_edit(stage):
        existing = environment_prim(stage)
        previous = []
        if existing is not None:
            previous = list(existing.GetCustomDataByKey(_DEACTIVATED_KEY) or [])
        light = UsdLux.DomeLight.Define(stage, ENVIRONMENT_PATH)
        prim = light.GetPrim()
        light.CreateTextureFileAttr().Set(Sdf.AssetPath(str(texture).replace("\\", "/")))
        light.CreateTextureFormatAttr().Set(UsdLux.Tokens.latlong)
        light.CreateIntensityAttr().Set(float(intensity))
        light.CreateExposureAttr().Set(float(exposure))
        xform = UsdGeom.Xformable(prim)
        op = None
        for candidate in xform.GetOrderedXformOps():
            if candidate.GetOpType() in (UsdGeom.XformOp.TypeRotateY,
                                         UsdGeom.XformOp.TypeRotateZ):
                op = candidate
        if op is None:
            op = _up_axis_op(xform, UsdGeom.GetStageUpAxis(stage))
        op.Set(float(rotation))

        deactivated = list(previous)
        if replace_stage_domes:
            for path in stage_dome_lights(stage):
                if str(path) not in deactivated:
                    stage.GetPrimAtPath(path).SetActive(False)
                    deactivated.append(str(path))
        else:
            _reactivate(stage, previous)
            deactivated = []
        prim.SetCustomDataByKey(_DEACTIVATED_KEY, Vt.StringArray(deactivated))
        return [Sdf.Path(p) for p in deactivated]


def _reactivate(stage, paths):
    layer = stage.GetSessionLayer()
    for path in paths:
        spec = layer.GetPrimAtPath(Sdf.Path(path))
        if spec is not None:
            spec.ClearActive()
            _prune_empty_over(layer, spec)


def _prune_empty_over(layer, spec):
    """Drop session `over`s left empty by clearing an active opinion."""
    while spec is not None and spec.specifier == Sdf.SpecifierOver and \
            not spec.properties and not spec.nameChildren and \
            set(spec.ListInfoKeys()) <= {"specifier"}:
        parent = spec.nameParent or layer.pseudoRoot  # None for a root prim
        del parent.nameChildren[spec.name]
        spec = None if parent == layer.pseudoRoot else parent


def remove_environment(stage):
    """Remove the environment and restore any stage domes it deactivated.

    Returns True when there was an environment to remove.
    """
    prim = environment_prim(stage)
    if prim is None:
        return False
    deactivated = list(prim.GetCustomDataByKey(_DEACTIVATED_KEY) or [])
    with _session_edit(stage):
        _reactivate(stage, deactivated)
        if not stage.RemovePrim(ENVIRONMENT_PATH):
            Tf.Warn("prismUsdview: could not remove %s" % ENVIRONMENT_PATH)
    return True


def usd_default_environment():
    """USD's default dome texture (Stinson Beach), found via the hdx plugin."""
    plugin = Plug.Registry().GetPluginWithName("hdx")
    if not plugin:
        return None
    path = os.path.join(plugin.resourcePath, "textures", "StinsonBeach.hdr")
    return path if os.path.isfile(path) else None


def _asset_roots(environ=os.environ, start=None):
    roots = [r for r in environ.get(ROOTS_ENV, "").split(os.pathsep) if r]
    here = os.path.abspath(start or os.path.dirname(__file__))
    for _ in range(8):
        candidate = os.path.join(here, "assets", "environment")
        if os.path.isdir(candidate):
            roots.append(candidate)
            break
        parent = os.path.dirname(here)
        if parent == here:
            break
        here = parent
    return roots


def environment_choices(environ=os.environ, start=None):
    """[(label, path)]: USD's Stinson Beach, then the HDR/EXR files of the
    repository's assets/environment (or $PRISM_USDVIEW_ENVIRONMENT_DIR)."""
    choices = []
    beach = usd_default_environment()
    if beach:
        choices.append(("Stinson Beach (USD default)", beach))
    seen = set()
    for root in _asset_roots(environ, start):
        for path in sorted(glob.glob(os.path.join(root, "*"))):
            key = os.path.normcase(os.path.abspath(path))
            if path.lower().endswith(EXTENSIONS) and key not in seen:
                seen.add(key)
                choices.append((os.path.basename(path), path))
    return choices


def dialog_class():
    """The dialog, built on first use so the module imports without Qt widgets."""
    from pxr.Usdviewq.qt import QtCore, QtWidgets

    class EnvironmentDialog(QtWidgets.QDialog):
        _last = {}  # choices of this session, kept across openings

        def __init__(self, api):
            super().__init__(api.qMainWindow)
            self._api = api
            self.setWindowTitle("Prism Environment")
            self._building = True
            layout = QtWidgets.QFormLayout(self)

            self.source = QtWidgets.QComboBox()
            self.source.addItem("Off (usdview default dome)", None)
            for label, path in environment_choices():
                self.source.addItem(label, path)
            self.browse = QtWidgets.QPushButton("Browse...")
            row = QtWidgets.QHBoxLayout()
            row.addWidget(self.source, 1)
            row.addWidget(self.browse)
            layout.addRow("Environment", row)

            self.intensity = QtWidgets.QDoubleSpinBox()
            self.intensity.setRange(0.0, 1.0e6)
            self.intensity.setDecimals(3)
            self.intensity.setSingleStep(0.1)
            self.exposure = QtWidgets.QDoubleSpinBox()
            self.exposure.setRange(-20.0, 20.0)
            self.exposure.setDecimals(2)
            self.exposure.setSingleStep(0.25)
            self.rotation = QtWidgets.QDoubleSpinBox()
            self.rotation.setRange(-360.0, 360.0)
            self.rotation.setDecimals(1)
            self.rotation.setSingleStep(5.0)
            self.rotation.setSuffix(" deg")
            layout.addRow("Intensity", self.intensity)
            layout.addRow("Exposure (stops)", self.exposure)
            self.rotation_label = QtWidgets.QLabel("Rotation")
            layout.addRow(self.rotation_label, self.rotation)

            self.replace = QtWidgets.QCheckBox("Deactivate the stage's own dome lights")
            self.replace.setChecked(True)
            layout.addRow(self.replace)
            self.note = QtWidgets.QLabel()
            self.note.setWordWrap(True)
            layout.addRow(self.note)

            self._load()
            self._building = False
            self.source.currentIndexChanged.connect(self._changed)
            for spin in (self.intensity, self.exposure, self.rotation):
                spin.valueChanged.connect(self._changed)
            self.replace.toggled.connect(self._changed)
            self.browse.clicked.connect(self._browse)
            self._changed()

        def _stage(self):
            return self._api.stage

        def _load(self):
            stage = self._stage()
            up = "Z" if UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z else "Y"
            self.rotation_label.setText("Rotation (about %s, the up axis)" % up)
            state = current_settings(stage)
            last = self._last
            intensity, exposure, rotation = 1.0, 0.0, 0.0
            if state:
                path, intensity, exposure, rotation = state
                self._select(path)
            if not state:
                intensity = last.get("intensity", intensity)
                exposure = last.get("exposure", exposure)
                rotation = last.get("rotation", rotation)
            self.intensity.setValue(intensity)
            self.exposure.setValue(exposure)
            self.rotation.setValue(rotation)
            self.replace.setChecked(last.get("replace", True))

        def _select(self, path):
            for i in range(self.source.count()):
                data = self.source.itemData(i)
                if data and os.path.normcase(data) == os.path.normcase(path):
                    self.source.setCurrentIndex(i)
                    return
            self.source.addItem(os.path.basename(path), path)
            self.source.setCurrentIndex(self.source.count() - 1)

        def _browse(self):
            path, _ = QtWidgets.QFileDialog.getOpenFileName(
                self, "Choose an HDR or EXR environment", "",
                "Environment maps (*.hdr *.exr);;All files (*)")
            if path:
                self._select(path)

        def _changed(self, *unused):
            if self._building:
                return
            stage = self._stage()
            path = self.source.currentData()
            self._last.update(path=path or self._last.get("path"),
                              intensity=self.intensity.value(),
                              exposure=self.exposure.value(),
                              rotation=self.rotation.value(),
                              replace=self.replace.isChecked())
            domes = stage_dome_lights(stage)
            if path:
                deactivated = set_environment(
                    stage, path, self.intensity.value(), self.exposure.value(),
                    self.rotation.value(), self.replace.isChecked())
                text = "Authored in the session layer as %s." % ENVIRONMENT_PATH
                if deactivated:
                    text += " Deactivated stage dome light(s): %s." % ", ".join(
                        str(p) for p in deactivated)
                elif domes:
                    text += (" The stage has its own dome light(s) (%s), which "
                             "PrismRT combines with this one." %
                             ", ".join(str(p) for p in domes))
            else:
                remove_environment(stage)
                text = "No environment: the stage's lights, or usdview's default dome."
            self.note.setText(text)

    return EnvironmentDialog


_dialogs = {}


def show_dialog(api):
    """Command callback: open (or raise) the environment dialog."""
    dialog = _dialogs.get("dialog")
    if dialog is None or dialog._stage() is not api.stage:
        dialog = _dialogs["dialog"] = dialog_class()(api)
        dialog.finished.connect(lambda *_: _dialogs.pop("dialog", None))
    dialog.show()
    dialog.raise_()
    dialog.activateWindow()
