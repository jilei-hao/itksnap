// The Language preference (#260).
//
//   1. Priority: the --lang option, then the preference, then the system. A
//      preference naming a translation that is not shipped is ignored.
//   2. The list of translations is read from the translation file names.
//   3. Each language is shown by its own name (e.g., "Deutsch").
//   4. The preference is saved and read back under the same key, and is
//      empty (automatic) by default.
//
// Usage: TestUILanguage

#include "QtUILanguage.h"
#include "DefaultBehaviorSettings.h"
#include "ColorMap.h"
#include "Registry.h"
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

// DefaultBehaviorSettings names its default color map preset
class SimpleColorMapSource : public AbstractColorMapPresetNameSource
{
public:
  std::string GetPresetName(ColorMap::SystemPreset p, bool) override
    { return "Preset " + std::to_string(static_cast<int>(p)); }
};

static int g_Failures = 0;

#define CHECK(cond, msg)                                    \
  if (!(cond))                                              \
    {                                                       \
    std::cerr << "FAIL: " << msg << std::endl;              \
    ++g_Failures;                                           \
    }

static const char *SourceName(UILanguageSource s)
{
  switch (s)
    {
    case UI_LANGUAGE_FROM_COMMAND_LINE: return "command line";
    case UI_LANGUAGE_FROM_PREFERENCES:  return "preferences";
    case UI_LANGUAGE_FROM_SYSTEM:       return "system";
    }
  return "?";
}

static void CheckSource(const QString &cmd, const QString &pref, const QStringList &avail,
                        UILanguageSource expected)
{
  UILanguageSource s = ChooseUILanguageSource(cmd, pref, avail);
  CHECK(s == expected, "--lang '" << cmd.toStdString() << "', preference '" << pref.toStdString()
                                  << "': chose " << SourceName(s) << ", expected "
                                  << SourceName(expected));
}

int main()
{
  // 1. Priority
  QStringList avail = { "de", "en", "es", "zh_CN" };
  CheckSource("es", "de", avail, UI_LANGUAGE_FROM_COMMAND_LINE);
  CheckSource("es", "", avail, UI_LANGUAGE_FROM_COMMAND_LINE);
  CheckSource("", "de", avail, UI_LANGUAGE_FROM_PREFERENCES);
  CheckSource("", "zh_CN", avail, UI_LANGUAGE_FROM_PREFERENCES);
  CheckSource("", "", avail, UI_LANGUAGE_FROM_SYSTEM);
  CheckSource("", "fr", avail, UI_LANGUAGE_FROM_SYSTEM);

  // 2. Available translations, from the file names
  QTemporaryDir dir;
  CHECK(dir.isValid(), "cannot create a temporary directory");
  for (const char *f : { "itksnap_de.qm", "itksnap_zh_CN.qm", "qt_de.qm", "itksnap_es.ts" })
    {
    QFile file(dir.filePath(f));
    CHECK(file.open(QIODevice::WriteOnly), "cannot create " << f);
    }
  QStringList found = GetAvailableUILanguages(dir.path());
  CHECK(found == QStringList({ "de", "zh_CN" }),
        "available translations: " << found.join(",").toStdString() << ", expected de,zh_CN");

  // 3. Native names
  const std::pair<const char *, const char *> names[] = {
    { "de", "Deutsch" }, { "en", "English" }, { "es", "Español" }, { "zh_CN", "简体中文" }
  };
  CHECK(GetUILanguageNativeName("fr") == QString::fromUtf8("Français"),
        "fr is shown as '" << GetUILanguageNativeName("fr").toStdString() << "', expected 'Français'");
  for (const auto &n : names)
    {
    QString name = GetUILanguageNativeName(n.first);
    CHECK(name == QString::fromUtf8(n.second),
          n.first << " is shown as '" << name.toStdString() << "', expected '" << n.second << "'");
    }

  // 4. The preference is saved and read back, and is automatic by default
  SimpleColorMapSource cmSource;
  ColorMap::SetColorMapPresetNameSource(&cmSource);
  auto dbs = DefaultBehaviorSettings::New();
  CHECK(dbs->GetUILanguage().empty(), "the default language is '" << dbs->GetUILanguage()
                                                                    << "', expected automatic");
  dbs->SetUILanguage("zh_CN");
  Registry reg;
  dbs->WriteToRegistry(reg);
  CHECK(reg["UILanguage"][""] == std::string("zh_CN"), "the preference is not saved as UILanguage");
  auto dbs2 = DefaultBehaviorSettings::New();
  dbs2->ReadFromRegistry(reg);
  CHECK(dbs2->GetUILanguage() == "zh_CN",
        "the preference reads back as '" << dbs2->GetUILanguage() << "', expected zh_CN");

  if (g_Failures)
    {
    std::cerr << g_Failures << " check(s) failed" << std::endl;
    return 1;
    }
  std::cout << "TestUILanguage passed" << std::endl;
  return 0;
}
