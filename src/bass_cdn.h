#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#include <bass.h>
#include <bassopus.h>

static inline void CALLBACK cdn_free_buffer(HSYNC, DWORD, DWORD, void *user)
{
  delete static_cast<QByteArray *>(user);
}

static inline HSTREAM bass_create_stream(const QString &path, DWORD disk_flags)
{
  const bool opus = path.endsWith(".opus");
  if (path.startsWith("cdnfs:/"))
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
      return 0;
    }
    QByteArray *buffer = new QByteArray(file.readAll());
    const DWORD flags = disk_flags & ~(DWORD(BASS_UNICODE) | DWORD(BASS_ASYNCFILE));
    const HSTREAM stream = opus ? BASS_OPUS_StreamCreateFile(TRUE, buffer->constData(), 0, buffer->size(), flags) : BASS_StreamCreateFile(TRUE, buffer->constData(), 0, buffer->size(), flags);
    if (stream == 0)
    {
      delete buffer;
      return 0;
    }
    BASS_ChannelSetSync(stream, BASS_SYNC_FREE, 0, cdn_free_buffer, buffer);
    return stream;
  }
  return opus ? BASS_OPUS_StreamCreateFile(FALSE, path.utf16(), 0, 0, disk_flags) : BASS_StreamCreateFile(FALSE, path.utf16(), 0, 0, disk_flags);
}
