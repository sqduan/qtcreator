/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "locator.h"
#include "locatorwidget.h"
#include "locatorconstants.h"
#include "locatorsearchutils.h"
#include "ilocatorfilter.h"

#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/fileiconprovider.h>
#include <coreplugin/find/searchresulttreeitemdelegate.h>
#include <coreplugin/find/searchresulttreeitemroles.h>
#include <coreplugin/icontext.h>
#include <coreplugin/mainwindow.h>
#include <utils/algorithm.h>
#include <utils/appmainwindow.h>
#include <utils/asconst.h>
#include <utils/fancylineedit.h>
#include <utils/hostosinfo.h>
#include <utils/itemviews.h>
#include <utils/progressindicator.h>
#include <utils/qtcassert.h>
#include <utils/runextensions.h>
#include <utils/stylehelper.h>
#include <utils/utilsicons.h>

#include <QColor>
#include <QFileInfo>
#include <QTimer>
#include <QEvent>
#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTreeView>
#include <QToolTip>

Q_DECLARE_METATYPE(Core::LocatorFilterEntry)

namespace Core {
namespace Internal {

/* A model to represent the Locator results. */
class LocatorModel : public QAbstractListModel
{
public:

    enum Columns {
        DisplayNameColumn,
        ExtraInfoColumn,
        ColumnCount
    };

    LocatorModel(QObject *parent = 0)
        : QAbstractListModel(parent)
        , mBackgroundColor(Utils::creatorTheme()->color(Utils::Theme::TextColorHighlightBackground).name())
    {}

    void clear();
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void addEntries(const QList<LocatorFilterEntry> &entries);

private:
    mutable QList<LocatorFilterEntry> mEntries;
    bool hasExtraInfo = false;
    QColor mBackgroundColor;
};

class CompletionList : public Utils::TreeView
{
public:
    CompletionList(QWidget *parent = 0);

    void resize();
    void resizeHeaders();
    QSize preferredSize() const { return m_preferredSize; }

    void focusOutEvent (QFocusEvent *event) {
        if (event->reason() == Qt::ActiveWindowFocusReason)
            hide();
        QTreeView::focusOutEvent(event);
    }

    void next() {
        int index = currentIndex().row();
        ++index;
        if (index >= model()->rowCount(QModelIndex())) {
            // wrap
            index = 0;
        }
        setCurrentIndex(model()->index(index, 0));
    }

    void previous() {
        int index = currentIndex().row();
        --index;
        if (index < 0) {
            // wrap
            index = model()->rowCount(QModelIndex()) - 1;
        }
        setCurrentIndex(model()->index(index, 0));
    }

private:
    QSize m_preferredSize;
};

// =========== LocatorModel ===========

void LocatorModel::clear()
{
    beginResetModel();
    mEntries.clear();
    hasExtraInfo = false;
    endResetModel();
}

int LocatorModel::rowCount(const QModelIndex & parent) const
{
    if (parent.isValid())
        return 0;
    return mEntries.size();
}

int LocatorModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return hasExtraInfo ? ColumnCount : 1;
}

QVariant LocatorModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= mEntries.size())
        return QVariant();

    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == DisplayNameColumn)
            return mEntries.at(index.row()).displayName;
        else if (index.column() == ExtraInfoColumn)
            return mEntries.at(index.row()).extraInfo;
        break;
    case Qt::ToolTipRole:
        if (mEntries.at(index.row()).extraInfo.isEmpty())
            return QVariant(mEntries.at(index.row()).displayName);
        else
            return QVariant(mEntries.at(index.row()).displayName
                            + QLatin1String("\n\n") + mEntries.at(index.row()).extraInfo);
        break;
    case Qt::DecorationRole:
    case ItemDataRoles::ResultIconRole:
        if (index.column() == DisplayNameColumn) {
            LocatorFilterEntry &entry = mEntries[index.row()];
            if (!entry.displayIcon && !entry.fileName.isEmpty())
                entry.displayIcon = FileIconProvider::icon(entry.fileName);
            return entry.displayIcon ? entry.displayIcon.value() : QIcon();
        }
        break;
    case Qt::ForegroundRole:
        if (index.column() == ExtraInfoColumn)
            return QColor(Qt::darkGray);
        break;
    case ItemDataRoles::ResultItemRole:
        return qVariantFromValue(mEntries.at(index.row()));
    case ItemDataRoles::ResultBeginColumnNumberRole:
    case ItemDataRoles::SearchTermLengthRole: {
        LocatorFilterEntry &entry = mEntries[index.row()];
        const int highlightColumn = entry.highlightInfo.dataType == LocatorFilterEntry::HighlightInfo::DisplayName
                                                                 ? DisplayNameColumn
                                                                 : ExtraInfoColumn;
        if (highlightColumn == index.column()) {
            const bool startIndexRole = role == ItemDataRoles::ResultBeginColumnNumberRole;
            return startIndexRole ? entry.highlightInfo.startIndex : entry.highlightInfo.length;
        }
        break;
    }
    case ItemDataRoles::ResultHighlightBackgroundColor:
        return mBackgroundColor;
    }

    return QVariant();
}

void LocatorModel::addEntries(const QList<LocatorFilterEntry> &entries)
{
    beginInsertRows(QModelIndex(), mEntries.size(), mEntries.size() + entries.size() - 1);
    mEntries.append(entries);
    endInsertRows();
    if (hasExtraInfo)
        return;
    if (Utils::anyOf(entries, [](const LocatorFilterEntry &e) { return !e.extraInfo.isEmpty();})) {
        beginInsertColumns(QModelIndex(), 1, 1);
        hasExtraInfo = true;
        endInsertColumns();
    }
}

// =========== CompletionList ===========

CompletionList::CompletionList(QWidget *parent)
    : Utils::TreeView(parent)
{
    setItemDelegate(new SearchResultTreeItemDelegate(0, this));
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    header()->hide();
    header()->setStretchLastSection(true);
    // This is too slow when done on all results
    //header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    setWindowFlags(Qt::ToolTip);
    if (Utils::HostOsInfo::isMacHost()) {
        if (horizontalScrollBar())
            horizontalScrollBar()->setAttribute(Qt::WA_MacMiniSize);
        if (verticalScrollBar())
            verticalScrollBar()->setAttribute(Qt::WA_MacMiniSize);
    }
}

void CompletionList::resize()
{
    const QStyleOptionViewItem &option = viewOptions();
    const QSize shint = itemDelegate()->sizeHint(option, model()->index(0, 0));
    const QSize windowSize = ICore::mainWindow()->size();

    const int width = qMax(730, windowSize.width() * 2 / 3);
    m_preferredSize = QSize(width, shint.height() * 17 + frameWidth() * 2);
    QTreeView::resize(m_preferredSize);
    resizeHeaders();
}

void CompletionList::resizeHeaders()
{
    header()->resizeSection(0, m_preferredSize.width() / 2);
    header()->resizeSection(1, 0); // last section is auto resized because of stretchLastSection
}

// =========== LocatorWidget ===========

LocatorWidget::LocatorWidget(Locator *locator) :
    m_locatorModel(new LocatorModel(this)),
    m_completionList(new CompletionList(this)),
    m_filterMenu(new QMenu(this)),
    m_refreshAction(new QAction(tr("Refresh"), this)),
    m_configureAction(new QAction(ICore::msgShowOptionsDialog(), this)),
    m_fileLineEdit(new Utils::FancyLineEdit)
{
    // Explicitly hide the completion list popup.
    m_completionList->hide();

    setAttribute(Qt::WA_Hover);
    setFocusProxy(m_fileLineEdit);
    resize(200, 90);
    QSizePolicy sizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    setSizePolicy(sizePolicy);
    setMinimumSize(QSize(200, 0));

    QHBoxLayout *layout = new QHBoxLayout(this);
    setLayout(layout);
    layout->setMargin(0);
    layout->addWidget(m_fileLineEdit);

    const QPixmap pixmap = Utils::Icons::MAGNIFIER.pixmap();
    m_fileLineEdit->setFiltering(true);
    m_fileLineEdit->setButtonPixmap(Utils::FancyLineEdit::Left, pixmap);
    m_fileLineEdit->setButtonToolTip(Utils::FancyLineEdit::Left, tr("Options"));
    m_fileLineEdit->setFocusPolicy(Qt::ClickFocus);
    m_fileLineEdit->setButtonVisible(Utils::FancyLineEdit::Left, true);
    // We set click focus since otherwise you will always get two popups
    m_fileLineEdit->setButtonFocusPolicy(Utils::FancyLineEdit::Left, Qt::ClickFocus);
    m_fileLineEdit->setAttribute(Qt::WA_MacShowFocusRect, false);

    m_fileLineEdit->installEventFilter(this);
    this->installEventFilter(this);

    m_completionList->setModel(m_locatorModel);
    m_completionList->resize();
    connect(m_locatorModel, &QAbstractItemModel::columnsInserted,
            m_completionList, &CompletionList::resizeHeaders);

    m_filterMenu->addAction(m_refreshAction);
    m_filterMenu->addAction(m_configureAction);

    m_fileLineEdit->setButtonMenu(Utils::FancyLineEdit::Left, m_filterMenu);

    connect(m_refreshAction, &QAction::triggered,
            locator, [locator]() { locator->refresh(); });
    connect(m_configureAction, &QAction::triggered, this, &LocatorWidget::showConfigureDialog);
    connect(m_fileLineEdit, &QLineEdit::textChanged,
        this, &LocatorWidget::showPopup);
    connect(m_completionList, &QAbstractItemView::activated,
            this, &LocatorWidget::scheduleAcceptEntry);

    m_entriesWatcher = new QFutureWatcher<LocatorFilterEntry>(this);
    connect(m_entriesWatcher, &QFutureWatcher<LocatorFilterEntry>::resultsReadyAt,
            this, &LocatorWidget::addSearchResults);
    connect(m_entriesWatcher, &QFutureWatcher<LocatorFilterEntry>::finished,
            this, &LocatorWidget::handleSearchFinished);

    m_showPopupTimer.setInterval(100);
    m_showPopupTimer.setSingleShot(true);
    connect(&m_showPopupTimer, &QTimer::timeout, this, &LocatorWidget::showPopupNow);

    m_progressIndicator = new Utils::ProgressIndicator(Utils::ProgressIndicator::Small,
                                                       m_fileLineEdit);
    m_progressIndicator->raise();
    m_progressIndicator->hide();
    m_showProgressTimer.setSingleShot(true);
    m_showProgressTimer.setInterval(50); // don't show progress for < 50ms tasks
    connect(&m_showProgressTimer, &QTimer::timeout, [this]() { setProgressIndicatorVisible(true);});

    Command *locateCmd = ActionManager::command(Constants::LOCATE);
    if (QTC_GUARD(locateCmd)) {
        connect(locateCmd, &Command::keySequenceChanged, this, [this,locateCmd] {
            updatePlaceholderText(locateCmd);
        });
        updatePlaceholderText(locateCmd);
    }

    connect(locator, &Locator::filtersChanged, this, &LocatorWidget::updateFilterList);
    updateFilterList();
}

void LocatorWidget::updatePlaceholderText(Command *command)
{
    QTC_ASSERT(command, return);
    if (command->keySequence().isEmpty())
        m_fileLineEdit->setPlaceholderText(tr("Type to locate"));
    else
        m_fileLineEdit->setPlaceholderText(tr("Type to locate (%1)").arg(
                                        command->keySequence().toString(QKeySequence::NativeText)));
}

void LocatorWidget::updateFilterList()
{
    m_filterMenu->clear();
    const QList<ILocatorFilter *> filters = Locator::filters();
    for (ILocatorFilter *filter : filters) {
        Command *cmd = ActionManager::command(filter->actionId());
        if (cmd)
            m_filterMenu->addAction(cmd->action());
    }
    m_filterMenu->addSeparator();
    m_filterMenu->addAction(m_refreshAction);
    m_filterMenu->addAction(m_configureAction);
}

bool LocatorWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_fileLineEdit && event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_P:
        case Qt::Key_N:
            if (keyEvent->modifiers() == Qt::KeyboardModifiers(Utils::HostOsInfo::controlModifier())) {
                event->accept();
                return true;
            }
        }
    } else if (obj == m_fileLineEdit && event->type() == QEvent::KeyPress) {
        if (m_possibleToolTipRequest)
            m_possibleToolTipRequest = false;
        if (QToolTip::isVisible())
            QToolTip::hideText();

        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            showCompletionList();
            QApplication::sendEvent(m_completionList, event);
            return true;
        case Qt::Key_Home:
        case Qt::Key_End:
            if (Utils::HostOsInfo::isMacHost()
                    != (keyEvent->modifiers() == Qt::KeyboardModifiers(Qt::ControlModifier))) {
                showCompletionList();
                QApplication::sendEvent(m_completionList, event);
                return true;
            }
            break;
        case Qt::Key_Enter:
        case Qt::Key_Return:
            QApplication::sendEvent(m_completionList, event);
            return true;
        case Qt::Key_Escape:
            m_completionList->hide();
            return true;
        case Qt::Key_Tab:
            m_completionList->next();
            return true;
        case Qt::Key_Backtab:
            m_completionList->previous();
            return true;
        case Qt::Key_Alt:
            if (keyEvent->modifiers() == Qt::AltModifier) {
                m_possibleToolTipRequest = true;
                return true;
            }
            break;
        case Qt::Key_P:
        case Qt::Key_N:
            if (keyEvent->modifiers() == Qt::KeyboardModifiers(Utils::HostOsInfo::controlModifier()))
            {
                if (keyEvent->key() == Qt::Key_P)
                    m_completionList->previous();
                else
                    m_completionList->next();
                return true;
            }
            break;
        default:
            break;
        }
    } else if (obj == m_fileLineEdit && event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (m_possibleToolTipRequest) {
            m_possibleToolTipRequest = false;
            if (m_completionList->isVisible()
                    && (keyEvent->key() == Qt::Key_Alt)
                    && (keyEvent->modifiers() == Qt::NoModifier)) {
                const QModelIndex index = m_completionList->currentIndex();
                if (index.isValid()) {
                    QToolTip::showText(m_completionList->pos() + m_completionList->visualRect(index).topRight(),
                                       m_locatorModel->data(index, Qt::ToolTipRole).toString());
                    return true;
                }
            }
        }
    } else if (obj == m_fileLineEdit && event->type() == QEvent::FocusOut) {
        QFocusEvent *fev = static_cast<QFocusEvent *>(event);
        if (fev->reason() != Qt::ActiveWindowFocusReason || !m_completionList->isActiveWindow())
            m_completionList->hide();
    } else if (obj == m_fileLineEdit && event->type() == QEvent::FocusIn) {
        QFocusEvent *fev = static_cast<QFocusEvent *>(event);
        if (fev->reason() != Qt::ActiveWindowFocusReason)
            showPopupNow();
    } else if (obj == m_window && event->type() == QEvent::Resize) {
        m_completionList->resize();
    } else if (obj == this && event->type() == QEvent::ParentChange) {
        if (m_window != window()) {
            if (m_window)
                m_window->removeEventFilter(this);
            m_window = window();
            if (m_window)
                m_window->installEventFilter(this);
        }
    } else if (obj == this && event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            if (!ke->modifiers()) {
                event->accept();
                QTimer::singleShot(0, this, &LocatorWidget::setFocusToCurrentMode);
                return true;
            }
            break;
        case Qt::Key_Alt:
            if (ke->modifiers() == Qt::AltModifier) {
                event->accept();
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void LocatorWidget::setFocusToCurrentMode()
{
    ModeManager::setFocusToCurrentMode();
}

void LocatorWidget::showCompletionList()
{
    const int border = m_completionList->frameWidth();
    const QSize size = m_completionList->preferredSize();
    const QRect rect(mapToGlobal(QPoint(-border, -size.height() - border)), size);
    m_completionList->setGeometry(rect);
    m_completionList->show();
}

void LocatorWidget::showPopup()
{
    m_updateRequested = true;
    m_showPopupTimer.start();
}

void LocatorWidget::showPopupNow()
{
    m_showPopupTimer.stop();
    updateCompletionList(m_fileLineEdit->text());
    showCompletionList();
}

QList<ILocatorFilter *> LocatorWidget::filtersFor(const QString &text, QString &searchText)
{
    const int length = text.size();
    int firstNonSpace;
    for (firstNonSpace = 0; firstNonSpace < length; ++firstNonSpace) {
        if (!text.at(firstNonSpace).isSpace())
            break;
    }
    const int whiteSpace = text.indexOf(QChar::Space, firstNonSpace);
    const QList<ILocatorFilter *> filters = Locator::filters();
    if (whiteSpace >= 0) {
        const QString prefix = text.mid(firstNonSpace, whiteSpace - firstNonSpace).toLower();
        QList<ILocatorFilter *> prefixFilters;
        foreach (ILocatorFilter *filter, filters) {
            if (prefix == filter->shortcutString()) {
                searchText = text.mid(whiteSpace).trimmed();
                prefixFilters << filter;
            }
        }
        if (!prefixFilters.isEmpty())
            return prefixFilters;
    }
    searchText = text.trimmed();
    return Utils::filtered(filters, &ILocatorFilter::isIncludedByDefault);
}

void LocatorWidget::setProgressIndicatorVisible(bool visible)
{
    if (!visible) {
        m_progressIndicator->hide();
        return;
    }
    const QSize iconSize = m_progressIndicator->sizeHint();
    m_progressIndicator->setGeometry(m_fileLineEdit->button(Utils::FancyLineEdit::Right)->geometry().x()
                                     - iconSize.width(),
                                     (m_fileLineEdit->height() - iconSize.height()) / 2 /*center*/,
                                     iconSize.width(),
                                     iconSize.height());
    m_progressIndicator->show();
}

void LocatorWidget::updateCompletionList(const QString &text)
{
    m_updateRequested = true;
    if (m_entriesWatcher->future().isRunning()) {
        // Cancel the old future. We may not just block the UI thread to wait for the search to
        // actually cancel, so try again when the finshed signal of the watcher ends up in
        // updateEntries() (which will call updateCompletionList again with the
        // requestedCompletionText)
        m_requestedCompletionText = text;
        m_entriesWatcher->future().cancel();
        return;
    }

    m_showProgressTimer.start();
    m_needsClearResult = true;
    QString searchText;
    const QList<ILocatorFilter *> filters = filtersFor(text, searchText);

    foreach (ILocatorFilter *filter, filters)
        filter->prepareSearch(searchText);
    QFuture<LocatorFilterEntry> future = Utils::runAsync(&runSearch, filters, searchText);
    m_entriesWatcher->setFuture(future);
}

void LocatorWidget::handleSearchFinished()
{
    m_showProgressTimer.stop();
    setProgressIndicatorVisible(false);
    m_updateRequested = false;
    if (m_rowRequestedForAccept >= 0) {
        acceptEntry(m_rowRequestedForAccept);
        m_rowRequestedForAccept = -1;
        return;
    }
    if (m_entriesWatcher->future().isCanceled()) {
        const QString text = m_requestedCompletionText;
        m_requestedCompletionText.clear();
        updateCompletionList(text);
        return;
    }

    if (m_needsClearResult) {
        m_locatorModel->clear();
        m_needsClearResult = false;
    }
}

void LocatorWidget::scheduleAcceptEntry(const QModelIndex &index)
{
    if (m_updateRequested) {
        // don't just accept the selected entry, since the list is not up to date
        // accept will be called after the update finished
        m_rowRequestedForAccept = index.row();
        // do not wait for the rest of the search to finish
        m_entriesWatcher->future().cancel();
    } else {
        acceptEntry(index.row());
    }
}

void LocatorWidget::acceptEntry(int row)
{
    if (!m_completionList->isVisible())
        return;
    if (row >= m_locatorModel->rowCount())
        return;
    const QModelIndex index = m_locatorModel->index(row, 0);
    if (!index.isValid())
        return;
    const LocatorFilterEntry entry = m_locatorModel->data(index, ItemDataRoles::ResultItemRole).value<LocatorFilterEntry>();
    Q_ASSERT(entry.filter != nullptr);
    QString newText;
    int selectionStart = -1;
    int selectionLength = 0;
    entry.filter->accept(entry, &newText, &selectionStart, &selectionLength);
    if (newText.isEmpty()) {
        m_completionList->hide();
        m_fileLineEdit->clearFocus();
    } else {
        showText(newText, selectionStart, selectionLength);
    }
}

void LocatorWidget::showText(const QString &text, int selectionStart, int selectionLength)
{
    if (!text.isEmpty())
        m_fileLineEdit->setText(text);
    m_fileLineEdit->setFocus();
    showPopupNow();
    ICore::raiseWindow(m_window);

    if (selectionStart >= 0) {
        m_fileLineEdit->setSelection(selectionStart, selectionLength);
        if (selectionLength == 0) // make sure the cursor is at the right position (Mac-vs.-rest difference)
            m_fileLineEdit->setCursorPosition(selectionStart);
    } else {
        m_fileLineEdit->selectAll();
    }
}

QString LocatorWidget::currentText() const
{
    return m_fileLineEdit->text();
}

void LocatorWidget::showConfigureDialog()
{
    ICore::showOptionsDialog(Constants::FILTER_OPTIONS_PAGE);
}

void LocatorWidget::addSearchResults(int firstIndex, int endIndex)
{
    if (m_needsClearResult) {
        m_locatorModel->clear();
        m_needsClearResult = false;
    }
    const bool selectFirst = m_locatorModel->rowCount() == 0;
    QList<LocatorFilterEntry> entries;
    for (int i = firstIndex; i < endIndex; ++i)
        entries.append(m_entriesWatcher->resultAt(i));
    m_locatorModel->addEntries(entries);
    if (selectFirst) {
        m_completionList->setCurrentIndex(m_locatorModel->index(0, 0));
        if (m_rowRequestedForAccept >= 0)
            m_rowRequestedForAccept = 0;
    }
}

} // namespace Internal
} // namespace Core
