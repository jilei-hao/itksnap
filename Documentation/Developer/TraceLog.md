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
default the layer saved last, which is the one the copy next to a file belongs
to. It starts from that layer's last starting point (when recording began, or
when the layer was added, loaded or reloaded from its file). Each step stores
the old label of every voxel it changes; a voxel with any other label stops the
replay at that step.

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
| `ctx`  | where it came from, when known: `view` (0-2 slice views, 3 the 3D view), `plane` (`axial`, `coronal`, `sagittal`, `3d`), `action` (`click`, `keys`, `scroll`, `slider`, `zoom`, `pan`, `paint`, `polygon`, ...), the view's `slice` and `zoom`, and `rotated` if the views are freely rotated |

### Session and data

| Type | Fields |
|------|--------|
| `session` | `format` (`itksnap-trace`), `version` (1), `session` (random id), `user`, `app`, `started` (UTC) |
| `image` | `loaded`; if loaded, `size`, `tps` (time points), `spacing`, `origin`, `direction` (row-major 3x3) |
| `layer` | starting point of a segmentation layer: `layer` (id), `kind` (`present` when recording started, `added`, `loaded`), `size`, `tps`, `checksum`, `blank`; if not blank, `content`: its labels as runs `[n, label]` over all voxels in raster order, or `content_seq`: the event that already holds the same content (since the image was loaded) |
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
| `cursor` | `from`, `to`: each with `pos` (voxel), `tp` and `slices` (the slice shown in each of the three views). A slice change by key or wheel is one event, so the time spent on each slice can be measured; a crosshair drag or 4D playback is merged like the state changes below. |
| `view` | `view`, `plane`, `from`, `to`: each with `zoom`, `pos` (view centre), `fit` |
| `contrast` | `layer`, `from`, `to`: each with `range` (intensities the curve spans) and `points` (`[t, x]`: fraction t of the range is shown at brightness x) |
| `tool` | `from`, `to`: `mode` (`crosshairs`, `zoom_pan`, `paintbrush`, `polygon`, ...) |
| `labels` | `from`, `to`: `draw` (active label), `over` (`all`, `visible`, `one`), `over_label` |
| `segmentation` | `from`, `to`: the selected segmentation `layer` |
| `layout` | `from`, `to`: `panels` (`all` or the one view shown), `layers` (`stacked`, `tiled`) |

`cursor`, `view`, `contrast`, `tool`, `labels`, `segmentation` and `layout` are
changes of state. Changes of the same state less than a second apart are merged
into one event, with the state before (`from`) and after (`to`), the number of
changes `n`, and `ms` to `ms_end`. A drag is one event; zooming one view and the
views linked to it gives one event per view.

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
