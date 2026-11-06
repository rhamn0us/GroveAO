#include "cdn_characters.h"

#include <QtCore/private/qabstractfileengine_p.h>

#include <QFile>

#include <QtGlobal>

#include <memory>
#include <optional>

class CdnFileEngine : public QAbstractFileEngine
{
public:
  explicit CdnFileEngine(const QString &path) { setFileName(path); }
  ~CdnFileEngine() override { releaseStream(); }

  void setFileName(const QString &file) override
  {
    releaseStream();
    m_path = file;
    m_valid = CdnCharacters::fromVirtualPath(file, &m_url);
    m_open = false;
    m_pos = 0;
  }

  bool open(QIODevice::OpenMode mode, std::optional<QFile::Permissions> = std::nullopt) override
  {
    if (!m_valid || (mode & (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Truncate)))
    {
      return false;
    }
    // Start streaming and return as soon as the server has answered. The body keeps
    // arriving in the background and read() hands it out as it comes in.
    releaseStream();
    m_stream = cdn_open_stream(m_url);
    m_open = m_stream->waitOpen();
    if (!m_open)
    {
      releaseStream();
    }
    m_pos = 0;
    return m_open;
  }

  bool close() override
  {
    m_open = false;
    releaseStream();
    return true;
  }

  bool flush() override { return true; }
  bool syncToDisk() override { return true; }
  qint64 size() const override { return m_open ? m_stream->size() : 0; }
  qint64 pos() const override { return m_pos; }

  bool seek(qint64 pos) override
  {
    // Seeking ahead of what has arrived is fine: the next read waits for those bytes.
    if (!m_open || pos < 0)
    {
      return false;
    }
    m_pos = pos;
    return true;
  }

  bool isSequential() const override { return false; }
  bool caseSensitive() const override { return true; }
  bool isRelativePath() const override { return false; }

  qint64 read(char *data, qint64 maxlen) override
  {
    if (!m_open)
    {
      return -1;
    }
    const qint64 n = m_stream->read(m_pos, data, maxlen);
    if (n > 0)
    {
      m_pos += n;
    }
    return n;
  }

  qint64 write(const char *, qint64) override { return -1; }

  FileFlags fileFlags(FileFlags type = FileInfoAll) const override
  {
    FileFlags flags;
    if (m_valid && (m_open || cdn_exists(m_url)))
    {
      flags = ExistsFlag | FileType | ReadOwnerPerm | ReadUserPerm | ReadGroupPerm | ReadOtherPerm;
    }
    return flags & type;
  }

  QString fileName(FileName file = DefaultName) const override
  {
    switch (file)
    {
    case BaseName:
      return m_path.mid(m_path.lastIndexOf('/') + 1);
    case PathName:
    case AbsolutePathName:
    case CanonicalPathName:
      return m_path.left(m_path.lastIndexOf('/'));
    default:
      return m_path;
    }
  }

  QByteArray id() const override { return m_path.toUtf8(); }

private:
  void releaseStream()
  {
    if (m_stream)
    {
      m_stream->cancel(); // no-op when the transfer already finished
      m_stream.reset();
    }
  }

  QString m_path;
  QUrl m_url;
  std::shared_ptr<CdnStream> m_stream;
  qint64 m_pos = 0;
  bool m_valid = false;
  bool m_open = false;
};

class CdnFileEngineHandler : public QAbstractFileEngineHandler
{
public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  // Qt 6.8+ changed the private handler API to fucking fix an owning pointer.
  std::unique_ptr<QAbstractFileEngine> create(const QString &fileName) const override
  {
    if (!fileName.startsWith("cdnfs:/"))
      return nullptr;
    return std::make_unique<CdnFileEngine>(fileName);
  }
#else
  QAbstractFileEngine *create(const QString &fileName) const override { return fileName.startsWith("cdnfs:/") ? new CdnFileEngine(fileName) : nullptr; }
#endif
};

void cdn_install_file_engine()
{
  static CdnFileEngineHandler handler;
  (void)handler;
}
