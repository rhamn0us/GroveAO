#pragma once

#include "cdn_characters.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QUrl>

#include <bass.h>
#include <bassopus.h>

// Creates a BASS stream for a resolved asset path.
//  - Local files go through BASS_StreamCreateFile as before.
//  - CDN assets (cdnfs:/ virtual paths) are streamed straight from their URL by BASS itself:
//    playback starts once BASS has pre-buffered the first part, the rest keeps arriving while
//    it plays, and nothing is downloaded up front or written to disk.
static inline HSTREAM bass_create_stream(const QString &path, DWORD disk_flags)
{
  const bool opus = path.endsWith(".opus");
  if (path.startsWith("cdnfs:/"))
  {
    QUrl url;
    if (!CdnCharacters::fromVirtualPath(path, &url))
    {
      return 0;
    }
    // BASS_UNICODE / BASS_ASYNCFILE describe local file paths; the URL is passed as plain
    // percent-encoded ASCII.
    const DWORD flags = disk_flags & ~(DWORD(BASS_UNICODE) | DWORD(BASS_ASYNCFILE));
    const QByteArray encoded = url.toEncoded();
    return opus ? BASS_OPUS_StreamCreateURL(encoded.constData(), 0, flags, nullptr, nullptr) : BASS_StreamCreateURL(encoded.constData(), 0, flags, nullptr, nullptr);
  }
  return opus ? BASS_OPUS_StreamCreateFile(FALSE, path.utf16(), 0, 0, disk_flags) : BASS_StreamCreateFile(FALSE, path.utf16(), 0, 0, disk_flags);
}
