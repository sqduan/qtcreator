/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2011 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** No Commercial Usage
**
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**************************************************************************/

#include "watchwindow.h"

#include "breakhandler.h"
#include "debuggeractions.h"
#include "debuggerconstants.h"
#include "debuggercore.h"
#include "debuggerdialogs.h"
#include "debuggerengine.h"
#include "debuggerstartparameters.h"
#include "watchdelegatewidgets.h"
#include "watchhandler.h"
#include "debuggertooltipmanager.h"
#include "memoryviewwidget.h"

#include <utils/qtcassert.h>
#include <utils/savedaction.h>

#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QVariant>

#include <QtGui/QApplication>
#include <QtGui/QPalette>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QHeaderView>
#include <QtGui/QItemDelegate>
#include <QtGui/QMenu>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QInputDialog>

/////////////////////////////////////////////////////////////////////
//
// WatchDelegate
//
/////////////////////////////////////////////////////////////////////

namespace Debugger {
namespace Internal {

static DebuggerEngine *currentEngine()
{
    return debuggerCore()->currentEngine();
}

class WatchDelegate : public QItemDelegate
{
public:
    explicit WatchDelegate(WatchWindow *parent)
        : QItemDelegate(parent), m_watchWindow(parent)
    {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
        const QModelIndex &index) const
    {
        // Value column: Custom editor. Apply integer-specific settings.
        if (index.column() == 1) {
            const QVariant::Type type =
                static_cast<QVariant::Type>(index.data(LocalsEditTypeRole).toInt());
            switch (type) {
            case QVariant::Bool:
                return new BooleanComboBox(parent);
            default:
                break;
            }
            WatchLineEdit *edit = WatchLineEdit::create(type, parent);
            edit->setFrame(false);
            IntegerWatchLineEdit *intEdit
                = qobject_cast<IntegerWatchLineEdit *>(edit);
            if (intEdit)
                intEdit->setBase(index.data(LocalsIntegerBaseRole).toInt());
            return edit;
        }

        // Standard line edits for the rest.
        QLineEdit *lineEdit = new QLineEdit(parent);
        lineEdit->setFrame(false);
        return lineEdit;
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const
    {
        // Standard handling for anything but the watcher name column (change
        // expression), which removes/recreates a row, which cannot be done
        // in model->setData().
        if (index.column() != 0) {
            QItemDelegate::setModelData(editor, model, index);
            return;
        }
        const QMetaProperty userProperty = editor->metaObject()->userProperty();
        QTC_ASSERT(userProperty.isValid(), return);
        const QString value = editor->property(userProperty.name()).toString();
        const QString exp = index.data(LocalsExpressionRole).toString();
        if (exp == value)
            return;
        m_watchWindow->removeWatchExpression(exp);
        m_watchWindow->watchExpression(value);
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
        const QModelIndex &) const
    {
        editor->setGeometry(option.rect);
    }

private:
    WatchWindow *m_watchWindow;
};

// Watch model query helpers.
static inline quint64 addressOf(const QModelIndex &m)
    { return m.data(LocalsAddressRole).toULongLong(); }
static inline quint64 pointerValueOf(const QModelIndex &m)
    { return m.data(LocalsPointerValueRole).toULongLong(); }
static inline QString nameOf(const QModelIndex &m)
    { return m.data().toString(); }
static inline uint sizeOf(const QModelIndex &m)
    { return m.data(LocalsSizeRole).toUInt(); }

// Helper functionality to obtain a address-sorted list of member variables
// of a watch model index and its size. Restrict this to the size passed
// in since static members can be contained that are in different areas.
struct MemberVariable
{
    MemberVariable(quint64 a = 0, uint s = 0, const QString &n = QString()) :
        address(a), size(s), name(n) {}

    quint64 address;
    uint size;
    QString name;
};

bool lessThanMV(const MemberVariable &m1, const MemberVariable &m2)
{
    return m1.address < m2.address;
}

static QVector<MemberVariable> sortedMemberVariables(const QModelIndex &m,
                                                     quint64 start, quint64 end)
{
    const int rowCount = m.model()->rowCount(m);
    if (!rowCount)
        return QVector<MemberVariable>();
    QVector<MemberVariable> result;
    result.reserve(rowCount);
    for (int r = 0; r < rowCount; r++) {
        const QModelIndex childIndex = m.child(r, 0);
        const quint64 childAddress = addressOf(childIndex);
        const uint childSize = sizeOf(childIndex);
        if (childAddress && childAddress >= start
            && (childAddress + childSize) <= end) { // Non-static, within area?
            result.push_back(MemberVariable(childAddress, childSize, nameOf(childIndex)));
        }
    }
    qStableSort(result.begin(), result.end(), lessThanMV);
    return result;
}

/*!
    \fn variableMemoryMarkup()

    \brief Creates markup for a variable in the memory view.

    Marks the 1st order children with alternating colors in the parent, that is, for
    \code
    struct Foo {
    char c1
    char c2
    int x2;
    }
    \endcode
    create something like:
    \code
    0 memberColor1
    1 memberColor2
    2 base color (padding area of parent)
    3 base color
    4 member color1
    ...
    \endcode

   Fixme: When dereferencing a pointer, the size of the pointee is not
   known, currently. So, we take an area of 1024 and fill the background
   with the default color so that just the members are shown
   (sizeIsEstimate=true). This could be fixed by passing the pointee size
   as well from the debugger, but would require expensive type manipulation.

    \sa Debugger::Internal::MemoryViewWidget
*/

typedef QList<MemoryViewWidget::Markup> MemoryViewWidgetMarkup;

static inline MemoryViewWidgetMarkup
    variableMemoryMarkup(const QModelIndex &m, quint64 address, quint64 size,
                         bool sizeIsEstimate,
                         const QTextCharFormat &defaultFormat,
                         const QColor &defaultBackground)
{
    enum { debug = 0 };

    typedef QPair<QColor, QString> ColorNamePair;
    typedef QVector<ColorNamePair> ColorNameVector;

    MemoryViewWidgetMarkup result;
    const QVector<MemberVariable> members = sortedMemberVariables(m, address, address + size);
    // Starting out from base, create an array representing the area filled with base
    // color. Fill children with alternating member colors,
    // leaving the padding areas of the parent colored with the base color.
    if (sizeIsEstimate && members.isEmpty())
        return result; // Fixme: Exact size not known, no point in filling if no children.
    const QColor baseColor = sizeIsEstimate ? defaultBackground : Qt::lightGray;
    const QString name = nameOf(m);
    ColorNameVector ranges(size, ColorNamePair(baseColor, name));
    if (!members.isEmpty()) {
        QColor memberColor1 = QColor(Qt::yellow).lighter();
        QColor memberColor2 = QColor(Qt::cyan).lighter();
        for (int m = 0; m < members.size(); m++) {
            QColor memberColor;
            if (m & 1) {
                memberColor = memberColor1;
                memberColor1 = memberColor1.darker(120);
            } else {
                memberColor = memberColor2;
                memberColor2 = memberColor2.darker(120);
            }
            const quint64 childOffset = members.at(m).address - address;
            const QString toolTip = WatchWindow::tr("%1.%2 at #%3")
                    .arg(name, members.at(m).name).arg(childOffset);
            qFill(ranges.begin() + childOffset,
                  ranges.begin() + childOffset + members.at(m).size,
                  ColorNamePair(memberColor, toolTip));
        }
    }

    if (debug) {
        QDebug dbg = qDebug().nospace();
        dbg << name << ' ' << address << ' ' << size << '\n';
        foreach (const MemberVariable &mv, members)
            dbg << ' ' << mv.name << ' ' << mv.address << ' ' << mv.size << '\n';
        QString name;
        for (unsigned i = 0; i < size; i++)
            if (name != ranges.at(i).second) {
                dbg << ",[" << i << ' ' << ranges.at(i).first << ' ' << ranges.at(i).second << ']';
                name = ranges.at(i).second;
            }
    }

    // Condense ranges of identical color into markup ranges.
    for (unsigned i = 0; i < size; i++) {
        const ColorNamePair &range = ranges.at(i);
        if (result.isEmpty() || result.back().format.background().color() != range.first) {
            QTextCharFormat format = defaultFormat;
            format.setBackground(QBrush(range.first));
            result.push_back(MemoryViewWidget::Markup(address + i, 1, format, range.second));
        } else {
            result.back().size++;
        }
    }

    if (debug) {
        QDebug dbg = qDebug().nospace();
        dbg << name << ' ' << address << ' ' << size << '\n';
        foreach (const MemberVariable &mv, members)
            dbg << ' ' << mv.name << ' ' << mv.address << ' ' << mv.size << '\n';
        QString name;
        for (unsigned i = 0; i < size; i++)
            if (name != ranges.at(i).second) {
                dbg << ',' << i << ' ' << ranges.at(i).first << ' ' << ranges.at(i).second;
                name = ranges.at(i).second;
            }
        dbg << '\n';
        foreach (const MemoryViewWidget::Markup &m, result)
            dbg << m.address <<  ' ' << m.size << ' '  << m.toolTip << '\n';
    }

    return result;
}

// Convenience to create a memory view of a variable.
static void addVariableMemoryView(DebuggerEngine *engine,
                                  const QModelIndex &m, bool deferencePointer,
                                  const QPoint &p, QWidget *parent)
{
    const QColor background = parent->palette().color(QPalette::Normal, QPalette::Base);
    LocalsMemoryViewWidget *w = new LocalsMemoryViewWidget(parent);
    const quint64 address = deferencePointer ? pointerValueOf(m) : addressOf(m);
    // Fixme: Get the size of pointee (see variableMemoryMarkup())?
    // Also, gdb does not report the size yet as of 8.4.2011
    const quint64 typeSize = sizeOf(m);
    const bool sizeIsEstimate = deferencePointer || !typeSize;
    const quint64 size    = sizeIsEstimate ? 1024 : typeSize;
    if (!address)
         return;
    const MemoryViewWidgetMarkup markup
        = variableMemoryMarkup(m, address, size, sizeIsEstimate,
                               w->textCharFormat(), background);
    w->init(address, qMax(size, LocalsMemoryViewWidget::defaultLength), nameOf(m));
    w->setMarkup(markup);
    w->move(p);
    engine->addMemoryView(w);
}

/////////////////////////////////////////////////////////////////////
//
// WatchWindow
//
/////////////////////////////////////////////////////////////////////

WatchWindow::WatchWindow(Type type, QWidget *parent)
  : QTreeView(parent),
    m_type(type)
{
    setObjectName(QLatin1String("WatchWindow"));
    m_grabbing = false;

    setFrameStyle(QFrame::NoFrame);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setWindowTitle(tr("Locals and Watchers"));
    setIndentation(indentation() * 9/10);
    setUniformRowHeights(true);
    setItemDelegate(new WatchDelegate(this));
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);

    QAction *useColors = debuggerCore()->action(UseAlternatingRowColors);
    setAlternatingRowColors(useColors->isChecked());

    QAction *adjustColumns = debuggerCore()->action(AlwaysAdjustLocalsColumnWidths);

    connect(useColors, SIGNAL(toggled(bool)),
        SLOT(setAlternatingRowColorsHelper(bool)));
    connect(adjustColumns, SIGNAL(triggered(bool)),
        SLOT(setAlwaysResizeColumnsToContents(bool)));
    connect(this, SIGNAL(expanded(QModelIndex)),
        SLOT(expandNode(QModelIndex)));
    connect(this, SIGNAL(collapsed(QModelIndex)),
        SLOT(collapseNode(QModelIndex)));
}

void WatchWindow::expandNode(const QModelIndex &idx)
{
    setModelData(LocalsExpandedRole, true, idx);
}

void WatchWindow::collapseNode(const QModelIndex &idx)
{
    setModelData(LocalsExpandedRole, false, idx);
}

void WatchWindow::keyPressEvent(QKeyEvent *ev)
{
    if (ev->key() == Qt::Key_Delete && m_type == WatchersType) {
        QModelIndex idx = currentIndex();
        QModelIndex idx1 = idx.sibling(idx.row(), 0);
        QString exp = idx1.data(LocalsRawExpressionRole).toString();
        removeWatchExpression(exp);
    } else if (ev->key() == Qt::Key_Return
            && ev->modifiers() == Qt::ControlModifier
            && m_type == LocalsType) {
        QModelIndex idx = currentIndex();
        QModelIndex idx1 = idx.sibling(idx.row(), 0);
        QString exp = model()->data(idx1).toString();
        watchExpression(exp);
    }
    QTreeView::keyPressEvent(ev);
}

void WatchWindow::dragEnterEvent(QDragEnterEvent *ev)
{
    //QTreeView::dragEnterEvent(ev);
    if (ev->mimeData()->hasFormat("text/plain")) {
        ev->setDropAction(Qt::CopyAction);
        ev->accept();
    }
}

void WatchWindow::dragMoveEvent(QDragMoveEvent *ev)
{
    //QTreeView::dragMoveEvent(ev);
    if (ev->mimeData()->hasFormat("text/plain")) {
        ev->setDropAction(Qt::CopyAction);
        ev->accept();
    }
}

void WatchWindow::dropEvent(QDropEvent *ev)
{
    if (ev->mimeData()->hasFormat("text/plain")) {
        watchExpression(ev->mimeData()->text());
        //ev->acceptProposedAction();
        ev->setDropAction(Qt::CopyAction);
        ev->accept();
    }
    //QTreeView::dropEvent(ev);
}

void WatchWindow::mouseDoubleClickEvent(QMouseEvent *ev)
{
    const QModelIndex idx = indexAt(ev->pos());
    if (!idx.isValid()) {
        // The "<Edit>" case.
        watchExpression(QString());
        return;
    }
    QTreeView::mouseDoubleClickEvent(ev);
}

// Text for add watch action with truncated expression
static inline QString addWatchActionText(QString exp)
{
    if (exp.isEmpty())
        return WatchWindow::tr("Watch Expression");
    if (exp.size() > 30) {
        exp.truncate(30);
        exp.append(QLatin1String("..."));
    }
    return WatchWindow::tr("Watch Expression \"%1\"").arg(exp);
}

// Text for add watch action with truncated expression
static inline QString removeWatchActionText(QString exp)
{
    if (exp.isEmpty())
        return WatchWindow::tr("Remove Watch Expression");
    if (exp.size() > 30) {
        exp.truncate(30);
        exp.append(QLatin1String("..."));
    }
    return WatchWindow::tr("Remove Watch Expression \"%1\"").arg(exp);
}

void WatchWindow::contextMenuEvent(QContextMenuEvent *ev)
{
    DebuggerEngine *engine = currentEngine();
    WatchHandler *handler = engine->watchHandler();

    const QModelIndex idx = indexAt(ev->pos());
    const QModelIndex mi0 = idx.sibling(idx.row(), 0);
    const QModelIndex mi1 = idx.sibling(idx.row(), 1);
    const QModelIndex mi2 = idx.sibling(idx.row(), 2);
    const quint64 address = addressOf(mi0);
    const uint size = sizeOf(mi0);
    const quint64 pointerValue = pointerValueOf(mi0);
    const QString exp = mi0.data(LocalsExpressionRole).toString();
    const QString type = mi2.data().toString();

    const QStringList alternativeFormats =
        mi0.data(LocalsTypeFormatListRole).toStringList();
    const int typeFormat =
        mi0.data(LocalsTypeFormatRole).toInt();
    const int individualFormat =
        mi0.data(LocalsIndividualFormatRole).toInt();
    const int effectiveIndividualFormat =
        individualFormat == -1 ? typeFormat : individualFormat;
    const int unprintableBase = handler->unprintableBase();

    QMenu formatMenu;
    QList<QAction *> typeFormatActions;
    QList<QAction *> individualFormatActions;
    QAction *clearTypeFormatAction = 0;
    QAction *clearIndividualFormatAction = 0;
    QAction *showUnprintableUnicode = 0;
    QAction *showUnprintableOctal = 0;
    QAction *showUnprintableHexadecimal = 0;
    formatMenu.setTitle(tr("Change Display Format..."));
    showUnprintableUnicode =
        formatMenu.addAction(tr("Treat All Characters as Printable"));
    showUnprintableUnicode->setCheckable(true);
    showUnprintableUnicode->setChecked(unprintableBase == 0);
    showUnprintableOctal =
        formatMenu.addAction(tr("Show Unprintable Characters as Octal"));
    showUnprintableOctal->setCheckable(true);
    showUnprintableOctal->setChecked(unprintableBase == 8);
    showUnprintableHexadecimal =
        formatMenu.addAction(tr("Show Unprintable Characters as Hexadecimal"));
    showUnprintableHexadecimal->setCheckable(true);
    showUnprintableHexadecimal->setChecked(unprintableBase == 16);
    if (idx.isValid() /*&& !alternativeFormats.isEmpty() */) {
        const QString spacer = QLatin1String("     ");
        formatMenu.addSeparator();
        QAction *dummy = formatMenu.addAction(
            tr("Change Display for Object Named \"%1\":").arg(mi0.data().toString()));
        dummy->setEnabled(false);
        clearIndividualFormatAction
            = formatMenu.addAction(spacer + tr("Use Display Format Based on Type"));
        //clearIndividualFormatAction->setEnabled(individualFormat != -1);
        clearIndividualFormatAction->setCheckable(true);
        clearIndividualFormatAction->setChecked(effectiveIndividualFormat == -1);
        for (int i = 0; i != alternativeFormats.size(); ++i) {
            const QString format = spacer + alternativeFormats.at(i);
            QAction *act = new QAction(format, &formatMenu);
            act->setCheckable(true);
            if (i == effectiveIndividualFormat)
                act->setChecked(true);
            formatMenu.addAction(act);
            individualFormatActions.append(act);
        }
        formatMenu.addSeparator();
        dummy = formatMenu.addAction(
            tr("Change Display for Type \"%1\":").arg(type));
        dummy->setEnabled(false);
        clearTypeFormatAction = formatMenu.addAction(spacer + tr("Automatic"));
        //clearTypeFormatAction->setEnabled(typeFormat != -1);
        //clearTypeFormatAction->setEnabled(individualFormat != -1);
        clearTypeFormatAction->setCheckable(true);
        clearTypeFormatAction->setChecked(typeFormat == -1);
        for (int i = 0; i != alternativeFormats.size(); ++i) {
            const QString format = spacer + alternativeFormats.at(i);
            QAction *act = new QAction(format, &formatMenu);
            act->setCheckable(true);
            //act->setEnabled(individualFormat != -1);
            if (i == typeFormat)
                act->setChecked(true);
            formatMenu.addAction(act);
            typeFormatActions.append(act);
        }
    } else {
        QAction *dummy = formatMenu.addAction(
            tr("Change Display for Type or Item..."));
        dummy->setEnabled(false);
    }

    const bool actionsEnabled = engine->debuggerActionsEnabled();
    const unsigned engineCapabilities = engine->debuggerCapabilities();
    const bool canHandleWatches = engineCapabilities & AddWatcherCapability;
    const DebuggerState state = engine->state();
    const bool canInsertWatches = (state==InferiorStopOk) || ((state==InferiorRunOk) && engine->acceptsWatchesWhileRunning());

    QMenu menu;
    QAction *actInsertNewWatchItem = menu.addAction(tr("Insert New Watch Item"));
    actInsertNewWatchItem->setEnabled(canHandleWatches && canInsertWatches);
    QAction *actSelectWidgetToWatch = menu.addAction(tr("Select Widget to Watch"));
    actSelectWidgetToWatch->setEnabled(canHandleWatches && (engine->canWatchWidgets()));

    // Offer to open address pointed to or variable address.
    const bool createPointerActions = pointerValue && pointerValue != address;

    menu.addSeparator();

    QAction *actSetWatchpointAtVariableAddress = 0;
    QAction *actSetWatchpointAtPointerValue = 0;
    const bool canSetWatchpoint = engineCapabilities & WatchpointCapability;
    if (canSetWatchpoint && address) {
        actSetWatchpointAtVariableAddress =
            new QAction(tr("Add Watchpoint at Object's Address (0x%1)")
                .arg(address, 0, 16), &menu);
        actSetWatchpointAtVariableAddress->
            setChecked(mi0.data(LocalsIsWatchpointAtAddressRole).toBool());
        if (createPointerActions) {
            actSetWatchpointAtPointerValue =
                new QAction(tr("Add Watchpoint at Referenced Address (0x%1)")
                    .arg(pointerValue, 0, 16), &menu);
            actSetWatchpointAtPointerValue->setCheckable(true);
            actSetWatchpointAtPointerValue->
                setChecked(mi0.data(LocalsIsWatchpointAtPointerValueRole).toBool());
        }
    } else {
        actSetWatchpointAtVariableAddress =
            new QAction(tr("Add Watchpoint"), &menu);
        actSetWatchpointAtVariableAddress->setEnabled(false);
    }
    actSetWatchpointAtVariableAddress->setToolTip(
        tr("Setting a watchpoint on an address will cause the program "
           "to stop when the data at the address it modified."));

    QAction *actWatchExpression = new QAction(addWatchActionText(exp), &menu);
    actWatchExpression->setEnabled(canHandleWatches && !exp.isEmpty());

    // Can remove watch if engine can handle it or session engine.
    QAction *actRemoveWatchExpression = new QAction(removeWatchActionText(exp), &menu);
    actRemoveWatchExpression->setEnabled(
        (canHandleWatches || state == DebuggerNotReady) && !exp.isEmpty());
    QAction *actRemoveWatches = new QAction(tr("Remove All Watch Items"), &menu);
    actRemoveWatches->setEnabled(!WatchHandler::watcherNames().isEmpty());

    if (m_type == LocalsType)
        menu.addAction(actWatchExpression);
    else {
        menu.addAction(actRemoveWatchExpression);
        menu.addAction(actRemoveWatches);
    }

    QMenu memoryMenu;
    memoryMenu.setTitle(tr("Open Memory Editor..."));
    QAction *actOpenMemoryEditAtVariableAddress = new QAction(&memoryMenu);
    QAction *actOpenMemoryEditAtPointerValue = new QAction(&memoryMenu);
    QAction *actOpenMemoryEditor = new QAction(&memoryMenu);
    QAction *actOpenMemoryViewAtVariableAddress = new QAction(&memoryMenu);
    QAction *actOpenMemoryViewAtPointerValue = new QAction(&memoryMenu);
    if (engineCapabilities & ShowMemoryCapability) {
        actOpenMemoryEditor->setText(tr("Open Memory Editor..."));
        if (address) {
            actOpenMemoryEditAtVariableAddress->setText(
                tr("Open Memory Editor at Object's Address (0x%1)")
                    .arg(address, 0, 16));
            actOpenMemoryViewAtVariableAddress->setText(
                    tr("Open Memory View at Object's Address (0x%1)")
                        .arg(address, 0, 16));
        } else {
            actOpenMemoryEditAtVariableAddress->setText(
                tr("Open Memory Editor at Object's Address"));
            actOpenMemoryEditAtVariableAddress->setEnabled(false);
            actOpenMemoryViewAtVariableAddress->setText(
                    tr("Open Memory View at Object's Address"));
            actOpenMemoryViewAtVariableAddress->setEnabled(false);
        }
        if (createPointerActions) {
            actOpenMemoryEditAtPointerValue->setText(
                tr("Open Memory Editor at Referenced Address (0x%1)")
                    .arg(pointerValue, 0, 16));
            actOpenMemoryViewAtPointerValue->setText(
                tr("Open Memory View at Referenced Address (0x%1)")
                    .arg(pointerValue, 0, 16));
        } else {
            actOpenMemoryEditAtPointerValue->setText(
                tr("Open Memory Editor at Referenced Address"));
            actOpenMemoryEditAtPointerValue->setEnabled(false);
            actOpenMemoryViewAtPointerValue->setText(
                tr("Open Memory View at Referenced Address"));
            actOpenMemoryViewAtPointerValue->setEnabled(false);
        }
        memoryMenu.addAction(actOpenMemoryViewAtVariableAddress);
        memoryMenu.addAction(actOpenMemoryViewAtPointerValue);
        memoryMenu.addAction(actOpenMemoryEditAtVariableAddress);
        memoryMenu.addAction(actOpenMemoryEditAtPointerValue);
        memoryMenu.addAction(actOpenMemoryEditor);
    } else {
        memoryMenu.setEnabled(false);
    }

    QAction *actCopy = new QAction(tr("Copy Contents to Clipboard"), &menu);

    menu.addAction(actInsertNewWatchItem);
    menu.addAction(actSelectWidgetToWatch);
    menu.addMenu(&formatMenu);
    menu.addMenu(&memoryMenu);
    menu.addAction(actSetWatchpointAtVariableAddress);
    if (actSetWatchpointAtPointerValue)
        menu.addAction(actSetWatchpointAtPointerValue);
    menu.addAction(actCopy );
    menu.addSeparator();

    menu.addAction(debuggerCore()->action(UseDebuggingHelpers));
    menu.addAction(debuggerCore()->action(UseToolTipsInLocalsView));
    menu.addAction(debuggerCore()->action(AutoDerefPointers));
    menu.addAction(debuggerCore()->action(ShowStdNamespace));
    menu.addAction(debuggerCore()->action(ShowQtNamespace));
    menu.addAction(debuggerCore()->action(SortStructMembers));

    QAction *actAdjustColumnWidths =
        menu.addAction(tr("Adjust Column Widths to Contents"));
    menu.addAction(debuggerCore()->action(AlwaysAdjustLocalsColumnWidths));
    menu.addSeparator();

    QAction *actClearCodeModelSnapshot
        = new QAction(tr("Refresh Code Model Snapshot"), &menu);
    actClearCodeModelSnapshot->setEnabled(actionsEnabled
        && debuggerCore()->action(UseCodeModel)->isChecked());
    menu.addAction(actClearCodeModelSnapshot);
    QAction *actShowInEditor
        = new QAction(tr("Show View Contents in Editor"), &menu);
    actShowInEditor->setEnabled(actionsEnabled);
    menu.addAction(actShowInEditor);
    menu.addAction(debuggerCore()->action(SettingsDialog));

    QAction *actCloseEditorToolTips = new QAction(tr("Close Editor Tooltips"), &menu);
    actCloseEditorToolTips->setEnabled(DebuggerToolTipManager::instance()->hasToolTips());
    menu.addAction(actCloseEditorToolTips);

    QAction *act = menu.exec(ev->globalPos());
    if (act == 0)
        return;

    if (act == actAdjustColumnWidths) {
        resizeColumnsToContents();
    } else if (act == actInsertNewWatchItem) {
        bool ok;
        QString newExp = QInputDialog::getText(this, tr("Enter watch expression"),
                                   tr("Expression:"), QLineEdit::Normal,
                                   QString(), &ok);
        if (ok && !newExp.isEmpty()) {
            watchExpression(newExp);
        }
    } else if (act == actOpenMemoryEditAtVariableAddress) {
        currentEngine()->openMemoryView(address);
    } else if (act == actOpenMemoryEditAtPointerValue) {
        currentEngine()->openMemoryView(pointerValue);
    } else if (act == actOpenMemoryEditor) {
        AddressDialog dialog;
        if (dialog.exec() == QDialog::Accepted)
            currentEngine()->openMemoryView(dialog.address());
    } else if (act == actOpenMemoryViewAtVariableAddress) {
        addVariableMemoryView(currentEngine(), mi0, false, ev->globalPos(), this);
    } else if (act == actOpenMemoryViewAtPointerValue) {
        addVariableMemoryView(currentEngine(), mi0, true, ev->globalPos(), this);
    } else if (act == actSetWatchpointAtVariableAddress) {
        setWatchpoint(address, size);
    } else if (act == actSetWatchpointAtPointerValue) {
        setWatchpoint(pointerValue, 1);
    } else if (act == actSelectWidgetToWatch) {
        grabMouse(Qt::CrossCursor);
        m_grabbing = true;
    } else if (act == actWatchExpression) {
        watchExpression(exp);
    } else if (act == actRemoveWatchExpression) {
        removeWatchExpression(exp);
    } else if (act == actCopy ) {
        const QString clipboardText = DebuggerTreeViewToolTipWidget::treeModelClipboardContents(model());
        QClipboard *clipboard = QApplication::clipboard();
#ifdef Q_WS_X11
        clipboard->setText(clipboardText, QClipboard::Selection);
#endif
        clipboard->setText(clipboardText, QClipboard::Clipboard);
    } else if (act == actRemoveWatches) {
        currentEngine()->watchHandler()->clearWatches();
    } else if (act == actClearCodeModelSnapshot) {
        debuggerCore()->clearCppCodeModelSnapshot();
    } else if (act == clearTypeFormatAction) {
        setModelData(LocalsTypeFormatRole, -1, mi1);
    } else if (act == clearIndividualFormatAction) {
        setModelData(LocalsIndividualFormatRole, -1, mi1);
    } else if (act == actShowInEditor) {
        QString contents = handler->editorContents();
        debuggerCore()->openTextEditor(tr("Locals & Watchers"), contents);
    } else if (act == showUnprintableUnicode) {
        handler->setUnprintableBase(0);
    } else if (act == showUnprintableOctal) {
        handler->setUnprintableBase(8);
    } else if (act == showUnprintableHexadecimal) {
        handler->setUnprintableBase(16);
    } else if (act == actCloseEditorToolTips) {
        DebuggerToolTipManager::instance()->closeAllToolTips();
    } else {
        for (int i = 0; i != typeFormatActions.size(); ++i) {
            if (act == typeFormatActions.at(i))
                setModelData(LocalsTypeFormatRole, i, mi1);
        }
        for (int i = 0; i != individualFormatActions.size(); ++i) {
            if (act == individualFormatActions.at(i))
                setModelData(LocalsIndividualFormatRole, i, mi1);
        }
    }
}

void WatchWindow::resizeColumnsToContents()
{
    resizeColumnToContents(0);
    resizeColumnToContents(1);
}

void WatchWindow::setAlwaysResizeColumnsToContents(bool on)
{
    if (!header())
        return;
    QHeaderView::ResizeMode mode = on
        ? QHeaderView::ResizeToContents : QHeaderView::Interactive;
    header()->setResizeMode(0, mode);
    header()->setResizeMode(1, mode);
}

bool WatchWindow::event(QEvent *ev)
{
    if (m_grabbing && ev->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mev = static_cast<QMouseEvent *>(ev);
        m_grabbing = false;
        releaseMouse();
        currentEngine()->watchPoint(mapToGlobal(mev->pos()));
    }
    return QTreeView::event(ev);
}

void WatchWindow::editItem(const QModelIndex &idx)
{
    Q_UNUSED(idx) // FIXME
}

void WatchWindow::setModel(QAbstractItemModel *model)
{
    QTreeView::setModel(model);

    setRootIsDecorated(true);
    if (header()) {
        setAlwaysResizeColumnsToContents(
            debuggerCore()->boolSetting(AlwaysAdjustLocalsColumnWidths));
        header()->setDefaultAlignment(Qt::AlignLeft);
        if (m_type != LocalsType)
            header()->hide();
    }

    connect(model, SIGNAL(layoutChanged()), SLOT(resetHelper()));
    connect(model, SIGNAL(enableUpdates(bool)), SLOT(setUpdatesEnabled(bool)));
    // Potentially left in disabled state in case engine crashes when expanding.
    setUpdatesEnabled(true);
}

void WatchWindow::setUpdatesEnabled(bool enable)
{
    //qDebug() << "ENABLING UPDATES: " << enable;
    QTreeView::setUpdatesEnabled(enable);
}

void WatchWindow::resetHelper()
{
    bool old = updatesEnabled();
    setUpdatesEnabled(false);
    resetHelper(model()->index(0, 0));
    setUpdatesEnabled(old);
}

void WatchWindow::resetHelper(const QModelIndex &idx)
{
    if (idx.data(LocalsExpandedRole).toBool()) {
        //qDebug() << "EXPANDING " << model()->data(idx, INameRole);
        if (!isExpanded(idx)) {
            expand(idx);
            for (int i = 0, n = model()->rowCount(idx); i != n; ++i) {
                QModelIndex idx1 = model()->index(i, 0, idx);
                resetHelper(idx1);
            }
        }
    } else {
        //qDebug() << "COLLAPSING " << model()->data(idx, INameRole);
        if (isExpanded(idx))
            collapse(idx);
    }
}

void WatchWindow::watchExpression(const QString &exp)
{
    currentEngine()->watchHandler()->watchExpression(exp);
}

void WatchWindow::removeWatchExpression(const QString &exp)
{
    currentEngine()->watchHandler()->removeWatchExpression(exp);
}

void WatchWindow::setModelData
    (int role, const QVariant &value, const QModelIndex &index)
{
    QTC_ASSERT(model(), return);
    model()->setData(index, value, role);
}

void WatchWindow::setWatchpoint(quint64 address, unsigned size)
{
    BreakpointParameters data(Watchpoint);
    data.address = address;
    data.size = size;
    BreakpointId id = breakHandler()->findWatchpoint(data);
    if (id) {
        qDebug() << "WATCHPOINT EXISTS";
        //   removeBreakpoint(index);
        return;
    }
    breakHandler()->appendBreakpoint(data);
}

} // namespace Internal
} // namespace Debugger

