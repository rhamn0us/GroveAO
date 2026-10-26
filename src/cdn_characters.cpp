#include "cdn_characters.h"

#include "file_functions.h"

#include <QDateTime>
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

static const qint64 kMissTtlMs = 5 * 60 * 1000;
static const qint64 kRamBudget = 256LL * 1024 * 1024;

static bool allow_local_for_tests()
{
  return qEnvironmentVariableIsSet("GROVEAO_TEST_ALLOW_LOCAL_CDN");
}

static int http_request(const QUrl &url, bool head, QByteArray *body, int timeout_ms = 5000)
{
  QNetworkAccessManager nam;
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(timeout_ms);
  req.setHeader(QNetworkRequest::UserAgentHeader, "GroveAO");
  QNetworkReply *reply = head ? nam.head(req) : nam.get(req);
  QEventLoop loop;
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (body && status == 200)
  {
    *body = reply->readAll();
  }
  reply->deleteLater();
  return status;
}

namespace
{
struct Ram
{
  QMutex mutex;
  QHash<QUrl, QByteArray> bodies;
  QList<QUrl> order;
  qint64 bytes = 0;
  QHash<QUrl, qint64> misses;
  QHash<QUrl, bool> hits;
};

Ram &ram()
{
  static Ram r;
  return r;
}

void ram_store(const QUrl &url, const QByteArray &data)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  if (r.bodies.contains(url))
  {
    return;
  }
  r.bodies.insert(url, data);
  r.order.append(url);
  r.bytes += data.size();
  while (r.bytes > kRamBudget && r.order.size() > 1)
  {
    const QUrl old = r.order.takeFirst();
    r.bytes -= r.bodies.take(old).size();
  }
}
} // namespace

bool cdn_exists(const QUrl &url)
{
  Ram &r = ram();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  {
    QMutexLocker lock(&r.mutex);
    if (r.bodies.contains(url) || r.hits.value(url, false))
    {
      return true;
    }
    if (r.misses.contains(url) && now - r.misses.value(url) < kMissTtlMs)
    {
      return false;
    }
  }
  int status = http_request(url, true, nullptr);
  if (status != 200 && status != 404 && status != 0)
  {
    QByteArray body;
    status = http_request(url, false, &body, 8000);
    if (status == 200)
    {
      ram_store(url, body);
    }
  }
  QMutexLocker lock(&r.mutex);
  if (status == 200)
  {
    r.hits.insert(url, true);
    return true;
  }
  r.misses.insert(url, now);
  return false;
}

QByteArray cdn_fetch(const QUrl &url, bool *ok)
{
  Ram &r = ram();
  {
    QMutexLocker lock(&r.mutex);
    if (r.bodies.contains(url))
    {
      *ok = true;
      return r.bodies.value(url);
    }
  }
  QByteArray body;
  const bool good = http_request(url, false, &body, 15000) == 200;
  *ok = good;
  if (good)
  {
    ram_store(url, body);
  }
  return body;
}

QString CdnCharacters::toVirtualPath(const QUrl &url)
{
  QString host = url.host();
  if (url.port() > 0)
  {
    host += ":" + QString::number(url.port());
  }
  return "cdnfs:/" + url.scheme() + "/" + host + url.path();
}

bool CdnCharacters::fromVirtualPath(const QString &path, QUrl *url)
{
  static const QRegularExpression re(R"(^cdnfs:/(https?)/([^/]+)(/.*)$)");
  const QRegularExpressionMatch m = re.match(path);
  if (!m.hasMatch())
  {
    return false;
  }
  if (m.captured(1) == "http" && !allow_local_for_tests())
  {
    return false;
  }
  QStringList segs = m.captured(3).split('/');
  for (QString &s : segs)
  {
    s = QString::fromLatin1(QUrl::toPercentEncoding(s));
  }
  QUrl u = QUrl::fromEncoded(QString("%1://%2%3").arg(m.captured(1), m.captured(2), segs.join('/')).toLatin1());
  if (!u.isValid() || !isSafeHost(u.host()))
  {
    return false;
  }
  *url = u;
  return true;
}

bool CdnCharacters::isSafeHost(const QString &host_in)
{
  if (allow_local_for_tests())
  {
    return true;
  }
  const QString host = host_in.toLower();
  if (host.isEmpty() || host == "localhost" || host.endsWith(".local") || host.endsWith(".internal"))
  {
    return false;
  }
  if (host.contains(':'))
  {
    return false;
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
  if (!allow_local_for_tests())
  {
    url.setScheme("https");
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
  QStringList a;
  QStringList b;
  QStringList c;
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

CdnCharacters &CdnCharacters::instance()
{
  static CdnCharacters inst;
  return inst;
}

CdnCharacters::CdnCharacters()
{
  load();
}

QString CdnCharacters::sourcesFile() const
{
  return get_base_path() + "cdn_sources.txt";
}

QStringList CdnCharacters::sources() const
{
  QStringList out;
  QFile f(sourcesFile());
  if (!f.exists())
  {
    return {"https://mastyra.com/base/", "https://raw.githubusercontent.com/OfficialLawyerEmporium/OLEBase/refs/heads/master/base/"};
  }
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    return out;
  }
  for (QString line : QString::fromUtf8(f.readAll()).split('\n'))
  {
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith('#'))
    {
      continue;
    }
    QUrl base;
    if (normalize(line, &base, nullptr, nullptr))
    {
      out << base.toString();
    }
  }
  return out;
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

void CdnCharacters::registerEntry(const QString &name, const QUrl &base, bool persist)
{
  QMutexLocker lock(&m_mutex);
  bool found = false;
  for (Entry &e : m_entries)
  {
    if (e.name.compare(name, Qt::CaseInsensitive) == 0)
    {
      e.base = base;
      found = true;
      break;
    }
  }
  if (!found)
  {
    m_entries.append({name, base});
  }
  m_discover_miss.remove(name.toLower());
  if (persist)
  {
    save();
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
  registerEntry(name, base, true);
  if (name_out)
  {
    *name_out = name;
  }
  return true;
}

void CdnCharacters::remove(const QString &name)
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
  m_discovered.remove(name.toLower());
  save();
}

QString CdnCharacters::ingestName(const QString &raw)
{
  const int idx = raw.indexOf("~cdn~");
  if (idx < 0)
  {
    return raw;
  }
  const QString name = raw.left(idx);
  QString loc = raw.mid(idx + 5);
  if (loc.startsWith('!'))
  {
    loc.remove(0, 1);
  }
  QUrl base;
  if (!name.isEmpty() && normalize("https://" + loc, &base, nullptr, nullptr))
  {
    registerEntry(name, base, false);
  }
  return name.isEmpty() ? raw : name;
}

static QString enc_path(const QString &rel)
{
  QStringList segs = rel.split('/');
  for (QString &s : segs)
  {
    s = QString::fromLatin1(QUrl::toPercentEncoding(s));
  }
  return segs.join('/');
}

QList<QUrl> CdnCharacters::characterBases(const QString &name, const std::function<bool(const QString &)> &is_local)
{
  const QString key = name.toLower();
  if (is_local(name))
  {
    return {};
  }
  QList<QUrl> bases;
  {
    QMutexLocker lock(&m_mutex);
    for (const Entry &e : m_entries)
    {
      if (e.name.compare(name, Qt::CaseInsensitive) == 0)
      {
        bases << e.base;
      }
    }
    bases << m_discovered.value(key);
    if (!bases.isEmpty())
    {
      return bases;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_discover_miss.contains(key) && now - m_discover_miss.value(key) < kMissTtlMs)
    {
      return {};
    }
  }
  QList<QUrl> found;
  for (const QString &src : sources())
  {
    for (const QString &variant : QStringList{name} + caseVariants(name))
    {
      const QUrl base = QUrl(src + "characters/" + enc_path(variant) + "/");
      if (cdn_exists(QUrl(base.toString() + "char.ini")))
      {
        found << base;
        break;
      }
    }
  }
  QMutexLocker lock(&m_mutex);
  if (found.isEmpty())
  {
    m_discover_miss.insert(key, QDateTime::currentMSecsSinceEpoch());
  }
  else
  {
    m_discovered.insert(key, found);
  }
  return found;
}

QList<QUrl> CdnCharacters::soundRoots() const
{
  QList<QUrl> roots;
  auto add_root = [&](const QUrl &char_base) {
    const QUrl root = char_base.resolved(QUrl("../../"));
    if (!roots.contains(root))
    {
      roots << root;
    }
  };
  QMutexLocker lock(&m_mutex);
  for (const Entry &e : m_entries)
  {
    add_root(e.base);
  }
  for (const QList<QUrl> &list : m_discovered)
  {
    for (const QUrl &b : list)
    {
      add_root(b);
    }
  }
  return roots;
}

QString CdnCharacters::probe(const QList<QUrl> &dirs, const QString &rel, const QStringList &suffixes)
{
  for (const QString &suffix : suffixes)
  {
    for (const QUrl &dir : dirs)
    {
      for (const QString &r : QStringList{rel + suffix} + caseVariants(rel + suffix))
      {
        const QUrl url(dir.toString() + enc_path(r));
        if (cdn_exists(url))
        {
          return toVirtualPath(url);
        }
      }
    }
  }
  return QString();
}

QString CdnCharacters::resolve(const QString &vpath, const QStringList &suffixes, const std::function<bool(const QString &)> &is_local)
{
  if (vpath.isEmpty() || vpath.startsWith("http") || vpath.startsWith("cdnfs:"))
  {
    return QString();
  }
  static const QRegularExpression charRe(R"(^characters/([^/]+)/(.+)$)", QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression soundRe(R"(^sounds/([^/]+)/(.+)$)", QRegularExpression::CaseInsensitiveOption);

  const QRegularExpressionMatch cm = charRe.match(vpath);
  if (cm.hasMatch())
  {
    const QList<QUrl> bases = characterBases(cm.captured(1), is_local);
    return bases.isEmpty() ? QString() : probe(bases, cm.captured(2), suffixes);
  }
  const QRegularExpressionMatch sm = soundRe.match(vpath);
  if (sm.hasMatch())
  {
    QList<QUrl> dirs;
    for (const QUrl &root : soundRoots())
    {
      dirs << QUrl(root.toString() + "sounds/" + enc_path(sm.captured(1)) + "/");
    }
    return dirs.isEmpty() ? QString() : probe(dirs, sm.captured(2), suffixes);
  }
  return QString();
}
