#include "androidstorage.h"

#ifdef Q_OS_ANDROID

#include "file_functions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>

namespace
{
QString g_root; // resolved by android_setup_storage()

QJniObject activity()
{
  return QNativeInterface::QAndroidApplication::context();
}

QString shared_storage_dir()
{
  QJniObject dir = QJniObject::callStaticObjectMethod("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;");
  QJniEnvironment().checkAndClearExceptions();
  if (!dir.isValid())
  {
    return {};
  }
  return dir.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString();
}

QString app_specific_dir()
{
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
} // namespace

bool android_storage_access_granted()
{
  if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
  {
    // "All files access" (MANAGE_EXTERNAL_STORAGE)
    const bool ok = QJniObject::callStaticMethod<jboolean>("android/os/Environment", "isExternalStorageManager");
    QJniEnvironment().checkAndClearExceptions();
    return ok;
  }

  const QJniObject permission = QJniObject::fromString("android.permission.WRITE_EXTERNAL_STORAGE");
  const jint result = activity().callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I", permission.object<jstring>());
  QJniEnvironment().checkAndClearExceptions();
  return result == 0; // PackageManager.PERMISSION_GRANTED
}

void android_request_storage_access()
{
  QJniEnvironment env;
  QJniObject act = activity();

  if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
  {
    const QJniObject package_name = act.callObjectMethod("getPackageName", "()Ljava/lang/String;");
    const QJniObject uri_string = QJniObject::fromString("package:" + package_name.toString());
    const QJniObject uri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", uri_string.object<jstring>());
    const QJniObject action = QJniObject::fromString("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    const QJniObject intent("android/content/Intent", "(Ljava/lang/String;Landroid/net/Uri;)V", action.object<jstring>(), uri.object());
    act.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    env.checkAndClearExceptions();
    return;
  }

  // Android 6-10: classic runtime permission dialog.
  const QStringList names{"android.permission.READ_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE"};
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray array = env->NewObjectArray(names.size(), string_class, nullptr);
  for (int i = 0; i < names.size(); ++i)
  {
    const QJniObject name = QJniObject::fromString(names.at(i));
    env->SetObjectArrayElement(array, i, name.object<jstring>());
  }
  act.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", array, 1);
  env->DeleteLocalRef(array);
  env->DeleteLocalRef(string_class);
  env.checkAndClearExceptions();
}

QString android_storage_root()
{
  if (!g_root.isEmpty())
  {
    return g_root;
  }
  return app_specific_dir();
}

// Blocks (with the event loop running) until access is granted, or the user comes
// back to the app from the system screen/dialog, or two minutes pass.
static void wait_for_access()
{
  QEventLoop loop;
  bool left_app = false;

  QObject::connect(
      qApp,
      &QGuiApplication::applicationStateChanged,
      &loop,
      [&](Qt::ApplicationState state)
      {
        if (state != Qt::ApplicationActive)
        {
          left_app = true;
        }
        else if (left_app)
        {
          loop.quit();
        }
      });

  QTimer poll;
  QObject::connect(
      &poll,
      &QTimer::timeout,
      &loop,
      [&]()
      {
        if (android_storage_access_granted())
        {
          loop.quit();
        }
      });
  poll.start(500);

  QTimer::singleShot(120000, &loop, &QEventLoop::quit);
  loop.exec();
}

void android_setup_storage()
{
  const QString shared = shared_storage_dir();

  if (!shared.isEmpty() && !android_storage_access_granted())
  {
    QMessageBox box;
    box.setWindowTitle(QCoreApplication::translate("android_setup_storage", "File access"));
    box.setIcon(QMessageBox::Question);
    box.setText(QCoreApplication::translate(
                    "android_setup_storage",
                    "GroveAO needs access to your phone's files to read your base folder:\n\n%1/GroveAO/base\n\n"
                    "Without it, the app can only use its private folder, which file managers cannot open on newer Android versions.")
                    .arg(shared));
    QPushButton *grant = box.addButton(QCoreApplication::translate("android_setup_storage", "Grant access"), QMessageBox::AcceptRole);
    box.addButton(QCoreApplication::translate("android_setup_storage", "Not now"), QMessageBox::RejectRole);
    box.exec();

    if (box.clickedButton() == grant)
    {
      android_request_storage_access();
      wait_for_access();
    }
  }

  if (!shared.isEmpty() && android_storage_access_granted())
  {
    g_root = shared + "/GroveAO";
  }
  else
  {
    g_root = app_specific_dir();
  }
  QDir().mkpath(g_root);

  // Create the base folder and the usual sub-folders so the user has somewhere to drop content.
  const QString base = get_base_path();
  const QStringList folders{"", "characters", "background", "sounds/general", "sounds/music", "themes", "misc", "evidence", "cases"};
  for (const QString &folder : folders)
  {
    if (!QDir().mkpath(base + folder))
    {
      qWarning() << "Could not create" << (base + folder);
    }
  }
  qInfo() << "Base folder:" << base;
}

#else // !Q_OS_ANDROID

bool android_storage_access_granted()
{
  return true;
}

void android_request_storage_access() {}

QString android_storage_root()
{
  return {};
}

void android_setup_storage() {}

#endif
