package com.xiaozhi.display.ui;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.lifecycle.ViewModelProvider;
import androidx.recyclerview.widget.LinearLayoutManager;

import com.xiaozhi.display.R;
import com.xiaozhi.display.data.DeviceConfig;
import com.xiaozhi.display.data.OtaInfo;
import com.xiaozhi.display.data.OtaPackage;
import com.xiaozhi.display.databinding.ActivityOtaBinding;
import com.xiaozhi.display.viewmodel.Event;
import com.xiaozhi.display.viewmodel.OtaViewModel;

import java.util.List;

/**
 * OTA 升级页：
 *   1) 列表显示下位机 SD 卡 /sdcard/OTA 里的升级包（含镜像里的版本号），
 *      点击某个包 → 用这个包重新安装（换包重装 / 回退旧版本）；
 *   2) 底部「选择手机文件并升级」→ 通过 SAF 选手机里的 .bin，直接推给下位机安装；
 *      下位机会同时把包存一份到 /sdcard/OTA，之后可以在上面的列表里再次点它。
 *
 * 升级期间下位机屏幕上有进度条；本页在上传时显示手机侧的真实百分比，
 * 安装阶段（设备写 flash）显示不确定进度，等待设备返回后自动刷新版本。
 */
public class OtaActivity extends AppCompatActivity {

    /** 系统文件选择器（SAF）：选 .bin 固件包，不需要存储权限 */
    private static final int REQ_PICK_FILE = 2001;

    private ActivityOtaBinding binding;
    private OtaViewModel viewModel;
    private OtaAdapter adapter;
    private String baseUrl;
    /** 升级中：屏蔽列表点击与选包，避免并发第二个升级请求 */
    private boolean upgrading;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        binding = ActivityOtaBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        baseUrl = DeviceConfig.getBaseUrl(this);
        viewModel = new ViewModelProvider(this).get(OtaViewModel.class);

        adapter = new OtaAdapter(this::confirmApply);
        binding.rvPackages.setLayoutManager(new LinearLayoutManager(this));
        binding.rvPackages.setAdapter(adapter);

        binding.tvDevice.setText("设备：" + baseUrl + "（长按首页标题可改地址）");
        binding.btnBack.setOnClickListener(v -> finish());
        binding.btnRefresh.setOnClickListener(v -> viewModel.refresh(baseUrl));
        binding.btnPickFile.setOnClickListener(v -> pickFile());

        observeViewModel();
        viewModel.refresh(baseUrl);
    }

    private void observeViewModel() {
        viewModel.getInfo().observe(this, this::renderInfo);
        viewModel.getPackages().observe(this, this::renderPackages);

        viewModel.getLoading().observe(this, loading -> {
            boolean busy = Boolean.TRUE.equals(loading);
            if (busy && adapter.getItemCount() == 0) {
                binding.tvEmpty.setVisibility(View.VISIBLE);
                binding.tvEmpty.setText(R.string.loading);
            }
        });

        viewModel.getProgress().observe(this, p -> {
            int v = p == null ? OtaViewModel.PROGRESS_IDLE : p;
            if (v == OtaViewModel.PROGRESS_IDLE) {
                binding.pbProgress.setVisibility(View.GONE);
            } else if (v == OtaViewModel.PROGRESS_ANY) {
                binding.pbProgress.setVisibility(View.VISIBLE);
                binding.pbProgress.setIndeterminate(true);
            } else {
                binding.pbProgress.setVisibility(View.VISIBLE);
                binding.pbProgress.setIndeterminate(false);
                binding.pbProgress.setProgress(v);
            }
        });

        viewModel.getProgressText().observe(this, text -> binding.tvProgress.setText(text == null ? "" : text));

        viewModel.getUpgrading().observe(this, value -> {
            upgrading = Boolean.TRUE.equals(value);
            binding.btnPickFile.setEnabled(!upgrading);
            binding.btnRefresh.setEnabled(!upgrading);
        });

        viewModel.getToast().observe(this, event -> {
            String text = consume(event);
            if (text != null) {
                Toast.makeText(this, text, Toast.LENGTH_LONG).show();
            }
        });
    }

    private String consume(Event<String> event) {
        return event == null ? null : event.getContentIfNotHandled();
    }

    private void renderInfo(OtaInfo info) {
        if (info == null) {
            return;
        }
        binding.tvCurVer.setText(getString(R.string.ota_cur_ver, info.getShortVersion(), info.getSlot()));
    }

    private void renderPackages(List<OtaPackage> packages) {
        adapter.submit(packages);
        boolean empty = packages == null || packages.isEmpty();
        binding.tvEmpty.setVisibility(empty ? View.VISIBLE : View.GONE);
        if (empty) {
            binding.tvEmpty.setText(R.string.ota_empty);
        }
    }

    /** 功能1 的前置确认：点设备上的包 → 用它重装 */
    private void confirmApply(OtaPackage pkg) {
        if (upgrading) {
            Toast.makeText(this, R.string.ota_busy_hint, Toast.LENGTH_SHORT).show();
            return;
        }
        if (!pkg.isValid()) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.ota_bad_package_title)
                    .setMessage(getString(R.string.ota_bad_package_msg, pkg.getName()))
                    .setPositiveButton(R.string.ota_continue, (d, w) -> viewModel.applyPackage(baseUrl, pkg))
                    .setNegativeButton(R.string.ota_cancel, null)
                    .show();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.ota_confirm_title)
                .setMessage(getString(R.string.ota_confirm_msg, pkg.getName(), pkg.getVersion()))
                .setPositiveButton(R.string.ota_start, (d, w) -> viewModel.applyPackage(baseUrl, pkg))
                .setNegativeButton(R.string.ota_cancel, null)
                .show();
    }

    /** 功能2：选手机里的固件包 */
    private void pickFile() {
        if (upgrading) {
            Toast.makeText(this, R.string.ota_busy_hint, Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // 部分机型把 .bin 识别成 application/octet-stream，用 */* 兜住
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQ_PICK_FILE);
        } catch (Exception e) {
            Toast.makeText(this, "无法打开文件选择器：" + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_FILE || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        String name = queryName(uri);
        long size = querySize(uri);

        if (name == null || !name.toLowerCase().endsWith(".bin")) {
            Toast.makeText(this, R.string.ota_need_bin, Toast.LENGTH_LONG).show();
            return;
        }
        if (size > 4L * 1024 * 1024) {
            Toast.makeText(this, getString(R.string.ota_too_big, (int) (size / (1024 * 1024))), Toast.LENGTH_LONG)
                    .show();
            return;
        }

        new AlertDialog.Builder(this)
                .setTitle(R.string.ota_upload_title)
                .setMessage(getString(R.string.ota_upload_msg, name, size > 0 ? (size / 1024) + " KB" : "大小未知"))
                .setPositiveButton(R.string.ota_start, (d, w) ->
                        viewModel.uploadFromUri(getApplicationContext(), baseUrl, uri, name, size))
                .setNegativeButton(R.string.ota_cancel, null)
                .show();
    }

    private String queryName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    return c.getString(idx);
                }
            }
        } catch (Exception ignored) {
            // 某些提供方不支持查询
        }
        String last = uri.getLastPathSegment();
        return last == null ? null : last;
    }

    private long querySize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.SIZE);
                if (idx >= 0 && !c.isNull(idx)) {
                    return c.getLong(idx);
                }
            }
        } catch (Exception ignored) {
            // 忽略：返回 -1 由 ViewModel 走"先落缓存再上传"的分支
        }
        return -1L;
    }
}
