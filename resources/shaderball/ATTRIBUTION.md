# Standard Shader Ball (USD Working Group assets)

The files in this directory, except this note and `.gitattributes`, are the
**Standard Shader Ball** of the USD Working Group's assets repository,
copied unchanged:

- Source: https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall
- Contributors (from the asset's README): geometry and textures by Chris
  Rydalch; specification and validation by André Mazzone; original scene,
  inspiration and consultation by Thomas Anagnostou (his "Simball" /
  "Material Preview" scene, http://rayflectar.com/p05-Misc/misc.html).
- Licence: Creative Commons Attribution 4.0 International (CC BY 4.0),
  https://creativecommons.org/licenses/by/4.0/ — full text in `LICENCE`.

PrismRT ships a subset (what the scene's default variants compose, plus the
example materials and emitter maps its variants reference): the scene file,
`layers/`, `material_surface_geo/subdiv.usda`, `example_materials/` and
`maps/`. Not shipped: the `triangulated` geometry variant, the README, the
media, cards, thumbnails and source files. No file was modified.

PrismRT uses it as the built-in material preview (`PrismRT --material`,
`prismrt.preview_material`; docs/material_preview.md): the previewed
material is bound to `/standard_shader_ball_scene/shader_ball/material_surface`
when the stage is loaded, without editing the files.
