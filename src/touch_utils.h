#pragma once

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QStyledItemDelegate>
#include <QTimer>

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

// Turns a long press into the same context-menu event a right-click produces, so every
// widget with Qt::CustomContextMenu works with a finger. Qt does not do this on Android
// by itself. When the long press fires, the widget's own click is cancelled.
//
// Combo boxes open their popup on press, which would swallow the long press, so for them
// the popup is opened on release instead.
class LongPressFilter : public QObject
{
public:
  explicit LongPressFilter(QWidget *target, int delay_ms = 450)
      : QObject(target)
      , m_target(target)
      , m_watched(target)
      , m_combo(qobject_cast<QComboBox *>(target))
  {
    if (auto *area = qobject_cast<QAbstractScrollArea *>(target))
    {
      m_watched = area->viewport(); // item views receive their mouse events on the viewport
    }

    m_timer.setSingleShot(true);
    m_timer.setInterval(delay_ms);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { fire(); });
    m_watched->installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override
  {
    if (watched != m_watched)
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
        m_moved = false;
        m_press_pos = mouse->position().toPoint();
        m_timer.start();
        if (m_combo != nullptr)
        {
          return true;
        }
      }
      break;
    }
    case QEvent::MouseMove:
    {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (m_timer.isActive() && (mouse->position().toPoint() - m_press_pos).manhattanLength() > QApplication::startDragDistance())
      {
        m_timer.stop(); // finger moved: not a long press
        m_moved = true;
      }
      if (m_combo != nullptr)
      {
        return true;
      }
      break;
    }
    case QEvent::MouseButtonRelease:
    {
      m_timer.stop();
      if (m_fired)
      {
        m_fired = false;
        return true; // swallow the release so the widget does not emit clicked()
      }
      if (m_combo != nullptr)
      {
        if (!m_moved && m_combo->isEnabled())
        {
          m_combo->showPopup();
        }
        return true;
      }
      break;
    }
    default:
      break;
    }
    return false;
  }

private:
  void fire()
  {
    m_fired = true;
    if (auto *button = qobject_cast<QAbstractButton *>(m_target))
    {
      button->setDown(false); // no click when the finger lifts
    }
    QContextMenuEvent menu_event(QContextMenuEvent::Mouse, m_press_pos, m_watched->mapToGlobal(m_press_pos));
    QCoreApplication::sendEvent(m_watched, &menu_event);
  }

  QWidget *m_target;
  QWidget *m_watched;
  QComboBox *m_combo;
  QTimer m_timer;
  QPoint m_press_pos;
  bool m_fired = false;
  bool m_moved = false;
};

// Call after setContextMenuPolicy(Qt::CustomContextMenu). Does nothing off Android.
inline void enable_long_press_context_menu(QWidget *widget)
{
#ifdef Q_OS_ANDROID
  new LongPressFilter(widget);
#else
  Q_UNUSED(widget);
#endif
}
