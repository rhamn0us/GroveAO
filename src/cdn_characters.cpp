#include "cdn_characters.h"

#include "file_functions.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QVector>

#include <atomic>

static const qint64 kNotFoundTtlMs = 5 * 60 * 1000;  // a real 404
static const qint64 kErrorTtlMs = 15 * 1000;         // timeouts, refused connections, 5xx: retry soon
static const qint64 kRamBudget = 64LL * 1024 * 1024; // finished small files only, RAM only, never disk
static const qint64 kRamMaxItem = 16LL * 1024 * 1024;
// The GUI thread must never sit on the network for long: one probe batch may hold it for at most
// kGuiWaitMs. After a timeout the probes keep running in the background (their answers land in the
// shared memory below) and the GUI thread does not wait on the network at all for kGuiBackoffMs.
static const qint64 kGuiWaitMs = 1500;
static const qint64 kGuiBackoffMs = 10 * 1000;
static std::atomic<qint64> g_gui_backoff_until{0};
static const qint64 kMaxStreamBytes = 512LL * 1024 * 1024; // sanity cap for a single stream

static bool allow_local_for_tests()
{
  return qEnvironmentVariableIsSet("GROVEAO_TEST_ALLOW_LOCAL_CDN");
}

namespace
{
// ---------------------------------------------------------------------------
// Small RAM-only memory of what we already know about a URL.
//   hits    : URL answered 200 once
//   misses  : URL answered 404 (long TTL) or failed (short TTL)
//   bodies  : files that finished streaming completely and are small, so re-opening
//             them (char.ini is re-read for every chat message) costs no network.
// Nothing here is ever written to disk.
// ---------------------------------------------------------------------------
struct Miss
{
  qint64 expiry = 0;
  int status = 0;
};

struct Ram
{
  QMutex mutex;
  QHash<QUrl, QByteArray> bodies;
  QList<QUrl> order;
  qint64 bytes = 0;
  QHash<QUrl, Miss> misses;
  QHash<QUrl, bool> hits;
  QSet<QUrl> inflight; // URLs a probe is currently asking about
};

Ram &ram()
{
  static Ram r;
  return r;
}

void ram_store(const QUrl &url, const QByteArray &data)
{
  if (data.size() > kRamMaxItem)
  {
    return;
  }
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

bool ram_lookup(const QUrl &url, QByteArray *out)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  const auto it = r.bodies.constFind(url);
  if (it == r.bodies.constEnd())
  {
    return false;
  }
  *out = it.value();
  return true;
}

// 200 / the recorded failure status / -1 when we know nothing (or the memory expired).
int cached_status(const QUrl &url)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  if (r.bodies.contains(url) || r.hits.value(url, false))
  {
    return 200;
  }
  const auto it = r.misses.constFind(url);
  if (it != r.misses.constEnd() && QDateTime::currentMSecsSinceEpoch() < it->expiry)
  {
    return it->status;
  }
  return -1;
}

void note_status(const QUrl &url, int status)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  r.inflight.remove(url);
  if (status == 200)
  {
    r.hits.insert(url, true);
    r.misses.remove(url);
    return;
  }
  const bool definitive = (status == 404 || status == 410);
  r.misses.insert(url, Miss{QDateTime::currentMSecsSinceEpoch() + (definitive ? kNotFoundTtlMs : kErrorTtlMs), status});
}

// True when the caller should start a probe for this URL; false when one is already running.
bool claim_probe(const QUrl &url)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  if (r.inflight.contains(url))
  {
    return false;
  }
  r.inflight.insert(url);
  return true;
}

void release_probe(const QUrl &url)
{
  Ram &r = ram();
  QMutexLocker lock(&r.mutex);
  r.inflight.remove(url);
}

bool on_gui_thread()
{
  QCoreApplication *app = QCoreApplication::instance();
  return app && QThread::currentThread() == app->thread();
}

// ---------------------------------------------------------------------------
// One dedicated network thread with ONE QNetworkAccessManager, so connections (and TLS
// sessions) are reused instead of rebuilding a manager for every probe. Callers on any
// thread hand work over and wait on a condition variable; nobody spins a nested event loop.
// ---------------------------------------------------------------------------
struct Net
{
  QThread thread;
  QObject *ctx = nullptr;
  QNetworkAccessManager *nam = nullptr;

  static Net &get()
  {
    static Net *n = new Net; // intentionally leaked: lives until process exit
    return *n;
  }

  Net()
  {
    thread.setObjectName("GroveAO CDN network");
    ctx = new QObject;
    ctx->moveToThread(&thread);
    thread.start();
    QMetaObject::invokeMethod(
        ctx, [this] { nam = new QNetworkAccessManager(ctx); }, Qt::BlockingQueuedConnection);
  }

  template <class F>
  void post(F f)
  {
    QMetaObject::invokeMethod(ctx, std::move(f), Qt::QueuedConnection);
  }
};

QNetworkRequest make_request(const QUrl &url, int timeout_ms)
{
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(timeout_ms);
  req.setHeader(QNetworkRequest::UserAgentHeader, "GroveAO");
  // Assets are already compressed; identity keeps Content-Length truthful so size() is exact.
  req.setRawHeader("Accept-Encoding", "identity");
  return req;
}

int status_of(QNetworkReply *r)
{
  return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Stop a transfer we no longer need. A response that is already a failure (404, 403, ...)
// has only a tiny error body left, and Qt warns when it is aborted because it reports the
// HTTP error right afterwards, so those are left to finish on their own.
void hang_up(QNetworkReply *r)
{
  if (r && r->isRunning() && r->error() == QNetworkReply::NoError && status_of(r) < 400)
  {
    r->abort();
  }
}

// ---------------------------------------------------------------------------
// Existence probes. Many candidate URLs are asked about at the same time, so resolving an
// asset costs one round trip instead of one round trip per suffix / folder / case variant.
// ---------------------------------------------------------------------------
struct Batch
{
  QMutex m;
  QWaitCondition cv;
  QVector<int> st;                          // -1 = not answered yet
  QVector<QPointer<QNetworkReply>> replies; // network thread only
  std::atomic<bool> cancel{false};
};

void batch_record(const std::shared_ptr<Batch> &b, int i, const QUrl &url, int status)
{
  if (b->cancel)
  {
    release_probe(url);
    return; // we aborted it ourselves; that says nothing about the URL
  }
  {
    QMutexLocker l(&b->m);
    if (b->st[i] != -1)
    {
      return;
    }
    b->st[i] = status;
  }
  note_status(url, status);
  b->cv.wakeAll();
}

// network thread
void start_probe(const std::shared_ptr<Batch> &b, int i, const QUrl &url, bool use_get)
{
  if (b->cancel)
  {
    release_probe(url);
    return;
  }
  Net &n = Net::get();
  QNetworkReply *r = use_get ? n.nam->get(make_request(url, 6000)) : n.nam->head(make_request(url, 6000));
  b->replies[i] = r;
  if (use_get)
  {
    // HEAD was refused by this server: ask with GET but only read the headers, then hang up.
    // The body is never downloaded.
    QObject::connect(r, &QNetworkReply::metaDataChanged, n.ctx, [b, i, url, r] {
      const int st = status_of(r);
      if (st > 0 && (st < 300 || st >= 400))
      {
        batch_record(b, i, url, st);
        hang_up(r);
      }
    });
  }
  QObject::connect(r, &QNetworkReply::finished, n.ctx, [b, i, url, r, use_get] {
    const int st = status_of(r);
    if (!use_get && st != 0 && st != 200 && st != 404 && !b->cancel)
    {
      start_probe(b, i, url, true);
    }
    else
    {
      batch_record(b, i, url, st);
    }
    r->deleteLater();
  });
}

// Returns one status per URL (-1 = unanswered). With stop_at_first the call returns as soon
// as the first URL in list order is known to exist and every URL before it is known to
// not exist, so list order is priority order.
QVector<int> probe_batch(const QList<QUrl> &urls, bool stop_at_first)
{
  const int n = urls.size();
  QVector<int> out(n, -1);
  QList<int> todo;
  for (int i = 0; i < n; ++i)
  {
    out[i] = cached_status(urls[i]);
    if (out[i] == -1)
    {
      todo << i;
    }
  }
  if (todo.isEmpty())
  {
    return out;
  }

  std::shared_ptr<Batch> b;
  auto value = [&](int i) {
    if (out[i] != -1)
    {
      return out[i];
    }
    if (b && b->st[i] != -1)
    {
      return b->st[i];
    }
    // Another caller may be asking about the same URL; its answer lands in the shared memory.
    return cached_status(urls[i]);
  };
  auto decided = [&]() {
    for (int i = 0; i < n; ++i)
    {
      const int v = value(i);
      if (v == -1)
      {
        if (stop_at_first)
        {
          return false; // an earlier-priority URL is still pending
        }
        continue;
      }
      if (v == 200 && stop_at_first)
      {
        return true;
      }
    }
    for (int i = 0; i < n; ++i)
    {
      if (value(i) == -1)
      {
        return false;
      }
    }
    return true;
  };

  if (decided())
  {
    return out;
  }

  b = std::make_shared<Batch>();
  b->st = QVector<int>(n, -1);
  b->replies.resize(n);
  QList<int> mine; // URLs nobody else is already asking about
  for (int i : todo)
  {
    if (claim_probe(urls[i]))
    {
      mine << i;
    }
  }
  if (!mine.isEmpty())
  {
    Net::get().post([b, urls, mine] {
      for (int i : mine)
      {
        start_probe(b, i, urls[i], false);
      }
    });
  }

  const bool gui = on_gui_thread();
  const bool backoff = gui && QDateTime::currentMSecsSinceEpoch() < g_gui_backoff_until.load();
  const qint64 limit = backoff ? 0 : (gui ? kGuiWaitMs : 30000);
  bool timed_out = false;
  {
    QElapsedTimer waited;
    waited.start();
    QMutexLocker l(&b->m);
    while (!decided())
    {
      if (waited.elapsed() >= limit)
      {
        timed_out = true;
        break;
      }
      b->cv.wait(&b->m, 20UL);
    }
    for (int i = 0; i < n; ++i)
    {
      out[i] = value(i);
    }
  }
  if (gui && timed_out)
  {
    // Leave the probes running: they fill the shared memory, so the next lookup is instant.
    if (!backoff)
    {
      g_gui_backoff_until = QDateTime::currentMSecsSinceEpoch() + kGuiBackoffMs;
    }
    return out;
  }
  b->cancel = true;
  Net::get().post([b, urls, mine] {
    for (const QPointer<QNetworkReply> &r : std::as_const(b->replies))
    {
      hang_up(r.data());
    }
    for (int i : mine)
    {
      release_probe(urls[i]);
    }
  });
  return out;
}
} // namespace

// ---------------------------------------------------------------------------
// CdnStream: progressive download that the consumer reads while it is still arriving.
// ---------------------------------------------------------------------------
void CdnStream::onHeaders(int status, qint64 length)
{
  {
    QMutexLocker l(&m_mutex);
    if (m_headers)
    {
      return;
    }
    m_status = status;
    m_total = length;
    m_headers = true;
  }
  note_status(url, status);
  m_cond.wakeAll();
}

void CdnStream::onData(const QByteArray &data)
{
  bool abort_now = false;
  {
    QMutexLocker l(&m_mutex);
    if (m_cancelled || data.isEmpty())
    {
      return;
    }
    m_buf.append(data);
    if (m_buf.size() > kMaxStreamBytes)
    {
      m_failed = true;
      abort_now = true;
    }
  }
  m_cond.wakeAll();
  if (abort_now)
  {
    cancel();
  }
}

void CdnStream::onFinished(bool ok)
{
  QByteArray copy;
  bool store = false;
  bool no_status = false;
  {
    QMutexLocker l(&m_mutex);
    m_finished = true;
    no_status = !m_headers;
    m_headers = true;
    if (!ok || m_status != 200 || (m_total >= 0 && m_buf.size() != m_total))
    {
      m_failed = true;
    }
    store = !m_failed && !m_cancelled;
    if (store)
    {
      copy = m_buf;
    }
  }
  if (no_status)
  {
    note_status(url, 0);
  }
  m_cond.wakeAll();
  if (store)
  {
    ram_store(url, copy);
  }
}

void CdnStream::completeFrom(const QByteArray &body)
{
  QMutexLocker l(&m_mutex);
  m_buf = body;
  m_status = 200;
  m_total = body.size();
  m_headers = true;
  m_finished = true;
}

void CdnStream::failWith(int status)
{
  QMutexLocker l(&m_mutex);
  m_status = status;
  m_headers = true;
  m_finished = true;
  m_failed = true;
}

bool CdnStream::waitOpen()
{
  QMutexLocker l(&m_mutex);
  while (!m_headers && !m_finished)
  {
    if (!m_cond.wait(&m_mutex, 30000UL))
    {
      m_failed = true;
      break;
    }
  }
  return m_headers && m_status == 200 && !m_failed;
}

qint64 CdnStream::size()
{
  QMutexLocker l(&m_mutex);
  if (m_total >= 0)
  {
    return m_total;
  }
  while (!m_finished)
  {
    if (!m_cond.wait(&m_mutex, 30000UL))
    {
      m_failed = true;
      break;
    }
  }
  return m_buf.size();
}

qint64 CdnStream::read(qint64 pos, char *dst, qint64 len)
{
  if (pos < 0 || len <= 0)
  {
    return 0;
  }
  QMutexLocker l(&m_mutex);
  qint64 want = len;
  if (m_total >= 0)
  {
    want = qMin(len, m_total - pos);
    if (want <= 0)
    {
      return 0;
    }
  }
  while (m_buf.size() < pos + want && !m_finished)
  {
    if (!m_cond.wait(&m_mutex, 30000UL))
    {
      m_failed = true;
      break;
    }
  }
  const qint64 avail = qMax<qint64>(0, qMin<qint64>(len, m_buf.size() - pos));
  if (avail == 0 && m_failed)
  {
    return -1;
  }
  memcpy(dst, m_buf.constData() + pos, size_t(avail));
  return avail;
}

void CdnStream::cancel()
{
  {
    QMutexLocker l(&m_mutex);
    if (m_finished || m_cancelled)
    {
      return;
    }
    m_cancelled = true;
  }
  auto self = shared_from_this();
  Net::get().post([self] { hang_up(qobject_cast<QNetworkReply *>(self->reply.data())); });
}

std::shared_ptr<CdnStream> cdn_open_stream(const QUrl &url)
{
  auto s = std::make_shared<CdnStream>();
  s->url = url;
  QByteArray cached;
  if (ram_lookup(url, &cached))
  {
    s->completeFrom(cached);
    return s;
  }
  const int known = cached_status(url);
  if (known != -1 && known != 200)
  {
    s->failWith(known); // recently answered 404 / failed: don't ask the network again yet
    return s;
  }
  Net::get().post([s, url] {
    Net &n = Net::get();
    QNetworkReply *r = n.nam->get(make_request(url, 20000));
    s->reply = r;
    QObject::connect(r, &QNetworkReply::metaDataChanged, n.ctx, [s, r] {
      const int st = status_of(r);
      if (st == 0 || (st >= 300 && st < 400))
      {
        return; // not the final response yet
      }
      const QVariant len = r->header(QNetworkRequest::ContentLengthHeader);
      s->onHeaders(st, len.isValid() ? len.toLongLong() : -1);
    });
    QObject::connect(r, &QNetworkReply::readyRead, n.ctx, [s, r] { s->onData(r->readAll()); });
    QObject::connect(r, &QNetworkReply::finished, n.ctx, [s, r] {
      if (r->isOpen())
      {
        s->onData(r->readAll()); // an aborted reply is closed and has nothing left to read
      }
      s->onFinished(r->error() == QNetworkReply::NoError);
      r->deleteLater();
    });
  });
  return s;
}

bool cdn_exists(const QUrl &url)
{
  return probe_batch({url}, false).value(0) == 200;
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
    if (QDateTime::currentMSecsSinceEpoch() < m_discover_miss.value(key, 0))
    {
      return {};
    }
  }
  // Ask every source about the character at the same time (exact spelling first, then
  // case variants only for sources that had no exact match): one round trip, not one per guess.
  const QStringList srcs = sources();
  QHash<int, QUrl> got;
  bool transport_error = false;
  bool pending = false; // some probe has not answered yet (GUI thread gave up waiting)
  auto discover = [&](bool variants) {
    QList<QUrl> cand;
    QList<QUrl> cand_base;
    QList<int> owner;
    for (int si = 0; si < srcs.size(); ++si)
    {
      if (got.contains(si))
      {
        continue;
      }
      for (const QString &v : variants ? caseVariants(name) : QStringList{name})
      {
        const QUrl base = QUrl(srcs[si] + "characters/" + enc_path(v) + "/");
        cand << QUrl(base.toString() + "char.ini");
        cand_base << base;
        owner << si;
      }
    }
    if (cand.isEmpty())
    {
      return;
    }
    const QVector<int> st = probe_batch(cand, false);
    for (int i = 0; i < cand.size(); ++i)
    {
      if (st[i] == -1)
      {
        pending = true;
      }
      else if (st[i] != 200 && st[i] != 404)
      {
        transport_error = true;
      }
      if (st[i] == 200 && !got.contains(owner[i]))
      {
        got.insert(owner[i], cand_base[i]);
      }
    }
  };
  discover(false);
  if (got.size() < srcs.size() && !pending)
  {
    discover(true);
  }
  QList<QUrl> found;
  for (int si = 0; si < srcs.size(); ++si)
  {
    if (got.contains(si))
    {
      found << got.value(si);
    }
  }
  if (pending)
  {
    return found; // incomplete answer: remember nothing, the next lookup finds the finished probes in memory
  }
  QMutexLocker lock(&m_mutex);
  if (found.isEmpty())
  {
    // A real 404 is remembered for minutes; a timeout / refused connection only briefly,
    // so one dropped packet no longer disables a character for five minutes.
    m_discover_miss.insert(key, QDateTime::currentMSecsSinceEpoch() + (transport_error ? kErrorTtlMs : kNotFoundTtlMs));
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
  // All candidates of a wave are asked at once; list order (suffix, then folder) is the
  // priority, exactly as before. Exact spelling is tried first, case variants only if nothing matched.
  bool pending = false;
  auto wave = [&](bool variants) -> QString {
    QList<QUrl> cand;
    for (const QString &suffix : suffixes)
    {
      for (const QUrl &dir : dirs)
      {
        const QString full = rel + suffix;
        for (const QString &r : variants ? caseVariants(full) : QStringList{full})
        {
          cand << QUrl(dir.toString() + enc_path(r));
        }
      }
    }
    if (cand.isEmpty())
    {
      return QString();
    }
    const QVector<int> st = probe_batch(cand, true);
    for (int i = 0; i < cand.size(); ++i)
    {
      if (st[i] == 200)
      {
        return toVirtualPath(cand[i]);
      }
      if (st[i] == -1)
      {
        pending = true; // a higher-priority candidate has not answered yet: do not settle for a lower one
        return QString();
      }
    }
    return QString();
  };
  const QString hit = wave(false);
  return (hit.isEmpty() && !pending) ? wave(true) : hit;
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
