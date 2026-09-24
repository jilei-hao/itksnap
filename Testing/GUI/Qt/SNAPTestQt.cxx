#include "SNAPTestQt.h"
#include "MainImageWindow.h"

#include <QAction>
#include <QLineEdit>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QMouseEvent>
#include <QApplication>
#include <QKeySequence>
#include <QDir>
#include <SNAPQApplication.h>
#include <QDeadlineTimer>
#include <QAtomicInt>
#include <QMetaMethod>
#include <memory>


#include "SNAPQtCommon.h"

#if QT_VERSION >= 0x050000
  #include <QJSEngine>
#else
  #include <QScriptEngine>
  #define QJSEngine QScriptEngine
  #define QJSValue QScriptValue
#endif

using namespace std;

namespace {

// Set once the test has queued its exit code -- see SNAPTestQt::IsExiting()
QAtomicInt g_Exiting(0);

/**
 * Run fn on the thread that owns ctx and wait for it to finish.
 *
 * Used for anything that has to return a value. This cannot deadlock against a
 * modal dialog -- QDialog::exec() runs an event loop, so the posted call is
 * still delivered -- but it would deadlock if the GUI thread were blocked
 * outside an event loop, which is why writes and actions are posted instead of
 * waited on, and why nothing waits once the application is quitting.
 */
template <class Func>
void RunOnOwnerThreadAndWait(QObject *ctx, Func fn)
{
  if(!ctx)
    return;

  if(QThread::currentThread() == ctx->thread())
    fn();
  else if(!SNAPTestQt::IsExiting())
    QMetaObject::invokeMethod(ctx, fn, Qt::BlockingQueuedConnection);
}

} // namespace

void SNAPTestQt::AssertOnGuiThread(const char *what)
{
  if(QThread::currentThread() != QCoreApplication::instance()->thread())
    qFatal("Test harness: '%s' ran on a worker thread instead of the GUI thread. "
           "Scripted access to the application must go through TestObjectProxy.", what);
}

bool SNAPTestQt::IsExiting()
{
  return g_Exiting.loadAcquire() != 0;
}


TestObjectProxy::TestObjectProxy(QObject *target, QObject *parent)
  : QObject(parent), m_Target(target)
{
}

QObject *TestObjectProxy::target() const
{
  SNAPTestQt::AssertOnGuiThread("TestObjectProxy::target");
  return m_Target.data();
}

bool TestObjectProxy::checkTarget(const char *what) const
{
  if(m_Target.isNull())
    {
    qWarning() << QString("Test harness: %1 on an object that has been destroyed").arg(what);
    return false;
    }
  return true;
}

QVariant TestObjectProxy::getProperty(const char *name) const
{
  QVariant result;
  QByteArray prop(name);

  // const_cast: we are only using ourselves as the thread context to hop to
  RunOnOwnerThreadAndWait(const_cast<TestObjectProxy *>(this), [&]() {
    if(this->checkTarget(prop.constData()))
      result = this->target()->property(prop.constData());
    });

  return result;
}

void TestObjectProxy::syncWithGuiThread()
{
  RunOnOwnerThreadAndWait(this, []() {});
}

void TestObjectProxy::setProperty_(const char *name, const QVariant &value)
{
  QByteArray prop(name);

  QMetaObject::invokeMethod(this, [this, prop, value]() {
    if(this->checkTarget(prop.constData()))
      this->target()->setProperty(prop.constData(), value);
    }, Qt::QueuedConnection);

  syncWithGuiThread();
}

void TestObjectProxy::invokeSlot(const char *slot)
{
  QByteArray name(slot);

  QMetaObject::invokeMethod(this, [this, name]() {
    if(this->checkTarget(name.constData()))
      QMetaObject::invokeMethod(this->target(), name.constData(), Qt::DirectConnection);
    }, Qt::QueuedConnection);

  syncWithGuiThread();
}

void TestObjectProxy::click()
{
  invokeSlot("click");
}

void TestObjectProxy::toggle()
{
  invokeSlot("toggle");
}

void TestObjectProxy::trigger()
{
  invokeSlot("trigger");
}

void TestObjectProxy::invoke(QString slot)
{
  invokeSlot(slot.toUtf8().constData());
}

QVariant TestObjectProxy::get(QString property_name)
{
  return getProperty(property_name.toUtf8().constData());
}

void TestObjectProxy::set(QString property_name, QVariant value)
{
  setProperty_(property_name.toUtf8().constData(), value);
}

void TestObjectProxy::setCurrentIndex(int index)
{
  QMetaObject::invokeMethod(this, [this, index]() {
    if(this->checkTarget("setCurrentIndex"))
      QMetaObject::invokeMethod(this->target(), "setCurrentIndex",
                                Qt::DirectConnection, Q_ARG(int, index));
    }, Qt::QueuedConnection);

  syncWithGuiThread();
}

void TestObjectProxy::setSelected(bool value)
{
  QMetaObject::invokeMethod(this, [this, value]() {
    if(this->checkTarget("setSelected"))
      QMetaObject::invokeMethod(this->target(), "setSelected",
                                Qt::DirectConnection, Q_ARG(bool, value));
    }, Qt::QueuedConnection);

  syncWithGuiThread();
}

void TestObjectProxy::setCurrentWidget(TestObjectProxy *widget)
{
  QPointer<TestObjectProxy> arg(widget);

  QMetaObject::invokeMethod(this, [this, arg]() {
    QWidget *page = arg ? qobject_cast<QWidget *>(arg->target()) : NULL;
    if(!page)
      {
      qWarning() << "Test harness: setCurrentWidget with no target widget";
      return;
      }
    if(this->checkTarget("setCurrentWidget"))
      QMetaObject::invokeMethod(this->target(), "setCurrentWidget",
                                Qt::DirectConnection, Q_ARG(QWidget *, page));
    }, Qt::QueuedConnection);

  syncWithGuiThread();
}


bool TestObjectProxy::callMethod(QString method, QVariantList args)
{
  // Mirrors the fixed-size argument arrays below; QMetaMethod::invoke takes
  // at most this many anyway
  const int max_args = 5;
  if(args.size() > max_args)
    {
    qWarning() << QString("Test harness: callMethod %1 with %2 arguments (max %3)")
                  .arg(method).arg(args.size()).arg(max_args);
    return false;
    }

  QByteArray name = method.toUtf8();

  // Any object among the arguments is a proxy the script was handed earlier.
  // Hold it through a QPointer so one deleted in the meantime reads as null
  // instead of dangling.
  QList<QPointer<QObject> > objects;
  for(const QVariant &arg : args)
    objects.append(arg.canConvert<QObject *>() ? arg.value<QObject *>() : nullptr);

  // Whether a matching method exists is decided on the GUI thread. The barrier
  // below is delivered only after the posted call has at least reached the
  // invoke, so the answer is in place by the time we read it back.
  auto found = std::make_shared<bool>(false);

  QMetaObject::invokeMethod(this, [this, name, args, objects, found]() {
    if(!this->checkTarget(name.constData()))
      return;

    QObject *target = this->target();
    const QMetaObject *mo = target->metaObject();
    for(int i = 0; i < mo->methodCount(); i++)
      {
      QMetaMethod mm = mo->method(i);
      if(mm.name() != name || mm.parameterCount() != args.size())
        continue;

      *found = true;

      // Convert each argument to the declared parameter type. A proxy becomes
      // the object it wraps -- we are on the GUI thread here, so target() is
      // allowed. The type names have to outlive the invoke: QMetaMethod
      // checks them against the declared parameter types.
      QVariant converted[max_args];
      QByteArray type_names[max_args];
      QGenericArgument ga[max_args];
      for(int j = 0; j < args.size(); j++)
        {
        converted[j] = args[j];
        if(objects[j])
          {
          QObject *obj = objects[j].data();
          if(TestObjectProxy *proxy = qobject_cast<TestObjectProxy *>(obj))
            obj = proxy->target();
          if(!obj)
            qWarning() << QString("Test harness: argument %1 of %2 refers to a destroyed object")
                          .arg(j).arg(name.constData());
          converted[j] = QVariant::fromValue(obj);
          }
        converted[j].convert(QMetaType(mm.parameterType(j)));
        type_names[j] = mm.parameterTypeName(j);
        ga[j] = QGenericArgument(type_names[j].constData(), converted[j].data());
        }

      mm.invoke(target, Qt::DirectConnection, ga[0], ga[1], ga[2], ga[3], ga[4]);
      return;
      }
    }, Qt::QueuedConnection);

  syncWithGuiThread();
  return *found;
}

SNAPTestQt::SNAPTestQt(MainImageWindow *win,
    std::string datadir, double accel_factor)
: m_Acceleration(accel_factor), m_Worker(NULL), m_Parent(win)
{
  // We need a dummy parent to prevent self-deletion
  m_DummyParent = new QObject();
  this->setParent(m_DummyParent);

  // Every proxy handed to the script hangs off this object, so they share our
  // (GUI) thread affinity and are destroyed with us
  m_ProxyOwner = new QObject();

  // Create the script engine
  m_ScriptEngine = new QJSEngine();

  // Assign the window as a variable in the script engine
  QJSValue mwin = m_ScriptEngine->newQObject(wrap(win));
  m_ScriptEngine->globalObject().setProperty("mainwin", mwin);

  // Provide a pointer to the engine
  QJSValue vthis = m_ScriptEngine->newQObject(this);
  m_ScriptEngine->globalObject().setProperty("engine", vthis);

  QJSValue test = m_ScriptEngine->newQObject(wrap(win->findChild<QPushButton *>("btnLoadMain")));
  m_ScriptEngine->globalObject().setProperty("btn", test);

  // Assign the data directory to the script engine
  m_ScriptEngine->globalObject().setProperty("datadir", from_utf8(datadir));
}

SNAPTestQt::~SNAPTestQt()
{
  // We are destroyed after the GUI event loop has stopped, and the script
  // thread may still be inside evaluate(). Give it a moment to unwind --
  // application_exit() has already told the marshalling helpers to stop waiting
  // on that event loop, so it should return promptly.
  // (qualified: TestWorker::wait is the script-facing sleep, not QThread::wait)
  if(m_Worker && m_Worker->isRunning())
    m_Worker->QThread::wait(QDeadlineTimer(2000));

  if(m_Worker && m_Worker->isRunning())
    {
    // Still running. Deleting the engine and proxies out from under it, or
    // letting ~QObject destroy a live QThread, would turn a finished test into
    // a crash report; leave them to process teardown instead.
    qWarning() << "Test script thread did not finish; skipping test engine cleanup";
    m_Worker->setParent(NULL);
    setParent(NULL);
    return;
    }

  delete m_ScriptEngine;
  delete m_ProxyOwner;
  setParent(NULL);
  delete m_DummyParent;
}

#include <QFileInfo>

void
SNAPTestQt::LaunchTest(std::string test)
{
  // Special case: listing all tests
  if(test == "list")
    {
    ListTests();
    application_exit(SUCCESS);
    }

  // Create and run the thread
  m_Worker = new TestWorker(this, from_utf8(test), m_ScriptEngine, m_Acceleration);

  connect(m_Worker, SIGNAL(finished()), m_Worker, SLOT(deleteLater()));

  m_Worker->start();
}

TestObjectProxy *SNAPTestQt::wrap(QObject *obj)
{
  if(!obj)
    return NULL;

  AssertOnGuiThread("wrap");

  auto it = m_ProxyCache.find(obj);
  if(it != m_ProxyCache.end())
    {
    if(it.value()->target() == obj)
      return it.value();

    // The wrapped object died and a new one was allocated at its address
    delete it.value();
    m_ProxyCache.erase(it);
    }

  TestObjectProxy *proxy = new TestObjectProxy(obj, m_ProxyOwner);
  m_ProxyCache.insert(obj, proxy);
  return proxy;
}

QObject *SNAPTestQt::findChildObject(QObject *parent, const QString &name)
{
  AssertOnGuiThread("findChild");
  return parent ? parent->findChild<QObject *>(name) : NULL;
}

TestObjectProxy *SNAPTestQt::findChild(TestObjectProxy *parent, QString child)
{
  TestObjectProxy *result = NULL;
  QPointer<TestObjectProxy> owner(parent);

  RunOnOwnerThreadAndWait(this, [&]() {
    QObject *pobj = owner ? owner->target() : NULL;
    result = wrap(findChildObject(pobj, child));
    });

  return result;
}

TestObjectProxy *SNAPTestQt::findWidget(QString widgetName)
{
  TestObjectProxy *result = NULL;

  RunOnOwnerThreadAndWait(this, [&]() {
    AssertOnGuiThread("findWidget");
    foreach(QWidget *w, QApplication::allWidgets())
      if(w->objectName() == widgetName)
        {
        result = wrap(w);
        return;
        }
    });

  return result;
}


QVariant SNAPTestQt::tableItemText(TestObjectProxy *table, int row, int col)
{
  QVariant result;
  QPointer<TestObjectProxy> owner(table);

  RunOnOwnerThreadAndWait(this, [&]() {
    AssertOnGuiThread("tableItemText");
    QAbstractItemView *view = owner ? dynamic_cast<QAbstractItemView *>(owner->target()) : NULL;
    if(view)
      {
      QAbstractItemModel *model = view->model();
      result = model->data(model->index(row, col));
      }
    });

  return result;
}


QModelIndex SNAPTestQt::findItem(QObject *container, QVariant text)
{
  AssertOnGuiThread("findItem");

  QAbstractItemModel *model = NULL;

  // Is it a combo box?
  if(QComboBox *combo = dynamic_cast<QComboBox *>(container))
    model = combo->model();

  // Is it an item view?
  else if(QAbstractItemView *itemview = dynamic_cast<QAbstractItemView *>(container))
    model = itemview->model();

  // Find the item
  if(model)
    {
    QModelIndexList found = model->match(model->index(0,0),Qt::DisplayRole,text);
    if(found.size())
      return found.at(0);
    }

  return QModelIndex();
}

void SNAPTestQt::invoke(TestObjectProxy *object, QString slot)
{
  if(!object)
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("Invoked slot %1 on null object").arg(slot));
  else
    object->invoke(slot);
}

void SNAPTestQt::trigger(QString action_name, TestObjectProxy *parent)
{
  TestObjectProxy *action = NULL;
  QPointer<TestObjectProxy> owner(parent);

  RunOnOwnerThreadAndWait(this, [&]() {
    QObject *pobj = owner ? owner->target() : static_cast<QObject *>(this->m_Parent);
    action = wrap(dynamic_cast<QAction *>(findChildObject(pobj, action_name)));
    });

  invoke(action, "trigger");
}

void SNAPTestQt::comboBoxSelect(TestObjectProxy *widget, QString itemText)
{
  bool is_combo = false;
  QPointer<TestObjectProxy> owner(widget);

  RunOnOwnerThreadAndWait(this, [&]() {
    AssertOnGuiThread("comboBoxSelect");
    is_combo = owner && dynamic_cast<QComboBox *>(owner->target()) != NULL;
    });

  if(!is_combo)
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("comboBoxSelect target not a combo box"));

  int row = findItemRow(widget, itemText).toInt();
  if(widget)
    widget->setCurrentIndex(row);
}

// The engine-level helpers below are what the test scripts call
// (engine.setChildProperty(...), engine.clickChild(...), ...). Each one is a
// thin layer over the TestObjectProxy the script already holds, so it marshals
// onto the GUI thread the same way a direct call on the proxy does. A null
// object is reported as a script error rather than ignored: it always means a
// lookup failed, and a test that silently skips a step is a test that cannot
// fail.

QVariant SNAPTestQt::getProperty(TestObjectProxy *obj, QString name)
{
  if(!obj)
    {
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("getProperty %1 on null object").arg(name));
    return QVariant();
    }

  return obj->get(name);
}

void SNAPTestQt::setProperty(TestObjectProxy *obj, QString name, QVariant value)
{
  if(!obj)
    {
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("setProperty %1 on null object").arg(name));
    return;
    }

  obj->set(name, value);
}

// `block` is accepted for script compatibility only. Every proxy action already
// round-trips through the GUI thread's event queue before returning, and that
// barrier is safe against a nested modal loop (see TestObjectProxy), so there
// is no non-blocking mode to fall back to and nothing for the flag to select.
void SNAPTestQt::callMethod(TestObjectProxy *obj, QString method, QVariantList args, bool block)
{
  Q_UNUSED(block);

  if(!obj)
    {
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("callMethod %1 on null object").arg(method));
    return;
    }

  if(!obj->callMethod(method, args) && !IsExiting())
    m_ScriptEngine->throwError(QJSValue::ReferenceError,
                               QString("No method %1/%2 found").arg(method).arg(args.size()));
}

QVariant SNAPTestQt::getChildProperty(TestObjectProxy *parent, QString childName, QString propName)
{
  return getProperty(findChild(parent, childName), propName);
}

void SNAPTestQt::setChildProperty(TestObjectProxy *parent, QString childName, QString propName, QVariant value)
{
  setProperty(findChild(parent, childName), propName, value);
}

void SNAPTestQt::callChildMethod(TestObjectProxy *parent, QString childName, QString method, QVariantList args, bool block)
{
  callMethod(findChild(parent, childName), method, args, block);
}

void SNAPTestQt::click(TestObjectProxy *obj, bool block)
{
  callMethod(obj, "click", QVariantList(), block);
}

void SNAPTestQt::clickChild(TestObjectProxy *parent, QString childName, bool block)
{
  click(findChild(parent, childName), block);
}

void SNAPTestQt::toggle(TestObjectProxy *obj, bool block)
{
  callMethod(obj, "toggle", QVariantList(), block);
}

void SNAPTestQt::close(TestObjectProxy *obj, bool block)
{
  callMethod(obj, "close", QVariantList(), block);
}

void SNAPTestQt::closeChild(TestObjectProxy *parent, QString childName, bool block)
{
  close(findChild(parent, childName), block);
}

void SNAPTestQt::validateProperty(TestObjectProxy *obj, QString name, QVariant expected, double precision)
{
  validateValue(getProperty(obj, name), expected, precision);
}

void SNAPTestQt::validateChildProperty(TestObjectProxy *parent, QString childName, QString propName,
                                        QVariant expected, double precision)
{
  validateValue(getChildProperty(parent, childName, propName), expected, precision);
}


QVariant SNAPTestQt::findItemRow(TestObjectProxy *container, QVariant text)
{
  QVariant result;
  QPointer<TestObjectProxy> owner(container);

  RunOnOwnerThreadAndWait(this, [&]() {
    QModelIndex idx = findItem(owner ? owner->target() : NULL, text);
    if(idx.isValid())
      result = idx.row();
    });

  return result;
}

QVariant SNAPTestQt::findItemColumn(TestObjectProxy *container, QVariant text)
{
  QVariant result;
  QPointer<TestObjectProxy> owner(container);

  RunOnOwnerThreadAndWait(this, [&]() {
    QModelIndex idx = findItem(owner ? owner->target() : NULL, text);
    if(idx.isValid())
      result = idx.column();
    });

  return result;
}


void SNAPTestQt::print(QString text)
{
  qDebug() << text;
}

void SNAPTestQt::printChildrenRecursive(QObject *parent, QString offset, const char *className)
{
  AssertOnGuiThread("printChildren");

  if(parent)
    {
    if(!className || parent->inherits(className))
      {
      QString line = QString("%1%2 : %3").arg(offset,parent->metaObject()->className(),parent->objectName());
      qDebug() << line;
      }

    foreach (QObject* child, parent->children())
      {
      QWidget *widget = dynamic_cast<QWidget *>(child);
      if(widget)
        printChildrenRecursive(child, offset + "  ", className);
      }
    }
  else
    {
    qDebug() << "NULL passed to printChild";
    }
}

void SNAPTestQt::printChildren(TestObjectProxy *parent)
{
  QPointer<TestObjectProxy> owner(parent);

  RunOnOwnerThreadAndWait(this, [&]() {
    printChildrenRecursive(owner ? owner->target() : NULL, "");
    });
}

void SNAPTestQt::printChildren(TestObjectProxy *parent, QString className)
{
  QPointer<TestObjectProxy> owner(parent);

  RunOnOwnerThreadAndWait(this, [&]() {
    const char *cn = NULL;
    QByteArray ba = className.toLocal8Bit();
    if(!className.isNull())
      cn = ba.data();
    printChildrenRecursive(owner ? owner->target() : NULL, "", cn);
    });
}

void SNAPTestQt::validateValue(QVariant v1, QVariant v2, double precision)
{
  // precision < 0 means exact comparison; otherwise numeric within tolerance
  bool failed;
  QString msg;
  if(precision >= 0)
    {
    failed = fabs(v1.toDouble() - v2.toDouble()) > precision;
    msg = QString("Validation %1 == %2 (with precision %3) %4!")
        .arg(v1.toDouble()).arg(v2.toDouble()).arg(precision).arg(failed ? "failed" : "ok");
    }
  else
    {
    failed = v1 != v2;
    msg = QString("Validation %1 == %2 %3!").arg(v1.toString(), v2.toString(), failed ? "failed" : "ok");
    }

  if(failed)
    {
    qWarning() << msg;
    m_ScriptEngine->throwError(QJSValue::GenericError, msg);
    }
  else
    {
    qDebug() << msg;
    }
}

void SNAPTestQt::application_exit(int rc)
{
  // Stop the marshalling helpers from waiting on an event loop that is about to
  // stop; the test's outcome is already decided at this point
  g_Exiting.storeRelease(1);

  QMetaObject::invokeMethod(
        QCoreApplication::instance(), "quitWithReturnCode", Qt::QueuedConnection,
        Q_ARG(int, rc));
}

void SNAPTestQt::postKeyEventInternal(QObject *object, QString key)
{
    AssertOnGuiThread("postKeyEvent");

    QWidget *widget = dynamic_cast<QWidget *>(object);
    if(widget)
    {
        QKeySequence seq(key);
        if(seq.count() == 1)
        {
            QKeyCombination code = seq[0];
            Qt::Key key = code.key();
            Qt::KeyboardModifiers mods = code.keyboardModifiers();

            QKeyEvent *ev = new QKeyEvent(QEvent::KeyPress, key, mods);
            QApplication::postEvent(widget, ev);
        }
    }
}

void SNAPTestQt::sleep(int milli_sec)
{
  // Scale requested sleep time by acceleration factor
  int ms_actual = (int)(milli_sec / m_Acceleration);

  // Sleep
  TestWorker::sleep_ms(ms_actual);
}

void SNAPTestQt::testFailed(QString reason)
{
  qWarning() << reason;
  m_ScriptEngine->throwError(QJSValue::GenericError, reason);
  // application_exit(REGRESSION_TEST_FAILURE);
}


void SNAPTestQt::postMouseEvent(TestObjectProxy *object, double rel_x, double rel_y, QString eventType, QString button)
{
  // Special case handlers
  if(eventType == "click")
    {
    postMouseEvent(object, rel_x, rel_y, "press", button);
    postMouseEvent(object, rel_x, rel_y, "release", button);
    return;
    }

  QPointer<TestObjectProxy> owner(object);

  // The geometry has to be read on the GUI thread; posting the event afterwards
  // is thread-safe either way
  RunOnOwnerThreadAndWait(this, [&]() {
    AssertOnGuiThread("postMouseEvent");

    QWidget *widget = owner ? dynamic_cast<QWidget *>(owner->target()) : NULL;
    if(!widget)
      return;

    QSize size = widget->size();
    QPointF localPos((int)(size.width() * rel_x), (int)(size.height() * rel_y));
    QPointF globalPos = widget->mapToGlobal(localPos); // added global pos to fix deprected QMouseEvent Constructor issue

    Qt::MouseButton btn = Qt::NoButton;
    if(button == "left")
      btn = Qt::LeftButton;
    else if(button == "right")
      btn = Qt::RightButton;
    else if(button == "middle")
      btn = Qt::MiddleButton;

    QEvent::Type type = QEvent::None;
    if(eventType == "press")
      type = QEvent::MouseButtonPress;
    else if(eventType == "release")
      type = QEvent::MouseButtonRelease;

    QMouseEvent *event = new QMouseEvent(type, localPos, globalPos, btn, btn, Qt::NoModifier);
    QApplication::postEvent(widget, event);
    });
}

void SNAPTestQt::postKeyEvent(TestObjectProxy *object, QString key)
{
  // We need the code to run in the main thread
  QPointer<TestObjectProxy> owner(object);

  QMetaObject::invokeMethod(this, [this, owner, key]() {
    if(owner)
      postKeyEventInternal(owner->target(), key);
    }, Qt::QueuedConnection);
}


SNAPTestQt::ReturnCode
SNAPTestQt::ListTests()
{
  QDir script_dir(":/scripts/Scripts");
  QStringList filters; filters << "test_*.js";
  script_dir.setNameFilters(filters);
  QStringList files = script_dir.entryList();

  QRegularExpression rx("test_(.*).js");

  cout << "Available Tests" << endl;
  foreach(const QString &test, files)
    {
    auto rm = rx.match(test);
    if(rm.hasMatch())
      cout << "  " << rm.captured(1).toStdString() << endl;
    }

  return SUCCESS;
}


TestWorker::TestWorker(QObject *parent, QString script, QJSEngine *engine, double accel_factor)
  : QThread(parent)
{
  m_MainScript = script;
  m_Engine = engine;
  m_Acceleration = accel_factor > 0.0 ? accel_factor : 1.0;
}

void TestWorker::run()
{
  // Add ourselves to the engine
  QJSValue mwin = m_Engine->newQObject(this);
  m_Engine->globalObject().setProperty("thread", mwin);

  // Make sure full output is captured
  qDebug() << "CTEST_FULL_OUTPUT";

  // Run the top-level script
  source(m_MainScript);
}

void TestWorker::sleep_ms(unsigned int msec)
{
  QThread::msleep(msec);
}

void TestWorker::wait(unsigned int msec)
{
  msleep(msec);
}

void TestWorker::readScript(QString script_url, QString &script)
{
  // Find the script file corresponding to the test
  QFile file(script_url);
  if(!file.open(QIODevice::ReadOnly))
    {
    qWarning() << QString("Unable to read test script %1").arg(script_url);
    SNAPTestQt::application_exit(SNAPTestQt::NO_SUCH_TEST);
    }

  // Read the script
  QTextStream stream(&file);

  // Read the script line by line, making substitutions
  while(!stream.atEnd())
    {
    QString line = stream.readLine();
    auto rmSleep = QRegularExpression("^\\s*$").match(line);
    auto rmComment = QRegularExpression("//===\\s+(\\w+.*)").match(line);
    // QRegExp rxInclude("include.*\\((\\w+.*)\\)");
    auto rmInclude = QRegularExpression("include.*\"(\\w+.*)\".*").match(line);

    if(rmSleep.hasMatch())
      {
      line = QString("engine.sleep(500)");
      }
    else if(rmComment.hasMatch())
      {
      line = QString("engine.print(\"%1\")").arg(rmComment.captured(1));
      }
    else if(rmInclude.hasMatch())
      {
      QString child_url = rmInclude.captured(1);
      if(!QFileInfo(child_url).isReadable())
        child_url = QString(":/scripts/Scripts/test_%1.js").arg(child_url);

      qDebug() << "Including : " << child_url;

      this->readScript(child_url, script);
      line = "";
      }

    script += line;
    script += "\n";
    }

  // Close the file
  file.close();
}

void TestWorker::source(QString script_url)
{
  // The test may be a path to an actual file
  if(!QFileInfo(script_url).isReadable())
    script_url = QString(":/scripts/Scripts/test_%1.js").arg(script_url);

  // Report which test we are accessing
  qDebug() << "Running test: " << script_url;

  QString script;
  this->readScript(script_url, script);

  // Execute it
  QJSValue rc = m_Engine->evaluate(script);
  qWarning() << "Return code from evaluate is " << rc.toString();
  if(rc.isError())
    {
    qWarning() << "JavaScript exception:" << rc.toString();
    SNAPTestQt::application_exit(SNAPTestQt::EXCEPTION_CAUGHT);
    }
  else
    {
    qDebug() << "Successfully completed test script with return code " << rc.toString();
    SNAPTestQt::application_exit(SNAPTestQt::SUCCESS);
    }
}
