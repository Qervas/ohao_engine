---
module: systems
id: public-scope
title: Public scope note
standard: v2
---

## The repository hides nothing

Every citation chip on this site is a link into a public tree. The build resolves
each content-anchored citation to a line number and emits a `blob/master` URL on
the same repository the engine is developed in:

{{cite site/tools/monograph/cite.py "GITHUB_BLOB_BASE = "https://github.com/Qervas/ohao_engine"}}

So "scope" here is not a visibility boundary. Every file this monograph declines
to discuss is tracked, un-ignored, and one click away for anyone who browses the
repository directly — `git ls-files` on the two omitted directories returns all
38 of their files. No first-party source file is on the `.gitignore` list at all.
What is: build output, downloaded models, the vendored DLSS SDK, a scratch
directory, everything under `renders/` — and one block of research artefacts,
training checkpoints, logs and run history for the local ML experiments:

{{cite .gitignore "# C1 inverse ML local data / checkpoints"}}

That block is the honest exception to "nothing research-shaped": the research
path's *outputs* stay local, regenerable per its own comment. Its inputs — every
`.cpp`, `.hpp` and CMake line this page describes — are committed.

The earlier version of this note called the omitted trees "private". They are
not. The boundary is editorial: which files get a chapter.

## Thirty-eight files, two directories

The chapter list is data. Each module under `site/tools/monograph/tree/` declares
the file paths its units are about, and that declaration is the whole mechanism —
there is no coverage assertion in the build, so nothing fails if a source is
never named. Walking the tree against the repository, 248 `.cpp`/`.hpp` files
live under `ohao/`.

How many are unclaimed depends on how a `files=` entry is read. Taken literally,
68 of the 248 appear in no list — but 30 of those are the other half of a
declared pair: `gbuffer_pass.cpp` beside a declared `gbuffer_pass.hpp`,
`sobol_generator.hpp` beside a declared `sobol_generator.cpp`. Count a declared
path as claiming its sibling translation unit, and exactly 38 remain, in
precisely two directories:

- `ohao/inverse/` — 26 headers, ~7,100 lines, zero `.cpp`.
- `ohao/render/diff/` — 12 files, ~1,300 lines, one `.cpp`.

Under `shaders/` the omission is a single file. `shaders/_disabled/` — listed as
an omission by the previous version of this note — in fact has its own chapter:

{{cite site/tools/monograph/tree/shaders.py "files=['shaders/_disabled']"}}

What no unit names is `shaders/shadow/shadow_csm.geom`. The shaders target globs
`*.geom` recursively, so it is compiled every build:

{{cite shaders/CMakeLists.txt "CMAKE_CURRENT_SOURCE_DIR}/*.geom"}}

and `CSMPass::createPipeline` loads the compiled module as the geometry stage of
the cascaded shadow pipeline:

{{cite ohao/render/deferred/csm_pass.cpp "shadow_shadow_csm.geom.spv"}}

Its sibling `shadow_csm.vert` is declared twice, by the shaders module and by the
deferred module; the `.geom` is declared nowhere. One live shader is out of scope
by this page's own criterion — not by decision, by oversight.

Under `examples/`, the helper function is called six times: five unconditionally,
and the viewer only inside a `find_package` guard, which is why a machine without
GLFW builds five example binaries and not six.

{{cite examples/CMakeLists.txt "if(glfw3_FOUND)"}}

The examples chapter covers five of the six. The sixth is the research entry
point: a 30-line translation unit whose `main` parses argv and returns the fit's
status, both of those calls landing in `ohao/inverse/` headers. Its
other job is the preamble: it is the one example that defines *both* STB
implementation macros, where the other five define only the image-write one. The
exclusion is declared in the tree data next to the units it applies to:

{{cite site/tools/monograph/tree/systems.py "Research-only CLIs under examples/ are out of the public monograph scope."}}

## Compiled into the archive, linked into nothing

The two directories are not symmetric, and the difference matters to anyone
reasoning about what a linked binary contains.

`ohao/inverse/` is not a library. `ohao/CMakeLists.txt` adds six subdirectories
and `inverse` is not among them:

{{cite ohao/CMakeLists.txt "add_subdirectory(render)"}}

It has no `CMakeLists.txt` of its own, and no glob reaches it — the engine's
source globs are rooted at `core`, `scene`, `gpu/vulkan`, `render`, `audio` and
`physics`. Being header-only, it is compiled exclusively through the research
CLI's translation unit. No engine target sees it.

`ohao/render/diff/` is the opposite at compile time. The render library globs its
sources recursively, and `diff/` sits underneath the glob root:

{{cite ohao/render/CMakeLists.txt "file(GLOB_RECURSE RENDERER_SOURCES"}}

So its one `.cpp` compiles on every build, and it is real Vulkan host code: it
creates a command pool, an `R32G32B32A32_SFLOAT` image with a staging buffer, and
walks `VkPhysicalDeviceMemoryProperties` to choose a memory type.

{{cite ohao/render/diff/diff_pipeline.cpp "if (vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_)"}}

None of it ever runs, and none of it ships. `ohao_renderer` is a static archive —

{{cite ohao/render/CMakeLists.txt "add_library(ohao_renderer ${RENDERER_SOURCES})"}}

— and every example links it inside a `--start-group`/`--end-group` pair, with no
`--whole-archive` anywhere in the tree:

{{cite examples/CMakeLists.txt "-Wl,--start-group ohao_renderer"}}

An archive member joins the link only when it resolves a symbol some other object
left undefined. Outside the two files that declare and define it, `DiffPipeline`
has exactly one reference in the repository — a data member of `DiffSession`:

{{cite ohao/render/diff/diff_session.hpp "DiffPipeline pipeline;"}}

and `DiffSession` is instantiated nowhere. The one file outside `diff/` that
includes `diff_session.hpp` is a wrapper in the research tree —

{{cite ohao/inverse/backend/diff_formation.hpp "render/diff/diff_session.hpp"}}

— and that wrapper is itself included by nothing. So `diff_pipeline.cpp.o` is an
archive member no executable pulls in: `nm` finds `DiffPipeline::init` and its
siblings in the render library's archive and in none of the built binaries, the
research CLI included. The namespace they belong to is named only under
`ohao/render/diff/` and `ohao/inverse/`; no product pipeline mentions it.

Undocumented, unreferenced by the product pipelines, and paid for on every build —
all three at once. What it costs is compile time and a Vulkan surface the engine
must keep compiling against, not bytes in a shipped binary. Being a glob member is
not being in the binary.

## The layering runs upward

Four of the eleven headers in the render library's `diff/` directory include
headers from `ohao/inverse/` directly: `diff_forward.hpp`, `diff_map_bind.hpp`,
`diff_vk_forward.hpp`, and `diff_session.hpp`.

{{cite ohao/render/diff/diff_session.hpp "include "inverse/scene_builder.hpp""}}

A fifth, the umbrella `diff_module.hpp`, reaches `ohao/inverse/` transitively by
including two of them.

A rendering library reaching up into an application-level research module is
backwards, and it is currently harmless only by accident: those are headers, and
the single compiled `.cpp` pulls in no project header but `diff_pipeline.hpp`
and, through it, `diff_types.hpp`. Add one `.cpp` under `ohao/render/diff/` that
includes `diff_session.hpp`, and `ohao_renderer` acquires a compile-time
dependency on a directory no target declares — and it will compile, quietly,
because the library already publishes the whole of `ohao/` on its include path,
which is how the four headers above resolve `inverse/...` today:

{{cite ohao/render/CMakeLists.txt "${CMAKE_SOURCE_DIR}/ohao"}}

:::why
Neither directory is behind an `option()`. `ohao/render/diff/` is inside the
render library's recursive glob, so it compiles in every configuration;
`ohao/inverse/` is outside every glob and is reached only through the research
CLI's include path. The rejected alternative — gating the research trees with a
CMake switch, the way NRD and DLSS are gated — would make the boundary explicit
at configure time, at the cost of a build variant that nobody tests. The engine
instead draws the line only in the documentation, which is why the line is
invisible to the compiler.
:::

## The only enforcement is a string grep in CI

The scope note is checked in exactly one place: the Pages deploy. Staging skips
the research media directory and any research-prefixed still before they reach
the artifact —

{{cite .github/workflows/pages.yml "skip inverse media"}}

— and then a single `grep -E` over every `*.html` in the staged tree aborts the
deploy if four product tokens appear anywhere in it:

{{cite .github/workflows/pages.yml "inverse product content found in site publish tree"}}

That guard matches strings, not paths. A chapter may cite into `ohao/render/diff/`
freely, because the citation chip renders only the matched line — but eight of
those twelve files open with a header comment carrying one of the four tokens, so
a citation anchored on a file header would embed it in the HTML and fail the
deploy. Anchoring on a body line, a member declaration or an include, as the three
citations into that directory above do, does not.

:::key
This page cannot name the things it omits. Its own rendered HTML is inside the
tree the guard scans, so writing the research binary's target name, the module's
product name, one test tag, or one plate flag into this markdown would fail the
next push to `master`. The paraphrase throughout is a build constraint, not
discretion — and it is the only part of the scope policy that a machine will
catch.
:::

## Contracts

- The chapter list is a declaration in `site/tools/monograph/tree/*.py` with no
  coverage check behind it. Adding a `files=` entry pointing into `ohao/inverse/`
  or `ohao/render/diff/` silently pulls those trees into scope; deleting a unit
  silently drops a real source out of it. Neither breaks the build, and
  `shaders/shadow/shadow_csm.geom` is already on the wrong side of that gap.
- The deploy guard scans strings in staged HTML. Any new page — including this
  one — that quotes the guard's own regex, or cites a line containing one of its
  tokens, fails the workflow at the staging step, before deployment.
- `ohao/render/diff/diff_pipeline.cpp` is unconditionally in `ohao_renderer`'s
  source glob. The library's one `list(FILTER ... EXCLUDE REGEX ...)` matches
  `/_disabled/`, a path that exists nowhere under `ohao/`, so no source there is
  actually filtered and the file compiles in every configuration. It stays out of
  the executables only because nothing references `DiffPipeline`; the first
  reference from any linked object pulls the member into that binary.
- `ohao/inverse/` compiles only through the research CLI. It is not in any
  `add_subdirectory` and inside no glob, so a change that breaks it will not be
  caught by building the engine libraries or the five documented examples.
