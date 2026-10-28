// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Owned CUE/track fixtures exposing the SAF document contract with opaque IDs. */
public final class TestGameDocumentsProvider extends ContentProvider {
    private static final String CUE = "FILE \"track.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n";
    @Override public boolean onCreate() { return true; }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : new String[] {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_FLAGS });
        if ("children".equals(uri.getLastPathSegment())) {
            addDocument(cursor, "root/cue");
            addDocument(cursor, "root/track");
        } else {
            addDocument(cursor, "open".equals(uri.getPathSegments().get(0))
                ? "root/track" : DocumentsContract.getDocumentId(uri));
        }
        return cursor;
    }

    private static void addDocument(MatrixCursor cursor, String id) {
        String[] columns = cursor.getColumnNames();
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            switch (columns[i]) {
                case DocumentsContract.Document.COLUMN_DOCUMENT_ID: row[i] = id; break;
                case DocumentsContract.Document.COLUMN_DISPLAY_NAME:
                    row[i] = id.equals("root/cue") ? "disc.cue" : id.equals("root/track") ? "track.bin" : id.equals("root/archive") ? "fixture.zip" : "root";
                    break;
                case DocumentsContract.Document.COLUMN_MIME_TYPE:
                    row[i] = id.equals("root") ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream";
                    break;
                case DocumentsContract.Document.COLUMN_SIZE: row[i] = id.equals("root/cue") ? CUE.length() : 64 * 2352; break;
                default: row[i] = 0; break;
            }
        }
        cursor.addRow(row);
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        String id = DocumentsContract.getDocumentId(uri);
        if (id.equals("root/archive")) {
            int flags = mode.equals("r") ? ParcelFileDescriptor.MODE_READ_ONLY :
                ParcelFileDescriptor.MODE_WRITE_ONLY | ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE;
            return ParcelFileDescriptor.open(new File(getContext().getCacheDir(), "archive-game.fixture"), flags);
        }
        if (!mode.equals("r")) throw new FileNotFoundException("Read-only fixture");
        File file = new File(getContext().getCacheDir(), id.equals("root/cue") ? "swap-cue.fixture" : "swap-track.fixture");
        if (!file.exists()) {
            try (FileOutputStream output = new FileOutputStream(file)) {
                output.write(id.equals("root/cue") ? CUE.getBytes(StandardCharsets.UTF_8) : new byte[64 * 2352]);
            } catch (IOException error) {
                throw new FileNotFoundException(error.toString());
            }
        }
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Bundle call(String method, String arg, Bundle extras) {
        if (method.equals("clearArchiveFixture")) {
            if (!"com.sbro.emucorer".equals(arg)) throw new IllegalArgumentException("Test target only");
            new File(getContext().getCacheDir(), "archive-game.fixture").delete();
            return new Bundle();
        }
        if (!method.equals("grantFixture") && !method.equals("grantSingleFixture") && !method.equals("grantArchiveFixture")) return super.call(method, arg, extras);
        if (!"com.sbro.emucorer".equals(arg)) throw new IllegalArgumentException("Test target only");
        Uri tree = method.equals("grantArchiveFixture")
            ? DocumentsContract.buildDocumentUri("com.sbro.emucorer.test.documents", "root/archive")
            : method.equals("grantSingleFixture")
            ? DocumentsContract.buildDocumentUri("com.sbro.emucorer.test.documents", "root/track")
            : DocumentsContract.buildTreeDocumentUri("com.sbro.emucorer.test.documents", "root");
        int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
            Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
        if (method.equals("grantArchiveFixture")) flags |= Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        getContext().grantUriPermission(arg, tree, flags);
        return new Bundle();
    }
    @Override public String getType(Uri uri) { return "application/octet-stream"; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
}
