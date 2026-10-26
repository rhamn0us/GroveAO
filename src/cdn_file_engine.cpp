#include "cdn_characters.h"

#include <QtCore/private/qabstractfileengine_p.h>

#include <QFile>

#include <optional>

class CdnFileEngine : public QAbstractFileEngine
{
public:
  explicit CdnFileEngine(const QString &path) { setFileName(path); }

  void setFileName(const QString &file) override
  {
    m_path = file;
    m_valid = CdnCharacters::fromVirtualPath(file, &m_url);
    m_open = false;
    m_data.clear();
    m_pos = 0;
  }

  bool open(QIODevice::OpenMode mode, std::optional<QFile::Permissions> = std::nullopt) override
  {
    if (!m_valid || (mode & (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Truncate)))
    {
      return false;
    }
    bool ok = false;
    m_data = cdn_fetch(m_url, &ok);
    m_open = ok;
    m_pos = 0;
    return ok;
  }

  bool close() override
  {
    m_open = false;
    m_data.clear();
    return true;
  }

  bool flush() override { return true; }
  bool syncToDisk() override { return true; }
  qint64 size() const override { return m_open ? m_data.size() : 0; }
  qint64 pos() const override { return m_pos; }

  bool seek(qint64 pos) override
  {
    if (!m_open || pos < 0 || pos > m_data.size())
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
    const qint64 n = qMin<qint64>(maxlen, m_data.size() - m_pos);
    if (n <= 0)
    {
      return 0;
    }
    memcpy(data, m_data.constData() + m_pos, size_t(n));
    m_pos += n;
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
  QString m_path;
  QUrl m_url;
  QByteArray m_data;
  qint64 m_pos = 0;
  bool m_valid = false;
  bool m_open = false;
};

class CdnFileEngineHandler : public QAbstractFileEngineHandler
{
public:
  QAbstractFileEngine *create(const QString &fileName) const override { return fileName.startsWith("cdnfs:/") ? new CdnFileEngine(fileName) : nullptr; }
};

void cdn_install_file_engine()
{
  static CdnFileEngineHandler handler;
  (void)handler;
}
