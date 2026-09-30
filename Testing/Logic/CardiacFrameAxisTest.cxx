// Cardiac 4D frame axis: load, save and load again.
//
// Each time point of a 4D cardiac CT belongs to a phase of the heartbeat
// (%R-R), and each frame of a 4D echo to a time in ms. ITK-SNAP keeps this
// frame axis from loading to saving. This test fails if a format drops the
// axis, or pairs its values with the wrong frames:
//
//   1. CT. A 10-phase image covering 0-95 %R-R (step 10.56, so not "exact") is
//      written with plain ITK to a .nrrd carrying the cardiac keys and a few
//      patient fields. After loading, every time point must report its %R-R.
//      The image is then saved as .seq.nrrd, as .nii.gz (with its .json
//      sidecar) and as .nrrd, and each file is loaded again: the values, the
//      exact flag and the frame each value belongs to must survive. The .nrrd
//      export must drop the patient name and ID, and top-code an age of 95.
//   2. Echo. The Philips Cartesian test file loads with one frame time per
//      frame; saving it as .seq.nrrd and .nii.gz and loading again must keep
//      them.
//
// Usage: CardiacFrameAxisTest <TestData dir> <scratch dir>

#include "IRISApplication.h"
#include "IRISImageData.h"
#include "GuidedNativeImageIO.h"
#include "ImageIODelegates.h"
#include "ImageWrapperBase.h"
#include "TimePointProperties.h"
#include "IRISException.h"
#include "Registry.h"
#include "UIReporterDelegates.h"
#include "ColorMap.h"
#include <itkImage.h>
#include <itkVectorImage.h>
#include <itkImageFileWriter.h>
#include <itkImageRegionIteratorWithIndex.h>
#include <itkMetaDataObject.h>
#include <itksys/SystemTools.hxx>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
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

typedef itk::Image<short, 4> CT4DType;

static int g_Failures = 0;

#define CHECK(cond, msg)                                    \
  if (!(cond))                                              \
    {                                                       \
    std::cerr << "FAIL: " << msg << std::endl;              \
    ++g_Failures;                                           \
    }

// The synthetic CT: 10 phases over 0-95 %R-R, as ITK-SNAP derives them from a
// "0 - 95 %" series description
static const unsigned int NT = 10;
static const itk::Size<4> CTSize = {{4, 3, 2, NT}};

static std::vector<double> ExpectedRR()
{
  std::vector<double> rr(NT);
  for (unsigned int t = 0; t < NT; t++)
    rr[t] = t * 95.0 / (NT - 1);
  return rr;
}

// A different value for every voxel of every frame, so a frame that moves is seen
static short CTVoxel(const itk::Index<4> &i)
{
  return static_cast<short>(1000 + 100 * i[3] + i[0] + 4 * i[1] + 12 * i[2]);
}

static std::string Join(const std::vector<double> &v)
{
  std::ostringstream ss;
  ss.precision(10);
  for (size_t i = 0; i < v.size(); i++)
    ss << (i ? " " : "") << v[i];
  return ss.str();
}

static std::string ReadFileText(const std::string &fn)
{
  std::ifstream f(fn.c_str(), std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Remove an output, and the sidecar of a NIfTI output, so a file left by an
// earlier run cannot make this run pass
static void RemoveOutput(const std::string &fn)
{
  itksys::SystemTools::RemoveFile(fn);
  const std::string nii = ".nii.gz";
  if (itksys::SystemTools::StringEndsWith(fn, nii.c_str()))
    itksys::SystemTools::RemoveFile(fn.substr(0, fn.size() - nii.size()) + ".json");
}

// Write the CT source file with plain ITK, independent of the code under test
static void WriteCTSource(const std::string &fn)
{
  std::vector<double> rr = ExpectedRR();

  auto img = CT4DType::New();
  img->SetRegions(itk::ImageRegion<4>(CTSize));
  CT4DType::SpacingType spacing;
  spacing[0] = 0.7; spacing[1] = 0.8; spacing[2] = 1.5; spacing[3] = (rr[1] - rr[0]) / 100.0;
  img->SetSpacing(spacing);
  CT4DType::PointType origin;
  origin[0] = 10; origin[1] = -5; origin[2] = 3; origin[3] = 0;
  img->SetOrigin(origin);
  img->Allocate();
  itk::ImageRegionIteratorWithIndex<CT4DType> it(img, img->GetLargestPossibleRegion());
  for (; !it.IsAtEnd(); ++it)
    it.Set(CTVoxel(it.GetIndex()));

  // The keys GuidedNativeImageIO sets when it reads a 4D cardiac CT series
  itk::MetaDataDictionary &d = img->GetMetaDataDictionary();
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_FrameAxis_Values", Join(rr));
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_FrameAxis_Unit", "%");
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_FrameAxis_Label", "%R-R");
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_Cardiac_RRPercent", Join(rr));
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_Cardiac_RRPercentSource", "series_description");
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_Cardiac_RRPercentExact", "0");
  itk::EncapsulateMetaData<std::string>(d, "ITKSNAP_Cardiac_NumberOfPhases", std::to_string(NT));

  // DICOM fields: two that identify the patient, two that are kept on export
  itk::EncapsulateMetaData<std::string>(d, "0008|0060", "CT");
  itk::EncapsulateMetaData<std::string>(d, "0010|0010", "Doe^Jane");
  itk::EncapsulateMetaData<std::string>(d, "0010|0020", "MRN-4455667");
  itk::EncapsulateMetaData<std::string>(d, "0010|1010", "095Y");
  itk::EncapsulateMetaData<std::string>(d, "0018|0050", "1.25");

  auto writer = itk::ImageFileWriter<CT4DType>::New();
  writer->SetInput(img);
  writer->SetFileName(fn);
  writer->Update();
}

// Read one voxel of the main image at a time point. The main image is a
// VectorImage of its native component type, short here.
static bool GetMainVoxel(IRISApplication *app, const itk::Index<3> &idx,
                         unsigned int tp, short &value)
{
  app->SetCursorTimePoint(tp);
  ImageWrapperBase *main = app->GetIRISImageData()->GetMain();
  if (auto *vi = dynamic_cast<itk::VectorImage<short, 3> *>(main->GetImageBase()))
    {
    value = vi->GetPixel(idx)[0];
    return true;
    }
  if (auto *si = dynamic_cast<itk::Image<short, 3> *>(main->GetImageBase()))
    {
    value = si->GetPixel(idx);
    return true;
    }
  return false;
}

// Load a CT file as the main image and check the frame axis and the frames
static SmartPtr<IRISApplication> LoadAndCheckCT(const std::string &fn, const std::string &what)
{
  auto app = IRISApplication::New();
  IRISWarningList wl;
  app->OpenImage(fn.c_str(), MAIN_ROLE, wl);

  unsigned int nt = app->GetNumberOfTimePoints();
  CHECK(nt == NT, what << ": " << nt << " time points, expected " << NT);
  if (nt != NT)
    return app;

  std::vector<double> rr = ExpectedRR();
  TimePointProperties *tpp = app->GetIRISImageData()->GetTimePointProperties();
  for (unsigned int t = 0; t < NT; t++)
    {
    // TimePointProperties is indexed from 1
    TimePointProperty *p = tpp->GetProperty(t + 1);
    CHECK(p && p->HasRRPercent(), what << ": time point " << t + 1 << " has no %R-R");
    if (!p || !p->HasRRPercent())
      continue;
    CHECK(std::fabs(p->GetRRPercent() - rr[t]) < 1e-6,
          what << ": time point " << t + 1 << " %R-R " << p->GetRRPercent()
               << ", expected " << rr[t]);
    CHECK(!p->GetRRPercentExact(),
          what << ": time point " << t + 1 << " marked exact, but 95/9 is not an integer step");
    CHECK(p->HasFrameValue() && std::fabs(p->GetFrameValue() - rr[t]) < 1e-6
            && p->GetFrameUnit() == "%",
          what << ": time point " << t + 1 << " frame value " << p->GetFrameValue()
               << " '" << p->GetFrameUnit() << "', expected " << rr[t] << " '%'");
    }

  // The %R-R values must still label the frames they came with
  const itk::Index<3> probes[] = {{{0, 0, 0}}, {{3, 2, 1}}, {{1, 2, 0}}};
  for (unsigned int t = 0; t < NT; t++)
    for (const auto &idx : probes)
      {
      short v = 0;
      itk::Index<4> i4 = {{idx[0], idx[1], idx[2], (itk::IndexValueType) t}};
      bool ok = GetMainVoxel(app, idx, t, v);
      CHECK(ok, what << ": main image is not a short image");
      if (!ok)
        return app;
      CHECK(v == CTVoxel(i4), what << ": voxel " << idx << " at time point " << t + 1
                                   << " is " << v << ", expected " << CTVoxel(i4));
      }

  return app;
}

// Save the main image the way the GUI does, then check it loads with its axis
static void SaveAndReloadCT(IRISApplication *app, const std::string &fn, const std::string &what)
{
  RemoveOutput(fn);
  Registry hints;
  app->GetIRISImageData()->GetMain()->WriteToFile(fn.c_str(), hints);
  CHECK(itksys::SystemTools::FileExists(fn), what << ": " << fn << " was not written");
  LoadAndCheckCT(fn, what);
}

// Load the echo test file, and return its frame times in ms
static std::vector<double> LoadEchoFrameTimes(IRISApplication *app, const std::string &fn,
                                              Registry *hints, const std::string &what)
{
  IRISWarningList wl;
  app->OpenImage(fn.c_str(), MAIN_ROLE, wl, nullptr, hints);

  std::vector<double> times;
  unsigned int nt = app->GetNumberOfTimePoints();
  TimePointProperties *tpp = app->GetIRISImageData()->GetTimePointProperties();
  for (unsigned int t = 1; t <= nt; t++)
    {
    TimePointProperty *p = tpp->GetProperty(t);
    CHECK(p && p->HasFrameValue() && p->GetFrameUnit() == "ms",
          what << ": frame " << t << " has no time in ms");
    CHECK(p && !p->HasRRPercent(), what << ": echo frame " << t << " has a %R-R");
    times.push_back(p && p->HasFrameValue() ? p->GetFrameValue() : NAN);
    }
  return times;
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

  try
    {
    // 1. CT
    std::string src = tempdir + "/CardiacFrameAxis_source.nrrd";
    WriteCTSource(src);
    SmartPtr<IRISApplication> app = LoadAndCheckCT(src, "source .nrrd");

    std::string seq = tempdir + "/CardiacFrameAxis_ct.seq.nrrd";
    SaveAndReloadCT(app, seq, ".seq.nrrd");
    std::string header = ReadFileText(seq);
    header = header.substr(0, header.find("\n\n"));
    CHECK(header.find("labels: \"%R-R\"") != std::string::npos,
          ".seq.nrrd: the frame axis is not labeled %R-R");
    CHECK(header.find("axis 0 index values:=" + Join(ExpectedRR())) != std::string::npos,
          ".seq.nrrd: the frame axis index values are not the %R-R");

    std::string nii = tempdir + "/CardiacFrameAxis_ct.nii.gz";
    SaveAndReloadCT(app, nii, ".nii.gz");
    CHECK(itksys::SystemTools::FileExists(tempdir + "/CardiacFrameAxis_ct.json"),
          ".nii.gz: no .json sidecar was written");

    std::string nrrd = tempdir + "/CardiacFrameAxis_ct.nrrd";
    SaveAndReloadCT(app, nrrd, ".nrrd");
    std::string text = ReadFileText(nrrd);
    CHECK(text.find("Doe^Jane") == std::string::npos, ".nrrd export kept the patient name");
    CHECK(text.find("MRN-4455667") == std::string::npos, ".nrrd export kept the patient ID");
    CHECK(text.find("095Y") == std::string::npos, ".nrrd export kept an age over 89");
    CHECK(text.find("090Y") != std::string::npos, ".nrrd export dropped the top-coded age");

    // 2. Echo
    Registry echoHints;
    GuidedNativeImageIO::SetFileFormat(echoHints, GuidedNativeImageIO::FORMAT_ECHO_CARTESIAN_DICOM);
    auto echo = IRISApplication::New();
    std::vector<double> times = LoadEchoFrameTimes(
          echo, datadir + "/echo_cartesian_dummy.dcm", &echoHints, "echo DICOM");
    CHECK(times.size() > 1, "echo DICOM: " << times.size() << " frames, expected several");
    if (times.size() > 1)
      CHECK(times[0] == 0.0 && times[1] > 0.0,
            "echo DICOM: frame times start " << times[0] << ", " << times[1]);

    for (std::string ext : {".seq.nrrd", ".nii.gz"})
      {
      std::string fn = tempdir + "/CardiacFrameAxis_echo" + ext;
      RemoveOutput(fn);
      Registry hints;
      echo->GetIRISImageData()->GetMain()->WriteToFile(fn.c_str(), hints);

      auto reload = IRISApplication::New();
      std::vector<double> t2 = LoadEchoFrameTimes(reload, fn, nullptr, "echo " + ext);
      CHECK(t2.size() == times.size(),
            "echo " << ext << ": " << t2.size() << " frames, expected " << times.size());
      for (size_t i = 0; i < t2.size() && i < times.size(); i++)
        CHECK(std::fabs(t2[i] - times[i]) < 1e-6,
              "echo " << ext << ": frame " << i + 1 << " at " << t2[i]
                      << " ms, expected " << times[i]);
      }
    }
  catch (std::exception &exc)
    {
    std::cerr << "FAIL: unexpected exception: " << exc.what() << std::endl;
    return 1;
    }

  if (g_Failures)
    {
    std::cerr << g_Failures << " check(s) failed" << std::endl;
    return 1;
    }

  std::cout << "CardiacFrameAxisTest passed" << std::endl;
  return 0;
}
