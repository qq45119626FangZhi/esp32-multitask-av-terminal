package com.xiaozhi.display.viewmodel;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.xiaozhi.display.data.OtaApi;
import com.xiaozhi.display.data.OtaInfo;
import com.xiaozhi.display.data.OtaPackage;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * OTA 升级页面 ViewModel。
 *
 * 进度约定（getProgress）：
 *   PROGRESS_IDLE  = -2  空闲，不显示进度条
 *   PROGRESS_ANY   = -1  不确定进度（设备在写 flash，App 无法得知具体百分比）
 *   0..100             手机上传时的实际百分比
 *
 * 说明：设备的"安装"接口是同步的（一直等到写完 + 校验 + 切分区才返回），
 * 期间 httpd 任务被占住，所以 App 不轮询 /api/ota/status，只等这一次响应；
 * 具体的百分比在下位机屏幕上显示（用户需求 Q2）。
 */
public class OtaViewModel extends ViewModel {

    public static final int PROGRESS_IDLE = -2;
    public static final int PROGRESS_ANY = -1;

    private final MutableLiveData<OtaInfo> info = new MutableLiveData<>();
    private final MutableLiveData<List<OtaPackage>> packages = new MutableLiveData<>(Collections.emptyList());
    private final MutableLiveData<Boolean> loading = new MutableLiveData<>(false);
    private final MutableLiveData<Integer> progress = new MutableLiveData<>(PROGRESS_IDLE);
    private final MutableLiveData<String> progressText = new MutableLiveData<>("");
    private final MutableLiveData<Boolean> upgrading = new MutableLiveData<>(false);
    private final MutableLiveData<Event<String>> toast = new MutableLiveData<>();

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private volatile boolean busy;

    public LiveData<OtaInfo> getInfo() {
        return info;
    }

    public LiveData<List<OtaPackage>> getPackages() {
        return packages;
    }

    public LiveData<Boolean> getLoading() {
        return loading;
    }

    public LiveData<Integer> getProgress() {
        return progress;
    }

    public LiveData<String> getProgressText() {
        return progressText;
    }

    public LiveData<Boolean> getUpgrading() {
        return upgrading;
    }

    public LiveData<Event<String>> getToast() {
        return toast;
    }

    /** 刷新设备上的包列表 + 当前版本（进入页面、升级完成后调用） */
    public void refresh(String baseUrl) {
        loading.postValue(true);
        executor.execute(() -> {
            try {
                OtaInfo in = OtaApi.fetchInfo(baseUrl);
                info.postValue(in);
                packages.postValue(in.getPackages());
            } catch (Exception e) {
                packages.postValue(Collections.emptyList());
                toastText("读取升级包失败：" + e.getMessage());
            } finally {
                loading.postValue(false);
            }
        });
    }

    /** 功能1：点击设备上的包 → 用该包安装（等于换包重装） */
    public void applyPackage(String baseUrl, OtaPackage pkg) {
        if (busy) {
            toastText("正在升级，请等待完成");
            return;
        }
        busy = true;
        upgrading.postValue(true);
        progress.postValue(PROGRESS_ANY);
        progressText.postValue("设备正在写入 flash，请勿断电（约 20~40 秒）…");
        executor.execute(() -> {
            try {
                OtaApi.apply(baseUrl, pkg.getPath());
                onUpgradeDone(baseUrl);
            } catch (Exception e) {
                onUpgradeFailed("升级失败：" + e.getMessage());
            } finally {
                busy = false;
            }
        });
    }

    /** 功能2：把手机里的文件推给设备（设备写 flash 的同时存一份到 SD 卡） */
    public void uploadFromUri(Context appContext, String baseUrl, Uri uri, String name, long size) {
        if (busy) {
            toastText("正在升级，请等待完成");
            return;
        }
        busy = true;
        upgrading.postValue(true);
        progress.postValue(0);
        progressText.postValue("上传中 0%");

        final ContentResolver cr = appContext.getContentResolver();
        final File cacheDir = appContext.getCacheDir();
        executor.execute(() -> {
            File tmp = null;
            InputStream in = null;
            try {
                in = cr.openInputStream(uri);
                if (in == null) {
                    throw new IOException("无法打开所选文件");
                }
                long len = size;
                if (len <= 0) {
                    // 尺寸拿不到（部分网盘/文档提供方）：先落到应用缓存，再按实际长度上传
                    tmp = File.createTempFile("ota_", ".bin", cacheDir);
                    try (FileOutputStream fos = new FileOutputStream(tmp)) {
                        byte[] buf = new byte[16384];
                        int n;
                        while ((n = in.read(buf)) > 0) {
                            fos.write(buf, 0, n);
                        }
                    }
                    in.close();
                    in = null;
                    len = tmp.length();
                    in = new FileInputStream(tmp);
                }

                final long total = len;
                OtaApi.upload(baseUrl, name, in, total, (sent, all) -> {
                    int pct = all > 0 ? (int) (sent * 100 / all) : 0;
                    progress.postValue(pct);
                    progressText.postValue("上传中 " + pct + "%（" + (sent / 1024) + " KB / " + (all / 1024) + " KB）");
                });
                progress.postValue(100);
                onUpgradeDone(baseUrl);
            } catch (Exception e) {
                onUpgradeFailed("升级失败：" + e.getMessage());
            } finally {
                if (in != null) {
                    try {
                        in.close();
                    } catch (IOException ignored) {
                    }
                }
                if (tmp != null) {
                    //noinspection ResultOfMethodCallIgnored
                    tmp.delete();
                }
                busy = false;
            }
        });
    }

    /** 升级成功：设备 1.5 s 后自动重启，等它起来再刷新列表/版本 */
    private void onUpgradeDone(String baseUrl) {
        progressText.postValue("升级完成，设备重启中…");
        toastText("升级成功，设备重启中…");
        // 设备重启 + WiFi 重连大约 8~15 s，这里重试 3 次刷新
        for (int i = 0; i < 3; i++) {
            try {
                Thread.sleep(8000);
                OtaInfo in = OtaApi.fetchInfo(baseUrl);
                info.postValue(in);
                packages.postValue(in.getPackages());
                progress.postValue(PROGRESS_IDLE);
                progressText.postValue("");
                upgrading.postValue(false);
                toastText("设备已重启，当前固件 " + in.getShortVersion());
                return;
            } catch (Exception ignored) {
                // 设备还没起来，继续等
            }
        }
        progress.postValue(PROGRESS_IDLE);
        progressText.postValue("设备可能还在重启，请稍后点「刷新」");
        upgrading.postValue(false);
    }

    private void onUpgradeFailed(String message) {
        progress.postValue(PROGRESS_IDLE);
        progressText.postValue("");
        upgrading.postValue(false);
        toastText(message);
    }

    private void toastText(String text) {
        mainHandler.post(() -> toast.setValue(new Event<>(text)));
    }

    @Override
    protected void onCleared() {
        executor.shutdownNow();
        super.onCleared();
    }
}
