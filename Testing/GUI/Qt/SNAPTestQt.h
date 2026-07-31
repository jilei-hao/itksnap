#ifndef SNAPTESTQT_H
#define SNAPTESTQT_H

#include <SNAPCommon.h>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QHash>
#include <QModelIndex>

class MainImageWindow;
class GlobalUIModel;
class QJSEngine;
class QQmlEngine;
class QTimer;

#if QT_VERSION >= 0x050000
  class QJSEngine;
#else
  class QScriptEngine;
  #define QJSEngine QScriptEngine
#endif



class TestWorker : public QThread
{
  Q_OBJECT

public:
  TestWorker(QObject *parent, QString script, QJSEngine *engine, double accel_factor);

  void run();
  static void sleep_ms(unsigned int msec);

public slots:

  void wait(unsigned int msec);
  void source(QString script_url);

protected:
  QString m_MainScript;
  QJSEngine *m_Engine;

  // Acceleration factor
  double m_Acceleration;

  // Returns false if the script (or one of its includes) could not be read.
  // Callers must not report success when this returns false -- application_exit()
  // only queues a quit, so execution continues past a failed read.
  bool readScript(QString script_url, QString &script);

};

/**
 * The only view a test script ever gets of an object in the running application.
 *
 * Test scripts execute on TestWorker's thread, not the GUI thread. They have to:
 * a script must stay responsive while the GUI thread sits inside a modal
 * QDialog::exec() -- which is how every test that opens an image drives the
 * ImageIOWizard. But QJSEngine invokes a QObject's slots and property accessors
 * *directly on the calling thread*, so handing a script a real widget puts every
 * scripted click() and every `w.text = ...` on the worker thread, which Qt does
 * not support. That is what raised NSInternalInconsistencyException on macOS and
 * the long-standing "Timers cannot be started from another thread" warning.
 *
 * So scripts never see a widget. findChild()/findWidget() return one of these,
 * and every member below hops to the widget's own thread before touching it.
 * Reads block until the GUI thread answers; actions and property writes are
 * posted, because a scripted click can open a modal dialog and waiting for one
 * would deadlock against the script that is supposed to dismiss it. Posted calls
 * are delivered in order, so a write followed by a click still lands in that
 * order.
 *
 * The wrapping is what keeps this fixed: anything not declared here is simply
 * unreachable from a script. A script that calls an unwrapped method of the
 * underlying widget gets a TypeError and fails the test, instead of silently
 * reintroducing an off-thread call.
 */
class TestObjectProxy : public QObject
{
  Q_OBJECT

  // Properties the test scripts read or assign. These are deliberately a
  // curated list rather than a passthrough -- adding one is a decision to
  // support it, and each goes through the same marshalling as everything else.
  Q_PROPERTY(QVariant text         READ get_text         WRITE set_text)
  Q_PROPERTY(QVariant value        READ get_value        WRITE set_value)
  Q_PROPERTY(QVariant visible      READ get_visible      WRITE set_visible)
  Q_PROPERTY(QVariant maximum      READ get_maximum      WRITE set_maximum)
  Q_PROPERTY(QVariant currentText  READ get_currentText  WRITE set_currentText)
  Q_PROPERTY(QVariant currentIndex READ get_currentIndex WRITE set_currentIndex)

public:

  TestObjectProxy(QObject *target, QObject *parent);

  // The wrapped object. Only safe to dereference on the GUI thread.
  QObject *target() const { return m_Target.data(); }

  // Property accessors backing the Q_PROPERTYs above. Not slots, so they do not
  // become part of the script-visible method surface.
  QVariant get_text() const                  { return getProperty("text"); }
  void     set_text(const QVariant &v)       { setProperty_("text", v); }
  QVariant get_value() const                 { return getProperty("value"); }
  void     set_value(const QVariant &v)      { setProperty_("value", v); }
  QVariant get_visible() const               { return getProperty("visible"); }
  void     set_visible(const QVariant &v)    { setProperty_("visible", v); }
  QVariant get_maximum() const               { return getProperty("maximum"); }
  void     set_maximum(const QVariant &v)    { setProperty_("maximum", v); }
  QVariant get_currentText() const           { return getProperty("currentText"); }
  void     set_currentText(const QVariant &v){ setProperty_("currentText", v); }
  QVariant get_currentIndex() const          { return getProperty("currentIndex"); }
  void     set_currentIndex(const QVariant &v){ setProperty_("currentIndex", v); }

public slots:

  // Widget actions used by the test scripts. All are posted to the GUI thread.
  void click();
  void toggle();
  void trigger();
  void setCurrentIndex(int index);
  void setCurrentWidget(TestObjectProxy *widget);
  void setSelected(bool value);

  // Generic escape hatches, marshalled the same way, for slots and properties
  // that do not yet have a named member above.
  void invoke(QString slot);
  QVariant get(QString property_name);
  void set(QString property_name, QVariant value);

protected:

  // Read a property on the GUI thread and wait for the answer.
  QVariant getProperty(const char *name) const;

  // Post a property write to the GUI thread.
  void setProperty_(const char *name, const QVariant &value);

  // Post a no-argument slot invocation to the GUI thread.
  void invokeSlot(const char *slot);

  // Wait until the GUI thread has worked through everything queued so far.
  //
  // Actions are posted rather than waited on, because a scripted click can open
  // a modal dialog and the script is what dismisses it. But a script still
  // needs each step to have landed before it takes the next one: ITK-SNAP
  // delivers model updates to widgets as *queued* events, so a widget that the
  // previous step re-enables or repopulates is only up to date once those have
  // been drained. Without this, a script that clicks twice in a row has its
  // second click hit a widget still in the pre-first-click state.
  //
  // Posting an empty call and waiting for it does that: queued calls are
  // delivered in order, so anything the previous step queued runs first. It
  // does not wait for a modal dialog to close -- the dialog's own event loop
  // delivers this too.
  void syncWithGuiThread();

  // Warn and return false when the wrapped object has been destroyed.
  bool checkTarget(const char *what) const;

  QPointer<QObject> m_Target;
};

class SNAPTestQt : public QObject
{
  Q_OBJECT

public:

  enum ReturnCode {
    SUCCESS = 0,
    EXCEPTION_CAUGHT,
    REGRESSION_TEST_FAILURE,
    NO_SUCH_TEST,
    UNKNOWN_ERROR
    };


  SNAPTestQt(MainImageWindow *win, std::string datadir, double accel_factor);
  ~SNAPTestQt();

  void LaunchTest(std::string test);

  // Abort the process if we are not on the GUI thread. Called from inside every
  // block that touches the application's objects, so that a marshalling step
  // lost in a future edit fails immediately and visibly rather than producing
  // the intermittent, platform-specific failures this replaced.
  static void AssertOnGuiThread(const char *what);

  // True once the test has decided its outcome and asked the application to
  // quit. Past that point the GUI event loop is on its way out, so nothing may
  // block waiting for it.
  static bool IsExiting();

public slots:

  // Find a child of an object visible to the script
  TestObjectProxy *findChild(TestObjectProxy *parent, QString child);

  // Find a widget by name globally
  TestObjectProxy *findWidget(QString widgetName);

  // Invoke a slot
  void invoke(TestObjectProxy *object, QString slot);

  // Trigger an action_name(by default, in the main menu)
  void trigger(QString action_name, TestObjectProxy *parent = nullptr);

  // Select an item in a combo box
  void comboBoxSelect(TestObjectProxy *widget, QString itemText);

  // Return the contents of an item in a table
  QVariant tableItemText(TestObjectProxy *table, int row, int col);

  // Find the index of an item in a widget (combo, list)
  QVariant findItemRow(TestObjectProxy *container, QVariant text);

  // Find the index of an item in a widget (combo, list)
  QVariant findItemColumn(TestObjectProxy *container, QVariant text);

  void print(QString text);

  void printChildren(TestObjectProxy *parent);

  void printChildren(TestObjectProxy *parent, QString className);

  void testFailed(QString reason);

  void validateValue(QVariant v1, QVariant v2);

  void validateFloatValue(double v1, double v2, double precision);

  void postMouseEvent(TestObjectProxy *widget, double rel_x, double rel_y, QString eventType, QString button);

  void postKeyEvent(TestObjectProxy *object, QString key);

  void sleep(int milli_sec);

  static void application_exit(int rc);

protected slots:

  void postKeyEventInternal(QObject *object, QString key);

protected:

  ReturnCode ListTests();

  // Wrap an application object for the script. Returns NULL for NULL, so a
  // failed lookup still reads as null in JavaScript. Proxies are cached and
  // owned by m_ProxyOwner, which lives on the GUI thread; must be called there.
  TestObjectProxy *wrap(QObject *obj);

  // Resolve a named child on the GUI thread and wait for the answer.
  QObject *findChildObject(QObject *parent, const QString &name);

  // The data directory for testing
  std::string m_DataDir;

  // We own a script engine
  QJSEngine *m_ScriptEngine;

  // A dummy parent object for this object
  QObject *m_DummyParent;

  // Parent of every TestObjectProxy, so the proxies live on the GUI thread and
  // are destroyed with us rather than leaking into the leak-canary baseline.
  QObject *m_ProxyOwner;

  // One proxy per wrapped object
  QHash<QObject *, TestObjectProxy *> m_ProxyCache;

  // Acceleration factor
  double m_Acceleration;

  // Test worker. Held by QPointer because it deletes itself on finished(), so
  // it may already be gone by the time we are destroyed.
  QPointer<TestWorker> m_Worker;

  // Main window pointer
  MainImageWindow *m_Parent;

  // Helper functions
  QModelIndex findItem(QObject *container, QVariant text);
  void printChildrenRecursive(QObject *parent, QString offset, const char *className=NULL);
};

#endif // SNAPTESTQT_H
