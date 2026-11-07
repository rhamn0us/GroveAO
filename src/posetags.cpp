#include "posetags.h"

#include <QRegularExpression>

namespace PoseTags
{
ExtractResult extract(const QString &p_text)
{
  static const QRegularExpression pose_tag(QStringLiteral("<pose:\\s*(\\d+)\\s*>"), QRegularExpression::CaseInsensitiveOption);

  ExtractResult result;
  qsizetype last = 0;
  QRegularExpressionMatchIterator it = pose_tag.globalMatch(p_text);
  while (it.hasNext())
  {
    const QRegularExpressionMatch match = it.next();
    result.text += QStringView(p_text).mid(last, match.capturedStart() - last);

    bool ok = false;
    const int pose = match.captured(1).toInt(&ok);
    // A number too big for an int can't be a real emote, keep it as "invalid"
    result.tags.append({static_cast<int>(result.text.size()), ok ? pose : 0});

    last = match.capturedEnd();
  }
  result.text += QStringView(p_text).mid(last);
  return result;
}

QVector<Segment> split(const QString &p_text, const std::function<bool(int)> &p_is_valid)
{
  const ExtractResult extracted = extract(p_text);

  QVector<PoseTag> valid;
  for (const PoseTag &tag : extracted.tags)
  {
    if (p_is_valid(tag.pose))
    {
      valid.append(tag);
    }
  }

  // Cut the tag-free text at every valid tag
  QVector<Segment> pieces;
  int pos = 0;
  int pose = Segment::NO_POSE;
  for (const PoseTag &tag : std::as_const(valid))
  {
    pieces.append({extracted.text.mid(pos, tag.at - pos), pose});
    pos = tag.at;
    pose = tag.pose;
  }
  pieces.append({extracted.text.mid(pos), pose});

  // Drop pieces that say nothing. Their whitespace sticks to the text next to them, and
  // the pose of a dropped piece is handed on to the next piece that does say something.
  QVector<Segment> segments;
  int pending_pose = Segment::NO_POSE;
  QString carry;
  for (const Segment &piece : std::as_const(pieces))
  {
    if (piece.pose != Segment::NO_POSE)
    {
      pending_pose = piece.pose;
    }
    if (piece.text.trimmed().isEmpty())
    {
      if (!segments.isEmpty())
      {
        segments.last().text += piece.text;
      }
      else
      {
        carry += piece.text;
      }
      continue;
    }
    segments.append({carry + piece.text, pending_pose});
    carry.clear();
    pending_pose = Segment::NO_POSE;
  }
  return segments;
}

QString serialize(const QVector<PoseTag> &p_tags)
{
  QStringList parts;
  for (const PoseTag &tag : p_tags)
  {
    parts.append(QString::number(tag.at) + QLatin1Char(':') + QString::number(tag.pose));
  }
  return parts.join(QLatin1Char(','));
}

QVector<PoseTag> deserialize(const QString &p_data)
{
  QVector<PoseTag> tags;
  const QStringList parts = p_data.split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString &part : parts)
  {
    const QStringList numbers = part.split(QLatin1Char(':'));
    if (numbers.size() != 2)
    {
      continue;
    }
    bool at_ok = false;
    bool pose_ok = false;
    const int at = numbers.at(0).toInt(&at_ok);
    const int pose = numbers.at(1).toInt(&pose_ok);
    if (at_ok && pose_ok && at >= 0)
    {
      tags.append({at, pose});
    }
  }
  return tags;
}
} // namespace PoseTags
