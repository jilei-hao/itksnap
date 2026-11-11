/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogModel.cxx
  Language:  C++

  Copyright (c) 2026 Paul A. Yushkevich

  This file is part of ITK-SNAP

  ITK-SNAP is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

=========================================================================*/
#include "TraceLogModel.h"
#include "GlobalUIModel.h"
#include "GenericSliceModel.h"
#include "DisplayLayoutModel.h"
#include "IRISApplication.h"
#include "IRISException.h"
#include "GlobalState.h"
#include "GenericImageData.h"
#include "ImageCoordinateGeometry.h"
#include "LabelImageWrapper.h"
#include "ColorLabelTable.h"
#include "LayerIterator.h"
#include "DisplayMappingPolicy.h"
#include "IntensityCurveInterface.h"
#include "SegmentationUpdateIterator.h"
#include "SNAPEventListenerCallbacks.h"
#include "SystemInterface.h"
#include "TraceLog.h"
#include "TraceLogReplay.h"
#include "AffineTransformHelper.h"
#include "itksys/SystemTools.hxx"
#include "vnl/vnl_cross.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{

const char *ToolName(ToolbarModeType mode)
{
  switch (mode)
  {
    case CROSSHAIRS_MODE: return "crosshairs";
    case NAVIGATION_MODE: return "zoom_pan";
    case POLYGON_DRAWING_MODE: return "polygon";
    case PAINTBRUSH_MODE: return "paintbrush";
    case ROI_MODE: return "snake_roi";
    case ANNOTATION_MODE: return "annotation";
    case REGISTRATION_MODE: return "registration";
  }
  return "unknown";
}

const char *CoverageName(CoverageModeType mode)
{
  switch (mode)
  {
    case PAINT_OVER_ALL: return "all";
    case PAINT_OVER_VISIBLE: return "visible";
    case PAINT_OVER_ONE: return "one";
  }
  return "unknown";
}

const char *PanelLayoutName(DisplayLayoutModel::ViewPanelLayout layout)
{
  switch (layout)
  {
    case DisplayLayoutModel::VIEW_ALL: return "all";
    case DisplayLayoutModel::VIEW_AXIAL: return "axial";
    case DisplayLayoutModel::VIEW_CORONAL: return "coronal";
    case DisplayLayoutModel::VIEW_SAGITTAL: return "sagittal";
    case DisplayLayoutModel::VIEW_3D: return "3d";
  }
  return "unknown";
}

// Anatomical plane of a slice view. The third letter of the display-to-anatomy
// code is the direction the view looks along.
const char *PlaneName(GenericSliceModel *view)
{
  GenericImageData *id = view->GetDriver()->GetCurrentImageData();
  if (!id->IsMainLoaded())
    return "unknown";
  const std::string &rai = id->GetImageGeometry()->GetDisplayToAnatomyRAI(view->GetId());
  if (rai.size() < 3)
    return "unknown";
  switch (rai[2])
  {
    case 'S': case 'I': return "axial";
    case 'A': case 'P': return "coronal";
    case 'R': case 'L': return "sagittal";
  }
  return "unknown";
}

Json::Value Vec3(const Vector3i &v)
{
  Json::Value a(Json::arrayValue);
  for (unsigned int d = 0; d < 3; d++)
    a.append(v[d]);
  return a;
}

// Round to the given number of decimal places, so the log stays readable
double Round(double x, int places)
{
  double scale = std::pow(10.0, places);
  return std::round(x * scale) / scale;
}

Json::Value Vec3(const Vector3d &v, int places)
{
  Json::Value a(Json::arrayValue);
  for (unsigned int d = 0; d < 3; d++)
    a.append(Round(v[d], places));
  return a;
}

typedef AffineTransformHelper::Mat44 Mat44;

// Free rotation (Tools > Image Free Rotation) transforms the main image and
// the segmentations; the slice views stay in a fixed world, the reference
// space, and show at each point p of it the image point T(p). This gets T as
// a map from that world to the image's own anatomy, both in RAS millimetres.
// Returns false if the views are not rotated.
bool ViewToImageRAS(GenericImageData *id, Mat44 &T)
{
  if (!id->IsMainLoaded() || !id->GetReferenceSpaceWrapper() || !id->IsFreeRotation())
    return false;
  T = AffineTransformHelper::GetRASMatrix(id->GetReferenceSpaceWrapper()->GetITKTransform());
  return true;
}

// Where a point given in voxels of the views lies in the image's own anatomy,
// in RAS millimetres. T is null when the views are not rotated.
Vector3d VoxelToImageRAS(GenericImageData *id, const Vector3d &voxel, const Mat44 *T)
{
  Vector3d ras = id->GetReferenceSpaceWrapper()->TransformVoxelCIndexToNIFTICoordinates(voxel);
  if (!T)
    return ras;
  vnl_vector_fixed<double, 4> x(ras[0], ras[1], ras[2], 1.0), y = (*T) * x;
  return Vector3d(y[0], y[1], y[2]);
}

} // namespace

TraceLogModel::TraceLogModel() {}

TraceLogModel::~TraceLogModel() {}

void
TraceLogModel::SetParentModel(GlobalUIModel *parent)
{
  m_Parent = parent;
  IRISApplication *driver = parent->GetDriver();
  GlobalState     *gs = parent->GetGlobalState();

  // The listeners are always attached; they write only while recording
  AddListener(driver->GetTraceLog(), itk::ModifiedEvent(), this,
              &TraceLogModel::OnTraceLogModified);
  AddListener(driver, LayerChangeEvent(), this, &TraceLogModel::OnLayerChange);
  AddListener(driver, CursorUpdateEvent(), this, &TraceLogModel::OnCursorUpdate);
  AddListener(driver, WrapperDisplayMappingChangeEvent(), this,
              &TraceLogModel::OnDisplayMappingChange);
  AddListener(driver, WrapperPhysicalExtentsChangeEvent(), this,
              &TraceLogModel::OnRotationChange);
  for (unsigned int i = 0; i < 3; i++)
    AddListener(parent->GetSliceModel(i), SliceModelGeometryChangeEvent(), this,
                &TraceLogModel::OnViewGeometryChange);

  AddListener(gs->GetToolbarModeModel(), ValueChangedEvent(), this,
              &TraceLogModel::OnToolbarModeChange);
  AddListener(gs->GetDrawingColorLabelModel(), ValueChangedEvent(), this,
              &TraceLogModel::OnLabelsChange);
  AddListener(gs->GetDrawOverFilterModel(), ValueChangedEvent(), this,
              &TraceLogModel::OnLabelsChange);
  AddListener(gs->GetSelectedSegmentationLayerIdModel(), ValueChangedEvent(), this,
              &TraceLogModel::OnSelectedSegmentationChange);
  AddListener(gs->GetSliceViewLayerLayoutModel(), ValueChangedEvent(), this,
              &TraceLogModel::OnLayoutChange);
  AddListener(parent->GetDisplayLayoutModel()->GetViewPanelLayoutModel(), ValueChangedEvent(),
              this, &TraceLogModel::OnLayoutChange);
}

std::string
TraceLogModel::GetTraceLogDirectory() const
{
  if (m_Directory.size())
    return m_Directory;
  return SystemInterface::GetApplicationDataDirectory() + "/TraceLogs";
}

bool
TraceLogModel::StartRecording(const std::string &user)
{
  std::string app = SNAPSoftVersion; // "ITK-SNAP <version>"
  try
  {
    // Finding the application data folder can fail (e.g. no APPDATA on Windows)
    if (!m_Parent->GetDriver()->StartTraceLog(GetTraceLogDirectory(), user, app))
      return false;
  }
  catch (std::exception &)
  {
    return false;
  }

  this->LogSnapshot();
  this->InvokeEvent(ModelUpdateEvent());
  return true;
}

void
TraceLogModel::StopRecording()
{
  m_Parent->GetDriver()->StopTraceLog();
  this->InvokeEvent(ModelUpdateEvent());
}

bool
TraceLogModel::IsRecording() const
{
  return m_Parent && m_Parent->GetDriver()->GetTraceLog()->IsRecording();
}

std::string
TraceLogModel::GetFileName() const
{
  return m_Parent->GetDriver()->GetTraceLog()->GetFileName();
}

std::string
TraceLogModel::GetWriteError() const
{
  return m_Parent->GetDriver()->GetTraceLog()->GetWriteError();
}

void
TraceLogModel::OnTraceLogModified()
{
  this->InvokeEvent(ModelUpdateEvent());
}

void
TraceLogModel::OnLayerChange()
{
  // Segmentation layers can be added by many routes (workspaces, registration,
  // deep learning); each needs a starting point in the log
  if (this->IsRecording())
    m_Parent->GetDriver()->TraceSegmentationLayers();
}

Json::Value
TraceLogModel::ViewContext(GenericSliceModel *view, const char *action)
{
  if (!TraceLog::GetActive() || !view || !view->IsSliceInitialized())
    return Json::Value();

  Json::Value ctx(Json::objectValue);
  ctx["view"] = view->GetId();
  ctx["plane"] = PlaneName(view);
  ctx["action"] = action;
  ctx["slice"] = view->GetSliceIndex();
  ctx["zoom"] = view->GetViewZoom();

  // While the views are rotated, the slice number is still right in the views'
  // own world, but the plane it names cuts the anatomy at an angle. Record that
  // plane in the image's anatomy: the points x with normal . x = offset (RAS mm)
  GenericImageData *id = view->GetDriver()->GetCurrentImageData();
  Mat44             T;
  if (ViewToImageRAS(id, T))
  {
    ctx["rotated"] = true;

    // Three directions of the views' voxel grid, mapped into the anatomy
    unsigned int a = (unsigned int)view->GetSliceDirectionInImageSpace();
    Vector3d     v0 = to_double(view->GetDriver()->GetCursorPosition());
    v0[a] = view->GetSliceIndex();
    Vector3d p0 = VoxelToImageRAS(id, v0, &T), e[3];
    for (unsigned int d = 0; d < 3; d++)
    {
      Vector3d v = v0;
      v[d] += 1.0;
      e[d] = VoxelToImageRAS(id, v, &T) - p0;
    }

    // The normal points the way the slice number increases
    Vector3d n = vnl_cross_3d(e[(a + 1) % 3], e[(a + 2) % 3]);
    n.normalize();
    if (dot_product(n, e[a]) < 0)
      n = -n;
    ctx["normal"] = Vec3(n, 6);
    ctx["offset"] = Round(dot_product(n, p0), 3);
  }
  return ctx;
}

Json::Value
TraceLogModel::GetCursorState() const
{
  IRISApplication *driver = m_Parent->GetDriver();
  Json::Value      state(Json::objectValue), slices(Json::arrayValue);
  state["pos"] = Vec3(driver->GetCursorPosition());

  // The point under the cursor in the image's own anatomy, in RAS mm: unlike
  // the voxel, it means the same before, during and after a free rotation
  GenericImageData *id = driver->GetCurrentImageData();
  if (id->IsMainLoaded() && id->GetReferenceSpaceWrapper())
  {
    Mat44 T;
    bool  rotated = ViewToImageRAS(id, T);
    state["ras"] = Vec3(
      VoxelToImageRAS(id, to_double(driver->GetCursorPosition()), rotated ? &T : nullptr), 3);
  }
  state["tp"] = driver->GetCursorTimePoint();
  for (unsigned int i = 0; i < 3; i++)
  {
    GenericSliceModel *view = m_Parent->GetSliceModel(i);
    slices.append(view->IsSliceInitialized() ? view->GetSliceIndex() : -1);
  }
  state["slices"] = slices;
  return state;
}

Json::Value
TraceLogModel::GetViewState(unsigned int i) const
{
  GenericSliceModel *view = m_Parent->GetSliceModel(i);
  Json::Value        state(Json::objectValue), pos(Json::arrayValue);
  state["zoom"] = view->GetViewZoom();
  pos.append(view->GetViewPosition()[0]);
  pos.append(view->GetViewPosition()[1]);
  state["pos"] = pos;
  state["fit"] = view->GetViewIsFit();
  return state;
}

Json::Value
TraceLogModel::GetToolState() const
{
  Json::Value state(Json::objectValue);
  state["mode"] = ToolName(m_Parent->GetGlobalState()->GetToolbarMode());
  return state;
}

Json::Value
TraceLogModel::GetLabelState() const
{
  GlobalState   *gs = m_Parent->GetGlobalState();
  DrawOverFilter dof = gs->GetDrawOverFilter();
  Json::Value    state(Json::objectValue);
  state["draw"] = gs->GetDrawingColorLabel();
  state["over"] = CoverageName(dof.CoverageMode);
  if (dof.CoverageMode == PAINT_OVER_ONE)
    state["over_label"] = dof.DrawOverLabel;
  return state;
}

Json::Value
TraceLogModel::GetSegmentationState() const
{
  // The selected layer id means nothing until an image is loaded
  Json::Value state(Json::objectValue);
  if (m_Parent->GetDriver()->IsMainImageLoaded())
    state["layer"] = (Json::UInt64)m_Parent->GetGlobalState()->GetSelectedSegmentationLayerId();
  else
    state["layer"] = Json::Value();
  return state;
}

Json::Value
TraceLogModel::GetLayoutState() const
{
  Json::Value state(Json::objectValue);
  state["panels"] =
    PanelLayoutName(m_Parent->GetDisplayLayoutModel()->GetViewPanelLayout());
  state["layers"] =
    m_Parent->GetGlobalState()->GetSliceViewLayerLayout() == LAYOUT_TILED ? "tiled" : "stacked";
  return state;
}

Json::Value
TraceLogModel::GetRotationState() const
{
  Json::Value state(Json::objectValue);
  Mat44       T;
  if (!ViewToImageRAS(m_Parent->GetDriver()->GetCurrentImageData(), T))
  {
    state["rotated"] = false;
    return state;
  }

  // The rotation part of the matrix as an angle about an axis (the direction
  // it leaves alone). A move alone has angle 0 and no axis.
  state["rotated"] = true;
  double c = std::max(-1.0, std::min(1.0, (T(0, 0) + T(1, 1) + T(2, 2) - 1.0) / 2.0));
  double theta = std::acos(c);
  Vector3d axis(T(2, 1) - T(1, 2), T(0, 2) - T(2, 0), T(1, 0) - T(0, 1));
  if (axis.two_norm() < 1e-6 && theta > 1.0)
  {
    // A half turn: R + I is then 2 a a', so its longest column lies along a
    for (unsigned int j = 0; j < 3; j++)
    {
      Vector3d col(T(0, j), T(1, j), T(2, j));
      col[j] += 1.0;
      if (col.two_norm() > axis.two_norm())
        axis = col;
    }
  }
  state["angle"] = Round(theta * 180.0 / vnl_math::pi, 3);
  if (axis.two_norm() > 1e-6)
    state["axis"] = Vec3(axis.normalize(), 6);

  Json::Value m(Json::arrayValue);
  for (unsigned int i = 0; i < 4; i++)
    for (unsigned int j = 0; j < 4; j++)
      m.append(Round(T(i, j), 6));
  state["view_to_image"] = m;
  return state;
}

std::map<std::string, Json::Value>
TraceLogModel::GetContrastStates() const
{
  // The contrast curve of each image layer: control points (t, x) map the
  // fraction t of the intensity range to the display intensity x
  std::map<std::string, Json::Value> states;
  GenericImageData *id = m_Parent->GetDriver()->GetCurrentImageData();
  if (!id->IsMainLoaded())
    return states;

  for (LayerIterator it = id->GetLayers(MAIN_ROLE | OVERLAY_ROLE); !it.IsAtEnd(); ++it)
  {
    auto *dmp = dynamic_cast<AbstractContinuousImageDisplayMappingPolicy *>(
      it.GetLayer()->GetDisplayMapping());
    IntensityCurveInterface *curve = dmp ? dmp->GetIntensityCurve() : nullptr;
    if (!curve)
      continue;

    Json::Value state(Json::objectValue), range(Json::arrayValue), points(Json::arrayValue);
    Vector2d    r = dmp->GetNativeImageRangeForCurve();
    range.append(r[0]);
    range.append(r[1]);
    for (unsigned int i = 0; i < curve->GetControlPointCount(); i++)
    {
      double      t, x;
      Json::Value p(Json::arrayValue);
      curve->GetControlPoint(i, t, x);
      p.append(t);
      p.append(x);
      points.append(p);
    }
    state["range"] = range;
    state["points"] = points;
    states["contrast:" + std::to_string(it.GetLayer()->GetUniqueId())] = state;
  }
  return states;
}

void
TraceLogModel::LogSnapshot()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (!log->IsRecording())
    return;

  // One event with the state of everything, which the change events that
  // follow are relative to
  Json::Value ev(Json::objectValue), views(Json::arrayValue), contrast(Json::objectValue);
  if (m_Parent->GetDriver()->IsMainImageLoaded())
  {
    ev["cursor"] = this->GetCursorState();
    log->PrimeState("cursor", ev["cursor"]);
    for (unsigned int i = 0; i < 3; i++)
    {
      Json::Value vs = this->GetViewState(i);
      log->PrimeState("view:" + std::to_string(i), vs);
      vs["view"] = i;
      vs["plane"] = PlaneName(m_Parent->GetSliceModel(i));
      views.append(vs);
    }
    ev["views"] = views;
  }

  ev["tool"] = this->GetToolState();
  ev["labels"] = this->GetLabelState();
  ev["segmentation"] = this->GetSegmentationState();
  ev["layout"] = this->GetLayoutState();
  ev["rotation"] = this->GetRotationState();
  log->PrimeState("tool", ev["tool"]);
  log->PrimeState("rotation", ev["rotation"]);
  m_LastRotation = ev["rotation"];
  log->PrimeState("labels", ev["labels"]);
  log->PrimeState("segmentation", ev["segmentation"]);
  log->PrimeState("layout", ev["layout"]);

  for (const auto &kv : this->GetContrastStates())
  {
    log->PrimeState(kv.first, kv.second);
    contrast[kv.first.substr(9)] = kv.second;
  }
  ev["contrast"] = contrast;

  log->Log("snapshot", ev);
}

void
TraceLogModel::OnCursorUpdate()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (!log->IsRecording() || !m_Parent->GetDriver()->IsMainImageLoaded())
    return;

  // A slice change by key or wheel is its own event, so that the time spent
  // on each slice can be read from the log. Dragging the crosshairs, or 4D
  // playback, moves the cursor many times a second; those changes are merged
  // like other state changes.
  Json::Value state = this->GetCursorState();
  for (Json::ArrayIndex i = 0; i < state["slices"].size(); i++)
  {
    if (state["slices"][i].asInt() < 0)
    {
      m_CursorNotReady = true;
      return;
    }
  }

  log->LogStateChange("cursor", "cursor", state);
  std::string action = log->GetContext()["action"].asString();
  if (action == "keys" || action == "scroll")
    log->FlushPendingState();
}

void
TraceLogModel::OnViewGeometryChange(itk::Object *source, const itk::EventObject &)
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (!log->IsRecording())
    return;

  for (unsigned int i = 0; i < 3; i++)
  {
    GenericSliceModel *view = m_Parent->GetSliceModel(i);
    if (view == source && view->IsSliceInitialized())
    {
      Json::Value fields(Json::objectValue);
      fields["view"] = i;
      fields["plane"] = PlaneName(view);
      log->LogStateChange("view", "view:" + std::to_string(i), this->GetViewState(i), fields);
    }
  }

  // Once all the views are set up for a new image, the cursor's state is known
  if (m_CursorNotReady && m_Parent->GetSliceModel(0)->IsSliceInitialized() &&
      m_Parent->GetSliceModel(1)->IsSliceInitialized() &&
      m_Parent->GetSliceModel(2)->IsSliceInitialized())
  {
    m_CursorNotReady = false;
    log->PrimeState("cursor", this->GetCursorState());
  }
}

void
TraceLogModel::OnToolbarModeChange()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (log->IsRecording())
    log->LogStateChange("tool", "tool", this->GetToolState());
}

void
TraceLogModel::OnLabelsChange()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (log->IsRecording())
    log->LogStateChange("labels", "labels", this->GetLabelState());
}

void
TraceLogModel::OnSelectedSegmentationChange()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (log->IsRecording())
    log->LogStateChange("segmentation", "segmentation", this->GetSegmentationState());
}

void
TraceLogModel::OnLayoutChange()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (log->IsRecording())
    log->LogStateChange("layout", "layout", this->GetLayoutState());
}

void
TraceLogModel::OnRotationChange()
{
  // Fired when a layer's transform changes. Free rotation changes the main
  // image first, then each segmentation; the views follow the active one.
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (!log->IsRecording())
    return;

  Json::Value state = this->GetRotationState();
  if (state == m_LastRotation)
    return;
  m_LastRotation = state;
  log->LogStateChange("rotation", "rotation", state);

  // The cursor stays on the same voxel of the views, which now shows another
  // point of the anatomy. Keep that out of the cursor events, which record
  // moves of the cursor: the next one starts from the point it shows now,
  // which the rotation's view_to_image matrix gives.
  if (m_Parent->GetDriver()->IsMainImageLoaded() && !m_CursorNotReady &&
      m_Parent->GetSliceModel(0)->IsSliceInitialized() &&
      m_Parent->GetSliceModel(1)->IsSliceInitialized() &&
      m_Parent->GetSliceModel(2)->IsSliceInitialized())
    log->PrimeState("cursor", this->GetCursorState());
}

void
TraceLogModel::OnDisplayMappingChange()
{
  TraceLog *log = m_Parent->GetDriver()->GetTraceLog();
  if (!log->IsRecording())
    return;

  for (const auto &kv : this->GetContrastStates())
  {
    Json::Value fields(Json::objectValue);
    fields["layer"] = (Json::UInt64)std::stoul(kv.first.substr(9));
    log->LogStateChange("contrast", kv.first, kv.second, fields);
  }
}

std::string
TraceLogModel::FindLogOfSelectedSegmentation() const
{
  LabelImageWrapper *seg = m_Parent->GetDriver()->GetSelectedSegmentationLayer();
  const char        *fn = seg ? seg->GetFileName() : nullptr;
  if (!fn || !fn[0])
    return std::string();
  std::string sidecar = TraceLog::SidecarFileName(fn);
  return itksys::SystemTools::FileExists(sidecar, true) ? sidecar : std::string();
}

unsigned long
TraceLogModel::PrepareReplay(const std::string &file)
{
  IRISApplication *driver = m_Parent->GetDriver();
  if (!driver->IsMainImageLoaded())
    throw IRISException("Error: No image. Load the image the trace log was recorded on, "
                        "then replay the log.");

  std::unique_ptr<TraceLogReplay> replay(new TraceLogReplay());
  replay->Read(file);

  const TraceLogReplay::LayerState &base = replay->GetBase();
  if (base.checksum.empty())
    throw IRISException("Error: Nothing to replay. The trace log does not contain a "
                        "segmentation with a starting point.");

  // A segmentation has the size of the main image
  ImageWrapperBase *main = driver->GetCurrentImageData()->GetMain();
  Vector3ui         size = main->GetSize();
  if (size[0] != base.size[0] || size[1] != base.size[1] || size[2] != base.size[2] ||
      main->GetNumberOfTimePoints() != base.tps)
    throw IRISException("Error: Different image. The trace log was recorded on an image of "
                        "%u x %u x %u voxels with %u time point(s); the image loaded is "
                        "%u x %u x %u with %u.",
                        base.size[0], base.size[1], base.size[2], base.tps,
                        size[0], size[1], size[2], main->GetNumberOfTimePoints());

  // Replay into a new layer, from the starting point the log holds. A log
  // recorded before ITK-SNAP kept the starting segmentation in it can only be
  // replayed onto that segmentation, which must then be the one selected.
  m_ReplayIntoNewLayer = replay->HasStartingSegmentation();
  if (!m_ReplayIntoNewLayer)
  {
    // The selected segmentation may be the start, or a point along the history
    LabelImageWrapper *seg = driver->GetSelectedSegmentationLayer();
    std::string        checksum = seg ? TraceLog::ComputeChecksum(seg) : std::string();
    if (!seg || (checksum != base.checksum && !replay->StartFrom(checksum)))
      throw IRISException("Error: Starting segmentation needed. This trace log starts from a "
                          "segmentation that already had labels when recording began, and it "
                          "was recorded by an earlier version of ITK-SNAP, which did not keep a "
                          "copy of it in the log. Open that segmentation, select it, and "
                          "replay the log again.");
  }

  m_Replay = std::move(replay);
  return m_Replay->GetSteps().size();
}

void
TraceLogModel::FillStartingSegmentation(LabelImageWrapper *seg, std::set<LabelType> &labels)
{
  // The content runs cover all voxels in raster order, one time point after
  // another; the new layer is blank, so only labels other than 0 are written
  IRISApplication *driver = m_Parent->GetDriver();
  const auto      &content = m_Replay->GetBase().content;
  size_t           run = 0;
  unsigned long    used = 0;
  for (unsigned int t = 0; t < seg->GetNumberOfTimePoints(); t++)
  {
    if (seg->GetNumberOfTimePoints() > 1)
      driver->SetCursorTimePoint(t);

    SegmentationUpdateIterator it(seg, seg->GetBufferedRegion(), 0, DrawOverFilter());
    for (; !it.IsAtEnd() && run < content.size(); ++it)
    {
      if (content[run].second != 0)
      {
        it.ReplaceLabel(0, content[run].second);
        labels.insert(content[run].second);
      }
      if (++used == content[run].first)
      {
        ++run;
        used = 0;
      }
    }
    it.Finalize("Replay: starting segmentation");
  }
}

TraceLogModel::ReplayResult
TraceLogModel::Replay(unsigned long n_steps)
{
  ReplayResult     result;
  IRISApplication *driver = m_Parent->GetDriver();
  if (!m_Replay || !driver->GetSelectedSegmentationLayer())
    return result;

  const std::vector<TraceLogReplay::Step> &steps = m_Replay->GetSteps();
  result.total_steps = steps.size();
  result.log_sessions = (unsigned int)m_Replay->GetSessions().size();
  result.replay_sessions = m_Replay->GetReplaySessions();
  if (m_Replay->IsHistoryIncomplete())
    result.incomplete_from = m_Replay->GetBase().session + 1;
  n_steps = std::min(n_steps, (unsigned long)steps.size());

  // Mark the edits made by the replay as such, in case this session is being
  // recorded too
  Json::Value ctx(Json::objectValue);
  ctx["replay"] = m_Replay->GetSession()["session"];
  TraceLog::ScopedContext scoped(ctx);

  unsigned int        tp_start = driver->GetCursorTimePoint();
  std::set<LabelType> labels;

  // A new layer, starting from the segmentation the log holds; the
  // segmentation that was open is left as it is
  if (m_ReplayIntoNewLayer)
  {
    driver->AddBlankSegmentation();
    result.new_layer = true;
  }
  LabelImageWrapper *seg = driver->GetSelectedSegmentationLayer();
  if (m_ReplayIntoNewLayer && !m_Replay->GetBase().content.empty())
    this->FillStartingSegmentation(seg, labels);
  for (unsigned long i = 0; i < n_steps; i++)
  {
    const TraceLogReplay::Step &s = steps[i];
    if (seg->GetNumberOfTimePoints() > 1)
      driver->SetCursorTimePoint(s.tp);

    if (!seg->GetBufferedRegion().IsInside(s.region))
    {
      result.failed_seq = s.seq;
      result.failed_session = s.session + 1;
      result.outside = true;
      break;
    }

    // Check the whole step before changing anything, so that a step that does
    // not fit is left out rather than half applied
    unsigned long mismatches = 0;
    itk::ImageRegionConstIterator<LabelImageWrapper::ImageType> check(seg->GetImage(), s.region);
    for (const TraceLogReplay::Run &run : s.runs)
      for (unsigned long j = 0; j < run.length && !check.IsAtEnd(); j++, ++check)
        if (run.changed && check.Get() != run.old_label)
          ++mismatches;
    if (mismatches)
    {
      result.failed_seq = s.seq;
      result.failed_session = s.session + 1;
      result.mismatches = mismatches;
      break;
    }

    SegmentationUpdateIterator it(seg, s.region, 0, DrawOverFilter());
    for (const TraceLogReplay::Run &run : s.runs)
    {
      for (unsigned long j = 0; j < run.length && !it.IsAtEnd(); j++, ++it)
      {
        if (run.changed)
        {
          it.ReplaceLabel(run.old_label, run.new_label);
          labels.insert(run.new_label);
        }
      }
    }

    std::string title = (s.kind == "edit" ? "Replay: " : "Replay " + s.kind + ": ") + s.op;
    it.Finalize(title.c_str());
    result.applied_steps++;
  }

  // Labels the replay painted with are in use, as after loading a segmentation
  for (LabelType label : labels)
    driver->GetColorLabelTable()->SetColorLabelValid(label, true);

  if (seg->GetNumberOfTimePoints() > 1)
    driver->SetCursorTimePoint(tp_start);

  if (result.applied_steps == result.total_steps && !m_Replay->GetFinalChecksum().empty())
  {
    result.compared = true;
    result.final_matches = TraceLog::ComputeChecksum(seg) == m_Replay->GetFinalChecksum();
  }

  driver->InvokeEvent(SegmentationChangeEvent());
  return result;
}
