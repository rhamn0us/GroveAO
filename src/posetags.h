#pragma once

#include <QString>
#include <QVector>

#include <functional>

/**
 * Support for `<pose:N>` tags in IC messages.
 *
 * N is the number of an emote in the speaker's char.ini ([Emotions] N = ...),
 * the same number the emote button has. A tag swaps the speaker's pose in the
 * middle of a message, e.g. `Hello there. <pose:3>Oh, really?`
 */
namespace PoseTags
{
/**
 * @brief One `<pose:N>` marker found in a message.
 */
struct PoseTag
{
  /// Number of characters of the (tag-free) text that come before the marker
  int at = 0;
  /// Emote number from char.ini, starting at 1. 0 if the number was unusable.
  int pose = 0;

  bool operator==(const PoseTag &p_other) const { return at == p_other.at && pose == p_other.pose; }
};

struct ExtractResult
{
  QString text;
  QVector<PoseTag> tags;
};

/**
 * @brief One outgoing IC message produced by splitting on `<pose:N>` tags.
 */
struct Segment
{
  QString text;
  /// Emote number to use for this segment, or NO_POSE for the emote that is selected with the emote buttons.
  int pose = NO_POSE;

  static constexpr int NO_POSE = -1;

  bool operator==(const Segment &p_other) const { return text == p_other.text && pose == p_other.pose; }
};

/**
 * @brief Strips every `<pose:N>` tag out of a message and records where in the remaining text each one sat.
 *
 * Removing the tags before the text is displayed keeps the tick position of
 * the typing animation lined up with the tag positions, so a tag never shows up in the chatbox or the log.
 */
ExtractResult extract(const QString &p_text);

/**
 * @brief Splits a message you are about to send at its `<pose:N>` tags, so each part can be sent as its own IC message
 * with that pose's emote, preanim, sounds and frame effects.
 *
 * Tags for poses that don't exist (p_is_valid returns false) are removed but don't split anything.
 * A pose with no text after it (empty or whitespace only) is dropped, and whitespace is kept attached to the neighbouring text.
 * Returns an empty list when there is nothing left to say.
 */
QVector<Segment> split(const QString &p_text, const std::function<bool(int)> &p_is_valid);

/**
 * @brief Packs tags into a string ("at:pose,at:pose") so they can travel along with a queued chat message.
 */
QString serialize(const QVector<PoseTag> &p_tags);

/**
 * @brief Inverse of serialize(). Malformed entries are skipped.
 */
QVector<PoseTag> deserialize(const QString &p_data);
} // namespace PoseTags
