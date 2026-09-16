# Launcher icon

A white location pin flows into a route with a sky-blue endpoint, on cobalt blue.
No text, Wi-Fi arcs, shadows, or baked-in texture.

## Files

- `generated-concept.png`: selected concept from the built-in image_gen tool.
- `launcher.svg`: clean, manually reconstructed vector artwork for deterministic exports.
- `launcher-preview.png`: 512px preview rendered from that SVG.
- `../../android/XiaoGpsTrackerApp/app/src/main/res/drawable/ic_launcher_foreground.xml`:
  matching Android vector, with 108dp layers and artwork inside the central 66dp circle.
- `mipmap-anydpi-v26`: adaptive launcher resources for Android 8+.
- `mipmap-anydpi-v33`: explicit monochrome layer for supported themed launchers.
- Density PNGs: standard and circular compatibility icons, 48/72/96/144/192px.

Android's [adaptive icon guidance](https://developer.android.com/develop/ui/compose/system/icon_design_adaptive)
defines layer geometry, mask safety, and monochrome behavior. The vector foreground
has transparent negative space; the launcher applies its own mask to the solid background.
The two-color foreground's alpha also supplies the themed monochrome silhouette.

## Regenerate compatibility PNGs

Install `@resvg/resvg-js@2.6.2` in a temporary directory outside the project, then run:

```sh
node scripts/render_launcher_icons.cjs /path/to/node_modules/@resvg/resvg-js
```

Keep the paths and scale in the SVG and Android vector in sync. Only exports use
rounded/circular masks; the adaptive background remains full-bleed.
The app manifest and application ID are unchanged. Only an APK update is needed.

## Image-generation provenance

Skill: imagegen. Mode: built-in tool, not API/CLI fallback. The generated raster
was used as the design reference; native vectors remove artifacts and guarantee
sharp edges and predictable launcher masking.

Initial prompt:

> Use case: logo-brand. Asset type: production Android adaptive launcher icon foreground for XIAO GPS Tracker. Create ONE polished, minimal, flat graphic symbol: a bold white GPS location pin whose lower tip flows into a short rounded winding route, ending in one small sky-blue waypoint dot. The pin has a clean genuinely transparent circular cutout. Strong simple silhouette, smooth precise edges, chunky strokes legible at 48 pixels, balanced negative space. Existing app uses cobalt blue and white. This foreground will be placed over a solid cobalt blue background by Android. Canvas: square 1024 x 1024 with a GENUINELY TRANSPARENT background and alpha channel, not a checkerboard painting. Center the complete symbol, including route and dot, entirely inside a central circle of diameter 580 pixels; generous transparent margins on all four sides are mandatory for Android adaptive icon masks. White main mark, pale sky-blue waypoint accent only. No text, no letters, no WiFi arcs, no satellite drawing, no maps, no border, no rounded-square tile, no background, no shadow, no glow, no texture, no 3D, no mockup, no watermark. Deliver a single finished foreground asset, not a sheet of options.

Final refinement prompt (input: first generated foreground):

> Refine this image into a finished production Android launcher icon. Keep exactly the location-pin flowing into a short curved route ending in a sky-blue dot, but fix all ragged edges and stray pixels: perfectly smooth flat graphic silhouette, opaque pure-white mark with a perfectly circular clean opening. Replace ALL transparency, holes, exterior and background with a uniform solid cobalt blue #225DD8. Absolutely no flecks, texture, noise, shadows, glow or gradients. Full-bleed square blue canvas, no rounded corners, no border, no mockup. Center and scale the entire white symbol and dot so ALL non-blue artwork fits inside the central circular safe area whose diameter is 58% of canvas width. The background fills the whole canvas edge to edge. No text or extra elements. Deliver a single clean square icon.
