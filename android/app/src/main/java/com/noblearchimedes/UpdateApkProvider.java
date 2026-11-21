package com.noblearchimedes;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Minimal read-only ContentProvider that exposes only the verified downloaded
 * update APK to Android's package installer via a content:// URI (avoiding
 * FileUriExposedException on Android 8+ without requiring AndroidX).
 */
public final class UpdateApkProvider extends ContentProvider {
    public static final String AUTHORITY = "com.noblearchimedes.game.updateprovider";
    private static final String PATH = "/update.apk";
    private static final String MIME_APK = "application/vnd.android.package-archive";

    public static Uri uriForUpdate() {
        return new Uri.Builder()
                .scheme("content")
                .authority(AUTHORITY)
                .path(PATH)
                .build();
    }

    private File resolveApk(Uri uri) throws FileNotFoundException {
        Context ctx = getContext();
        if (ctx == null || uri == null || !PATH.equals(uri.getPath())) {
            throw new FileNotFoundException("Unknown update URI: " + uri);
        }
        File apk = UpdateChecker.updateApkFile(ctx);
        if (!apk.isFile()) {
            throw new FileNotFoundException("Update APK not found");
        }
        return apk;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return PATH.equals(uri.getPath()) ? MIME_APK : null;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        try {
            File apk = resolveApk(uri);
            String[] cols = (projection != null && projection.length > 0)
                    ? projection
                    : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
            Object[] row = new Object[cols.length];
            for (int i = 0; i < cols.length; i++) {
                if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) {
                    row[i] = "Noble-Archimedes-update.apk";
                } else if (OpenableColumns.SIZE.equals(cols[i])) {
                    row[i] = apk.length();
                } else {
                    row[i] = null;
                }
            }
            MatrixCursor cursor = new MatrixCursor(cols, 1);
            cursor.addRow(row);
            return cursor;
        } catch (FileNotFoundException e) {
            return null;
        }
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new SecurityException("Update APK provider is read-only");
        }
        return ParcelFileDescriptor.open(resolveApk(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Read-only provider");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only provider");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only provider");
    }
}
