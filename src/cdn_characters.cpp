#include "cdn_characters.h"

#include "file_functions.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <atomic>

static const QStringList kSoundExts = {".opus", ".ogg", ".mp3", ".wav"};
static const qint64 kMissTtlMs = 5 * 60 * 1000; // don't re-ask the CDN for 5 minutes

bool cdn_http_get(const QUrl &url, QByteArray *out, int timeout_ms)
{
  // One manager per call: this runs on whatever thread asked for the file.
  QNetworkAccessManager nam;
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(timeout_ms);
  req.setHeader(QNetworkRequest::UserAgentHeader, "GroveAO");
  QNetworkReply *reply = nam.get(req);
  QEventLoop loop;
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();
  const bool ok = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
  if (ok && out)
  {
    *out = reply->readAll();
  }
  reply->deleteLater();
  return ok;
}

CdnCharacters &CdnCharacters::instance()
{
  static CdnCharacters inst;
  return inst;
}

CdnCharacters::CdnCharacters()
{
  load();
}

QString CdnCharacters::cacheRoot() const
{
  return get_base_path() + "cdn_cache/";
}

QList<CdnCharacters::Entry> CdnCharacters::entries() const
{
  QMutexLocker lock(&m_mutex);
  return m_entries;
}

bool CdnCharacters::contains(const QString &name) const
{
  QMutexLocker lock(&m_mutex);
  for (const Entry &e : m_entries)
  {
    if (e.name.compare(name, Qt::CaseInsensitive) == 0)
    {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- validation

bool CdnCharacters::isSafeHost(const QString &host_in)
{
  if (qEnvironmentVariableIsSet("GROVEAO_TEST_ALLOW_LOCAL_CDN"))
  {
    return true; // automated tests serve a fake CDN from localhost
  }
  const QString host = host_in.toLower();
  if (host.isEmpty() || host == "localhost" || host.endsWith(".local") || host.endsWith(".internal"))
  {
    return false;
  }
  if (host.contains(':'))
  {
    return false; // raw IPv6 literal
  }
  static const QRegularExpression ipv4(R"(^(\d+)\.(\d+)\.(\d+)\.(\d+)$)");
  const QRegularExpressionMatch m = ipv4.match(host);
  if (m.hasMatch())
  {
    const int a = m.captured(1).toInt();
    const int b = m.captured(2).toInt();
    if (a == 10 || a == 127 || a == 0 || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168))
    {
      return false;
    }
  }
  return host.contains('.');
}

bool CdnCharacters::normalize(const QString &link, QUrl *base, QString *name, QString *error)
{
  auto fail = [&](const QString &msg) {
    if (error)
    {
      *error = msg;
    }
    return false;
  };
  QUrl url = QUrl::fromUserInput(link.trimmed());
  if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http"))
  {
    return fail(QObject::tr("Only http(s) links are supported."));
  }
  if (!url.userName().isEmpty() || !url.password().isEmpty())
  {
    return fail(QObject::tr("Links with a username/password aren't supported."));
  }
  if (!isSafeHost(url.host()))
  {
    return fail(QObject::tr("That host isn't allowed (local/private address)."));
  }
  if (!qEnvironmentVariableIsSet("GROVEAO_TEST_ALLOW_LOCAL_CDN"))
  {
    url.setScheme("https"); // plain http is not worth the risk
  }
  url.setQuery(QString());
  url.setFragment(QString());
  QString path = url.path();
  if (path.endsWith("/char.ini", Qt::CaseInsensitive))
  {
    path.chop(9);
  }
  if (!path.endsWith('/'))
  {
    path += '/';
  }
  const QStringList parts = path.split('/', Qt::SkipEmptyParts);
  if (parts.isEmpty())
  {
    return fail(QObject::tr("Paste the link to the character's folder, e.g. https://host/base/characters/Name/"));
  }
  url.setPath(path);
  if (base)
  {
    *base = url;
  }
  if (name)
  {
    *name = QUrl::fromPercentEncoding(parts.last().toUtf8());
  }
  return true;
}

QStringList CdnCharacters::caseVariants(const QString &path)
{
  auto cap = [](const QString &w) {
    return w.isEmpty() ? w : w.left(1).toUpper() + w.mid(1);
  };
  const QStringList segs = path.split('/');
  QStringList a, b, c;
  for (int i = 0; i < segs.size(); ++i)
  {
    a << cap(segs[i]);
    QStringList words = segs[i].split('-');
    for (QString &w : words)
    {
      w = cap(w);
    }
    b << words.join('-');
    c << (i == segs.size() - 1 ? cap(segs[i]) : segs[i]);
  }
  QStringList out;
  for (const QString &v : {a.join('/'), b.join('/'), c.join('/')})
  {
    if (v != path && !out.contains(v))
    {
      out << v;
    }
  }
  return out;
}

// ------------------------------------------------------------- persistence

void CdnCharacters::load()
{
  QFile f(get_base_path() + "cdn_characters.json");
  if (!f.open(QIODevice::ReadOnly))
  {
    return;
  }
  for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array())
  {
    const QJsonObject o = v.toObject();
    QUrl base;
    QString name;
    if (normalize(o.value("url").toString(), &base, &name, nullptr))
    {
      m_entries.append({o.value("name").toString(name), base});
    }
  }
}

void CdnCharacters::save() const
{
  QJsonArray arr;
  for (const Entry &e : m_entries)
  {
    arr.append(QJsonObject{{"name", e.name}, {"url", e.base.toString()}});
  }
  QFile f(get_base_path() + "cdn_characters.json");
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    f.write(QJsonDocument(arr).toJson());
  }
}

bool CdnCharacters::add(const QString &link, QString *name_out, QString *error)
{
  QUrl base;
  QString name;
  if (!normalize(link, &base, &name, error))
  {
    return false;
  }
  Entry entry{name, base};
  {
    QMutexLocker lock(&m_mutex);
    for (Entry &e : m_entries)
    {
      if (e.name.compare(name, Qt::CaseInsensitive) == 0)
      {
        e = entry;
        entry = e;
        name.clear();
        break;
      }
    }
    if (!name.isEmpty())
    {
      m_entries.append(entry);
    }
    m_misses.clear();
    save();
  }
  if (name_out)
  {
    *name_out = entry.name;
  }
  prefetch(entry);
  return true;
}

void CdnCharacters::remove(const QString &name)
{
  {
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_entries.size(); ++i)
    {
      if (m_entries[i].name.compare(name, Qt::CaseInsensitive) == 0)
      {
        m_entries.removeAt(i);
        break;
      }
    }
    save();
  }
  QDir(cacheRoot() + "characters/" + name.toLower()).removeRecursively();
  QDir(cacheRoot() + "characters/" + name).removeRecursively();
}

// ----------------------------------------------------------------- lookups

QString CdnCharacters::cachedPath(const QString &vpath, const QString &suffix) const
{
  const QString p = cacheRoot() + vpath + suffix;
  return file_exists(p) ? p : QString();
}

/// CDN URLs that could hold vpath+suffix. Characters: that character's folder.
/// sounds/…: the "base/" folder next to every registered character, plus the
/// character's own folder for sfx.
QStringList CdnCharacters::urlsFor(const QString &vpath, const QString &suffix) const
{
  QStringList urls;
  static const QRegularExpression charRe(R"(^characters/([^/]+)/(.+)$)", QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression soundRe(R"(^sounds/([^/]+)/(.+)$)", QRegularExpression::CaseInsensitiveOption);
  auto enc = [](const QString &rel) {
    QStringList segs = rel.split('/');
    for (QString &s : segs)
    {
      s = QString::fromLatin1(QUrl::toPercentEncoding(s));
    }
    return segs.join('/');
  };
  const QList<Entry> list = entries();
  const QRegularExpressionMatch cm = charRe.match(vpath);
  if (cm.hasMatch())
  {
    for (const Entry &e : list)
    {
      if (e.name.compare(cm.captured(1), Qt::CaseInsensitive) != 0)
      {
        continue;
      }
      const QString rel = cm.captured(2) + suffix;
      urls << e.base.toString() + enc(rel);
      for (const QString &v : caseVariants(rel))
      {
        urls << e.base.toString() + enc(v);
      }
    }
    return urls;
  }
  const QRegularExpressionMatch sm = soundRe.match(vpath);
  if (sm.hasMatch())
  {
    QSet<QString> seen;
    for (const Entry &e : list)
    {
      // .../characters/<Name>/ -> .../  (the CDN's "base/" folder)
      QUrl root = e.base.resolved(QUrl("../../"));
      const QString rootStr = root.toString();
      if (seen.contains(rootStr))
      {
        continue;
      }
      seen.insert(rootStr);
      const QString rel = "sounds/" + sm.captured(1) + "/" + sm.captured(2) + suffix;
      urls << rootStr + enc(rel);
      for (const QString &v : caseVariants(sm.captured(2) + suffix))
      {
        urls << rootStr + enc("sounds/" + sm.captured(1) + "/" + v);
      }
    }
  }
  return urls;
}

bool CdnCharacters::fetchInto(const QString &vpath, const QString &suffix, const QStringList &urls)
{
  for (const QString &u : urls)
  {
    QByteArray data;
    if (!cdn_http_get(QUrl(u), &data))
    {
      continue;
    }
    const QString dest = cacheRoot() + vpath + suffix;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QFile f(dest);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      f.write(data);
      return true;
    }
  }
  return false;
}

QString CdnCharacters::resolve(const QString &vpath, const QStringList &suffixes)
{
  if (vpath.isEmpty() || vpath.startsWith("http") || m_entries.isEmpty())
  {
    return QString();
  }
  for (const QString &suffix : suffixes)
  {
    const QString hit = cachedPath(vpath, suffix);
    if (!hit.isEmpty())
    {
      return hit;
    }
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const QString &suffix : suffixes)
  {
    const QString key = vpath + suffix;
    {
      QMutexLocker lock(&m_mutex);
      if (m_misses.contains(key) && now - m_misses.value(key) < kMissTtlMs)
      {
        continue;
      }
    }
    const QStringList urls = urlsFor(vpath, suffix);
    if (urls.isEmpty())
    {
      continue; // not something any CDN can have
    }
    if (fetchInto(vpath, suffix, urls))
    {
      return cachedPath(vpath, suffix);
    }
    QMutexLocker lock(&m_mutex);
    m_misses.insert(key, now);
  }
  return QString();
}

// ---------------------------------------------------------------- prefetch

/// Downloads char.ini, the icon and emote buttons, and (on GitHub raw links)
/// the whole folder in the background, so first use doesn't stall.
void CdnCharacters::prefetch(const Entry &entry)
{
  QThread *worker = QThread::create([this, entry]() {
    int fetched = 0;
    const QString vroot = "characters/" + entry.name + "/";
    QStringList files;

    // GitHub raw links: one API call lists the folder.
    if (entry.base.host() == "raw.githubusercontent.com")
    {
      QStringList parts = entry.base.path().split('/', Qt::SkipEmptyParts);
      if (parts.size() >= 4)
      {
        const QString owner = parts.takeFirst();
        const QString repo = parts.takeFirst();
        if (parts.size() >= 2 && parts[0] == "refs" && parts[1] == "heads")
        {
          parts = parts.mid(2);
        }
        const QString branch = parts.takeFirst();
        const QUrl api(QString("https://api.github.com/repos/%1/%2/git/trees/%3?recursive=1").arg(owner, repo, QString::fromLatin1(QUrl::toPercentEncoding(branch + ":" + parts.join('/')))));
        QByteArray body;
        if (cdn_http_get(api, &body, 10000))
        {
          for (const QJsonValue &v : QJsonDocument::fromJson(body).object().value("tree").toArray())
          {
            const QJsonObject o = v.toObject();
            if (o.value("type").toString() == "blob")
            {
              files << o.value("path").toString();
            }
          }
        }
      }
    }
    if (!files.contains("char.ini", Qt::CaseInsensitive))
    {
      files.prepend("char.ini");
    }

    QByteArray ini;
    QMutex count_mutex;
    auto download_one = [&](const QString &rel) {
      const QString dest = cacheRoot() + vroot + rel;
      if (file_exists(dest))
      {
        return;
      }
      QStringList segs = rel.split('/');
      for (QString &s : segs)
      {
        s = QString::fromLatin1(QUrl::toPercentEncoding(s));
      }
      QByteArray data;
      if (!cdn_http_get(QUrl(entry.base.toString() + segs.join('/')), &data, 15000))
      {
        return;
      }
      QDir().mkpath(QFileInfo(dest).absolutePath());
      QFile f(dest);
      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        f.write(data);
        QMutexLocker lock(&count_mutex);
        ++fetched;
        if (rel.compare("char.ini", Qt::CaseInsensitive) == 0)
        {
          ini = data;
        }
      }
    };
    // A few plain threads rather than the global pool, which is a single
    // thread on small machines and would starve behind this task.
    {
      std::atomic<int> next{0};
      QList<QThread *> helpers;
      auto drain = [&]() {
        for (int i = next++; i < files.size(); i = next++)
        {
          download_one(files.at(i));
        }
      };
      for (int i = 0; i < 5; ++i)
      {
        helpers << QThread::create(drain);
        helpers.last()->start();
      }
      drain();
      for (QThread *t : helpers)
      {
        t->wait();
        delete t;
      }
    }
    if (ini.isEmpty())
    {
      QFile f(cacheRoot() + vroot + "char.ini");
      if (f.open(QIODevice::ReadOnly))
      {
        ini = f.readAll();
      }
    }
    prefetchSounds(entry, ini);
    Q_EMIT prefetchFinished(entry.name, fetched);
  });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

/// Pulls the sounds and blips char.ini refers to from the CDN's sounds/ tree.
void CdnCharacters::prefetchSounds(const Entry &, const QByteArray &ini)
{
  QStringList general;
  QString blips;
  QString section;
  for (QString line : QString::fromUtf8(ini).split('\n'))
  {
    line = line.trimmed();
    if (line.startsWith('['))
    {
      section = line.toLower();
      continue;
    }
    const int eq = line.indexOf('=');
    if (eq < 0)
    {
      continue;
    }
    const QString key = line.left(eq).trimmed().toLower();
    const QString val = line.mid(eq + 1).trimmed();
    if (section == "[soundn]" && !val.isEmpty())
    {
      general << val.toLower();
    }
    else if (section == "[options]" && (key == "blips" || key == "gender") && blips.isEmpty())
    {
      blips = val.toLower();
    }
  }
  for (const QString &name : general)
  {
    resolve("sounds/general/" + name, kSoundExts);
  }
  if (!blips.isEmpty())
  {
    resolve("sounds/blips/" + blips, kSoundExts);
  }
}
