// Loading a 3D segmentation into a 4D workspace.
//
// In IRIS mode, a segmentation image with one time point loaded into a 4D
// workspace replaces the current time point of the selected segmentation. Since
// segmentations may have their own grid (seg_anchor), the 3D image has to match
// the selected segmentation. A size mismatch must be reported as a clear
// IRISException before anything changes, not as an assertion deep inside
// ImageWrapper::UpdateTimePoint.
//
//   1. 3D image whose size differs from the selected segmentation: refused with
//      "Mismatched Dimensions", segmentation unchanged.
//   2. 3D image on the selected segmentation's grid: only the current time
//      point changes.
//
// Usage: Seg3DInto4DTest <TestData dir> <scratch dir>

#include "IRISApplication.h"
#include "GenericImageData.h"
#include "ImageIODelegates.h"
#include "LabelImageWrapper.h"
#include "IRISException.h"
#include "UIReporterDelegates.h"
#include "ColorMap.h"
#include <itkImage.h>
#include <itkImageFileWriter.h>
#include <itksys/SystemTools.hxx>
#include <iostream>
#include <string>

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

typedef itk::Image<unsigned short, 3> Label3DType;

static int g_Failures = 0;

#define CHECK(cond, msg)                                    \
  if (!(cond))                                              \
    {                                                       \
    std::cerr << "FAIL: " << msg << std::endl;              \
    ++g_Failures;                                           \
    }

// Write a 3D label image on the given grid with 'label' at one voxel
static std::string WriteLabelImage(const std::string &fn, itk::ImageBase<3> *grid,
                                   const itk::Size<3> &size, const itk::Index<3> &voxel,
                                   unsigned short label)
{
  auto img = Label3DType::New();
  img->SetRegions(itk::ImageRegion<3>(size));
  img->SetSpacing(grid->GetSpacing());
  img->SetOrigin(grid->GetOrigin());
  img->SetDirection(grid->GetDirection());
  img->Allocate(true);
  img->SetPixel(voxel, label);

  auto writer = itk::ImageFileWriter<Label3DType>::New();
  writer->SetInput(img);
  writer->SetFileName(fn);
  writer->Update();
  return fn;
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

  // A voxel that is unlabeled at every time point of seg4d_11f_label1, and the
  // time point the 3D images are loaded into
  const itk::Index<3> probe = {{5, 5, 5}};
  const unsigned int tp = 6, nt = 11;

  try
    {
    IRISWarningList wl;
    auto app = IRISApplication::New();
    app->OpenImage((datadir + "/img4d_11f.nii.gz").c_str(), MAIN_ROLE, wl);
    app->OpenImage((datadir + "/seg4d_11f_label1.nii.gz").c_str(), LABEL_ROLE, wl);
    app->SetCursorTimePoint(tp);

    LabelImageWrapper *seg = app->GetSelectedSegmentationLayer();
    CHECK(seg && seg->GetNumberOfTimePoints() == nt, "4D segmentation not loaded");
    if (!seg)
      return 1;
    for (unsigned int t = 0; t < nt; t++)
      CHECK(seg->GetVoxel(probe, t) == 0, "probe voxel is labeled at time point " << t);

    // Case 1: size differs from the selected segmentation
    itk::Size<3> small = {{20, 20, 20}};
    std::string fn1 = WriteLabelImage(tempdir + "/Seg3DInto4DTest_wrongsize.nii.gz",
                                      seg->GetImageBase(), small, probe, 1);
    try
      {
      app->OpenImage(fn1.c_str(), LABEL_ROLE, wl);
      CHECK(false, "3D segmentation of a different size was accepted");
      }
    catch (IRISException &exc)
      {
      std::string what = exc.what();
      std::cout << "Refused as expected: " << what << std::endl;
      CHECK(what.find("Mismatched Dimensions") != std::string::npos,
            "unexpected message: " << what);
      }
    catch (std::exception &exc)
      {
      CHECK(false, "refused with a low-level exception instead of IRISException: "
                     << exc.what());
      }

    seg = app->GetSelectedSegmentationLayer();
    for (unsigned int t = 0; t < nt; t++)
      CHECK(seg->GetVoxel(probe, t) == 0,
            "refused load changed time point " << t);

    // Case 2: same grid as the selected segmentation
    itk::Size<3> size = seg->GetImageBase()->GetLargestPossibleRegion().GetSize();
    std::string fn2 = WriteLabelImage(tempdir + "/Seg3DInto4DTest_samegrid.nii.gz",
                                      seg->GetImageBase(), size, probe, 1);
    app->OpenImage(fn2.c_str(), LABEL_ROLE, wl);

    seg = app->GetSelectedSegmentationLayer();
    for (unsigned int t = 0; t < nt; t++)
      {
      unsigned int expected = (t == tp) ? 1 : 0;
      CHECK(seg->GetVoxel(probe, t) == expected,
            "after same-grid load, time point " << t << " reads "
            << seg->GetVoxel(probe, t) << ", expected " << expected);
      }
    }
  catch (std::exception &exc)
    {
    std::cerr << "FAIL: exception: " << exc.what() << std::endl;
    return 1;
    }

  if (g_Failures)
    {
    std::cerr << g_Failures << " check(s) failed" << std::endl;
    return 1;
    }

  std::cout << "All checks passed" << std::endl;
  return 0;
}
