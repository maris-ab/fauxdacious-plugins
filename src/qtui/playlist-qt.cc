/*
 * playlist.cc
 * Copyright 2014 Michał Lipski
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QSortFilterProxyModel>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/runtime.h>

#include "playlist-qt.h"
#include "playlist_header.h"
#include "playlist_model.h"

#include "../ui-common/menu-ops.h"

PlaylistWidget::PlaylistWidget (QWidget * parent, int playlist) :
    QTreeView (parent),
    m_playlist (playlist),
    model (new PlaylistModel (this, playlist)),
    proxyModel (new PlaylistProxyModel (this, playlist))
{
    /* setting up filtering model */
    proxyModel->setSourceModel (model);

    inUpdate = true; /* prevents changing focused row */
    setModel (proxyModel);
    inUpdate = false;

    auto header = new PlaylistHeader (this);
    setHeader (header);

    setAllColumnsShowFocus (true);
    setAlternatingRowColors (true);
    setAttribute (Qt::WA_MacShowFocusRect, false);
    setUniformRowHeights (true);
    setFrameShape (QFrame::NoFrame);
    setSelectionMode (ExtendedSelection);
    setDragDropMode (DragDrop);

    updateSettings ();
    header->updateColumns ();

    /* get initial selection and focus from core */
    inUpdate = true;
    updateSelection (0, 0);
    inUpdate = false;
}

PlaylistWidget::~PlaylistWidget ()
{
    delete model;
    delete proxyModel;
}

QModelIndex PlaylistWidget::rowToIndex (int row)
{
    if (row < 0)
        return QModelIndex ();

    return proxyModel->mapFromSource (model->index (row, firstVisibleColumn));
}

int PlaylistWidget::indexToRow (const QModelIndex & index)
{
    if (! index.isValid ())
        return -1;

    return proxyModel->mapToSource (index).row ();
}

void PlaylistWidget::contextMenuEvent (QContextMenuEvent * event)
{
    if (contextMenu)
        contextMenu->popup (event->globalPos ());
}

void PlaylistWidget::keyPressEvent (QKeyEvent * event)
{
    auto CtrlShiftAlt = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier;
    if (! (event->modifiers () & CtrlShiftAlt))
    {
        switch (event->key ())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            playCurrentIndex ();
            return;
        case Qt::Key_Right:
            aud_drct_seek (aud_drct_get_time () + aud_get_double ("qtui", "step_size") * 1000);
            return;
        case Qt::Key_Left:
            aud_drct_seek (aud_drct_get_time () - aud_get_double ("qtui", "step_size") * 1000);
            return;
        case Qt::Key_Space:
            aud_drct_play_pause ();
            return;
        case Qt::Key_Delete:
            pl_remove_selected ();
            return;
        case Qt::Key_Z:
            aud_drct_pl_prev ();
            return;
        case Qt::Key_X:
            aud_drct_play ();
            return;
        case Qt::Key_C:
            aud_drct_pause ();
            return;
        case Qt::Key_V:
            aud_drct_stop ();
            return;
        case Qt::Key_B:
            aud_drct_pl_next ();
            return;
        }
    }

    QTreeView::keyPressEvent (event);
}

void PlaylistWidget::mouseDoubleClickEvent (QMouseEvent * event)
{
    QModelIndex index = indexAt (event->pos ());
    if (! index.isValid ())
        return;

    if (event->button () == Qt::LeftButton)
        playCurrentIndex ();
}

/* Since Qt doesn't support both DragDrop and InternalMove at once,
 * this hack is needed to set the drag icon to "move" for internal drags. */
void PlaylistWidget::dragMoveEvent (QDragMoveEvent * event)
{
    if (event->source () == this)
        event->setDropAction (Qt::MoveAction);

    QTreeView::dragMoveEvent (event);

    if (event->source () == this)
        event->setDropAction (Qt::MoveAction);
}

void PlaylistWidget::dropEvent (QDropEvent * event)
{
    /* let Qt forward external drops to the PlaylistModel */
    if (event->source () != this)
        return QTreeView::dropEvent (event);

    int from = indexToRow (currentIndex ());
    if (from < 0)
        return;

    int to;
    switch (dropIndicatorPosition ())
    {
        case AboveItem: to = indexToRow (indexAt (event->pos ())); break;
        case BelowItem: to = indexToRow (indexAt (event->pos ())) + 1; break;
        case OnViewport: to = aud_playlist_entry_count (m_playlist); break;
        default: return;
    }

    /* Adjust the shift amount so that the selected entry closest to the
     * destination ends up at the destination. */
    if (to > from)
        to -= aud_playlist_selected_count (m_playlist, from, to - from);
    else
        to += aud_playlist_selected_count (m_playlist, to, from - to);

    aud_playlist_shift (m_playlist, from, to - from);

    event->acceptProposedAction ();
}

void PlaylistWidget::currentChanged (const QModelIndex & current, const QModelIndex & previous)
{
    QTreeView::currentChanged (current, previous);

    if (! inUpdate)
        aud_playlist_set_focus (m_playlist, indexToRow (current));
}

void PlaylistWidget::selectionChanged (const QItemSelection & selected,
 const QItemSelection & deselected)
{
    QTreeView::selectionChanged (selected, deselected);

    if (! inUpdate)
    {
        for (const QModelIndex & idx : selected.indexes ())
            aud_playlist_entry_set_selected (m_playlist, indexToRow (idx), true);
        for (const QModelIndex & idx : deselected.indexes ())
            aud_playlist_entry_set_selected (m_playlist, indexToRow (idx), false);
    }
}

void PlaylistWidget::scrollToCurrent (bool force)
{
    int entry = aud_playlist_get_position (m_playlist);

    if (aud_get_bool ("qtui", "autoscroll") || force)
    {
        aud_playlist_select_all (m_playlist, false);
        aud_playlist_entry_set_selected (m_playlist, entry, true);
        aud_playlist_set_focus (m_playlist, entry);

        scrollTo (rowToIndex (entry));
    }
}

void PlaylistWidget::updatePlaybackIndicator ()
{
    if (currentPos >= 0)
        model->entriesChanged (currentPos, 1);
}

void PlaylistWidget::getSelectedRanges (int rowsBefore, int rowsAfter,
 QItemSelection & selected, QItemSelection & deselected)
{
    int entries = aud_playlist_entry_count (m_playlist);

    QItemSelection ranges[2];
    QModelIndex first, last;
    bool prev = false;

    for (int row = rowsBefore; row < entries - rowsAfter; row ++)
    {
        auto idx = rowToIndex (row);
        if (! idx.isValid ())
            continue;

        bool sel = aud_playlist_entry_get_selected (m_playlist, row);

        if (sel != prev && first.isValid ())
            ranges[prev].merge (QItemSelection (first, last), QItemSelectionModel::Select);

        if (sel != prev || ! first.isValid ())
            first = idx;

        last = idx;
        prev = sel;
    }

    if (first.isValid ())
        ranges[prev].merge (QItemSelection (first, last), QItemSelectionModel::Select);

    selected = std::move (ranges[true]);
    deselected = std::move (ranges[false]);
}

void PlaylistWidget::updateSelection (int rowsBefore, int rowsAfter)
{
    QItemSelection selected, deselected;
    getSelectedRanges (rowsBefore, rowsAfter, selected, deselected);

    auto sel = selectionModel ();

    if (! selected.isEmpty ())
        sel->select (selected, sel->Select | sel->Rows);
    if (! deselected.isEmpty ())
        sel->select (deselected, sel->Deselect | sel->Rows);

    sel->setCurrentIndex (rowToIndex (aud_playlist_get_focus (m_playlist)), sel->NoUpdate);
}

void PlaylistWidget::playlistUpdate ()
{
    //x auto update = m_playlist.update_detail ();
    auto update = aud_playlist_update_detail (m_playlist);

    if (update.level == Playlist::NoUpdate)
        return;

    inUpdate = true;

    int entries = aud_playlist_entry_count (m_playlist);
    int changed = entries - update.before - update.after;

    if (update.level == Playlist::Structure)
    {
        int old_entries = model->rowCount ();
        int removed = old_entries - update.before - update.after;

        if (currentPos >= old_entries - update.after)
            currentPos += entries - old_entries;
        else if (currentPos >= update.before)
            currentPos = -1;

        model->entriesRemoved (update.before, removed);
        model->entriesAdded (update.before, changed);
    }
    else if (update.level == Playlist::Metadata || update.queue_changed)
        model->entriesChanged (update.before, changed);

    if (update.queue_changed)
    {
        for (int i = aud_playlist_queue_count (m_playlist); i --; )
        {
            int entry = aud_playlist_queue_get_entry (m_playlist, i);
            if (entry < update.before || entry >= entries - update.after)
                model->entriesChanged (entry, 1);
        }
    }

    int pos = aud_playlist_get_position (m_playlist);

    if (pos != currentPos)
    {
        if (currentPos >= 0)
            model->entriesChanged (currentPos, 1);
        if (pos >= 0)
            model->entriesChanged (pos, 1);

        currentPos = pos;
    }

    updateSelection (update.before, update.after);

    inUpdate = false;
}

void PlaylistWidget::playCurrentIndex ()
{
    aud_playlist_set_position (m_playlist, indexToRow (currentIndex ()));
    aud_playlist_play (m_playlist);
}

void PlaylistWidget::setFilter (const char * text)
{
    proxyModel->setFilter (text);

    int focus = aud_playlist_get_focus (m_playlist);
    QModelIndex index;

    // If there was a valid focus before filtering, Qt updates it for us via
    // currentChanged().  If not, we will set focus on the first visible row.

    if (focus >= 0)
        index = rowToIndex (focus);
    else
    {
        if (! proxyModel->rowCount ())
            return;

        index = proxyModel->index (0, 0);
        focus = indexToRow (index);
        aud_playlist_set_focus (m_playlist, focus);
    }

    if (! aud_playlist_entry_get_selected (m_playlist, focus))
    {
        aud_playlist_select_all (m_playlist, false);
        aud_playlist_entry_set_selected (m_playlist, focus, true);
    }

    scrollTo (index);
}

void PlaylistWidget::setFirstVisibleColumn (int col)
{
    inUpdate = true;
    firstVisibleColumn = col;

    // make sure current and selected indexes point to a visible column
    updateSelection (0, 0);

    inUpdate = false;
}

void PlaylistWidget::moveFocus (int distance)
{
    int visibleRows = proxyModel->rowCount ();
    if (! visibleRows)
        return;

    int row = currentIndex ().row ();
    row = aud::clamp (row + distance, 0, visibleRows - 1);
    setCurrentIndex (proxyModel->index (row, 0));
}

void PlaylistWidget::updateSettings ()
{
    setHeaderHidden (! aud_get_bool ("qtui", "playlist_headers"));
}
