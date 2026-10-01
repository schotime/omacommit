#include "Picker.h"
#include "Theme.h"

#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int IndexRole = Qt::UserRole + 1;
constexpr int DetailRole = Qt::UserRole + 2;

// The name on the left as usual, its detail muted and right-aligned.
class PickerDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString detail = index.data(DetailRole).toString();
        QStyleOptionViewItem o(option);
        initStyleOption(&o, index);
        const QWidget *w = o.widget;
        // The row's background in full, then the name in what the detail leaves.
        QStyleOptionViewItem bg(o);
        bg.text.clear();
        w->style()->drawControl(QStyle::CE_ItemViewItem, &bg, p, w);
        const int detailWidth = detail.isEmpty() ? 0 : qMin(o.fontMetrics.horizontalAdvance(detail) + 16, o.rect.width() / 2);
        QStyleOptionViewItem name(o);
        name.rect.setRight(name.rect.right() - detailWidth);
        w->style()->drawControl(QStyle::CE_ItemViewItem, &name, p, w);
        if (detailWidth) {
            const ThemeColors &c = Theme::instance().colors();
            p->save();
            p->setPen(option.state & QStyle::State_Selected ? option.palette.color(QPalette::HighlightedText) : c.muted);
            const QRect r = o.rect.adjusted(o.rect.width() - detailWidth, 0, -8, 0);
            p->drawText(r, Qt::AlignVCenter | Qt::AlignRight,
                        o.fontMetrics.elidedText(detail, Qt::ElideLeft, r.width()));
            p->restore();
        }
    }
};

class PickerDialog : public QDialog {
public:
    PickerDialog(QWidget *window, const QString &placeholder, const QVector<PickerItem> &items)
        : QDialog(window, Qt::Popup), m_items(items)
    {
        setObjectName(QStringLiteral("picker"));
        m_search = new QLineEdit;
        m_search->setPlaceholderText(placeholder);
        m_search->setClearButtonEnabled(true);
        m_search->installEventFilter(this);
        m_list = new QListWidget;
        m_list->setItemDelegate(new PickerDelegate(m_list));
        m_list->setUniformItemSizes(true);
        m_list->setFocusPolicy(Qt::NoFocus);   // typing always goes to the search
        m_list->setMouseTracking(true);
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(6, 6, 6, 6);
        l->setSpacing(6);
        l->addWidget(m_search);
        l->addWidget(m_list);

        connect(m_search, &QLineEdit::textChanged, this, &PickerDialog::filter);
        connect(m_search, &QLineEdit::returnPressed, this, &PickerDialog::pick);
        connect(m_list, &QListWidget::itemClicked, this, &PickerDialog::pick);
        filter();

        // Near the top of the window, centred, like a command palette.
        const int w = qMin(620, qMax(320, window->width() - 80));
        const int rowH = qMax(m_list->sizeHintForRow(0), fontMetrics().height() + 8);
        const int rows = qBound(4, int(m_items.size()), 12);
        resize(w, m_search->sizeHint().height() + rows * rowH + 24);
        move(window->mapToGlobal(QPoint((window->width() - w) / 2, window->height() / 8)));
        m_search->setFocus();
    }

    int picked() const { return m_picked; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_search && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
                const int step = key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : key == Qt::Key_PageUp ? -8 : 8;
                const int n = m_list->count();
                if (n)
                    m_list->setCurrentRow(qBound(0, m_list->currentRow() + step, n - 1));
                return true;
            }
        }
        return QDialog::eventFilter(watched, event);
    }

private:
    void filter()
    {
        const QStringList words = m_search->text().split(u' ', Qt::SkipEmptyParts);
        m_list->clear();
        // Matches on the name before matches only in the detail; otherwise in the given order.
        QVector<int> byName, byDetail, always;
        for (int i = 0; i < m_items.size(); ++i) {
            const PickerItem &it = m_items.at(i);
            if (it.always) {
                always << i;
                continue;
            }
            const QString hay = it.text + u' ' + it.detail;
            const bool all = std::all_of(words.begin(), words.end(), [&](const QString &w) {
                return hay.contains(w, Qt::CaseInsensitive);
            });
            if (!all)
                continue;
            const bool inName = std::all_of(words.begin(), words.end(), [&](const QString &w) {
                return it.text.contains(w, Qt::CaseInsensitive);
            });
            (inName ? byName : byDetail) << i;
        }
        for (const QVector<int> *group : {&byName, &byDetail, &always})
            for (int i : *group) {
                auto *row = new QListWidgetItem(m_items.at(i).text, m_list);
                row->setData(IndexRole, i);
                row->setData(DetailRole, m_items.at(i).detail);
                row->setToolTip(m_items.at(i).detail);
            }
        if (m_list->count())
            m_list->setCurrentRow(0);
    }

    void pick()
    {
        if (QListWidgetItem *it = m_list->currentItem()) {
            m_picked = it->data(IndexRole).toInt();
            accept();
        }
    }

    QVector<PickerItem> m_items;
    QLineEdit *m_search;
    QListWidget *m_list;
    int m_picked = -1;
};
} // namespace

int Picker::choose(QWidget *window, const QString &placeholder, const QVector<PickerItem> &items)
{
    PickerDialog dlg(window->window(), placeholder, items);
    return dlg.exec() == QDialog::Accepted ? dlg.picked() : -1;
}
