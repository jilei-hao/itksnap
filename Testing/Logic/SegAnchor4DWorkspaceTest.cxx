// Workspace round trip with 4D segmentations on their own grids (seg_anchor).
//
// Builds a workspace from a 4D main image and two 4D segmentations whose grids
// differ from the main image and from each other, edits one time point of the
// second segmentation (the active one), saves that segmentation and the
// workspace, opens the workspace in a new IRISApplication and checks that
//   - both segmentations come back in order, each on its own 4D grid, and the
//     saved one was written on its own grid, not the main image's;
//   - every time point of every segmentation holds the same labels, including
//     the edit;
//   - the first segmentation is the active one after loading, and every layer
//     uses it as the reference space;
//   - activating the other segmentation moves every layer's reference space to
//     it and keeps the cursor on the same physical point;
//   - changing the time point reaches the segmentation that is not active.
//
// Usage: SegAnchor4DWorkspaceTest <TestData dir> <scratch dir>
//
// See Testing/GUI/Qt/Scripts/test_SegAnchor4DSwitching.js for how the test data
// was made and how the probe point P was chosen.

#include "IRISApplication.h"
#include "GenericImageData.h"
#include "GlobalState.h"
#include "ImageIODelegates.h"
#include "GuidedNativeImageIO.h"
#include "LayerIterator.h"
#include "LabelImageWrapper.h"
#include "UIReporterDelegates.h"
#include "ColorMap.h"
#include <itksys/SystemTools.hxx>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

class SimpleSystemInfoDelegate : public SystemInfoDelegate
{
public:
  SimpleSystemInfoDelegate(const char *argv0) { m_Exe = argv0; }

  std::string GetApplicationDirectory() override
    { return itksys::SystemTools::GetFilenamePath(m_Exe); }
  std::string GetApplicationFile() override { return m_Exe; }
  std::string GetApplicationPermanentDataLocation() override { return ".itksnap_test"; }
  std::string GetUserDocumentsLocation() override { return ".itksnap_test"; }
  std::string GetTempDirectory() override { return m_Temp; }
  std::string EncodeServerURL(const std::string &url) override { return url; }

  typedef SystemInfoDelegate::GrayscaleImage GrayscaleImage;
  typedef SystemInfoDelegate::RGBAImageType  RGBAImageType;
  void LoadResourceAsImage2D(std::string, GrayscaleImage *) override {}
  void LoadResourceAsRegistry(std::string, Registry &) override {}
  void WriteRGBAImage2D(std::string, RGBAImageType *) override {}

  std::string m_Temp;

private:
  std::string m_Exe;
};

class SimpleColorMapSource : public AbstractColorMapPresetNameSource
{
public:
  std::string GetPresetName(ColorMap::SystemPreset p, bool) override
    { return "Preset " + std::to_string(static_cast<int>(p)); }
};

// What we know about each segmentation file (0-based voxel indices)
struct SegmentationFile
{
  std::string      name;
  Vector3ui        size;
  Vector3d         spacing;
  Vector3d         origin;   // LPS, as stored in the file
  itk::Index<3>    probe;    // the probe point P in this grid
  std::vector<int> labels;   // label at P for time points 0..10
};

static const unsigned int NT = 11;

static const SegmentationFile SEG1 = {
  "seg4d_11f_label1_x2crop.nii.gz",
  Vector3ui(14, 58, 66), Vector3d(2.78274, 2.76463, 2.42223),
  Vector3d(-89.9669, -6.4464, 15.3336),
  {{6, 28, 20}}, {1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0}
};

static const SegmentationFile SEG2 = {
  "seg4d_11f_label2_x15.nii.gz",
  Vector3ui(57, 53, 57), Vector3d(3.71032, 3.68617, 3.22964),
  Vector3d(-2.3107, -2.2994, 2.0113),
  {{28, 22, 19}}, {0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0}
};

// The edit made to SEG2 before saving: this label at P at this time point
static const unsigned int EDIT_TP = 6;
static const LabelType EDIT_LABEL = 7;

static int g_Failures = 0;

#define CHECK(cond, msg)                                              \
  if (!(cond))                                                        \
    {                                                                 \
    std::cerr << "FAIL [" << stage << "]: " << msg << std::endl;      \
    ++g_Failures;                                                     \
    }

static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static std::vector<ImageWrapperBase *> SegmentationLayers(IRISApplication *app)
{
  std::vector<ImageWrapperBase *> segs;
  for (LayerIterator it(app->GetCurrentImageData(), LABEL_ROLE); !it.IsAtEnd(); ++it)
    segs.push_back(it.GetLayer());
  return segs;
}

static void CheckGrid(const std::string &stage, ImageWrapperBase *layer, const SegmentationFile &f)
{
  CHECK(itksys::SystemTools::GetFilenameName(layer->GetFileName()) == f.name,
        "layer file is " << layer->GetFileName() << ", expected " << f.name);
  CHECK(layer->GetSize() == f.size, f.name << ": size " << layer->GetSize());
  CHECK(layer->GetNumberOfTimePoints() == NT,
        f.name << ": " << layer->GetNumberOfTimePoints() << " time points");

  auto sp = layer->GetImageBase()->GetSpacing();
  auto org = layer->GetImageBase()->GetOrigin();
  for (unsigned int d = 0; d < 3; d++)
    {
    CHECK(Near(sp[d], f.spacing[d], 1e-4), f.name << ": spacing[" << d << "] = " << sp[d]);
    CHECK(Near(org[d], f.origin[d], 1e-3), f.name << ": origin[" << d << "] = " << org[d]);
    }
}

// Labels at P, read per time point directly and through the cursor time point
static void CheckLabels(const std::string &stage, IRISApplication *app,
                        ImageWrapperBase *layer, const SegmentationFile &f)
{
  auto *seg = dynamic_cast<LabelImageWrapper *>(layer);
  CHECK(seg, f.name << " is not a label layer");
  if (!seg)
    return;

  for (unsigned int t = 0; t < NT; t++)
    {
    int stored = seg->GetVoxel(f.probe, t);
    CHECK(stored == f.labels[t], f.name << ": time point " << t << " holds " << stored
                                         << " at P, expected " << f.labels[t]);

    app->SetCursorTimePoint(t);
    int current = seg->GetVoxel(f.probe);
    CHECK(current == f.labels[t], f.name << ": at cursor time point " << t << " reads "
                                          << current << " at P, expected " << f.labels[t]);
    }
}

// The active segmentation is the reference space of every layer
static void CheckReference(const std::string &stage, IRISApplication *app,
                           ImageWrapperBase *expected_active, const SegmentationFile &f)
{
  GenericImageData *id = app->GetCurrentImageData();
  CHECK(id->GetActiveSegmentationLayer() == expected_active,
        "active segmentation is not " << f.name);

  for (LayerIterator it(id); !it.IsAtEnd(); ++it)
    CHECK(it.GetLayer()->GetReferenceSpace() == expected_active->GetImageBase(),
          "reference space of " << it.GetLayer()->GetFileName() << " is not " << f.name);
}

static void CheckWorkspace(const std::string &stage, IRISApplication *app,
                           const SegmentationFile &seg2, bool first_is_active)
{
  GenericImageData *id = app->GetCurrentImageData();
  CHECK(id->GetNumberOfTimePoints() == NT, "workspace has " << id->GetNumberOfTimePoints()
                                                            << " time points");

  auto segs = SegmentationLayers(app);
  CHECK(segs.size() == 2, segs.size() << " segmentation layers, expected 2");
  if (segs.size() != 2)
    return;

  CheckGrid(stage, segs[0], SEG1);
  CheckGrid(stage, segs[1], seg2);
  CheckLabels(stage, app, segs[0], SEG1);
  CheckLabels(stage, app, segs[1], seg2);

  if (first_is_active)
    CheckReference(stage, app, segs[0], SEG1);
  else
    CheckReference(stage, app, segs[1], seg2);
}

int main(int argc, char *argv[])
{
  if (argc < 3)
    {
    std::cerr << "Usage: " << argv[0] << " <TestData dir> <scratch dir>" << std::endl;
    return 1;
    }

  std::string datadir = argv[1];
  std::string tempdir = argv[2];
  itksys::SystemTools::MakeDirectory(tempdir);

  SimpleSystemInfoDelegate sidel(argv[0]);
  sidel.m_Temp = tempdir;
  SystemInterface::SetSystemInfoDelegate(&sidel);

  SimpleColorMapSource cmSource;
  ColorMap::SetColorMapPresetNameSource(&cmSource);

  std::string ws_file = tempdir + "/SegAnchor4DWorkspaceTest.itksnap";
  IRISWarningList wl;

  // SEG2 as saved by this test: same grid, one extra label, new file
  SegmentationFile SEG2_SAVED = SEG2;
  SEG2_SAVED.name = "SegAnchor4DWorkspaceTest_label2.nii.gz";
  SEG2_SAVED.labels[EDIT_TP] = EDIT_LABEL;

  try
    {
    // Build: label1 replaces the blank segmentation, label2 is added and
    // becomes active
    std::string stage = "build";
    auto app = IRISApplication::New();
    app->OpenImage((datadir + "/img4d_11f.nii.gz").c_str(), MAIN_ROLE, wl);
    app->OpenImage((datadir + "/" + SEG1.name).c_str(), LABEL_ROLE, wl);
    app->OpenImage((datadir + "/" + SEG2.name).c_str(), LABEL_ROLE, wl, nullptr, nullptr, true);
    CheckWorkspace(stage, app, SEG2, false);

    // Edit one time point of label2 and save it, as the GUI does
    stage = "edit and save";
    auto segs = SegmentationLayers(app);
    if (segs.size() != 2)
      return 1;
    auto *seg2 = dynamic_cast<LabelImageWrapper *>(segs[1]);
    app->SetCursorTimePoint(EDIT_TP);
    seg2->SetVoxel(SEG2.probe, EDIT_LABEL);

    auto save_delegate = app->CreateSaveDelegateForLayer(seg2, LABEL_ROLE);
    SmartPtr<GuidedNativeImageIO> io = GuidedNativeImageIO::New();
    Registry hints;
    save_delegate->SaveImage(tempdir + "/" + SEG2_SAVED.name, io, hints, wl);
    CHECK(save_delegate->IsSaveSuccessful(), "saving label2 failed");
    CheckWorkspace(stage, app, SEG2_SAVED, false);

    app->SaveProject(ws_file);

    // Reload: the first segmentation is the reference after loading
    stage = "reload";
    auto app2 = IRISApplication::New();
    app2->OpenWorkspace(ws_file, wl);
    CheckWorkspace(stage, app2, SEG2_SAVED, true);

    // Activate each segmentation in turn, with the cursor on P: every layer
    // follows the reference, and the cursor stays on P
    segs = SegmentationLayers(app2);
    if (segs.size() != 2)
      return 1;
    const Vector3i p1(SEG1.probe[0], SEG1.probe[1], SEG1.probe[2]);
    const Vector3i p2(SEG2.probe[0], SEG2.probe[1], SEG2.probe[2]);
    app2->SetCursorPosition(p1);

    stage = "activate label2";
    app2->GetGlobalState()->SetSelectedSegmentationLayerId(segs[1]->GetUniqueId());
    CheckReference(stage, app2, segs[1], SEG2_SAVED);
    CHECK(app2->GetCursorPosition() == p2, "cursor at " << app2->GetCursorPosition()
                                                        << ", expected " << p2);
    CheckLabels(stage, app2, segs[0], SEG1);
    CheckLabels(stage, app2, segs[1], SEG2_SAVED);

    stage = "activate label1";
    app2->GetGlobalState()->SetSelectedSegmentationLayerId(segs[0]->GetUniqueId());
    CheckReference(stage, app2, segs[0], SEG1);
    CHECK(app2->GetCursorPosition() == p1, "cursor at " << app2->GetCursorPosition()
                                                        << ", expected " << p1);
    }
  catch (std::exception &exc)
    {
    std::cerr << "FAIL: exception: " << exc.what() << std::endl;
    return 1;
    }

  for (auto &w : wl)
    std::cout << "Warning: " << w.what() << std::endl;

  if (g_Failures)
    {
    std::cerr << g_Failures << " check(s) failed" << std::endl;
    return 1;
    }

  std::cout << "All checks passed" << std::endl;
  return 0;
}
