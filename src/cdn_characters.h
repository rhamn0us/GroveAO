#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWaitCondition>

#include <functional>
#include <memory>

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

// A CDN file being streamed. Bytes become readable the moment they arrive; nothing is
// written to disk and nothing waits for the whole file. All consumer methods are safe from
// any thread and block on a condition variable (never a nested event loop, so a call from
// the GUI thread cannot re-enter application code).
class CdnStream : public std::enable_shared_from_this<CdnStream>
{
public:
  // Blocks until the response headers arrive. True only for HTTP 200.
  bool waitOpen();
  // Content-Length when the server sent one, otherwise blocks until the transfer ends.
  qint64 size();
  // Copies up to len bytes starting at pos. Blocks only until those bytes (or EOF) have
  // arrived. Returns bytes copied, 0 at EOF, -1 if the transfer failed with nothing to give.
  qint64 read(qint64 pos, char *dst, qint64 len);
  // No more data needed: abort the transfer if it is still running.
  void cancel();

  // --- network thread side (not for RAIDERS) ---
  QUrl url;
  QPointer<QObject> reply;
  void onHeaders(int status, qint64 length);
  void onData(const QByteArray &data);
  void onFinished(bool ok);
  void completeFrom(const QByteArray &body);
  void failWith(int status);

private:
  QMutex m_mutex;
  QWaitCondition m_cond;
  QByteArray m_buf;
  int m_status = 0;
  qint64 m_total = -1;
  bool m_headers = false;
  bool m_finished = false;
  bool m_failed = false;
  bool m_cancelled = false;
};

bool cdn_exists(const QUrl &url);
std::shared_ptr<CdnStream> cdn_open_stream(const QUrl &url);

void cdn_install_file_engine();
