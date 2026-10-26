#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class CdnCharacters : public QObject
{
  Q_OBJECT

public:
  struct Entry
  {
    QString name;
    QUrl base;
  };

  static CdnCharacters &instance();

  QString sourcesFile() const;
  QStringList sources() const;

  QList<Entry> entries() const;
  bool contains(const QString &name) const;

  static bool normalize(const QString &link, QUrl *base, QString *name, QString *error);
  static bool isSafeHost(const QString &host);
  static QStringList caseVariants(const QString &path);

  bool add(const QString &link, QString *name, QString *error);
  void remove(const QString &name);

  QString ingestName(const QString &raw);

  QString resolve(const QString &vpath, const QStringList &suffixes, const std::function<bool(const QString &)> &is_local);

  static QString toVirtualPath(const QUrl &url);
  static bool fromVirtualPath(const QString &path, QUrl *url);

private:
  CdnCharacters();
  void load();
  void save() const;
  void registerEntry(const QString &name, const QUrl &base, bool persist);
  QList<QUrl> characterBases(const QString &name, const std::function<bool(const QString &)> &is_local);
  QList<QUrl> soundRoots() const;
  QString probe(const QList<QUrl> &dirs, const QString &rel, const QStringList &suffixes);

  mutable QMutex m_mutex;
  QList<Entry> m_entries;
  QHash<QString, QList<QUrl>> m_discovered;
  QHash<QString, qint64> m_discover_miss;
};

bool cdn_exists(const QUrl &url);
QByteArray cdn_fetch(const QUrl &url, bool *ok);

void cdn_install_file_engine();
