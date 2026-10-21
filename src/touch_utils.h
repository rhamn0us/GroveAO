#pragma once

#include <QStyledItemDelegate>

// Item delegate that guarantees a minimum row height so rows are easy to hit with a finger.
class TouchRowDelegate : public QStyledItemDelegate
{
public:
  explicit TouchRowDelegate(int min_height, QObject *parent = nullptr)
      : QStyledItemDelegate(parent)
      , m_min_height(min_height)
  {}

  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
  {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), m_min_height));
    return size;
  }

private:
  int m_min_height;
};
