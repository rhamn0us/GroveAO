#pragma once

/**
 * CDN characters: use a character that lives on someone else's asset server
 * (e.g. https://host/base/characters/Name/) without downloading it by hand.
 *
 * How it plugs in: AOApplication::get_real_path() asks resolve() as a last
 * resort. If the requested file belongs to a registered CDN character (or is
 * a sound/blip and some CDN is registered), it is fetched from the CDN into
 * <base>/cdn_cache/ and that cached path is returned, so every existing
 * consumer (images, char.ini, emote buttons, sounds, blips) works unchanged.
 * Files already present in base/ or a mount path always win; the cache is
 * searched last. Other players keep receiving the plain character name, so
 * anyone who already owns that character sees it normally.
 */

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

class CdnCharacters : public QObject
{
  Q_OBJECT

public:
  struct Entry
  {
    QString name; // folder name as shown by the CDN, e.g. "JudgeDD"
    QUrl base;    // always ends with "/", e.g. https://host/base/characters/JudgeDD/
  };

  static CdnCharacters &instance();

  /// <base>/cdn_cache/ -- searched after base/ and all mount paths.
  QString cacheRoot() const;

  QList<Entry> entries() const;
  bool contains(const QString &name) const;

  /// Validates a pasted folder (or char.ini) link without registering it.
  static bool normalize(const QString &link, QUrl *base, QString *name, QString *error);
  /// Rejects loopback / LAN hosts. Public for tests.
  static bool isSafeHost(const QString &host);
  /// Other capitalisations worth trying on case-sensitive servers.
  static QStringList caseVariants(const QString &path);

  /// Registers the link, persists it and starts a background prefetch.
  bool add(const QString &link, QString *name, QString *error);
  /// Forgets the character and deletes its cached files.
  void remove(const QString &name);

  /**
   * Called by get_real_path(). Returns the local cached file for vpath+suffix
   * (downloading it if needed), or an empty string if no CDN has it.
   */
  QString resolve(const QString &vpath, const QStringList &suffixes);

Q_SIGNALS:
  void prefetchFinished(const QString &name, int files);

private:
  CdnCharacters();
  void load();
  void save() const;
  QString cachedPath(const QString &vpath, const QString &suffix) const;
  bool fetchInto(const QString &vpath, const QString &suffix, const QStringList &urls);
  QStringList urlsFor(const QString &vpath, const QString &suffix) const;
  void prefetch(const Entry &entry);
  void prefetchSounds(const Entry &entry, const QByteArray &ini);

  mutable QMutex m_mutex;
  QList<Entry> m_entries;
  QHash<QString, qint64> m_misses; // "vpath+suffix" -> time of last 404 (ms since epoch)
};

/// Blocking HTTP GET with a timeout. Returns true on HTTP 200.
bool cdn_http_get(const QUrl &url, QByteArray *out, int timeout_ms = 5000);
