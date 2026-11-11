# Trace logs

A trace log records a segmentation session: every edit with the voxels it
changed, and how the person looked at the image while tracing (zoom, pan,
slice changes, contrast, tools, labels). It serves two purposes:

- **Replay.** The log rebuilds the segmentation step by step, and shows
  whether the result is identical to the file that was saved.
- **Study.** The view events show how a structure was traced: which plane,
  which slices in which order, how closely, with which brush.

Code: `Logic/Framework/TraceLog.*` (recording), `Logic/Framework/TraceLogReplay.*`
(reading and replaying), `GUI/Model/TraceLogModel.*` (view events, GUI replay).
Tests: `Testing/Logic/TraceLogTest.cxx`, `Testing/GUI/Qt/Scripts/test_TraceLogGUI.js`.

## Recording

Recording is off until the person turns it on:

- **Tools > Trace Log > Start Recording...** asks for an ID. Use a pseudonym,
  not a name. While recording, the window title starts with `[Recording trace]`.
- Or from the command line: `ITK-SNAP --trace-log --trace-user P07 -g image.nii.gz`.
  Add `--trace-dir DIR` to keep the logs somewhere other than ITK-SNAP's
  application data folder (`TraceLogs/` there).

The log is written to a working file as events happen, one line at a time, so
it survives a crash. If the file cannot be written (a full disk, a removed
drive), recording stops and ITK-SNAP says so. **Each time a segmentation is
saved, the log is copied next to it** as `<segmentation file>.trace.jsonl`; for
example `liver.nii.gz` gets `liver.nii.gz.trace.jsonl`. The copy holds the
session event and everything since the current image was loaded, so edits made
earlier in the session to another image (another patient) do not go with it.
The working file keeps the whole session.

**The copy keeps the segmentation's history.** When a segmentation is loaded
from a file that has a log next to it, the sessions in that log come with it,
and every later save writes them first, then the current session. So working
on `liver.nii.gz` over several days, opening and saving it each time, leaves
one `liver.nii.gz.trace.jsonl` with all the sessions, oldest first. Saving
under a new name takes the history along; saving twice, or reloading the file
and saving again, writes each session once. Nothing is recorded while
recording is off, so a change made then is a gap in the history (see below).

No file names or paths are written to the log. An image is identified by its
geometry, a segmentation by a checksum.

## Replaying

A log holds what it needs to rebuild the segmentation: the segmentation as it
was when recording began (or when it was loaded), and every change after that.
The image is the only other thing needed.

- **GUI:** open the image, then Tools > Trace Log > Replay Trace Log.... If
  the open segmentation has a log saved next to it, ITK-SNAP offers that log;
  otherwise it asks for one. It asks how many edits to replay, and rebuilds the
  segmentation in a **new layer**, leaving the open segmentations as they
  are, so the result can be compared with the saved one. Each replayed edit
  can be undone (as far back as ITK-SNAP keeps undo steps). An edit that does
  not fit is not applied.
- **Command line:**
  ```
  itksnap-wt -trace-info liver.nii.gz.trace.jsonl
  itksnap-wt -trace-replay liver.nii.gz.trace.jsonl log rebuilt.nii.gz
  itksnap-wt -trace-replay liver.nii.gz.trace.jsonl log first10.nii.gz 10
  ```
  The start is `log` (the starting segmentation the log holds), `blank`, or a
  segmentation file; the optional last number replays only that many edits. A
  full replay reports whether the result is identical to the saved
  segmentation.

A replay applies the edits, undos and redos of one segmentation layer: by
default the layer saved last in the latest session, which is the one the copy
next to a file belongs to. It goes back through the layer's history as far as
the history is continuous:

- The layer's last starting point is when recording began, or when the layer
  was added, loaded or reloaded from its file.
- A segmentation that was loaded came from some save. If a save earlier in the
  log (in the same session or an earlier one) wrote exactly that content (same
  checksum), the replay goes on back from that save's own layer and starting
  point, and so on.
- It stops at a blank start, at a layer that was added with labels (not
  loaded), or at a loaded segmentation that no save in the log wrote. The last
  case is a **gap**: the file was changed while recording was off, so the
  edits before cannot lead to it. The replay then starts after the gap from the
  copy the log holds, and says so (`History: incomplete` in `-trace-info`).

So a full replay of a file worked on over several sessions rebuilds it from
the first session's start, and its result is compared with the last save.
Logs kept apart (one per copy of a segmentation, say) can be joined the same
way, oldest first: `cat day1.trace.jsonl day2.trace.jsonl > all.trace.jsonl`.
The sessions link wherever one loaded what another saved. The
command line can also start partway, from a segmentation file anywhere along
the history: a starting point or a save of the layer (found by its checksum).

Each step stores the old label of every voxel it changes; a voxel with any
other label stops the replay at that step.

Logs recorded before ITK-SNAP kept the starting segmentation in them can still
be replayed when they started blank. If they started from a segmentation with
labels, that segmentation has to be open and selected (in the GUI), or given
as the start (on the command line).

## Format

JSON Lines: one JSON object per line. Every event has:

| Field  | Meaning |
|--------|---------|
| `seq`  | 1, 2, 3, ... in file order |
| `ms`   | milliseconds since recording started (a steady clock) |
| `type` | the kind of event, below |
| `ctx`  | where it came from, when known: `view` (0-2 slice views, 3 the 3D view), `plane` (`axial`, `coronal`, `sagittal`, `3d`), `action` (`click`, `keys`, `scroll`, `slider`, `zoom`, `pan`, `paint`, `polygon`, `rotate`, `move`, ...), the view's `slice` and `zoom`; while the image is freely rotated, also `rotated`, and the view's plane in the image's anatomy as `normal` and `offset` (see [Free rotation](#free-rotation)) |

### Session and data

| Type | Fields |
|------|--------|
| `session` | `format` (`itksnap-trace`), `version` (1), `session` (random id), `user`, `app`, `started` (UTC). Starts each session; a copy saved with a segmentation can hold several sessions, oldest first, and `seq`, `ms` and layer ids start over in each. |
| `image` | `loaded`; if loaded, `size`, `tps` (time points), `spacing`, `origin`, `direction` (row-major 3x3) |
| `layer` | starting point of a segmentation layer: `layer` (id), `kind` (`present` when recording started, `added`, `loaded`), `size`, `tps`, `checksum`, `blank`; if not blank, `content`: its labels as runs `[n, label]` over all voxels in raster order, or `content_seq`: the event that already holds the same content (since the image was loaded); `history`: the number of earlier sessions it brought from the log next to the file it was read from |
| `layer_removed` | `layer` |
| `save` | `layer`, `tps`, `checksum` of what was saved |
| `undo_cleared` | `layer`: the undo history was cleared (e.g. after a Label Editor relabel) |
| `mode` | `mode`: `snake` when the active contour mode starts, with `roi` (`index`, `size`) and `roi_resampled_size`; `main` when it ends. In between, positions and edits are in the voxels of that region. |
| `stop` | recording stopped |

Checksums are `fnv1a64:<hex>`, an FNV-1a 64 bit hash. The image rows are taken
in raster order (y, then z, then time point); each row is split into maximal
runs of one label, and each run adds its length (4 bytes) and label (2 bytes),
low bytes first. A checksum detects accidental differences; it is not proof
against deliberate tampering.

### Segmentation changes

`edit`, `undo` and `redo` share these fields:

| Field | Meaning |
|-------|---------|
| `layer`, `tp` | the segmentation layer and time point |
| `op` | ITK-SNAP's name for the operation, e.g. `Drawing with paintbrush`, `Replace label` |
| `region` | `index` and `size` of the box the change lies in |
| `runs` | the voxels of the region in raster order: `[n]` is n unchanged voxels, `[n, old, new]` is n voxels that went from label `old` to label `new` |
| `changed` | number of changed voxels |
| `before`, `after` | label counts of the changed voxels before and after |
| `actor` | `human` or `agent` (edits only) |

Every change is recorded, including the temporary edit the deep learning
brush and lasso make and undo before calling the model (`op` is `Temporary
undo point`): it can be redone later, and a replay must see it either way.

A paintbrush drag is one edit, recorded as its net change. Its `ctx` adds
`tool` (`paintbrush`), `brush` (`size` as shown in the panel, `shape`, `mode`,
`3d`, `isotropic`, `chase`), `erase` (right button) and `stroke`: the voxels
under the brush as `[x, y, z, ms since the stroke started]`. A polygon edit's
`ctx` adds `tool` (`polygon`), `vertices` (slice coordinates) and `invert`.
3D spray and scalpel edits have `view` 3. With freely rotated views, the
paintbrush commits each dab as it is applied, each with the same context.

### Viewing

| Type | Fields |
|------|--------|
| `snapshot` | the state of everything below when recording started |
| `cursor` | `from`, `to`: each with `pos` (voxel), `ras` (the point under the cursor in the image's anatomy, RAS millimetres), `tp` and `slices` (the slice shown in each of the three views). A slice change by key or wheel is one event, so the time spent on each slice can be measured; a crosshair drag or 4D playback is merged like the state changes below. |
| `view` | `view`, `plane`, `from`, `to`: each with `zoom`, `pos` (view centre), `fit` |
| `contrast` | `layer`, `from`, `to`: each with `range` (intensities the curve spans) and `points` (`[t, x]`: fraction t of the range is shown at brightness x) |
| `tool` | `from`, `to`: `mode` (`crosshairs`, `zoom_pan`, `paintbrush`, `polygon`, ...) |
| `labels` | `from`, `to`: `draw` (active label), `over` (`all`, `visible`, `one`), `over_label` |
| `segmentation` | `from`, `to`: the selected segmentation `layer` |
| `layout` | `from`, `to`: `panels` (`all` or the one view shown), `layers` (`stacked`, `tiled`) |
| `rotation` | `from`, `to`: `rotated`; if rotated, `angle` (degrees), `axis` (unit vector, RAS) and `view_to_image` (row-major 4x4, below) |

`cursor`, `view`, `contrast`, `tool`, `labels`, `segmentation`, `layout` and
`rotation` are changes of state. Changes of the same state less than a second apart are merged
into one event, with the state before (`from`) and after (`to`), the number of
changes `n`, and `ms` to `ms_end`. A drag is one event; zooming one view and the
views linked to it gives one event per view.

### Free rotation

Tools > Image Free Rotation turns the main image and the segmentations in
space. The slice views do not move: they stay on a fixed grid of voxels (the
main image's grid before rotation) and show, at each point of it, the point of
the rotated image that now lies there. So, while rotated:

- **Voxel positions are on the views' grid.** `pos` and `slices` in `cursor`
  events, a view's `slice` in `ctx`, the `stroke` points of the paintbrush and
  the polygon `vertices` all count voxels of that fixed grid. They are exact
  there, but a slice of it cuts the anatomy at an angle.
- **`view_to_image`** in the `rotation` event maps that grid's world to the
  image's own anatomy, both in RAS millimetres: the views show, at the point
  x, the anatomy at `view_to_image` x. A voxel v of the grid is at x = the
  image's NIfTI sform times v (the `image` event gives the geometry to build
  the sform). The matrix also holds the shift that keeps the centre of
  rotation in place, and any move made with the free rotation tool. A drag of
  the rotation widget is one event, with `ctx.action` `rotate` (or `move`).
- **`ras` in `cursor` events is already in the image's anatomy**, so it means
  the same before, during and after a rotation. A rotation moves the point
  under the cursor without moving the cursor; that is not written as a cursor
  event, and the next cursor event starts from the point shown after the
  rotation.
- **`normal` and `offset` in `ctx`** give the plane the view shows, in the
  image's anatomy: the points x with `normal` . x = `offset` (RAS
  millimetres). The normal points the way the slice number grows.
- **Edits are not affected**: `runs` are always in the segmentation's own
  voxels, so a replay is the same whether or not the image was rotated.

## Limits

- **Edits in the active contour (snake) mode** are recorded against that
  mode's own layers, which have no starting point and which a replay ignores;
  the result accepted into the segmentation is recorded as an edit.
- **A change that bypasses ITK-SNAP's undo system** would not be recorded. A
  full replay then reports that the result differs from the saved file, so such
  a gap shows up rather than going unnoticed.
- **Whole-volume operations** (automatic segmentation, interpolation,
  smoothing) cover the whole image, so their lines can be long.
- **Merging is by time, not by gesture**: two quick gestures can merge, and a
  drag with a pause of over a second splits.
- A replay rebuilds the segmentation; it does not yet play the view events back.
- The user ID is whatever the person types. Whether recording people for a
  study needs consent or ethics approval is a question for your institution.
