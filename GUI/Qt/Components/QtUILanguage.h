#ifndef QTUILANGUAGE_H
#define QTUILANGUAGE_H

#include <QLocale>
#include <QString>
#include <QStringList>

/**
 * Helpers for choosing the language of the user interface. The language comes
 * from, in order of priority, the --lang command-line option, the Language
 * preference, and the system.
 */

/** Where the language of the user interface comes from */
enum UILanguageSource
{
  UI_LANGUAGE_FROM_COMMAND_LINE,
  UI_LANGUAGE_FROM_PREFERENCES,
  UI_LANGUAGE_FROM_SYSTEM
};

/**
 * Decide where the language comes from. The command line wins when it names a
 * language. The preference is used when it names one of the available
 * translations; a preference naming a translation that is no longer shipped is
 * ignored, so the system language is used instead.
 */
UILanguageSource
ChooseUILanguageSource(const QString     &command_line,
                       const QString     &preference,
                       const QStringList &available);

/**
 * The codes of the translations compiled into the application (e.g., "de",
 * "en", "zh_CN"), read from the ":/i18n" resource directory, sorted.
 */
QStringList
GetAvailableUILanguages(const QString &dir = QStringLiteral(":/i18n"));

/**
 * The name of a language written in that language (e.g., "Deutsch" for "de"),
 * so that users can find their own language in a list
 */
QString
GetUILanguageNativeName(const QString &code);

#endif // QTUILANGUAGE_H
