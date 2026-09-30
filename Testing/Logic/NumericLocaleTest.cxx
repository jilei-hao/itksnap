// NRRD headers under a decimal-comma locale (#256).
//
// On Windows, ITK-SNAP takes its C locale from the user's settings, which may
// write numbers with a decimal comma. NrrdIO parses header values with sscanf,
// which follows LC_NUMERIC, so a spacing of 0.4 was read as 0 and the image did
// not load. SystemInterface::UseCNumericLocale(), called at startup, must undo
// this.
//
//   1. Switch to a decimal-comma locale. If none is installed, skip.
//   2. Check that the bug shows here: without the fix, the NRRD from #256 does
//      not load with a spacing of 0.4. If it does, this platform cannot show
//      the bug, so skip.
//   3. After UseCNumericLocale(), it loads with a spacing of 0.4.
//
// On Linux and macOS "POSIX" works as well as "C", so only a Windows run can
// catch a return to "POSIX". Everywhere, the test fails if the call is removed
// or stops working.
//
// Usage: NumericLocaleTest <scratch dir>
// Exits with 77 (reported by ctest as skipped) when step 1 or 2 cannot run.

#include "GuidedNativeImageIO.h"
#include "Registry.h"
#include "SystemInterface.h"
#include <itksys/SystemTools.hxx>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static const int SKIPPED = 77;

// The image from #256: 2x2x2 voxels of 0.4 mm, with an ASCII body
static void WriteNrrd(const std::string &fn)
{
  std::ofstream f(fn.c_str(), std::ios::binary);
  f << "NRRD0004\n"
       "type: uint8\n"
       "dimension: 3\n"
       "space: left-posterior-superior\n"
       "sizes: 2 2 2\n"
       "space directions: (0.4,0,0) (0,0.4,0) (0,0,0.4)\n"
       "kinds: domain domain domain\n"
       "encoding: ascii\n"
       "space origin: (0,0,0)\n"
       "\n"
       "0 1 2 3 4 5 6 7\n";
}

// Load the image and return its x spacing, or -1 if it does not load
static double LoadSpacing(const std::string &fn, std::string &error)
{
  try
    {
    Registry hints;
    GuidedNativeImageIO::Pointer io = GuidedNativeImageIO::New();
    io->ReadNativeImage(fn.c_str(), hints);
    return io->GetNativeImage()->GetSpacing()[0];
    }
  catch (std::exception &exc)
    {
    error = exc.what();
    return -1;
    }
}

static bool DecimalComma()
{
  const char *dp = std::localeconv()->decimal_point;
  return dp && dp[0] == ',';
}

int main(int argc, char *argv[])
{
  if (argc < 2)
    {
    std::cerr << "Usage: " << argv[0] << " <scratch dir>" << std::endl;
    return 1;
    }

  std::string dir = argv[1];
  itksys::SystemTools::MakeDirectory(dir);
  std::string fn = dir + "/NumericLocaleTest_0.4mm.nrrd";
  WriteNrrd(fn);

  // 1. A decimal-comma locale, named the way each platform names it
  const char *candidates[] = { "de_DE.UTF-8", "de_DE.utf8", "de_DE", "de-DE.UTF8", "de-DE",
                               "German_Germany.1252", "fr_FR.UTF-8", "fr_FR.utf8", "fr-FR",
                               "French_France.1252" };
  std::string used;
  for (const char *name : candidates)
    if (std::setlocale(LC_ALL, name) && DecimalComma())
      {
      used = name;
      break;
      }
  if (used.empty())
    {
    std::cout << "SKIPPED: no decimal-comma locale is installed" << std::endl;
    return SKIPPED;
    }
  std::cout << "Decimal-comma locale: " << used << std::endl;

  // 2. The bug shows without the fix
  std::string error;
  double before = LoadSpacing(fn, error);
  std::cout << "Without the fix: "
            << (before < 0 ? "does not load (" + error.substr(0, 120) + ")"
                           : "spacing " + std::to_string(before))
            << std::endl;
  if (std::fabs(before - 0.4) < 1e-6)
    {
    std::cout << "SKIPPED: the bug does not show on this platform" << std::endl;
    return SKIPPED;
    }

  // 3. The fix
  if (!SystemInterface::UseCNumericLocale())
    {
    std::cerr << "FAIL: UseCNumericLocale() was refused by the C library" << std::endl;
    return 1;
    }
  if (DecimalComma())
    {
    std::cerr << "FAIL: the decimal separator is still a comma after UseCNumericLocale()"
              << std::endl;
    return 1;
    }
  double after = LoadSpacing(fn, error);
  if (std::fabs(after - 0.4) > 1e-6)
    {
    std::cerr << "FAIL: after UseCNumericLocale() the spacing is " << after
              << (after < 0 ? " (does not load: " + error + ")" : "") << ", expected 0.4"
              << std::endl;
    return 1;
    }

  std::cout << "With the fix: spacing " << after << std::endl;
  std::cout << "NumericLocaleTest passed" << std::endl;
  return 0;
}
