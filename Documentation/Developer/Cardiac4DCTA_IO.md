# 4D Cardiac CTA — Phase & Metadata I/O

This document describes how ITK-SNAP reads a 4D cardiac CTA (gated CT) DICOM series,
preserves its **cardiac phase axis** (`%R-R`) and **research-relevant, non-PHI metadata**, and
writes them back out to `.seq.nrrd`, `.nii.gz`, and `.nrrd`.

Audience: developers working on the image I/O layer. All code lives in
`Logic/ImageWrapper/GuidedNativeImageIO.cxx` unless noted.

---

## 1. What problem this solves

A 4D cardiac CTA is a gated acquisition: the same volume reconstructed at several points across the
cardiac cycle (the **phases**), each labeled by its position in the R-R interval as a percentage
(**`%R-R`** — e.g. `0, 5, 10, … 95` for a 20-phase study). That phase axis is the clinically
meaningful fourth dimension.

Previously ITK-SNAP loaded the volumes correctly but:

- gave the time axis a **hardcoded** spacing (`0.05`), discarding the real `%R-R`;
- kept only one DICOM frame's metadata;
- had no way to write the phase axis to any format; and
- on `.nrrd`/`.mha` export, dumped the **entire** DICOM dictionary (a PHI risk).

The cardiac I/O path fixes all four.

---

## 2. Reading a 4D CTA

### 2.1 Detection
A DICOM directory is classified as `FORMAT_DICOM_DIR_4DCTA` in `GuessFormatForFileName()` when
`Modality == CT` and the manufacturer is Siemens or GE. The frames are grouped and ordered by
`MultiFrameDicomSeriesSorter` (group by slice Z → rank phases by `InstanceNumber` → order slices
within a phase by `ImagePositionPatient`), then stacked into a 4D image in
`DoReadNative()`.

### 2.2 Recovering the `%R-R` phase axis
Standard cardiac-timing DICOM tags are often absent in Siemens "Func" recons, so the axis is
recovered **structurally** from the `SeriesDescription`:

- `ParseCardiacRRRange()` extracts the `start–end %` range from the description
  (e.g. `"Func DS_CorCTA 0.5 Bv36 4  0 - 95 %"` → `0 … 95`). It anchors on the last `%` and reads
  the two numbers before it, so unrelated numbers earlier in the string are ignored.
- `DeriveCardiacPhaseAxis()` turns that range + the phase count `N` into the per-phase values
  `linspace(start, end, N)`, and flags whether the step is a clean integer (`exact`). The ambiguous
  10-phase `0–95 %` case yields the non-uniform `0, 10.56, … 95` with `exact = false`.

The derived axis sets the 4D image's **temporal geometry** — `origin[3] = start/100`,
`spacing[3] = step/100` (a cardiac-cycle fraction) — replacing the old hardcoded `0.05`. When the
range cannot be parsed, it falls back to the previous `0.05` and marks the source `none`.

### 2.3 In-memory carrier: the MetaDataDictionary
The phase axis is stored on the image's ITK `MetaDataDictionary` using namespaced string keys, which
flow unchanged from the native image through to `ImageWrapper::m_Image4D` (the scalar cast copies the
dictionary) and out to the writers:

| Key | Meaning |
|---|---|
| `ITKSNAP_Cardiac_RRPercent` | space-separated `%R-R`, one value per time point |
| `ITKSNAP_Cardiac_RRPercentSource` | `series_description` or `none` |
| `ITKSNAP_Cardiac_RRPercentExact` | `1` if the step is a clean integer, else `0` |
| `ITKSNAP_Cardiac_NumberOfPhases` | number of time points |

The keys are plain identifiers (no `gggg|eeee` shape), so they never collide with DICOM tag keys and
round-trip cleanly as NRRD `key:=value` fields.

### 2.4 Typed per-time-point model
`TimePointProperty` (`Logic/Framework/TimePointProperties.h`) carries `RRPercent` + `RRPercentExact`
per time point. `TimePointProperties::CreateNewData()` populates them from the main image's
`ITKSNAP_Cardiac_RRPercent` metadata on load; the workspace `Save`/`Load` persist them
(`FormatVersion` bumped to **2**; older workspaces load with `RRPercent = NaN`).

---

## 3. Writing

All GUI/CLI save paths funnel through `GuidedNativeImageIO::SaveImage()`, the single authoritative
write path. (`ImageWrapper::WriteToFile` → internal-format path for identity-mapped images;
`WriteToFileAsFloat` for non-identity images now also routes here.)

### 3.1 `.seq.nrrd` (Slicer volume sequence) — recommended for the phase axis
`SaveNrrdSequence()` hand-writes the NRRD header (ITK's generic writer can't produce the seq format).
It emits the cardiac `%R-R` as the **frame axis index values**, which can be **non-uniform** — the
natural home for the 10-phase ambiguous case:

```
kinds: list domain domain domain
labels: "%R-R" "" "" ""
axis 0 index type:=numeric
axis 0 index values:=0 5 10 … 95
axis 0 index units:=%
ITKSNAP_Cardiac_RRPercent:=0 5 10 … 95
ITKSNAP_Cardiac_RRPercentSource:=series_description
ITKSNAP_Cardiac_RRPercentExact:=1
```

Geometry is LPS; the buffer is reordered from ITK's X-fastest to NRRD's T-fastest layout. If no
cardiac axis is present, it falls back to ordinal frame indices `0 … T-1`.

### 3.2 `.nii.gz` (NIfTI) — header + JSON sidecar (read **and** written)
NIfTI has no per-frame list and no slice-thickness field, so:

- the uniform `pixdim[4]` = frame step (from the 4D geometry); and
- `WriteCardiacJsonSidecar()` (jsoncpp) writes a `<name>.json` sidecar with the **authoritative**
  frame axis (values + unit + label, CT or echo) + `SliceThickness` + provenance:

```json
{ "FrameAxisLabel": "%R-R", "FrameAxisUnit": "%", "NumberOfFrames": 20,
  "FrameAxisValues": [0, 5, 10, … 95], "Source": "series_description", "Exact": true,
  "SliceThickness": 0.5 }
```

Crucially, the sidecar is **read back**: `ReadCardiacJsonSidecar()` is called at the end of
`DoReadNative` for 4D NIfTI/Analyze, parses `<name>.json`, and injects the `ITKSNAP_FrameAxis_*`
keys (plus the legacy `%R-R` keys when the unit is `%`, and `0018,0050` slice thickness) back into
the dictionary. So a NIfTI write→reload recovers the axis (and the GUI "Phase / time:" field) and the
thickness. (`toffset`/`xyzt_units` are still ITK defaults; the sidecar is canonical.)

### 3.3 `.nrrd` (plain NRRD)
The `ITKSNAP_Cardiac_*` keys ride the `MetaDataDictionary` and are serialized automatically by ITK's
`NrrdImageIO` as `key:=value` fields — no extra code. Reading the file back repopulates the keys.

---

## 4. Non-PHI metadata curation (export)

ITK's NRRD/MetaImage writers serialize the **whole** `MetaDataDictionary`. For a DICOM-sourced image
that includes patient identifiers (`0010,*`), dates, and private blobs — a PHI leak on
non-de-identified data. `SaveImage()` therefore swaps in a curated dictionary for the write and
restores the original afterward (curation is **export-only** — the Image Information inspector keeps
full fidelity):

- `CurateDicomDictionaryForExport()` keeps only an **allow-list** of research-relevant public tags
  (`CardiacExportKeepKeys()`: modality/scanner, protocol, CT technique, cardiac timing, geometry,
  intensity calibration, pixel format, pseudonymous hierarchy UIDs, de-id provenance, and the
  covariates sex/age/size/weight/BMI) plus the `ITKSNAP_*` keys.
- It is an allow-list, not a deny-list, so unenumerated, private, or free-text tags can never slip
  through.
- `PatientAge (0010,1010)` is **top-coded** to `090Y` when ≥ 90, per HIPAA Safe Harbor.
- It is a no-op for non-DICOM dictionaries, so custom keys on `.nrrd`/`.nii` inputs are preserved.

`.seq.nrrd` and NIfTI+sidecar are already PHI-clean (they emit only geometry + cardiac keys).

> HIPAA basis for retaining the covariates (sex/height/weight are not Safe Harbor identifiers; age is
> except ≥90): see `projects/4dcta_improvement/metadata_reference.md §3.3` in the wrapper repo.

---

## 5. GUI

The General Layer Inspector (General tab, 4D time-point section) shows a read-only **"Cardiac
phase:"** field next to the time-point nickname, e.g. `35% R-R` (or `35% R-R (approx)` for a
non-integer recon phase). It is driven by the read-only `CrntTimePointCardiacPhase` property on
`LayerGeneralPropertiesModel`, which reads the current time point's `TimePointProperty::RRPercent`
and updates on `CursorTimePointUpdateEvent`.

---

## 6. Format choice guide

| Downstream | Use | Why |
|---|---|---|
| Slicer / SlicerHeart, non-uniform phases | `.seq.nrrd` | first-class non-uniform frame axis |
| Generic tools, segflow4d / `.nii.gz` pipeline | `.nii.gz` + `.json` sidecar | sidecar survives any tool |
| In-file, no sidecar | `.nrrd` | `key:=value` round-trips |

---

## 7. Limitations / future work

- `WriteToFileAsFloat` casts to a **3D** float (current time point); 4D non-identity-mapped images
  export only the current phase. 4D CTA loads identity-mapped (short→short), so it uses the 4D path
  and is unaffected.
- NIfTI `toffset`/`xyzt_units` are ITK defaults (the sidecar is authoritative for the axis semantics).
- 4DCTA detection is a coarse "Siemens/GE CT directory" heuristic (benign: a single-phase series
  loads as a 1-time-point image).

(Done since earlier drafts: grid validation/quarantine for non-rectangular grids; `NumberOfPhases` in
the seq header; echo support; NIfTI sidecar reading; slice thickness across seq.nrrd/nrrd/sidecar.)

---

## 8. Testing notes

The phase derivation and round-trips were verified with a throwaway driver linking `itksnaplogic`
against the AVRP cohort (`bavcta005` clean 20-phase → `0 5 … 95`, `bavcta007` ambiguous 10-phase →
non-uniform `0 10.56 … 95`). To reproduce: add a temporary `ADD_EXECUTABLE` linking
`${SNAP_EXTERNAL_LIBS} itksnaplogic`, set the format to `FORMAT_DICOM_DIR_4DCTA`, call
`ReadNativeImage(<one DICOM file>)`, then `SaveNativeImage(<out>, <format hints>)`; inspect the NRRD
header / JSON sidecar.
