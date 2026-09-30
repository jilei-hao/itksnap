// Guards the contract that keeps scripted Qt access on the GUI thread.
//
// Test scripts run on TestWorker's thread, so anything a script can touch
// directly it touches off the GUI thread -- which Qt does not support. The
// harness therefore hands scripts a TestObjectProxy (see SNAPTestQt.h) instead
// of the widget itself, and every proxy member hops to the GUI thread first.
//
// This test fails if that is undone: returning raw Qt objects removes the proxy
// API checked below, and dropping the marshalling trips the GUI-thread
// assertion inside the proxy, which aborts the process.
//=== Checking that findChild returns a proxy
var btn = engine.findChild(mainwin, "btnLoadMain");
if(btn == null)
  engine.testFailed("btnLoadMain not found -- cannot check the harness contract");
if(typeof btn.get !== "function")
  engine.testFailed("findChild returned a raw Qt object, not a TestObjectProxy");
//=== Checking that the widget's own Qt properties are not exposed
// 'enabled' is a QWidget property and deliberately absent from the proxy, so a
// script cannot reach the widget without going through a marshalled member.
if(typeof btn.enabled !== "undefined")
  engine.testFailed("proxy exposes the widget's Qt properties -- scripted access would run off the GUI thread");
//=== Checking that the globals are wrapped
if(typeof mainwin.get !== "function")
  engine.testFailed("mainwin is not wrapped");
//=== Checking that findWidget returns a proxy
var w = engine.findWidget("btnLoadMain");
if(w == null)
  engine.testFailed("findWidget did not find btnLoadMain");
if(typeof w.get !== "function")
  engine.testFailed("findWidget returned a raw Qt object, not a TestObjectProxy");
//=== Checking that a marshalled read reaches the widget
// Reads the wrapped widget's objectName, not the proxy's own (which is empty).
engine.validateValue(btn.get("objectName"), "btnLoadMain");
engine.validateValue(w.get("objectName"), "btnLoadMain");
//=== Checking that a marshalled action reaches the widget
var editor = engine.findChild(mainwin, "actionLabel_Editor");
if(editor == null)
  engine.testFailed("actionLabel_Editor not found");
engine.validateValue(editor.get("objectName"), "actionLabel_Editor");
