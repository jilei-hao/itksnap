#include "QtUILanguage.h"
#include <QDir>
#include <QMap>

UILanguageSource
ChooseUILanguageSource(const QString     &command_line,
                       const QString     &preference,
                       const QStringList &available)
{
  if (!command_line.isEmpty())
    return UI_LANGUAGE_FROM_COMMAND_LINE;

  if (!preference.isEmpty() && available.contains(preference))
    return UI_LANGUAGE_FROM_PREFERENCES;

  return UI_LANGUAGE_FROM_SYSTEM;
}

QStringList
GetAvailableUILanguages(const QString &dir)
{
  // Translations are named itksnap_<code>.qm
  const QString prefix = QStringLiteral("itksnap_"), suffix = QStringLiteral(".qm");
  QStringList codes;
  for (const QString &file : QDir(dir).entryList({ prefix + "*" + suffix }, QDir::Files))
    codes << file.mid(prefix.size(), file.size() - prefix.size() - suffix.size());
  codes.sort();
  return codes;
}

QString
GetUILanguageNativeName(const QString &code)
{
  // Qt names the regional variant that a bare language code maps to (e.g.,
  // "American English", "Español de España"), so the shipped translations are
  // named here. Qt's name is the fallback for a translation added later.
  static const QMap<QString, QString> names = {
    { "de", QStringLiteral("Deutsch") },
    { "en", QStringLiteral("English") },
    { "es", QStringLiteral("Español") },
    { "zh_CN", QStringLiteral("简体中文") }
  };
  if (names.contains(code))
    return names.value(code);

  QString name = QLocale(code).nativeLanguageName();

  // Some languages write their own name in lower case (e.g., "français")
  if (!name.isEmpty())
    name[0] = name[0].toUpper();

  return name.isEmpty() ? code : name;
}
