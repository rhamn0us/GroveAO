#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QStyledItemDelegate>
#include <QTimer>

#include <functional>

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

// Calls a function when a button is held down for a while, like a right-click on desktop.
// Qt does not turn a long press into a context menu on Android by itself. When the long
// press fires, the button's own click is cancelled so releasing the finger does nothing else.
class LongPressFilter : public QObject
{
public:
  LongPressFilter(QAbstractButton *target, std::function<void(const QPoint &)> callback, int delay_ms = 450)
      : QObject(target)
      , m_target(target)
      , m_callback(std::move(callback))
  {
    m_timer.setSingleShot(true);
    m_timer.setInterval(delay_ms);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
      m_fired = true;
      m_target->setDown(false); // no click when the finger lifts
      m_callback(m_press_pos);
    });
    target->installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override
  {
    if (watched != m_target)
    {
      return false;
    }

    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton)
      {
        m_fired = false;
        m_press_pos = mouse->position().toPoint();
        m_timer.start();
      }
      break;
    }
    case QEvent::MouseMove:
    {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (m_timer.isActive() && (mouse->position().toPoint() - m_press_pos).manhattanLength() > QApplication::startDragDistance())
      {
        m_timer.stop(); // finger moved: not a long press
      }
      break;
    }
    case QEvent::MouseButtonRelease:
      m_timer.stop();
      if (m_fired)
      {
        m_fired = false;
        return true; // swallow the release so the button does not emit clicked()
      }
      break;
    default:
      break;
    }
    return false;
  }

private:
  QAbstractButton *m_target;
  std::function<void(const QPoint &)> m_callback;
  QTimer m_timer;
  QPoint m_press_pos;
  bool m_fired = false;
};
