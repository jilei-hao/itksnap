// Read the function library
include("Library");

// The Language preference (#260): Preferences › General › Default Behavior
// lists Automatic and every shipped translation, each in its own language.
// The dialog is closed without applying, so the user's preferences are unchanged.

//=== Open the preferences dialog
engine.trigger("actionPreferences");
engine.sleep(500);
var prefDialog = engine.findChild(mainwin, "PreferencesDialog");
if (!prefDialog)
    engine.testFailed("The preferences dialog did not open");

var combo = engine.findChild(prefDialog, "inUILanguage");
if (!combo)
    engine.testFailed("The Language drop-down is missing");

//=== Automatic first, then the four translations
engine.validateValue(engine.getProperty(combo, "count"), 5);

function rowOf(name)
{
    var row = engine.findItemRow(combo, name);
    if (row === undefined || row === null)
        engine.testFailed("The Language drop-down has no item '" + name + "'");
    return row;
}

engine.validateValue(rowOf("Automatic (system language)"), 0);
var names = ["Deutsch", "English", "Español", "简体中文"];
for (var i = 0; i < names.length; i++)
    engine.validateValue(rowOf(names[i]), i + 1);

// A lookup that finds nothing must not look like a match
var missing = engine.findItemRow(combo, "Klingon");
if (missing !== undefined && missing !== null)
    engine.testFailed("Item lookup matched a language that is not in the list");

//=== Close without applying
engine.invoke(prefDialog, "reject");
