#include "file_functions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

bool file_exists(QString file_path)
{
  if (file_path.isEmpty())
  {
    return false;
  }

  QFileInfo check_file(file_path);

  return check_file.exists() && check_file.isFile();
}

bool dir_exists(QString dir_path)
{
  if (dir_path == "")
  {
    return false;
  }

  QDir check_dir(dir_path);

  return check_dir.exists();
}

bool exists(QString p_path)
{
  QFile file(p_path);

  return file.exists();
}

QString get_app_path()
{
  QString path = QCoreApplication::applicationDirPath();

#ifdef Q_OS_ANDROID
  // App-specific external storage (/sdcard/Android/data/<pkg>/files): needs no
  // permissions on modern Android and is reachable by file managers and adb push.
  const QString external_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (!external_path.isEmpty())
  {
    QDir().mkpath(external_path);
    path = external_path;
  }
#endif

#ifdef Q_OS_LINUX
  QString app_path = qgetenv("APPIMAGE");
  if (!app_path.isEmpty())
  {
    path = QFileInfo(app_path).absoluteDir().path();
  }
#endif

#ifdef Q_OS_MAC
  path += "/../../..";
#endif

  if (path.endsWith(QDir::separator()))
  {
    path.chop(1);
  }

  return path;
}

QString get_base_path()
{
#ifdef Q_OS_MAC
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("base") + "/";
#else
  return QDir(get_app_path()).absoluteFilePath("base") + "/";
#endif
}

void seed_base_if_missing()
{
#ifdef Q_OS_MAC
  const QString user_base = get_base_path();
  if (dir_exists(user_base))
  {
    return;
  }

  const QString bundled_base = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources/base");
  if (!dir_exists(bundled_base))
  {
    return;
  }

  QProcess::execute("ditto", {bundled_base, user_base});
#endif
}
