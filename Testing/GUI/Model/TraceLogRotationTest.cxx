/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogRotationTest.cxx
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

// Records a trace log while the image is freely rotated (Tools > Image Free
// Rotation), and checks what the log says about it:
//
//  - a "rotation" event, with the angle, the axis and the matrix that maps the
//    views' fixed world to the image's own anatomy; a rotation done in steps
//    is one event, and it does not show up as a cursor move
//  - the cursor's position in the image's own anatomy, in RAS millimetres
//  - the plane of the view a stroke was painted in, in that anatomy
//
// and that the segmentation painted in the rotated view replays exactly from
// the log saved with it. Every expected position is worked out with ITK's own
// transform, not with the code under test.

#include "GlobalUIModel.h"
#include "GenericSliceModel.h"
#include "PaintbrushModel.h"
#include "InteractiveRegistrationModel.h"
#include "RegistrationModel.h"
#include "TraceLogModel.h"
#include "IRISApplication.h"
#include "GenericImageData.h"
#include "GlobalState.h"
#include "ImageWrapperBase.h"
#include "LabelImageWrapper.h"
#include "IRISException.h"
#include "ImageIODelegates.h"
#include "GuidedNativeImageIO.h"
#include "SystemInterface.h"
#include "ColorMap.h"
#include "UIReporterDelegates.h"
#include "TraceLog.h"
#include "itkImage.h"
#include "itkImageFileWriter.h"
#include "itkTransform.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "itksys/SystemTools.hxx"
#include <vnl/vnl_math.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

class DummySystemInfoDelegate : public SystemInfoDelegate
{
public:
  DummySystemInfoDelegate(const char *argv0, std::string dataDir)
    : m_ExecutableName(argv0), m_DataDir(dataDir) {}

  std::string GetApplicationDirectory() override
  { return itksys::SystemTools::GetFilenamePath(m_ExecutableName); }

  std::string GetApplicationFile() override { return m_ExecutableName; }
  std::string GetApplicationPermanentDataLocation() override { return m_DataDir; }
  std::string GetUserDocumentsLocation() override { return m_DataDir; }
  std::string GetTempDirectory() override { return m_DataDir; }
  std::string EncodeServerURL(const std::string &url) override { return url; }

  void LoadResourceAsImage2D(std::string, GrayscaleImage *) override {}
  void LoadResourceAsRegistry(std::string, Registry &) override {}
  void WriteRGBAImage2D(std::string, RGBAImageType *) override {}

protected:
  std::string m_ExecutableName, m_DataDir;
};

class DummyColorMapPresetNameSource : public AbstractColorMapPresetNameSource
{
public:
  std::string GetPresetName(ColorMap::SystemPreset preset, bool) override
  { return "Color Map " + std::to_string(static_cast<int>(preset)); }
};

// The slice views need the size of their window; this stands in for one
class FixedSizeReporter : public ViewportSizeReporter
{
public:
  irisITKObjectMacro(FixedSizeReporter, ViewportSizeReporter)
  bool      CanReportSize() override { return true; }
  Vector2ui GetViewportSize() override { return Vector2ui(400, 400); }
  float     GetViewportPixelRatio() override { return 1.0f; }
  Vector2ui GetLogicalViewportSize() override { return Vector2ui(400, 400); }

protected:
  FixedSizeReporter() {}
  ~FixedSizeReporter() override {}
};

static int n_failures = 0;

static void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!condition)
    n_failures++;
}

// Image geometry: anisotropic voxels and a direction that permutes and flips
// the axes, so that mixing up voxel, LPS and RAS coordinates shows up
static const unsigned int N[3] = { 48, 40, 44 };

template <class TImage>
static typename TImage::Pointer MakeImage()
{
  auto img = TImage::New();
  typename TImage::RegionType region;
  for (unsigned int d = 0; d < 3; d++)
    region.SetSize(d, N[d]);
  img->SetRegions(region);

  double spacing[3] = { 1.0, 1.25, 0.8 };
  double origin[3] = { -20.0, 15.0, 5.0 };
  img->SetSpacing(spacing);
  img->SetOrigin(origin);

  typename TImage::DirectionType dir;
  dir.Fill(0.0);
  dir(0, 0) = -1.0;
  dir(1, 2) = 1.0;
  dir(2, 1) = 1.0;
  img->SetDirection(dir);

  img->Allocate();
  img->FillBuffer(0);
  return img;
}

template <class TImage>
static void WriteImage(TImage *img, const std::string &fn)
{
  auto writer = itk::ImageFileWriter<TImage>::New();
  writer->SetInput(img);
  writer->SetFileName(fn);
  writer->Update();
}

static Vector3d LPSToRAS(const itk::Point<double, 3> &p)
{
  return Vector3d(-p[0], -p[1], p[2]);
}

// Where the views show voxel v of their own grid in the image's anatomy (RAS
// mm), from ITK: the reference point of v, moved by the image's transform
static Vector3d ExpectedRAS(LabelImageWrapper *seg, const Vector3d &v)
{
  itk::ContinuousIndex<double, 3> ci;
  for (unsigned int d = 0; d < 3; d++)
    ci[d] = v[d];
  itk::Point<double, 3> x_ref;
  seg->GetReferenceSpace()->TransformContinuousIndexToPhysicalPoint(ci, x_ref);
  return LPSToRAS(seg->GetITKTransform()->TransformPoint(x_ref));
}

// Pairs (point of the views' world, image point shown there), both RAS mm,
// for a few voxels of the views' grid, under the transform as it is now
typedef std::vector<std::pair<Vector3d, Vector3d>> WorldToImageSamples;

static WorldToImageSamples SampleWorldToImage(LabelImageWrapper *seg)
{
  WorldToImageSamples samples;
  for (Vector3d v : { Vector3d(0, 0, 0), Vector3d(10, 20, 15), Vector3d(47, 39, 43) })
  {
    itk::ContinuousIndex<double, 3> ci;
    for (unsigned int d = 0; d < 3; d++)
      ci[d] = v[d];
    itk::Point<double, 3> x_ref;
    seg->GetReferenceSpace()->TransformContinuousIndexToPhysicalPoint(ci, x_ref);
    samples.push_back(std::make_pair(LPSToRAS(x_ref), ExpectedRAS(seg, v)));
  }
  return samples;
}

// Largest error of a logged view_to_image matrix on the samples
static double MatrixError(const Json::Value &m, const WorldToImageSamples &samples)
{
  if (!m.isArray() || m.size() != 16)
    return 1e100;
  double worst = 0.0;
  for (const auto &xy : samples)
  {
    Vector3d r = xy.first, y;
    for (unsigned int i = 0; i < 3; i++)
      y[i] = m[4 * i].asDouble() * r[0] + m[4 * i + 1].asDouble() * r[1] +
             m[4 * i + 2].asDouble() * r[2] + m[4 * i + 3].asDouble();
    worst = std::max(worst, (y - xy.second).two_norm());
  }
  return worst;
}

static Vector3d JsonVec3(const Json::Value &a)
{
  if (!a.isArray() || a.size() != 3)
    return Vector3d(1e100, 1e100, 1e100);
  return Vector3d(a[0].asDouble(), a[1].asDouble(), a[2].asDouble());
}

static std::vector<Json::Value> ReadEvents(const std::string &file)
{
  std::ifstream            in(file.c_str());
  std::vector<Json::Value> events;
  std::string              line;
  Json::Reader             reader;
  while (std::getline(in, line))
  {
    Json::Value ev;
    if (reader.parse(line, ev, false))
      events.push_back(ev);
    else
      check(false, "every line of the log is JSON");
  }
  return events;
}

static std::vector<Json::Value> OfType(const std::vector<Json::Value> &events, const char *type)
{
  std::vector<Json::Value> out;
  for (const auto &ev : events)
    if (ev["type"].asString() == type)
      out.push_back(ev);
  return out;
}

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <temp_dir>" << std::endl;
    return EXIT_FAILURE;
  }

  std::string tmpdir = std::string(argv[1]) + "/TraceLogRotationTest";
  itksys::SystemTools::RemoveADirectory(tmpdir);
  itksys::SystemTools::MakeDirectory(tmpdir);

  DummySystemInfoDelegate sidel(argv[0], tmpdir);
  SystemInterface::SetSystemInfoDelegate(&sidel);
  DummyColorMapPresetNameSource color_map_source;
  ColorMap::SetColorMapPresetNameSource(&color_map_source);

  try
  {
    // A main image and a blank segmentation
    using GreyImage = itk::Image<short, 3>;
    using SegImage = itk::Image<LabelType, 3>;
    auto grey = MakeImage<GreyImage>();
    for (itk::ImageRegionIteratorWithIndex<GreyImage> it(grey, grey->GetBufferedRegion()); !it.IsAtEnd(); ++it)
      it.Set(static_cast<short>(it.GetIndex()[0] + 2 * it.GetIndex()[1] + 3 * it.GetIndex()[2]));
    auto segimg = MakeImage<SegImage>();

    std::string fn_grey = tmpdir + "/main.nii.gz", fn_seg = tmpdir + "/seg.nii.gz";
    WriteImage(grey.GetPointer(), fn_grey);
    WriteImage(segimg.GetPointer(), fn_seg);

    SmartPtr<GlobalUIModel> gui = GlobalUIModel::New();
    IRISApplication        *driver = gui->GetDriver();
    SmartPtr<FixedSizeReporter> size_reporter = FixedSizeReporter::New();
    for (unsigned int i = 0; i < 3; i++)
      gui->GetSliceModel(i)->SetSizeReporter(size_reporter);
    IRISWarningList         wl;
    driver->OpenImage(fn_grey.c_str(), MAIN_ROLE, wl);
    driver->OpenImage(fn_seg.c_str(), LABEL_ROLE, wl);
    for (unsigned int i = 0; i < 3; i++)
      gui->GetSliceModel(i)->Update();

    LabelImageWrapper *seg = driver->GetSelectedSegmentationLayer();
    if (!seg || !gui->GetSliceModel(0)->IsSliceInitialized())
      throw IRISException("The segmentation or the slice views are not set up");

    // Record
    TraceLogModel *tlm = gui->GetTraceLogModel();
    tlm->SetTraceLogDirectory(tmpdir + "/logs");
    if (!tlm->StartRecording("rotation-test"))
      throw IRISException("Could not start recording");
    std::string log_file = tlm->GetFileName();
    TraceLog   *log = driver->GetTraceLog();

    // Free rotation about the image centre, where the cursor starts
    RegistrationModel *reg = gui->GetRegistrationModel();
    reg->SetFreeRotationMode(true);
    reg->Update();
    reg->SetCenterOfRotationToCursor();

    // A cursor move before any rotation, away from the centre, so that the
    // rotation moves the point under the cursor. Flushing stands in for a
    // pause, so that each step is its own event.
    Vector3i c1(10, 20, 15);
    driver->SetCursorPosition(c1);
    log->FlushPendingState();
    Vector3d ras_c1_before = ExpectedRAS(seg, to_double(c1));

    // Rotate 30 degrees about an oblique axis, in two steps, as a drag of
    // the rotation widget does
    Vector3d axis(1.0, 2.0, 3.0);
    axis.normalize();
    reg->ApplyRotation(axis, 10.0 * vnl_math::pi / 180.0);
    reg->ApplyRotation(axis, 20.0 * vnl_math::pi / 180.0);
    log->FlushPendingState();
    if (!driver->GetCurrentImageData()->IsFreeRotation())
      throw IRISException("The image is not rotated");
    Vector3d ras_c1_after = ExpectedRAS(seg, to_double(c1));
    WorldToImageSamples after_first = SampleWorldToImage(seg);
    check((ras_c1_after - ras_c1_before).two_norm() > 1.0,
          "the rotation moves the point under the cursor (" +
            std::to_string((ras_c1_after - ras_c1_before).two_norm()) + " mm)");

    // A cursor move while rotated
    Vector3i c2(30, 12, 25);
    driver->SetCursorPosition(c2);
    log->FlushPendingState();
    Vector3d ras_c2 = ExpectedRAS(seg, to_double(c2));

    // Paint a short stroke in view 0 at the cursor, with label 1
    GenericSliceModel *view = gui->GetSliceModel(0);
    PaintbrushSettings pbs = driver->GetGlobalState()->GetPaintbrushSettings();
    pbs.radius = 2.0;
    pbs.shape = PAINTBRUSH_ROUND;
    pbs.smart_mode = PAINTBRUSH_MANUAL;
    pbs.volumetric = false;
    pbs.chase = false;
    driver->GetGlobalState()->SetPaintbrushSettings(pbs);
    driver->GetGlobalState()->SetDrawingColorLabel(1);

    PaintbrushModel *brush = gui->GetPaintbrushModel(0);
    Vector3d         x0 = view->MapImageToSlice(to_double(c2) + 0.5);
    Vector3d         x1 = x0 + Vector3d(3.0, 0.0, 0.0);
    brush->ProcessPushEvent(x0, Vector2ui(0, 0), false);
    brush->ProcessDragEvent(x1, x0, 3.0, true);
    log->FlushPendingState();

    // Points of the slice the stroke was painted on, and of the next slice,
    // in the anatomy as it was when painting
    unsigned int a = (unsigned int)view->GetSliceDirectionInImageSpace();
    std::vector<Vector3d> on_plane;
    Vector3d              e_b(0, 0, 0), e_c(0, 0, 0), e_a(0, 0, 0), v0 = to_double(c2);
    e_a[a] = 1.0;
    e_b[(a + 1) % 3] = 5.0;
    e_c[(a + 2) % 3] = -7.0;
    for (Vector3d v : { v0, Vector3d(v0 + e_b), Vector3d(v0 + e_c), Vector3d(v0 + e_b + e_c) })
      on_plane.push_back(ExpectedRAS(seg, v));
    Vector3d ahead = ExpectedRAS(seg, Vector3d(v0 + e_a)) - ExpectedRAS(seg, v0);

    unsigned long painted = 0;
    for (itk::ImageRegionConstIterator<LabelImageWrapper::ImageType> it(seg->GetImage(),
                                                                       seg->GetBufferedRegion());
         !it.IsAtEnd(); ++it)
      if (it.Get() == 1)
        painted++;
    std::cout << "Painted " << painted << " voxels" << std::endl;
    check(painted > 0, "the stroke painted the segmentation");

    // Save; the log is copied next to the segmentation
    std::string fn_out = tmpdir + "/painted.nii.gz";
    {
      SmartPtr<AbstractSaveImageDelegate> del = driver->CreateSaveDelegateForLayer(seg, LABEL_ROLE);
      SmartPtr<GuidedNativeImageIO>       io = GuidedNativeImageIO::New();
      Registry                            reg_io;
      IRISWarningList                     wl_save;
      del->SaveImage(fn_out, io, reg_io, wl_save);
      check(wl_save.empty(), "saving gave no warnings");
    }
    std::string sidecar = TraceLog::SidecarFileName(fn_out);
    check(itksys::SystemTools::FileExists(sidecar, true), "the log was saved next to the segmentation");

    // Turn the image further by dragging the rotation widget in view 0, a
    // circle about the centre of rotation: hover over it, then drag a quarter
    // of the way round
    InteractiveRegistrationModel *irm = gui->GetInteractiveRegistrationModel(0);
    unsigned long main_id = driver->GetCurrentImageData()->GetMain()->GetUniqueId();
    double        radius = irm->GetRotationWidgetRadius() / 2.0;
    Vector3d      ctr = view->MapImageToSlice(to_double(reg->GetRotationCenter()));
    Vector3d      sp = view->GetReferenceSpaceSpacing();
    Vector3d      w0 = ctr + Vector3d(radius / sp[0], 0.0, 0.0);
    Vector3d      w1 = ctr + Vector3d(0.0, radius / sp[1], 0.0);
    irm->ProcessMouseMoveEvent(w0, main_id);
    check(irm->IsHoveringOverRotationWidget(), "the mouse is over the rotation widget");
    irm->ProcessPushEvent(w0);
    irm->ProcessDragEvent(0.5 * (w0 + w1), w0);
    irm->ProcessReleaseEvent(w1, w0);
    log->FlushPendingState();
    WorldToImageSamples after_widget = SampleWorldToImage(seg);
    tlm->StopRecording();

    std::vector<Json::Value> events = ReadEvents(log_file);

    //=== Snapshot: not rotated, and the cursor in RAS mm
    std::vector<Json::Value> snapshots = OfType(events, "snapshot");
    check(snapshots.size() == 1, "one snapshot");
    if (snapshots.size())
    {
      check(snapshots[0]["rotation"]["rotated"] == false, "snapshot: not rotated");
      check(snapshots[0]["cursor"].isMember("ras"), "snapshot: the cursor has its RAS position");
    }

    //=== Cursor events: one per move, none for the rotation
    std::vector<Json::Value> cursors = OfType(events, "cursor");
    check(cursors.size() == 2, "two cursor events, for the two moves (got " +
                                 std::to_string(cursors.size()) + ")");
    if (cursors.size() == 2)
    {
      check((JsonVec3(cursors[0]["to"]["ras"]) - ras_c1_before).two_norm() < 1e-2,
            "before rotating, the cursor's RAS position is the image point under it");
      check((JsonVec3(cursors[1]["from"]["ras"]) - ras_c1_after).two_norm() < 1e-2,
            "the move after rotating starts from the point the rotated view shows");
      check((JsonVec3(cursors[1]["to"]["ras"]) - ras_c2).two_norm() < 1e-2,
            "while rotated, the cursor's RAS position is the image point under it");
      const Json::Value &pos = cursors[1]["to"]["pos"];
      check(pos[0].asInt() == c2[0] && pos[1].asInt() == c2[1] && pos[2].asInt() == c2[2],
            "the cursor's voxel is still recorded, in the views' grid");
    }

    //=== Rotation: one event for the two steps, with the angle, the axis and
    //=== the matrix
    std::vector<Json::Value> rotations = OfType(events, "rotation");
    check(rotations.size() == 2, "two rotation events, one per gesture (got " +
                                   std::to_string(rotations.size()) + ")");
    if (rotations.size() >= 1)
    {
      const Json::Value &to = rotations[0]["to"];
      check(rotations[0]["from"]["rotated"] == false, "rotation: from not rotated");
      check(rotations[0]["n"].asUInt() >= 2, "rotation: the two steps are merged into one event");
      check(to["rotated"] == true, "rotation: to rotated");
      check(std::fabs(to["angle"].asDouble() - 30.0) < 1e-2,
            "rotation: 30 degrees (got " + to["angle"].asString() + ")");

      // ApplyRotation takes the axis in ITK's LPS space
      Vector3d axis_ras(-axis[0], -axis[1], axis[2]);
      check(std::fabs(std::fabs(dot_product(JsonVec3(to["axis"]), axis_ras)) - 1.0) < 1e-4,
            "rotation: about the axis it was given");

      // The matrix maps a point of the views' world to the image point shown
      // there, as it was after this rotation
      check(MatrixError(to["view_to_image"], after_first) < 1e-3,
            "rotation: the matrix maps the views' world to the image");
    }

    //=== The widget drag: one event, from where the first rotation left off,
    //=== in view 0
    if (rotations.size() == 2)
    {
      const Json::Value &ev = rotations[1];
      check(ev["from"] == rotations[0]["to"], "widget: starts from the first rotation");
      check(ev["to"]["rotated"] == true && ev["to"] != ev["from"], "widget: turns the image");
      check(ev["ctx"]["action"].asString() == "rotate" && ev["ctx"]["view"].asInt() == 0,
            "widget: recorded as a rotate in view 0");
      check(MatrixError(ev["to"]["view_to_image"], after_widget) < 1e-3,
            "widget: the matrix maps the views' world to the image");
    }

    //=== The stroke: its view's plane in the image's anatomy
    unsigned int n_edits = 0, n_planes_ok = 0;
    for (const auto &ev : OfType(events, "edit"))
    {
      const Json::Value &ctx = ev["ctx"];
      if (ctx["tool"].asString() != "paintbrush")
        continue;
      n_edits++;

      // Points of the view's slice lie on the plane; the next slice is ahead
      Vector3d n = JsonVec3(ctx["normal"]);
      double   offset = ctx["offset"].asDouble();
      bool     ok = ctx["rotated"] == true && ctx["view"].asInt() == 0 &&
                ctx["slice"].asInt() == c2[a] && std::fabs(n.two_norm() - 1.0) < 1e-5;
      for (const Vector3d &x : on_plane)
        ok = ok && std::fabs(dot_product(n, x) - offset) < 1e-2;
      ok = ok && dot_product(n, ahead) > 0;
      if (ok)
        n_planes_ok++;
    }
    check(n_edits > 0, "the stroke was recorded as paintbrush edits");
    check(n_edits == n_planes_ok, "each edit gives the plane of the view it was painted in");

    //=== Replay the saved log: it rebuilds the painted segmentation exactly
    unsigned long n_steps = tlm->PrepareReplay(sidecar);
    TraceLogModel::ReplayResult result = tlm->Replay(n_steps);
    check(result.applied_steps == result.total_steps && result.total_steps > 0,
          "replay: all " + std::to_string(result.total_steps) + " steps applied");
    check(result.new_layer, "replay: into a new layer");
    check(result.compared && result.final_matches, "replay: identical to the saved segmentation");
  }
  catch (std::exception &e)
  {
    std::cerr << "Exception: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  if (n_failures)
  {
    std::cout << n_failures << " check(s) failed" << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "All checks passed" << std::endl;
  return EXIT_SUCCESS;
}
