#include "library.h"

#include <cctype>
#include <cstdlib>
#include <mutex>

#include "core.h"

namespace pt {

// Smart materials are layer-stack templates. "${name}" is replaced by a parameter value;
// "${expr}" may use + - * / and parentheses over numeric params, e.g. "${wear*0.5}" or "${seed+2}".
// Layers are bottom-to-top. Inner ids are namespaced under the instance id at expansion time.
// Split into several raw literals: MSVC caps a single string literal at 16 KB.
static const char* kBuiltinLibrary = R"JSON(
{
"smart_materials": {
  "metal": {
    "description": "Generic clean metal with subtle roughness variation and faint smudges.",
    "params": {"color": {"default": "#b8b9ba", "doc": "metal reflectance color (sRGB)"},
               "roughness": {"default": 0.35, "doc": "base roughness"},
               "seed": {"default": 0, "doc": "variation seed"}},
    "layers": [
      {"id": "base", "channels": {"basecolor": "${color}", "metallic": 1,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 5, "seed": "${seed}", "range": ["${roughness-0.07}", "${roughness+0.07}"]}}},
      {"id": "smudges", "opacity": 0.5, "channels": {"roughness": "${roughness+0.25}"},
        "mask": [{"type": "grunge", "style": "smudge", "amount": 0.35, "scale": 3, "seed": "${seed+1}"}]}
    ]
  },
  "steel": {"description": "Clean steel.", "extends": "metal", "params": {"color": {"default": "#b4b5b6"}, "roughness": {"default": 0.32}}},
  "iron": {"description": "Dark raw iron.", "extends": "metal", "params": {"color": {"default": "#8d8f91"}, "roughness": {"default": 0.5}}},
  "aluminum": {"description": "Aluminum.", "extends": "metal", "params": {"color": {"default": "#e8e9ea"}, "roughness": {"default": 0.3}}},
  "chrome": {"description": "Polished chrome.", "extends": "metal", "params": {"color": {"default": "#cfd0d1"}, "roughness": {"default": 0.06}}},
  "gold": {"description": "Polished gold.", "extends": "metal", "params": {"color": {"default": "#ffd88a"}, "roughness": {"default": 0.2}}},
  "copper": {"description": "Copper.", "extends": "metal", "params": {"color": {"default": "#f7b594"}, "roughness": {"default": 0.28}}},
  "brass": {"description": "Brass.", "extends": "metal", "params": {"color": {"default": "#d9ba7c"}, "roughness": {"default": 0.3}}},

  "brushed_metal": {
    "description": "Brushed metal with fine directional streaks in roughness and height.",
    "params": {"color": {"default": "#c9cacb"}, "roughness": {"default": 0.3}, "stretch": {"default": [40, 1, 1], "doc": "brush direction as feature stretch, e.g. [40,1,1] along X"},
               "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": "${color}", "metallic": 1,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 60, "stretch": "${stretch}", "octaves": 3, "seed": "${seed}", "range": ["${roughness-0.1}", "${roughness+0.1}"]},
        "height": {"type": "noise", "noise": "fbm", "scale": 90, "stretch": "${stretch}", "octaves": 2, "seed": "${seed+1}", "range": [-0.03, 0.03]}}}
    ]
  },

  "painted_metal": {
    "description": "Painted metal: paint over bare metal, chipped on edges, dirt in crevices.",
    "params": {"color": {"default": "#2f5d8c", "doc": "paint color"}, "wear": {"default": 0.4, "doc": "0..1 edge chipping"},
               "dirt": {"default": 0.35, "doc": "0..1 grime in crevices"}, "metal": {"default": "#9a9b9c", "doc": "bare metal color"},
               "roughness": {"default": 0.45, "doc": "paint roughness"}, "seed": {"default": 0}},
    "layers": [
      {"id": "metal", "channels": {"basecolor": "${metal}", "metallic": 1,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 8, "seed": "${seed}", "range": [0.25, 0.45]}}},
      {"id": "paint", "channels": {"basecolor": "${color}", "metallic": 0,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 4, "seed": "${seed+2}", "range": ["${roughness-0.06}", "${roughness+0.06}"]},
        "height": 0.25},
        "mask": [{"type": "edge_wear", "amount": "${wear}", "seed": "${seed+3}", "invert": true}]},
      {"id": "paint_tone", "opacity": 0.35, "blend": "multiply", "channels": {"basecolor": {"type": "noise", "noise": "fbm", "scale": 3, "seed": "${seed+4}", "gradient": ["#b0b0b0", "#ffffff"]}},
        "mask": [{"type": "layer", "layer": "paint"}]},
      {"id": "dirt", "opacity": 0.85, "channels": {"basecolor": "#3b3026", "roughness": 0.85, "metallic": 0},
        "mask": [{"type": "dirt", "amount": "${dirt}", "seed": "${seed+5}"}]}
    ]
  },

  "rusted_metal": {
    "description": "Iron with rust patches, rust in crevices and rust streaks.",
    "params": {"rust": {"default": 0.55, "doc": "0..1 rust coverage"}, "metal": {"default": "#6f7173"}, "seed": {"default": 0}},
    "layers": [
      {"id": "iron", "channels": {"basecolor": "${metal}", "metallic": 1,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 10, "seed": "${seed}", "range": [0.35, 0.6]}}},
      {"id": "rust", "channels": {
          "basecolor": {"type": "noise", "noise": "fbm", "scale": 9, "seed": "${seed+1}", "warp": 0.6, "gradient": ["#3a1d10", "#6b3a1e", "#8f4f27", "#b0692f"]},
          "metallic": 0, "roughness": {"type": "noise", "noise": "turbulence", "scale": 12, "seed": "${seed+2}", "range": [0.75, 0.95]},
          "height": {"type": "noise", "noise": "turbulence", "scale": 18, "seed": "${seed+3}", "range": [0.0, 0.25]}},
        "mask": [{"type": "grunge", "style": "rust", "amount": "${rust}", "seed": "${seed+4}"},
                 {"type": "cavity", "blend": "max", "levels": [0.15, 0.6], "opacity": "${rust}"}]},
      {"id": "streaks", "opacity": 0.7, "channels": {"basecolor": "#6b3a1e", "roughness": 0.85, "metallic": 0},
        "mask": [{"type": "streaks", "amount": "${rust*0.7}", "seed": "${seed+5}"}]}
    ]
  },

  "wrought_iron": {
    "description": "Forged iron for hoops, bands, hinges and fittings: gunmetal with hammer marks, mottled forge scale, grime in cavities, crumbly rust in crevices and on lower edges, bright worn edges.",
    "params": {"tint": {"default": "#56524d", "doc": "iron color; keep it mid-gray (a metal's base color is its reflectance: near-black metal renders black)"},
               "wear": {"default": 0.5, "doc": "0..1 bright worn edges"},
               "rust": {"default": 0.35, "doc": "0..1 rust amount"},
               "hammered": {"default": 1, "doc": "0..1 hammer-mark strength"},
               "seed": {"default": 0}},
    "layers": [
      {"id": "iron", "channels": {
          "basecolor": {"type": "noise", "noise": "fbm", "scale": 14, "seed": "${seed}", "gradient": [[0, "#3e3c39"], [0.5, "${tint}"], [1, "#67625b"]]},
          "metallic": 1,
          "roughness": {"type": "noise", "noise": "fbm", "scale": 22, "seed": "${seed+1}", "range": [0.36, 0.6]},
          "height": {"type": "noise", "noise": "cells", "scale": 26, "jitter": 0.9, "seed": "${seed+2}", "power": 0.6, "range": ["${0-0.3*hammered}", 0.0]}}},
      {"id": "pitting", "channels": {
          "roughness": {"type": "noise", "noise": "fbm", "scale": 140, "seed": "${seed+14}", "range": [-0.06, 0.1]},
          "height": {"type": "noise", "noise": "dots", "scale": 260, "size": 0.16, "jitter": 1, "seed": "${seed+15}", "range": [0.0, -0.05]}},
        "blend_modes": {"roughness": "add", "height": "add"}},
      {"id": "scale", "opacity": 0.65, "channels": {"basecolor": "#23211f", "metallic": 0.7, "roughness": 0.58},
        "mask": [{"type": "noise", "noise": "fbm", "scale": 7, "seed": "${seed+3}", "range": [-0.6, 1.3]},
                 {"type": "noise", "noise": "fbm", "scale": 60, "seed": "${seed+4}", "range": [0.5, 1.2], "blend": "multiply"}]},
      {"id": "grime", "opacity": 0.8, "channels": {"basecolor": "#1b1917", "roughness": 0.78, "metallic": 0.4},
        "mask": [{"type": "dirt", "amount": 0.45, "seed": "${seed+5}"}]},
      {"id": "rust", "channels": {
          "basecolor": {"type": "noise", "noise": "fbm", "scale": 110, "seed": "${seed+6}", "warp": 0.5, "gradient": [[0, "#24120a"], [0.45, "#43230f"], [0.8, "#6b3517"], [1, "#8c4a22"]]},
          "metallic": 0, "roughness": {"type": "noise", "noise": "fbm", "scale": 90, "seed": "${seed+7}", "range": [0.8, 0.95]},
          "height": {"type": "noise", "noise": "turbulence", "scale": 80, "seed": "${seed+8}", "range": [0.0, 0.08]}},
        "mask": [{"type": "dirt", "amount": "${rust}", "breakup": 0.8, "seed": "${seed+9}"},
                 {"type": "direction", "direction": "down", "min": 0.1, "max": 0.7, "breakup": 0.7, "scale": 30, "seed": "${seed+10}", "blend": "max", "opacity": "${rust}"},
                 {"type": "grunge", "style": "rust", "amount": "${rust*0.45}", "scale": 16, "seed": "${seed+11}", "blend": "max"},
                 {"type": "noise", "noise": "fbm", "scale": 150, "seed": "${seed+12}", "range": [-0.4, 1.8], "blend": "multiply"}]},
      {"id": "worn", "channels": {"basecolor": "#a09a91", "metallic": 1, "roughness": 0.26},
        "mask": [{"type": "edge_wear", "amount": "${wear}", "seed": "${seed+13}"}]}
    ]
  },
  "plastic": {
    "description": "Molded plastic with subtle roughness variation and light edge wear.",
    "params": {"color": {"default": "#c23b22"}, "roughness": {"default": 0.4}, "wear": {"default": 0.15}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": "${color}", "metallic": 0,
        "roughness": {"type": "noise", "noise": "fbm", "scale": 6, "seed": "${seed}", "range": ["${roughness-0.05}", "${roughness+0.05}"]},
        "height": {"type": "noise", "noise": "white", "scale": 400, "seed": "${seed+1}", "range": [0, 0.01]}}},
      {"id": "edge_rub", "opacity": 0.5, "blend": "screen", "channels": {"basecolor": "#5a5a5a", "roughness": "${roughness+0.15}"},
        "mask": [{"type": "edge_wear", "amount": "${wear}", "width": 0.3, "seed": "${seed+2}"}]}
    ]
  },
  "rubber": {
    "description": "Dark matte rubber with dust in crevices.",
    "params": {"color": {"default": "#1c1c1e"}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": "${color}", "metallic": 0, "roughness": {"type": "noise", "scale": 8, "seed": "${seed}", "range": [0.8, 0.92]}}},
      {"id": "dust", "opacity": 0.6, "channels": {"basecolor": "#6a655d", "roughness": 0.95}, "mask": [{"type": "dirt", "amount": 0.3, "seed": "${seed+1}"}]}
    ]
  },
  "wood": {
    "description": "Procedural wood: growth rings around the grain axis, fibers along it, per-UV-island tone variation (boards). space=uv runs the grain along V of every UV island (staves, planks laid out lengthwise).",
    "params": {"light": {"default": "#c0925a", "doc": "early wood"}, "dark": {"default": "#6e4323", "doc": "late wood"},
               "axis": {"default": "up", "doc": "grain direction: up | right | front (in uv space: up = along V)"},
               "space": {"default": "object", "doc": "object | uv (grain follows each UV island along V)"},
               "rings": {"default": 14, "doc": "growth rings per object size"}, "roughness": {"default": 0.55}, "seed": {"default": 0}},
    "layers": [
      {"id": "rings", "channels": {
          "basecolor": {"type": "noise", "space": "${space}", "noise": "rings", "axis": "${axis}", "stretch": "${axis}", "scale": "${rings}", "warp": 0.35, "offset": [0.31, 0.0, 0.17], "seed": "${seed}",
                        "gradient": [[0, "${light}"], [0.6, "${light}"], [1, "${dark}"]]},
          "metallic": 0,
          "roughness": {"type": "noise", "space": "${space}", "noise": "rings", "axis": "${axis}", "stretch": "${axis}", "scale": "${rings}", "warp": 0.35, "offset": [0.31, 0.0, 0.17], "seed": "${seed}",
                        "range": ["${roughness-0.06}", "${roughness+0.08}"]},
          "height": {"type": "noise", "space": "${space}", "noise": "rings", "axis": "${axis}", "stretch": "${axis}", "scale": "${rings}", "warp": 0.35, "offset": [0.31, 0.0, 0.17], "seed": "${seed}",
                     "range": [0.0, -0.04]}}},
      {"id": "fibers", "opacity": 0.45, "blend": "multiply", "channels": {
          "basecolor": {"type": "noise", "space": "${space}", "noise": "fbm", "scale": 90, "stretch": "${axis}", "octaves": 3, "seed": "${seed+2}", "gradient": ["#9c8c7a", "#ffffff"]},
          "height": {"type": "noise", "space": "${space}", "noise": "ridged", "scale": 120, "stretch": "${axis}", "octaves": 2, "seed": "${seed+4}", "range": [-0.02, 0.0]}},
        "blend_modes": {"height": "add"}},
      {"id": "boards", "opacity": 0.35, "blend": "multiply", "channels": {"basecolor": {"type": "island_random", "seed": "${seed+3}", "gradient": ["#c9b8a6", "#ffffff"]}}}
    ]
  },
)JSON" R"JSON(
  "leather": {
    "description": "Leather with pore grain, darker creases and lighter worn edges.",
    "params": {"color": {"default": "#5a3825"}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": {"type": "noise", "noise": "fbm", "scale": 5, "seed": "${seed}", "gradient": ["${color}", "${color}"]},
        "metallic": 0, "roughness": {"type": "noise", "noise": "cells", "scale": 90, "seed": "${seed+1}", "range": [0.5, 0.7]},
        "height": {"type": "noise", "noise": "cells", "scale": 90, "seed": "${seed+1}", "range": [0.08, 0.0]}}},
      {"id": "tone", "opacity": 0.3, "blend": "multiply", "channels": {"basecolor": {"type": "noise", "noise": "fbm", "scale": 4, "seed": "${seed+2}", "gradient": ["#9a9a9a", "#ffffff"]}}},
      {"id": "creases", "opacity": 0.6, "blend": "multiply", "channels": {"basecolor": "#7a7a7a"}, "mask": [{"type": "cavity", "levels": [0.1, 0.5]}]},
      {"id": "worn", "opacity": 0.4, "blend": "screen", "channels": {"basecolor": "#6a5040", "roughness": 0.4}, "mask": [{"type": "edge_wear", "amount": 0.35, "seed": "${seed+3}"}]}
    ]
  },
  "fabric": {
    "description": "Matte woven fabric with fine fiber height.",
    "params": {"color": {"default": "#4a5a6a"}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": {"type": "noise", "noise": "fbm", "scale": 8, "seed": "${seed}", "gradient": ["${color}", "${color}"]}, "metallic": 0, "roughness": 0.92,
        "height": {"type": "noise", "noise": "ridged", "scale": 250, "stretch": [1, 3, 1], "octaves": 2, "seed": "${seed+1}", "range": [0, 0.03]}}},
      {"id": "fuzz", "opacity": 0.25, "blend": "screen", "channels": {"basecolor": "#555555"}, "mask": [{"type": "edge_wear", "amount": 0.5, "width": 0.8, "breakup": 0.2}]}
    ]
  },
  "concrete": {
    "description": "Cast concrete: mottled gray, pits, grime in crevices.",
    "params": {"color": {"default": "#8f8c87"}, "dirt": {"default": 0.4}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": {"type": "noise", "noise": "fbm", "scale": 5, "seed": "${seed}", "gradient": ["#76736e", "${color}", "#a19e98"]},
        "metallic": 0, "roughness": {"type": "noise", "noise": "fbm", "scale": 20, "seed": "${seed+1}", "range": [0.82, 0.95]},
        "height": {"type": "noise", "noise": "fbm", "scale": 40, "seed": "${seed+2}", "range": [-0.05, 0.05]}}},
      {"id": "pits", "channels": {"height": -0.25, "basecolor": "#5f5c58"}, "mask": [{"type": "noise", "noise": "dots", "scale": 60, "size": 0.18, "seed": "${seed+3}"}]},
      {"id": "grime", "opacity": 0.7, "channels": {"basecolor": "#4a4640", "roughness": 0.95}, "mask": [{"type": "dirt", "amount": "${dirt}", "seed": "${seed+4}"}]}
    ]
  },
  "stone": {
    "description": "Rough natural stone with cellular variation.",
    "params": {"color": {"default": "#7d7a74"}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": {"type": "noise", "noise": "voronoi", "scale": 6, "seed": "${seed}", "gradient": ["#67645f", "${color}", "#96918a"]},
        "metallic": 0, "roughness": 0.85,
        "height": {"type": "noise", "noise": "ridged", "scale": 12, "seed": "${seed+1}", "range": [-0.15, 0.15]}}},
      {"id": "veins", "opacity": 0.5, "blend": "multiply", "channels": {"basecolor": "#8a8a8a"}, "mask": [{"type": "noise", "noise": "cracks", "scale": 8, "width": 0.05, "seed": "${seed+2}"}]}
    ]
  },
  "ceramic": {
    "description": "Glossy glazed ceramic.",
    "params": {"color": {"default": "#e9e4da"}, "seed": {"default": 0}},
    "layers": [
      {"id": "base", "channels": {"basecolor": "${color}", "metallic": 0, "roughness": {"type": "noise", "scale": 4, "seed": "${seed}", "range": [0.08, 0.16]}}}
    ]
  },

  "dirt_overlay": {
    "description": "Overlay: dirt and grime in crevices. Put it above a base material.",
    "params": {"amount": {"default": 0.5}, "color": {"default": "#3d3228"}, "seed": {"default": 0}},
    "layers": [{"id": "dirt", "channels": {"basecolor": "${color}", "roughness": 0.9, "metallic": 0}, "mask": [{"type": "dirt", "amount": "${amount}", "seed": "${seed}"}]}]
  },
  "dust_overlay": {
    "description": "Overlay: dust settled on upward-facing, sheltered surfaces.",
    "params": {"amount": {"default": 0.5}, "color": {"default": "#a39b8c"}, "seed": {"default": 0}},
    "layers": [{"id": "dust", "opacity": 0.85, "channels": {"basecolor": "${color}", "roughness": 0.95, "metallic": 0},
      "mask": [{"type": "direction", "direction": "up", "min": "${0.9-amount*0.8}", "max": "${1.2-amount*0.6}", "breakup": 0.4, "seed": "${seed}"},
               {"type": "grunge", "style": "smudge", "amount": "${amount+0.2}", "scale": 5, "seed": "${seed+1}", "blend": "multiply"}]}]
  },
  "edge_highlight": {
    "description": "Overlay: lighter, smoother worn edges (handling wear).",
    "params": {"amount": {"default": 0.4}, "seed": {"default": 0}},
    "layers": [{"id": "edges", "opacity": 0.6, "blend_modes": {"basecolor": "screen"}, "channels": {"basecolor": "#4a4a4a", "roughness": 0.25},
      "mask": [{"type": "edge_wear", "amount": "${amount}", "width": 0.35, "seed": "${seed}"}]}]
  },
  "moss_overlay": {
    "description": "Overlay: moss growing on top surfaces and in crevices.",
    "params": {"amount": {"default": 0.5}, "seed": {"default": 0}},
    "layers": [{"id": "moss", "channels": {
        "basecolor": {"type": "noise", "noise": "fbm", "scale": 12, "seed": "${seed}", "gradient": ["#2f3d17", "#4f6a22", "#6d8a31"]},
        "roughness": 0.9, "metallic": 0, "height": {"type": "noise", "noise": "turbulence", "scale": 40, "seed": "${seed+1}", "range": [0.05, 0.3]}},
      "mask": [{"type": "direction", "direction": "up", "min": "${0.8-amount*0.9}", "max": "${1.1-amount*0.7}", "breakup": 0.5, "seed": "${seed+2}"},
               {"type": "cavity", "blend": "max", "levels": [0.2, 0.7], "opacity": "${amount}"},
               {"type": "grunge", "style": "patches", "amount": "${amount+0.3}", "scale": 4, "seed": "${seed+3}", "blend": "multiply"}]}]
  },
  "snow_overlay": {
    "description": "Overlay: snow on upward-facing surfaces.",
    "params": {"amount": {"default": 0.5}, "seed": {"default": 0}},
    "layers": [{"id": "snow", "channels": {"basecolor": "#f2f5f8", "roughness": 0.7, "metallic": 0, "height": 0.3},
      "mask": [{"type": "direction", "direction": "up", "min": "${0.85-amount*0.8}", "max": "${1.0-amount*0.6}", "breakup": 0.25, "scale": 10, "seed": "${seed}"}]}]
  },
  "grime_streaks": {
    "description": "Overlay: dark water/grime streaks running down vertical faces.",
    "params": {"amount": {"default": 0.5}, "color": {"default": "#2e2a25"}, "seed": {"default": 0}},
    "layers": [{"id": "streaks", "opacity": 0.75, "channels": {"basecolor": "${color}", "roughness": 0.85},
      "mask": [{"type": "streaks", "amount": "${amount}", "seed": "${seed}"}]}]
  },
  "scratches_overlay": {
    "description": "Overlay: fine scratches (shinier, slightly recessed).",
    "params": {"amount": {"default": 0.5}, "seed": {"default": 0}},
    "layers": [{"id": "scratches", "channels": {"roughness": 0.18, "height": -0.08, "basecolor": "#9a9a9a"}, "blend_modes": {"basecolor": "screen"}, "channel_opacity": {"basecolor": 0.3},
      "mask": [{"type": "scratches", "density": "${amount}", "seed": "${seed}"}]}]
  },
  "rust_overlay": {
    "description": "Overlay: rust spots in crevices and streaks (use over painted metal).",
    "params": {"amount": {"default": 0.4}, "seed": {"default": 0}},
    "layers": [{"id": "rust", "channels": {
        "basecolor": {"type": "noise", "noise": "fbm", "scale": 9, "seed": "${seed}", "gradient": ["#3a1d10", "#6b3a1e", "#9a5528"]},
        "metallic": 0, "roughness": 0.85, "height": 0.05},
      "mask": [{"type": "dirt", "amount": "${amount}", "breakup": 0.8, "seed": "${seed+1}"},
               {"type": "streaks", "amount": "${amount*0.6}", "seed": "${seed+2}", "blend": "max", "opacity": 0.7}]}]
  }
},
)JSON" R"JSON(
"export_presets": {
  "blender": {"description": "Separate PBR maps for Blender's Principled BSDF (OpenGL normals).", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_BaseColor.png", "rgb": "basecolor", "srgb": true},
      {"key": "metallic", "file": "{set}_Metallic.png", "r": "metallic"},
      {"key": "roughness", "file": "{set}_Roughness.png", "r": "roughness"},
      {"key": "normal", "file": "{set}_Normal.png", "rgb": "normal"},
      {"key": "height", "file": "{set}_Height.png", "r": "height", "bits": 16},
      {"key": "ao", "file": "{set}_AO.png", "r": "ao"},
      {"key": "emissive", "file": "{set}_Emissive.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"},
      {"key": "opacity", "file": "{set}_Opacity.png", "r": "opacity", "if_used": "opacity"}]},
  "gltf": {"description": "glTF 2.0 metallic-roughness: ORM packed (R=AO, G=Roughness, B=Metallic), OpenGL normals. Use --glb to also write a textured .glb.", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_BaseColor.png", "rgb": "basecolor", "a": "opacity", "alpha_if_used": "opacity", "srgb": true},
      {"key": "orm", "file": "{set}_ORM.png", "r": "ao", "g": "roughness", "b": "metallic"},
      {"key": "normal", "file": "{set}_Normal.png", "rgb": "normal"},
      {"key": "emissive", "file": "{set}_Emissive.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"}]},
  "unreal": {"description": "Unreal Engine: BaseColor, packed ORM (R=AO, G=Roughness, B=Metallic), DirectX normals.", "normal": "directx",
    "maps": [
      {"key": "basecolor", "file": "T_{set}_BC.png", "rgb": "basecolor", "a": "opacity", "alpha_if_used": "opacity", "srgb": true},
      {"key": "orm", "file": "T_{set}_ORM.png", "r": "ao", "g": "roughness", "b": "metallic"},
      {"key": "normal", "file": "T_{set}_N.png", "rgb": "normal"},
      {"key": "emissive", "file": "T_{set}_E.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"}]},
  "unity_hdrp": {"description": "Unity HDRP Lit: BaseMap, MaskMap (R=Metallic, G=AO, B=Detail mask, A=Smoothness), OpenGL normals.", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_BaseMap.png", "rgb": "basecolor", "a": "opacity", "alpha_if_used": "opacity", "srgb": true},
      {"key": "mask_map", "file": "{set}_MaskMap.png", "r": "metallic", "g": "ao", "b": "one", "a": "smoothness"},
      {"key": "normal", "file": "{set}_Normal.png", "rgb": "normal"},
      {"key": "emissive", "file": "{set}_Emission.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"}]},
  "unity_urp": {"description": "Unity URP Lit: BaseMap, MetallicSmoothness (R=Metallic, A=Smoothness), Occlusion, OpenGL normals.", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_BaseMap.png", "rgb": "basecolor", "a": "opacity", "alpha_if_used": "opacity", "srgb": true},
      {"key": "metallic_smoothness", "file": "{set}_MetallicSmoothness.png", "r": "metallic", "g": "metallic", "b": "metallic", "a": "smoothness"},
      {"key": "occlusion", "file": "{set}_Occlusion.png", "r": "ao"},
      {"key": "normal", "file": "{set}_Normal.png", "rgb": "normal"},
      {"key": "emissive", "file": "{set}_Emission.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"}]},
  "godot": {"description": "Godot 4 StandardMaterial3D: albedo, ORM, OpenGL normals.", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_albedo.png", "rgb": "basecolor", "a": "opacity", "alpha_if_used": "opacity", "srgb": true},
      {"key": "orm", "file": "{set}_orm.png", "r": "ao", "g": "roughness", "b": "metallic"},
      {"key": "normal", "file": "{set}_normal.png", "rgb": "normal"},
      {"key": "emissive", "file": "{set}_emission.png", "rgb": "emissive", "srgb": true, "if_used": "emissive"}]},
  "maps": {"description": "Every channel plus baked mesh maps as separate files (debugging / custom pipelines).", "normal": "opengl",
    "maps": [
      {"key": "basecolor", "file": "{set}_basecolor.png", "rgb": "basecolor", "srgb": true},
      {"key": "metallic", "file": "{set}_metallic.png", "r": "metallic"},
      {"key": "roughness", "file": "{set}_roughness.png", "r": "roughness"},
      {"key": "normal", "file": "{set}_normal.png", "rgb": "normal"},
      {"key": "height", "file": "{set}_height.png", "r": "height", "bits": 16},
      {"key": "ao", "file": "{set}_ao.png", "r": "ao"},
      {"key": "emissive", "file": "{set}_emissive.png", "rgb": "emissive", "srgb": true},
      {"key": "opacity", "file": "{set}_opacity.png", "r": "opacity"},
      {"key": "bake_ao", "file": "{set}_bake_ao.png", "r": "bake_ao"},
      {"key": "curvature", "file": "{set}_curvature.png", "r": "curvature"},
      {"key": "thickness", "file": "{set}_thickness.png", "r": "thickness"},
      {"key": "bake_height", "file": "{set}_bake_height.png", "r": "bake_height", "bits": 16, "if_baked": "height"}]}
}
}
)JSON";

const Json& builtin_library() {
  static Json lib = Json::parse(kBuiltinLibrary);
  return lib;
}

static void merge_library_file(Json& mats, const std::string& file) {
  std::string text;
  if (!read_file(file, text)) return;
  Json j;
  std::string err;
  if (!Json::try_parse(text, j, err)) return;
  if (const Json* sm = j.find("smart_materials")) {
    for (auto& kv : sm->members()) mats.set(kv.first, kv.second);
  } else if (j.has("layers")) {
    mats.set(j.str("name", path_stem(file)), j);
  }
}

Json smart_materials(const std::string& project_dir) {
  Json mats = builtin_library()["smart_materials"];
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("PATINA_LIBRARY")) {
    std::string s = env;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++)
      if (i == s.size() || s[i] == ':' || s[i] == ';') { if (i > start) dirs.push_back(s.substr(start, i - start)); start = i + 1; }
  }
  if (!project_dir.empty()) dirs.push_back(path_join(project_dir, "library"));
  for (auto& d : dirs)
    if (is_directory(d))
      for (auto& f : list_dir(d, ".json")) merge_library_file(mats, f);
  return mats;
}

// ---------------------------------------------------------------- parameter substitution
namespace {
struct Expr {
  const char* p;
  const Json* params;
  bool ok = true;
  void ws() { while (*p == ' ') p++; }
  double atom() {
    ws();
    if (*p == '(') { p++; double v = sum(); ws(); if (*p == ')') p++; else ok = false; return v; }
    if (*p == '-') { p++; return -atom(); }
    if (std::isdigit((unsigned char)*p) || *p == '.') { char* e; double v = strtod(p, &e); p = e; return v; }
    if (std::isalpha((unsigned char)*p) || *p == '_') {
      std::string name;
      while (std::isalnum((unsigned char)*p) || *p == '_') name += *p++;
      const Json* v = params->find(name);
      if (!v || !v->is_number()) { ok = false; return 0; }
      return v->as_num();
    }
    ok = false;
    return 0;
  }
  double prod() {
    double v = atom();
    for (;;) {
      ws();
      if (*p == '*') { p++; v *= atom(); }
      else if (*p == '/') { p++; double d = atom(); v = d != 0 ? v / d : 0; }
      else return v;
    }
  }
  double sum() {
    double v = prod();
    for (;;) {
      ws();
      if (*p == '+') { p++; v += prod(); }
      else if (*p == '-') { p++; v -= prod(); }
      else return v;
    }
  }
};
}  // namespace

static Json substitute(const Json& t, const Json& params, std::vector<std::string>& warnings) {
  if (t.is_string()) {
    const std::string& s = t.as_str();
    if (s.size() > 3 && s[0] == '$' && s[1] == '{' && s.back() == '}' && s.find("${", 2) == std::string::npos) {
      std::string inner = s.substr(2, s.size() - 3);
      if (const Json* v = params.find(inner)) return *v;
      Expr e{inner.c_str(), &params};
      double v = e.sum();
      e.ws();
      if (!e.ok || *e.p) { warnings.push_back("smart material: cannot evaluate '" + s + "'"); return Json(0); }
      return Json(v);
    }
    // textual substitution inside longer strings
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
        size_t end = s.find('}', i);
        if (end != std::string::npos) {
          std::string name = s.substr(i + 2, end - i - 2);
          const Json* v = params.find(name);
          out += v ? (v->is_string() ? v->as_str() : v->dump()) : "";
          i = end;
          continue;
        }
      }
      out += s[i];
    }
    return Json(out);
  }
  if (t.is_array()) {
    Json a = Json::array();
    for (auto& v : t.items()) a.push(substitute(v, params, warnings));
    return a;
  }
  if (t.is_object()) {
    Json o = Json::object();
    for (auto& kv : t.members()) o.set(kv.first, substitute(kv.second, params, warnings));
    return o;
  }
  return t;
}

static void prefix_ids(Json& layers, const std::string& prefix, const std::vector<std::string>& inner_ids) {
  std::function<void(Json&)> walk = [&](Json& j) {
    if (j.is_array()) { for (auto& v : j.items()) walk(v); return; }
    if (!j.is_object()) return;
    if (Json* id = j.find("id"); id && id->is_string()) *id = Json(prefix + "/" + id->as_str());
    if (j.str("type", "") == "layer") {
      if (Json* ref = j.find("layer"); ref && ref->is_string()) {
        for (auto& iid : inner_ids) if (iid == ref->as_str()) { *ref = Json(prefix + "/" + iid); break; }
      }
    }
    for (auto& kv : j.members()) walk(kv.second);
  };
  walk(layers);
}

static void collect_ids(const Json& j, std::vector<std::string>& ids) {
  if (j.is_array()) { for (auto& v : j.items()) collect_ids(v, ids); return; }
  if (!j.is_object()) return;
  if (j["id"].is_string() && (j.has("channels") || j.has("layers") || j.has("mask"))) ids.push_back(j["id"].as_str());
  for (auto& kv : j.members()) collect_ids(kv.second, ids);
}

static Json resolve_template(const Json& mats, const std::string& name, Json params, std::vector<std::string>& warnings, int depth) {
  const Json* tmpl = mats.find(name);
  if (!tmpl) {
    std::string dym = did_you_mean(name, mats.keys());
    fail("unknown smart material '%s'%s (see `patina library`)", name.c_str(), dym.empty() ? "" : (" - did you mean '" + dym + "'?").c_str());
  }
  // fill defaults
  if (const Json* ps = tmpl->find("params")) {
    for (auto& kv : ps->members())
      if (!params.has(kv.first)) params.set(kv.first, kv.second.is_object() ? kv.second["default"] : kv.second);
  }
  if (tmpl->has("extends") && depth < 8) return resolve_template(mats, tmpl->str("extends"), params, warnings, depth + 1);
  return substitute((*tmpl)["layers"], params, warnings);
}

Json expand_smart_material(const Json& layer, const std::string& outer_id, const std::string& project_dir, std::vector<std::string>& warnings) {
  std::string name = layer.str("material", "");
  if (name.empty()) fail("smart layer needs \"material\" (see `patina library`)");
  Json mats = smart_materials(project_dir);
  Json params = layer["params"].is_object() ? layer["params"] : Json::object();
  // warn on unknown params
  if (const Json* tmpl = mats.find(name)) {
    Json known = Json::object();
    const Json* t = tmpl;
    for (int d = 0; t && d < 8; d++) {
      if (const Json* ps = t->find("params")) for (auto& kv : ps->members()) known.set(kv.first, true);
      t = t->has("extends") ? mats.find(t->str("extends")) : nullptr;
    }
    for (auto& kv : params.members())
      if (!known.has(kv.first)) {
        std::string dym = did_you_mean(kv.first, known.keys());
        warnings.push_back(strf("smart material '%s' has no param '%s'%s", name.c_str(), kv.first.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str()));
      }
  }
  Json layers = resolve_template(mats, name, params, warnings, 0);
  std::vector<std::string> ids;
  collect_ids(layers, ids);
  prefix_ids(layers, outer_id, ids);
  Json folder = Json::object();
  folder.set("type", "folder");
  folder.set("layers", layers);
  return folder;
}

Json smart_material_catalog(const std::string& project_dir) {
  Json mats = smart_materials(project_dir);
  Json out = Json::object();
  for (auto& kv : mats.members()) {
    Json e = Json::object();
    e.set("description", kv.second.str("description", ""));
    Json params = Json::object();
    const Json* t = &kv.second;
    for (int d = 0; t && d < 8; d++) {
      if (const Json* ps = t->find("params"))
        for (auto& p : ps->members()) {
          if (params.has(p.first)) continue;
          Json pe = Json::object();
          pe.set("default", p.second.is_object() ? p.second["default"] : p.second);
          if (p.second.is_object() && p.second.has("doc")) pe.set("doc", p.second["doc"]);
          params.set(p.first, pe);
        }
      t = t->has("extends") ? mats.find(t->str("extends")) : nullptr;
    }
    e.set("params", params);
    out.set(kv.first, e);
  }
  return out;
}

const Json* find_export_preset(const std::string& name) { return builtin_library()["export_presets"].find(to_lower(name)); }

Json export_preset_catalog() {
  Json out = Json::object();
  for (auto& kv : builtin_library()["export_presets"].members()) {
    Json e = Json::object();
    e.set("description", kv.second.str("description", ""));
    Json files = Json::array();
    for (auto& m : kv.second["maps"].items()) files.push(m.str("file", ""));
    e.set("files", files);
    out.set(kv.first, e);
  }
  return out;
}

}  // namespace pt
